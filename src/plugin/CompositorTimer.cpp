#include "CompositorTimer.h"

#include "FrameGpuTimer.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace csgov::CompositorTimer {

namespace {

// Minimal declarations rather than the OpenVR SDK.
//
// Only the shapes the two thunks pass through are needed, and every one of them
// is fixed by the published IVRCompositor ABI. Taking the SDK as a dependency
// would mean keeping a header in step with whatever the modlist ships, which is
// the coupling D-21 exists to remove.
using EVRCompositorError = int;
using EVREye = int;
using EVRSubmitFlags = int;

struct Texture_t
{
	void* handle;  // ID3D11Texture2D* for a D3D11 submit
	int eType;
	int eColorSpace;
};

struct VRTextureBounds_t
{
	float uMin, vMin, uMax, vMax;
};

using WaitGetPoses_t = EVRCompositorError (*)(void* self, void* renderPoses,
	std::uint32_t renderPoseCount, void* gamePoses, std::uint32_t gamePoseCount);
using Submit_t = EVRCompositorError (*)(void* self, EVREye eye, const Texture_t* texture,
	const VRTextureBounds_t* bounds, EVRSubmitFlags flags);

// Vtable slots, the same two the fork detours: WaitGetPoses is 2 and Submit
// is 5 on every IVRCompositor version Skyrim VR can be running.
constexpr std::size_t kWaitGetPosesSlot = 2;
constexpr std::size_t kSubmitSlot = 5;

FrameGpuTimer g_timer;
WaitGetPoses_t g_originalWaitGetPoses = nullptr;
Submit_t g_originalSubmit = nullptr;
std::atomic_bool g_active{ false };
std::once_flag g_installOnce;
bool g_installResult = false;
// The device is taken from the first submitted texture, so initialisation
// happens on the render thread rather than at install time.
std::atomic_bool g_deviceReady{ false };

void EnsureDeviceFromTexture(const Texture_t* a_texture)
{
	if (g_deviceReady.load(std::memory_order_acquire) || a_texture == nullptr ||
		a_texture->handle == nullptr) {
		return;
	}

	// The texture the application hands to the compositor was created by the
	// device that rendered the frame, so asking it is exact - no renderer
	// singleton to look up and no version-specific offsets to get wrong.
	auto* resource = static_cast<ID3D11Resource*>(a_texture->handle);
	Microsoft::WRL::ComPtr<ID3D11Device> device;
	resource->GetDevice(device.GetAddressOf());
	if (!device) {
		return;
	}

	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
	device->GetImmediateContext(context.GetAddressOf());
	if (!context) {
		return;
	}

	g_timer.Initialize(device.Get(), context.Get());
	g_deviceReady.store(true, std::memory_order_release);
	logger::info("[CompositorTimer] timing device acquired from the submitted texture");
}

EVRCompositorError WaitGetPosesThunk(void* a_self, void* a_renderPoses,
	std::uint32_t a_renderPoseCount, void* a_gamePoses, std::uint32_t a_gamePoseCount)
{
	// Close the previous frame BEFORE the wait. Everything between here and the
	// call returning is the compositor holding the application back, with the
	// GPU idle, and a timestamp delta counts idle as work (E-19, E-22).
	if (g_deviceReady.load(std::memory_order_acquire)) {
		g_timer.EndFrame();
	}

	const auto result = g_originalWaitGetPoses(
		a_self, a_renderPoses, a_renderPoseCount, a_gamePoses, a_gamePoseCount);

	// The runtime has released us to render: open the next bracket here.
	//
	// OnDrawSubmitted is what opens it - OnFrameSync only MOVES a bracket that
	// is already open, which is all the fork needed because CS opened at its
	// first draw and merely corrected the start if that came early. We have no
	// draw signal, so this call is the open, and it lands at exactly the point
	// the fork was correcting towards. A consequence worth stating: with the
	// bracket opened here by construction, FramesOpenedBeforeSync can only ever
	// read 0 on this path, so it is no longer the diagnostic that catches a bad
	// open point. The reference comparison in D-21 is.
	if (g_deviceReady.load(std::memory_order_acquire)) {
		g_timer.ArmFrame();
		g_timer.OnDrawSubmitted();
	}
	return result;
}

EVRCompositorError SubmitThunk(void* a_self, EVREye a_eye, const Texture_t* a_texture,
	const VRTextureBounds_t* a_bounds, EVRSubmitFlags a_flags)
{
	EnsureDeviceFromTexture(a_texture);

	// Stamped before the call, so submit-stage work done by whatever else hooks
	// this slot - Community Shaders performs its upscaling here - stays inside
	// the measurement. Later eyes re-stamp; the last one wins, which is the
	// documented behaviour of repeated End() on a timestamp query.
	if (g_deviceReady.load(std::memory_order_acquire)) {
		g_timer.OnCompositorSubmit();
	}
	return g_originalSubmit(a_self, a_eye, a_texture, a_bounds, a_flags);
}

// Replaces one vtable entry and returns what was there.
//
// Patching the vtable rather than the instance means anything else hooking the
// same slot still runs: whoever patches second calls the first through its own
// saved original, so Community Shaders' Submit hook and ours chain rather than
// displace each other.
void* PatchVtable(void* a_instance, std::size_t a_index, void* a_detour)
{
	auto** vtable = *reinterpret_cast<void***>(a_instance);
	DWORD previous = 0;
	if (!VirtualProtect(&vtable[a_index], sizeof(void*), PAGE_READWRITE, &previous)) {
		return nullptr;
	}
	void* original = vtable[a_index];
	vtable[a_index] = a_detour;
	DWORD ignored = 0;
	VirtualProtect(&vtable[a_index], sizeof(void*), previous, &ignored);
	return original;
}

void* FindCompositor()
{
	auto* module = GetModuleHandleW(L"openvr_api.dll");
	if (module == nullptr) {
		logger::warn("[CompositorTimer] openvr_api.dll is not loaded");
		return nullptr;
	}

	using GetGenericInterface_t = void* (*)(const char*, int*);
	auto* getInterface = reinterpret_cast<GetGenericInterface_t>(
		GetProcAddress(module, "VR_GetGenericInterface"));
	if (getInterface == nullptr) {
		logger::warn("[CompositorTimer] VR_GetGenericInterface not exported");
		return nullptr;
	}

	// Newest first. The interface version is part of the string, and asking for
	// one the runtime does not implement returns null rather than failing, so a
	// walk is both safe and the only way to work against OpenComposite as well
	// as SteamVR without pinning to whichever they implement this month.
	static constexpr const char* kVersions[]{
		"IVRCompositor_028", "IVRCompositor_027", "IVRCompositor_026", "IVRCompositor_025",
		"IVRCompositor_024", "IVRCompositor_023", "IVRCompositor_022",
	};
	for (const char* version : kVersions) {
		int error = 0;
		if (void* compositor = getInterface(version, &error); compositor != nullptr) {
			logger::info("[CompositorTimer] compositor found: {}", version);
			return compositor;
		}
	}
	logger::warn("[CompositorTimer] no known IVRCompositor version answered");
	return nullptr;
}

}

