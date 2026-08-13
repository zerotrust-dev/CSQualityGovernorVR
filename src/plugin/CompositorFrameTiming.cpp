#include "CompositorFrameTiming.h"

#include <atomic>
#include <cstddef>

namespace csgov::FrameTiming {

namespace {

// Slot 8 of IVRCompositor_022. See the header for why this index is trusted.
constexpr std::size_t kGetFrameTimingSlot = 8;

using GetFrameTiming_t = bool (*)(void* self, Compositor_FrameTiming* timing,
	std::uint32_t framesAgo);

// Poll() runs on the render thread, inside the WaitGetPoses hook. Last() and
// ReportIfDue() are called from the frame loop. Everything shared between them
// is therefore atomic - the same reason FrameGpuTimer packs its reading into one
// value rather than publishing fields separately.
//
// Relaxed ordering throughout: these are counters and a diagnostic snapshot, and
// nothing downstream takes a decision on them. The one thing that must not
// happen is a torn read pairing this frame's index with last frame's counters,
// which the packing prevents.
std::atomic_uint64_t g_packedLast{ 0 };

// fresh(1) | presents(8) | dropped(8) | misPresented(8) | reproj(16) | ok(1)
constexpr std::uint64_t kFresh = 1ull << 0;
constexpr std::uint64_t kOk = 1ull << 1;
constexpr int kPresentsShift = 8;
constexpr int kDroppedShift = 16;
constexpr int kMisPresentedShift = 24;
constexpr int kReprojShift = 32;
constexpr std::uint64_t kByte = 0xFFull;
constexpr std::uint64_t kWord = 0xFFFFull;

[[nodiscard]] std::uint64_t Pack(const Reading& a_r) noexcept
{
	// Counters are clamped to a byte. A frame that dropped more than 255 times
	// is not a measurement worth a wider field, and saturating keeps the packing
	// total.
	const auto clamp = [](std::uint32_t v) -> std::uint64_t {
		return v > 0xFFu ? 0xFFull : static_cast<std::uint64_t>(v);
	};
	return (a_r.fresh ? kFresh : 0ull) | (a_r.returnedTrue ? kOk : 0ull) |
	       (clamp(a_r.presents) << kPresentsShift) | (clamp(a_r.dropped) << kDroppedShift) |
	       (clamp(a_r.misPresented) << kMisPresentedShift) |
	       ((static_cast<std::uint64_t>(a_r.reprojectionFlags) & kWord) << kReprojShift);
}

std::atomic_uint64_t g_calls{ 0 };
std::atomic_uint64_t g_returnedTrue{ 0 };
std::atomic_uint64_t g_freshFrames{ 0 };

// A stub can return true forever with a fixed struct. These record whether any
// field was ever seen to CHANGE, which is the only thing separating real
// telemetry from a plausible constant.
std::atomic_bool g_frameIndexMoved{ false };
std::atomic_bool g_presentsVaried{ false };
std::atomic_bool g_droppedVaried{ false };
std::atomic_bool g_misPresentedVaried{ false };
std::atomic_bool g_reprojectionVaried{ false };
std::atomic_bool g_intervalVaried{ false };

// Severity totals. If the fields are populated but never report a single drop
// across a session that visibly dropped frames, that is a distinct - and equally
// decisive - negative result.
std::atomic_uint64_t g_framesWithExtraPresents{ 0 };
std::atomic_uint64_t g_framesWithDrops{ 0 };
std::atomic_uint64_t g_framesWithMisPresent{ 0 };
std::atomic_uint64_t g_framesWithReprojection{ 0 };

// Render thread only - never read from elsewhere, so these stay plain.
std::uint32_t g_lastFrameIndex = 0;
bool g_haveLastFrameIndex = false;
std::uint32_t g_firstPresents = 0;
std::uint32_t g_firstDropped = 0;
std::uint32_t g_firstMisPresented = 0;
std::uint32_t g_firstReprojection = 0;
float g_firstInterval = 0.0f;
bool g_haveFirst = false;

// Frame-loop thread only.
double g_nextReportAt = 0.0;
constexpr double kReportIntervalSeconds = 30.0;
bool g_reportedFatal = false;

// Kept unpacked for the report's last-value line, which does not need to be
// consistent with anything.
std::atomic<std::uint32_t> g_lastReportedIndex{ 0 };
std::atomic<float> g_lastInterval{ 0.0f };
std::atomic<float> g_lastCompositorGpuMs{ 0.0f };
std::atomic<float> g_lastPreSubmitGpuMs{ 0.0f };

void Note(std::atomic_bool& a_flag, bool a_changed) noexcept
{
	if (a_changed && !a_flag.load(std::memory_order_relaxed)) {
		a_flag.store(true, std::memory_order_relaxed);
	}
}

}

void Poll(void* a_compositor)
{
	if (a_compositor == nullptr) {
		return;
	}

	auto* vtable = *reinterpret_cast<void***>(a_compositor);
	if (vtable == nullptr) {
		return;
	}
	auto fn = reinterpret_cast<GetFrameTiming_t>(vtable[kGetFrameTimingSlot]);
	if (fn == nullptr) {
		return;
	}

	// Zeroed every call, so a runtime that fills nothing leaves zeros rather
	// than the previous frame's values - "no telemetry" and "frozen telemetry"
	// are different answers and must not be made to look alike.
	Compositor_FrameTiming timing{};
	timing.m_nSize = static_cast<std::uint32_t>(sizeof(Compositor_FrameTiming));

	g_calls.fetch_add(1, std::memory_order_relaxed);
	// framesAgo 0 is the frame just completed, which is what WaitGetPoses has
	// finished waiting on.
	const bool ok = fn(a_compositor, &timing, 0);

	Reading reading{};
	reading.called = true;
	reading.returnedTrue = ok;

	if (!ok) {
		g_packedLast.store(Pack(reading), std::memory_order_relaxed);
		return;
	}
	g_returnedTrue.fetch_add(1, std::memory_order_relaxed);

	reading.frameIndex = timing.m_nFrameIndex;
	reading.presents = timing.m_nNumFramePresents;
	reading.misPresented = timing.m_nNumMisPresented;
	reading.dropped = timing.m_nNumDroppedFrames;
	reading.reprojectionFlags = timing.m_nReprojectionFlags;
	reading.clientFrameIntervalMs = timing.m_flClientFrameIntervalMs;
	reading.compositorRenderGpuMs = timing.m_flCompositorRenderGpuMs;
	reading.preSubmitGpuMs = timing.m_flPreSubmitGpuMs;

	if (g_haveLastFrameIndex && timing.m_nFrameIndex != g_lastFrameIndex) {
		reading.fresh = true;
		g_frameIndexMoved.store(true, std::memory_order_relaxed);
		g_freshFrames.fetch_add(1, std::memory_order_relaxed);
	}
	g_lastFrameIndex = timing.m_nFrameIndex;
	g_haveLastFrameIndex = true;

	if (!g_haveFirst) {
		g_haveFirst = true;
		g_firstPresents = reading.presents;
		g_firstDropped = reading.dropped;
		g_firstMisPresented = reading.misPresented;
		g_firstReprojection = reading.reprojectionFlags;
		g_firstInterval = reading.clientFrameIntervalMs;
	} else {
		Note(g_presentsVaried, reading.presents != g_firstPresents);
		Note(g_droppedVaried, reading.dropped != g_firstDropped);
		Note(g_misPresentedVaried, reading.misPresented != g_firstMisPresented);
		Note(g_reprojectionVaried, reading.reprojectionFlags != g_firstReprojection);
		// Any movement at all, not an epsilon comparison: a constant stub
		// returns the identical bit pattern every time.
		Note(g_intervalVaried, reading.clientFrameIntervalMs != g_firstInterval);
	}

	// Severity counted on fresh frames only. Re-reading the same completed frame
	// would otherwise multiply one event by however many times we polled it.
	if (reading.fresh) {
		if (reading.presents > 1) {
			g_framesWithExtraPresents.fetch_add(1, std::memory_order_relaxed);
		}
		if (reading.dropped > 0) {
			g_framesWithDrops.fetch_add(1, std::memory_order_relaxed);
		}
		if (reading.misPresented > 0) {
			g_framesWithMisPresent.fetch_add(1, std::memory_order_relaxed);
		}
		if (reading.reprojectionFlags != 0) {
			g_framesWithReprojection.fetch_add(1, std::memory_order_relaxed);
		}
	}

	g_lastReportedIndex.store(reading.frameIndex, std::memory_order_relaxed);
	g_lastInterval.store(reading.clientFrameIntervalMs, std::memory_order_relaxed);
	g_lastCompositorGpuMs.store(reading.compositorRenderGpuMs, std::memory_order_relaxed);
	g_lastPreSubmitGpuMs.store(reading.preSubmitGpuMs, std::memory_order_relaxed);
	g_packedLast.store(Pack(reading), std::memory_order_relaxed);
}

bool LooksAlive() noexcept
{
	return g_frameIndexMoved.load(std::memory_order_relaxed);
}

Reading Last() noexcept
{
	const auto packed = g_packedLast.load(std::memory_order_relaxed);

	Reading r{};
	r.called = packed != 0;
	r.returnedTrue = (packed & kOk) != 0;
	r.fresh = (packed & kFresh) != 0;
	r.presents = static_cast<std::uint32_t>((packed >> kPresentsShift) & kByte);
	r.dropped = static_cast<std::uint32_t>((packed >> kDroppedShift) & kByte);
	r.misPresented = static_cast<std::uint32_t>((packed >> kMisPresentedShift) & kByte);
	r.reprojectionFlags = static_cast<std::uint32_t>((packed >> kReprojShift) & kWord);
	r.frameIndex = g_lastReportedIndex.load(std::memory_order_relaxed);
	r.clientFrameIntervalMs = g_lastInterval.load(std::memory_order_relaxed);
	r.compositorRenderGpuMs = g_lastCompositorGpuMs.load(std::memory_order_relaxed);
	r.preSubmitGpuMs = g_lastPreSubmitGpuMs.load(std::memory_order_relaxed);
	return r;
}

void ReportIfDue(double a_nowSeconds)
{
	const auto calls = g_calls.load(std::memory_order_relaxed);
	if (calls == 0) {
		return;
	}
	if (a_nowSeconds < g_nextReportAt) {
		return;
	}
	g_nextReportAt = a_nowSeconds + kReportIntervalSeconds;

	const auto returnedTrue = g_returnedTrue.load(std::memory_order_relaxed);
	if (returnedTrue == 0) {
		if (!g_reportedFatal) {
			g_reportedFatal = true;
			logger::warn("[FrameTiming] CAPABILITY FAIL: GetFrameTiming returned false on all "
						 "{} calls. This runtime exposes no compositor delivery telemetry; the "
						 "application frame interval is the only delivery signal available.",
				calls);
		}
		return;
	}

	const auto fresh = g_freshFrames.load(std::memory_order_relaxed);
	const bool indexMoved = g_frameIndexMoved.load(std::memory_order_relaxed);
	const double freshRate = static_cast<double>(fresh) / static_cast<double>(calls);
	const auto last = Last();

	// Three separate questions, because they have different consequences:
	//   1. does it answer at all;
	//   2. is the frame index live;
	//   3. do the delivery fields react.
	logger::info("[FrameTiming] calls {} | returned true {} ({:.1f}%) | frame index advancing {} "
				 "({:.1f}% of polls fresh)",
		calls, returnedTrue, 100.0 * static_cast<double>(returnedTrue) / static_cast<double>(calls),
		indexMoved ? "YES" : "NO", 100.0 * freshRate);

	const bool presentsVaried = g_presentsVaried.load(std::memory_order_relaxed);
	const bool droppedVaried = g_droppedVaried.load(std::memory_order_relaxed);
	const bool misVaried = g_misPresentedVaried.load(std::memory_order_relaxed);
	const bool reprojVaried = g_reprojectionVaried.load(std::memory_order_relaxed);

	logger::info("[FrameTiming] fields ever varied: presents {} | dropped {} | misPresented {} | "
				 "reprojection {} | clientInterval {}",
		presentsVaried ? "yes" : "NO", droppedVaried ? "yes" : "NO", misVaried ? "yes" : "NO",
		reprojVaried ? "yes" : "NO",
		g_intervalVaried.load(std::memory_order_relaxed) ? "yes" : "NO");

	logger::info("[FrameTiming] fresh {} | extra presents {} | drops {} | misPresented {} | "
				 "reprojected {} | last: idx {} presents {} dropped {} reproj 0x{:X} "
				 "interval {:.2f} ms compositorGpu {:.2f} ms",
		fresh, g_framesWithExtraPresents.load(std::memory_order_relaxed),
		g_framesWithDrops.load(std::memory_order_relaxed),
		g_framesWithMisPresent.load(std::memory_order_relaxed),
		g_framesWithReprojection.load(std::memory_order_relaxed), last.frameIndex, last.presents,
		last.dropped, last.reprojectionFlags, last.clientFrameIntervalMs,
		last.compositorRenderGpuMs);

	// The verdict, stated rather than left to be inferred from three lines of
	// counters. Deliberately conservative: "usable" requires the fields to have
	// been seen to move, not merely to be present.
	const bool anyDeliveryFieldVaried =
		presentsVaried || droppedVaried || misVaried || reprojVaried;
	if (!indexMoved) {
		logger::warn("[FrameTiming] VERDICT so far: answers, but the frame index never advances. "
					 "Frozen or synthetic - not usable as delivery telemetry.");
	} else if (!anyDeliveryFieldVaried) {
		logger::warn("[FrameTiming] VERDICT so far: live frame index, but no delivery field has "
					 "EVER changed. Either the runtime does not populate them or this session "
					 "dropped nothing. Needs the deliberate-overload test before any conclusion.");
	} else {
		logger::info("[FrameTiming] VERDICT so far: live frame index and at least one delivery "
					 "field reacting. Promising - still requires the deliberate-overload test to "
					 "confirm the fields track real delivery failures.");
	}
}

}
