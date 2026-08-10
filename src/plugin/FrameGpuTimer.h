#pragma once

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

// Whole-frame GPU timer.
//
// Measures GPU *work* for the frame, not present-to-present time. The
// distinction matters in VR: when the compositor holds the application at the
// headset refresh rate, frametime saturates and stops carrying any information
// about how much headroom is left. A timestamp pair does not saturate, provided
// it does not enclose the wait.
//
// A D3D11 timestamp delta includes any GPU idle between the two points, so both
// boundaries are placed to exclude the compositor's frame pacing:
//
//   open  - armed at Present and issued by the frame's first draw, then moved
//           forward again when WaitGetPoses returns. The second part matters:
//           if the game issues any draw before WaitGetPoses, the bracket would
//           otherwise open ahead of the pacing block and count the wait as
//           work. Re-stamping is legitimate because a timestamp is an End-only
//           query whose value is the LAST End (documented behaviour).
//   close - the frame's final compositor submit, re-stamped per eye so the
//           reading ends after every eye's work. Present is only a fallback for
//           frames that never reach the compositor.
//
// The close boundary used to be Present, which measured +3.95 ms over the
// reference at low load because the pacing block sits in the submit path.
//
// Readback is multi-buffered and never blocks: a query set that is not ready is
// left for a later frame, and a frame whose set is still in flight is not timed
// at all.
class FrameGpuTimer
{
public:
	static constexpr uint32_t kFrameLatency = 4;

	// A frame costing more than this is not a measurement, it is a stall during
	// a load or a driver hiccup. Discarded rather than published.
	static constexpr uint64_t kMaxPlausibleFrameGpuTimeUs = 1000000;  // 1 s

	void Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context);
	void Release();

	// Called at Present, after the swap chain call: the next draw opens the
	// bracket for the frame that is about to be rendered.
	void ArmFrame();

	// Called from the per-draw hook. Opens the bracket on the first call after
	// ArmFrame; a single predictable branch on every later call.
	void OnDrawSubmitted();

	// Called when IVRCompositor::WaitGetPoses returns - the point at which the
	// runtime has released the application for this frame, and the equivalent
	// of the reference implementation's xrBeginFrame.
	//
	// If the bracket is already open the game drew before this point, so the
	// wait would be inside the measurement. Moving the start timestamp forward
	// here excludes it. How often that happens is counted, because an
	// independent review identified this boundary as the remaining suspect for
	// a ~1.5 ms over-read and "it never happens" is a claim that should be
	// measured rather than assumed.
	void OnFrameSync();

	// Called when the game hands the frame to the VR compositor. This is the
	// close boundary: the pacing block lives inside the submit path, and a
	// timestamp delta counts GPU idle, so measuring past this point charges the
	// wait to the frame's GPU cost.
	//
	// Measured before this existed: our reading floored at ~11.5 ms while the
	// reference reached 7.41 ms, and the excess grew with available headroom -
	// +3.95 ms at 7-8 ms of real GPU work against +0.85 ms at 15-16. That is
	// the difference between "no budget left" and "a fifth of it spare".
	void OnCompositorSubmit();

	// Called at Present. Closes the bracket if no submit happened - loading
	// screens and flat menus never reach the compositor - and collects results.
	void EndFrame();

	// Thread-safe readers for the plugin API. 0 means no measurement available.
	//
	// Both come from ONE atomic. Publishing the time and the frame index
	// separately let a reader pair a new index with an older time, which is
	// invisible in an average and corrupts any per-frame join - and per-frame
	// joins are exactly what this exists for.
	uint64_t GetLastFrameGpuTimeUs() const
	{
		return lastFramePacked.load(std::memory_order_acquire) & kGpuTimeMask;
	}

	// Monotonic index of the frame the reading above came from. Lets a consumer
	// distinguish a stale value from a genuinely stable one.
	uint64_t GetLastFrameGpuTimeFrameIndex() const
	{
		return lastFramePacked.load(std::memory_order_acquire) >> kGpuTimeBits;
	}

	// Diagnostic counters, for the log rather than the API.
	uint64_t GetFramesOpenedBeforeSync() const
	{
		return framesOpenedBeforeSync.load(std::memory_order_relaxed);
	}

private:
	struct FrameQueries
	{
		Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
		Microsoft::WRL::ComPtr<ID3D11Query> begin;
		Microsoft::WRL::ComPtr<ID3D11Query> end;
		uint64_t frameIndex = 0;
		bool inFlight = false;
	};

	// Non-blocking. Publishes any completed frame and frees its slot.
	void CollectCompletedFrames();
	bool TryCollectFrame(FrameQueries& a_frame);

	ID3D11DeviceContext* context = nullptr;
	FrameQueries frames[kFrameLatency];

	uint32_t writeSlot = 0;
	uint32_t openSlot = 0;
	bool initialized = false;
	bool armed = false;
	bool bracketOpen = false;
	// Whether a compositor submit has already stamped the end timestamp this
	// frame. Later submits re-stamp it; Present only stamps if none did.
	bool endStamped = false;
	uint64_t submittedFrameIndex = 0;

	// (frameIndex << kGpuTimeBits) | microseconds, published as one value so a
	// reader can never see a mismatched pair. 20 bits holds 1 048 575 us, and
	// anything above kMaxPlausibleFrameGpuTimeUs (1 000 000) is rejected before
	// it gets here.
	static constexpr uint32_t kGpuTimeBits = 20;
	static constexpr uint64_t kGpuTimeMask = (1ull << kGpuTimeBits) - 1;
	std::atomic_uint64_t lastFramePacked{ 0 };

	std::atomic_uint64_t framesOpenedBeforeSync{ 0 };
	uint64_t frameCounter = 0;
};