bool Install()
{
	std::call_once(g_installOnce, [] {
		void* compositor = FindCompositor();
		if (compositor == nullptr) {
			return;
		}

		g_originalWaitGetPoses = reinterpret_cast<WaitGetPoses_t>(
			PatchVtable(compositor, kWaitGetPosesSlot, reinterpret_cast<void*>(&WaitGetPosesThunk)));
		g_originalSubmit = reinterpret_cast<Submit_t>(
			PatchVtable(compositor, kSubmitSlot, reinterpret_cast<void*>(&SubmitThunk)));

		if (g_originalWaitGetPoses == nullptr || g_originalSubmit == nullptr) {
			logger::error("[CompositorTimer] could not patch the compositor vtable");
			return;
		}

		g_active.store(true, std::memory_order_release);
		g_installResult = true;
		logger::info("[CompositorTimer] hooked WaitGetPoses (slot {}) and Submit (slot {}); "
					 "GPU timing no longer needs a forked Community Shaders",
			kWaitGetPosesSlot, kSubmitSlot);
	});
	return g_installResult;
}

void Uninstall()
{
	// Deliberately does not unpatch. Another mod may have hooked the same slot
	// afterwards and be holding our thunk as its original; restoring ours would
	// cut it out of the chain. Going quiet is the safe half.
	g_active.store(false, std::memory_order_release);
	g_deviceReady.store(false, std::memory_order_release);
	g_timer.Release();
}

bool Active() noexcept
{
	return g_active.load(std::memory_order_acquire) &&
	       g_deviceReady.load(std::memory_order_acquire);
}

std::uint64_t LastFrameGpuTimeUs() noexcept
{
	return g_timer.GetLastFrameGpuTimeUs();
}

std::uint64_t LastFrameGpuTimeFrameIndex() noexcept
{
	return g_timer.GetLastFrameGpuTimeFrameIndex();
}

std::uint64_t FramesOpenedBeforeSync() noexcept
{
	return g_timer.GetFramesOpenedBeforeSync();
}

}
