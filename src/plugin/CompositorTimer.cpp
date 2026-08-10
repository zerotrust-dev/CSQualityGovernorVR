#include "CompositorTimer.h"

#include "FrameGpuTimer.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>

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

// Vtable slots, the same two the fork detours.
//
// These are indices into IVRCompositor_022's vtable specifically - the version
// SkyrimVR.exe uses and the one the fork validated against. They are NOT
// stable across interface versions, which is the whole reason FindCompositor
// asks for 022 by name instead of taking whatever answers first.
constexpr std::size_t kWaitGetPosesSlot = 2;
constexpr std::size_t kSubmitSlot = 5;

FrameGpuTimer g_timer;
WaitGetPoses_t g_originalWaitGetPoses = nullptr;
Submit_t g_originalSubmit = nullptr;
std::atomic_bool g_active{ false };
bool g_installResult = false;
// Bounded so a stack that never publishes a compositor stops asking rather
// than probing every frame for the whole session. ~30 s at 72 fps is far
// longer than the game takes to bring VR up.
std::atomic_uint32_t g_attempts{ 0 };
constexpr std::uint32_t kMaxAttempts = 2200;
// The device is taken from the first submitted texture, so initialisation
// happens on the render thread rather than at install time.
std::atomic_bool g_deviceReady{ false };

// D-23. Frames still to withhold, decremented once per frame in WaitGetPoses
// rather than per submit, because a frame is two submits (one per eye) and
// holding one eye but not the other would be worse than holding neither.
std::atomic_uint32_t g_holdFrames{ 0 };
std::atomic_uint64_t g_withheld{ 0 };
std::atomic_uint64_t g_replayed{ 0 };
// Bounds the damage from a caller asking for something absurd. Half a second of
// held frames is already far past the point where a hitch beats an artefact.
constexpr std::uint32_t kMaxHoldFrames = 36;

// The device and context, kept from the first submit so the frame copy below
// has something to copy with.
Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;

// D-23 revised: the last good frame, per eye, to hand back during the hold.
//
// Skipping the submit entirely was the first attempt, on the assumption that a
// runtime with nothing to show reprojects what it showed last. OpenComposite
// does not - it presents black, so the gridded panel became a black one. So we
// keep a copy and submit that instead: the runtime sees a frame every time and
// reprojects it to the current head pose, which is the frozen image we wanted.
//
// One copy per preset change, not per frame. Copying speculatively every frame
// against the chance of needing one would be ~7 GB/s here; we know exactly when
// a change is coming because we are the ones causing it.
struct HeldEye
{
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	D3D11_TEXTURE2D_DESC desc{};
	bool valid = false;
};
HeldEye g_held[2];
// Set when a hold is armed; cleared once BOTH eyes have been captured in this
// window. The frame captured is the one right after the change is requested,
// which the measurements show is still a valid old-preset frame (E-40).
std::atomic_bool g_captureNext{ false };
// Captured during THIS arming, per eye. Distinct from HeldEye::valid, which
// only says a texture exists from some earlier change.
//
// Conflating the two is what produced the black right eye: the clear condition
// tested `valid` for both eyes, that was already true from a previous hold, and
// so the flag cleared after the first eye and the second kept a stale copy. The
// submitted geometry is not constant - CS alternates between per-eye textures
// (3494 wide here) and a double-wide atlas (6988) with bounds selecting halves -
// so a stale copy is not merely an old picture, it is the wrong shape (E-43).
bool g_capturedThisArm[2]{ false, false };

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

	g_device = device;
	g_context = context;
	g_timer.Initialize(device.Get(), context.Get());
	g_deviceReady.store(true, std::memory_order_release);
	logger::info("[CompositorTimer] timing device acquired from the submitted texture");
}

