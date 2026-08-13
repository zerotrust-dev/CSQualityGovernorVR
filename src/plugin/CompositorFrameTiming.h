#pragma once

#include <cstdint>

// D-25 / PROPOSED_SAFE_LADDER_GOVERNOR §17.2 — the capability test.
//
// Question this exists to answer, and nothing more: does OpenComposite populate
// `IVRCompositor::GetFrameTiming` with anything real?
//
// It matters because a whole architecture depends on the answer. If the runtime
// reports presents, drops and reprojections honestly, the governor can be told
// what the headset actually did with a frame - which is the one thing E-49 shows
// our own GPU timer cannot see. If it does not, the best available delivery
// signal is the application's own frame interval, and every design resting on
// compositor telemetry collapses to that.
//
// DIAGNOSTIC ONLY. Nothing here feeds a decision. It reads, it logs, it judges
// its own trustworthiness, and it stops there.
namespace csgov::FrameTiming {

// Verbatim from openvr.h (IVRCompositor_022). Reproduced rather than taken from
// the SDK for the same reason as CompositorTimer's declarations: the ABI is
// fixed and published, and a header dependency on whatever the modlist ships is
// the coupling D-21 exists to remove.
//
// Every field is present and in order even though only six are read. The struct
// is written by the runtime and `m_nSize` is how it decides what it may fill, so
// a truncated declaration would be a lie about our own buffer.
struct HmdMatrix34_t
{
	float m[3][4];
};

struct HmdVector3_t
{
	float v[3];
};

struct TrackedDevicePose_t
{
	HmdMatrix34_t mDeviceToAbsoluteTracking;
	HmdVector3_t vVelocity;
	HmdVector3_t vAngularVelocity;
	int eTrackingResult;
	bool bPoseIsValid;
	bool bDeviceIsConnected;
};

struct Compositor_FrameTiming
{
	std::uint32_t m_nSize;
	std::uint32_t m_nFrameIndex;
	std::uint32_t m_nNumFramePresents;
	std::uint32_t m_nNumMisPresented;
	std::uint32_t m_nNumDroppedFrames;
	std::uint32_t m_nReprojectionFlags;

	double m_flSystemTimeInSeconds;

	float m_flPreSubmitGpuMs;
	float m_flPostSubmitGpuMs;
	float m_flTotalRenderGpuMs;
	float m_flCompositorRenderGpuMs;
	float m_flCompositorRenderCpuMs;
	float m_flCompositorIdleCpuMs;

	float m_flClientFrameIntervalMs;
	float m_flPresentCallCpuMs;
	float m_flWaitForPresentCpuMs;
	float m_flSubmitFrameMs;

	float m_flWaitGetPosesCalledMs;
	float m_flNewPosesReadyMs;
	float m_flNewFrameReadyMs;
	float m_flCompositorUpdateStartMs;
	float m_flCompositorUpdateEndMs;
	float m_flCompositorRenderStartMs;

	TrackedDevicePose_t m_HmdPose;
};

// What one frame's reading told us. Deliberately small: the capability test is
// about whether the numbers move, not about their values.
struct Reading
{
	bool called = false;      // the vfunc was invoked
	bool returnedTrue = false;// it claimed success
	bool fresh = false;       // frame index advanced since the previous read
	std::uint32_t frameIndex = 0;
	std::uint32_t presents = 0;
	std::uint32_t misPresented = 0;
	std::uint32_t dropped = 0;
	std::uint32_t reprojectionFlags = 0;
	float clientFrameIntervalMs = 0.0f;
	float compositorRenderGpuMs = 0.0f;
	// Alignment canary. OpenComposite Unleashed's public source assigns this a
	// literal 8.0f ("sensible values until GPU timers implemented"). So:
	//
	//   reads exactly 8.0  -> our offsets are correct AND the build hardcodes,
	//                         which means every other field here is a constant
	//                         and none of them is worth reading.
	//   reads varying      -> the shipped build has real timers, and the
	//                         compositor figures are genuine measurements.
	//   reads nonsense     -> our struct layout does not match theirs and we
	//                         have been reading the wrong bytes all along.
	//
	// One known constant at a known offset separates three explanations that
	// otherwise need a whole session each to tell apart.
	float preSubmitGpuMs = 0.0f;
};

// Called once per frame from the WaitGetPoses hook, where the previous frame's
// timing is available and the call is already on the render thread.
//
// `a_compositor` is the same IVRCompositor_022 instance CompositorTimer found.
// Slot 8 is GetFrameTiming, verified against openvr.h by the two slots we
// already occupy: slot 2 is WaitGetPoses and slot 5 is Submit in the same
// vtable, and both hooks work. That agreement is the evidence the index is
// right - three crashes on this project came from calling a slot we had only
// assumed (E-34, E-36, E-37).
void Poll(void* a_compositor);

// Periodic verdict into the log. Says which of the capability criteria are met
// and, when they are not, which one failed - "no telemetry" and "frozen
// telemetry" and "telemetry that never reacts" are different answers and lead
// to different designs.
void ReportIfDue(double a_nowSeconds);

// The last reading, for the frame CSV. All zeros when unavailable.
[[nodiscard]] Reading Last() noexcept;

// True once the counters have been seen to move at all - not a full capability
// pass, just "this is not a stub returning constants".
[[nodiscard]] bool LooksAlive() noexcept;

}