// Copies one eye's submitted frame into our own texture, so it can be handed
// back for the next few frames.
//
// Returns false on anything unexpected rather than trying to recover: the
// caller falls back to withholding the frame, which is the previous behaviour
// and merely looks worse. Nothing here is worth risking the render thread for.
bool CaptureEye(std::size_t a_index, const Texture_t* a_texture)
{
	if (a_index >= 2 || a_texture == nullptr || a_texture->handle == nullptr || !g_device ||
		!g_context) {
		return false;
	}
	// eType 0 is TextureType_DirectX, i.e. an ID3D11Texture2D. Anything else is
	// not ours to copy.
	if (a_texture->eType != 0) {
		return false;
	}

	auto* source = static_cast<ID3D11Texture2D*>(a_texture->handle);
	D3D11_TEXTURE2D_DESC sourceDesc{};
	source->GetDesc(&sourceDesc);

	auto& held = g_held[a_index];
	const bool matches = held.texture && held.desc.Width == sourceDesc.Width &&
	                     held.desc.Height == sourceDesc.Height &&
	                     held.desc.Format == sourceDesc.Format &&
	                     held.desc.MipLevels == sourceDesc.MipLevels &&
	                     held.desc.ArraySize == sourceDesc.ArraySize &&
	                     held.desc.SampleDesc.Count == sourceDesc.SampleDesc.Count;

	if (!matches) {
		// The source description VERBATIM. Nothing here is ours to improve.
		//
		// The first version overrode MiscFlags and BindFlags on the reasoning
		// that we only needed something the compositor could read. That produced
		// a view split into rectangles with a black patch in one eye - the look
		// of a texture read with the wrong memory layout (E-42). OpenComposite
		// hands these on to an OpenXR runtime, which needs the sharing flags the
		// game created them with; clearing MiscFlags took those away.
		//
		// A copy that is to be handed back in place of the original has to BE
		// the original in every respect the runtime can observe.
		D3D11_TEXTURE2D_DESC desc = sourceDesc;

		held.texture.Reset();
		held.valid = false;
		if (FAILED(g_device->CreateTexture2D(&desc, nullptr, held.texture.GetAddressOf()))) {
			logger::warn("[CompositorTimer] could not create the hold texture for eye {} "
						 "({}x{} fmt {} misc 0x{:X} bind 0x{:X}); falling back to withholding",
				a_index, sourceDesc.Width, sourceDesc.Height,
				static_cast<int>(sourceDesc.Format), sourceDesc.MiscFlags, sourceDesc.BindFlags);
			return false;
		}
		held.desc = desc;
		// Logged once per size change, so the next diagnosis starts from facts
		// rather than from what the texture was assumed to be.
		logger::info("[CompositorTimer] hold texture eye {}: {}x{} fmt {} array {} samples {} "
					 "misc 0x{:X} bind 0x{:X}",
			a_index, desc.Width, desc.Height, static_cast<int>(desc.Format), desc.ArraySize,
			desc.SampleDesc.Count, desc.MiscFlags, desc.BindFlags);
	}

	g_context->CopyResource(held.texture.Get(), source);
	held.valid = true;
	return true;
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

	// D-23: one frame of the hold consumed. Decremented here, not in Submit,
	// so a frame is held for both eyes or neither.
	if (auto remaining = g_holdFrames.load(std::memory_order_acquire); remaining > 0) {
		g_holdFrames.store(remaining - 1, std::memory_order_release);
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

	// eEye is 0 for left, 1 for right on every IVRCompositor version.
	const std::size_t eye = a_eye == 0 ? 0u : 1u;

	// Capture the frame the change is about to invalidate. This one is still a
	// valid old-preset frame (E-40: the relatch does not begin for another two
	// or three frames), and it passes through normally as well.
	if (g_captureNext.load(std::memory_order_acquire) &&
		g_deviceReady.load(std::memory_order_acquire) && !g_capturedThisArm[eye]) {
		g_capturedThisArm[eye] = CaptureEye(eye, a_texture);
		if (g_capturedThisArm[0] && g_capturedThisArm[1]) {
			g_captureNext.store(false, std::memory_order_release);
		}
	}

	// D-23: during the relatch, hand back the copy instead of the frame being
	// rendered into targets that are being reallocated underneath it.
	//
	// Only a copy taken during THIS arming will do. An older one may be the
	// wrong geometry entirely, and submitting a per-eye texture with the bounds
	// meant for a double-wide one shows a black half.
	if (g_holdFrames.load(std::memory_order_acquire) > 0) {
		if (g_capturedThisArm[eye] && g_held[eye].valid) {
			Texture_t substitute = *a_texture;
			substitute.handle = g_held[eye].texture.Get();
			g_replayed.fetch_add(1, std::memory_order_relaxed);
			return g_originalSubmit(a_self, a_eye, &substitute, a_bounds, a_flags);
		}
		// No copy to give. Withholding shows black on OpenComposite, which is
		// worse than the artefact was - but it is the only remaining option,
		// and it should not happen: the capture runs before the first hold.
		g_withheld.fetch_add(1, std::memory_order_relaxed);
		return 0;  // VRCompositorError_None
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

	// EXACTLY the version the game itself uses. Not a walk, and never a newer
	// one.
	//
	// Asking for a different version does not fail - the runtime hands back a
	// different wrapper object implementing that version's ABI, with its own
	// vtable layout. Hooking that is wrong twice over: the slot indices do not
	// mean the same thing, and the game never calls through that object anyway,
	// so nothing would be measured even if it survived.
	//
	// It did not survive. Walking newest-first got IVRCompositor_024 from
	// OpenComposite, which exports up to 024, and patching slots 2 and 5 of it
	// crashed the game at the main menu (E-36).
	//
	// SkyrimVR.exe references exactly one compositor version, checked by
	// scanning the binary: IVRCompositor_022. The fork's slot 2 and slot 5 were
	// validated against that same interface, which is why they were right there.
	static constexpr const char* kGameVersion = "IVRCompositor_022";
	int lastError = 0;
	{
		int error = 0;
		if (void* compositor = getInterface(kGameVersion, &error); compositor != nullptr) {
			logger::info("[CompositorTimer] compositor found: {}", kGameVersion);
			return compositor;
		}
		lastError = error;
	}

	// Reported once per distinct error, because this runs every frame until it
	// succeeds and the first attempt is expected to fail: the runtime has not
	// handed out a compositor when the plugin loads, only once the game has
	// initialised VR. A repeated line per frame would bury the run that works.
	static int reported = -1;
	if (reported != lastError) {
		reported = lastError;
		logger::info("[CompositorTimer] no IVRCompositor yet (last error {}); retrying as the "
					 "game runs - VR is not initialised when the plugin loads",
			lastError);
	}
	return nullptr;
}

}

bool Install()
{
	// Retried rather than attempted once.
	//
	// The first attempt happens when the CS interface is acquired, and it
	// fails: OpenComposite has not published a compositor that early, so the
	// whole timer went quiet for a session and the run measured nothing. Since
	// there is no event for "VR is up", the honest approach is to keep asking
	// from the frame loop until it answers.
	if (g_installResult) {
		return true;
	}
	if (g_attempts.fetch_add(1, std::memory_order_relaxed) >= kMaxAttempts) {
		return false;
	}

	{
		void* compositor = FindCompositor();
		if (compositor == nullptr) {
			return false;
		}

		g_originalWaitGetPoses = reinterpret_cast<WaitGetPoses_t>(
			PatchVtable(compositor, kWaitGetPosesSlot, reinterpret_cast<void*>(&WaitGetPosesThunk)));
		g_originalSubmit = reinterpret_cast<Submit_t>(
			PatchVtable(compositor, kSubmitSlot, reinterpret_cast<void*>(&SubmitThunk)));

		if (g_originalWaitGetPoses == nullptr || g_originalSubmit == nullptr) {
			logger::error("[CompositorTimer] could not patch the compositor vtable");
			return false;
		}

		g_active.store(true, std::memory_order_release);
		g_installResult = true;
		logger::info("[CompositorTimer] hooked WaitGetPoses (slot {}) and Submit (slot {}) after "
					 "{} attempt(s); GPU timing no longer needs a forked Community Shaders",
			kWaitGetPosesSlot, kSubmitSlot, g_attempts.load(std::memory_order_relaxed));
	}
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

void HoldFrames(std::uint32_t a_frames) noexcept
{
	if (!g_active.load(std::memory_order_acquire)) {
		return;  // nothing hooked, nothing to hold
	}
	// Capture first, hold second. The next frame submitted is still a valid
	// old-preset one, so it is both the frame we show during the relatch and a
	// frame the player sees normally.
	//
	// Both eyes start uncaptured for this window. Carrying a copy over from a
	// previous change is not a saving: the submitted geometry alternates between
	// per-eye and double-wide, so the old copy may be the wrong shape (E-43).
	g_capturedThisArm[0] = false;
	g_capturedThisArm[1] = false;
	g_captureNext.store(true, std::memory_order_release);
	g_holdFrames.store(a_frames < kMaxHoldFrames ? a_frames : kMaxHoldFrames,
		std::memory_order_release);
}

std::uint64_t FramesWithheld() noexcept
{
	return g_withheld.load(std::memory_order_relaxed);
}

std::uint64_t FramesReplayed() noexcept
{
	return g_replayed.load(std::memory_order_relaxed);
}

}
