#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace csgov {

// Summary of a window of frametimes, in milliseconds.
//
// Percentiles matter more than the mean here: a locked framerate is lost at the
// tail, not on average. See docs/MEASUREMENT_METHOD.md.
struct FrameStats
{
	std::size_t samples = 0;
	double meanMs = 0.0;
	double minMs = 0.0;
	double maxMs = 0.0;
	double p50Ms = 0.0;
	double p95Ms = 0.0;
	double p99Ms = 0.0;
	double stdDevMs = 0.0;

	// Fraction of samples exceeding the frame budget by any amount.
	//
	// This is NOT a stutter metric, and using it as one was a mistake. When the
	// compositor holds you at the refresh rate, frametime sits exactly ON the
	// budget, so symmetric micro-jitter puts about half the samples fractionally
	// above it. The 2026-08-03 run reported 33-44% "miss" for the four capped
	// presets in a scene that felt completely smooth.
	//
	// What it is genuinely good for is a margin indicator:
	//     ~0%    comfortably under budget, real headroom
	//     ~30-50% sitting on the cap
	//     >80%   actually over budget
	double missRate = 0.0;

	// Fraction of samples that missed the interval outright - the thing you feel
	// when you turn your head.
	//
	// Under vsync a frame either makes its interval (~13.9 ms at 72 Hz) or waits
	// for the next one (~27.8 ms). Frametimes therefore quantise towards
	// multiples of the interval, and the honest detector is a threshold placed
	// between the first and second multiple rather than at the budget itself.
	double dropRate = 0.0;

	[[nodiscard]] bool Valid() const noexcept { return samples > 0; }
};

// Multiple of the budget above which a frame is counted as dropped rather than
// merely over. Half way between making one interval and taking two, so it
// separates the two populations without being sensitive to where exactly the
// jitter around each lands.
inline constexpr double kDropThresholdFactor = 1.5;

// Nearest-rank percentile on an already-sorted, non-empty range.
[[nodiscard]] double PercentileSorted(const std::vector<double>& a_sorted, double a_percentile) noexcept;

// budgetMs is used only to compute missRate and dropRate; pass 0 to skip both.
[[nodiscard]] FrameStats ComputeStats(std::vector<double> a_samplesMs, double a_budgetMs) noexcept;

// Rolling window of frametimes with a bounded sample count.
class FrameWindow
{
public:
	explicit FrameWindow(std::size_t a_capacity = 512) noexcept;

	void Push(double a_frameTimeMs);
	void Clear() noexcept;

	[[nodiscard]] std::size_t Size() const noexcept { return _samples.size(); }
	[[nodiscard]] bool Full() const noexcept { return _samples.size() >= _capacity; }
	[[nodiscard]] FrameStats Stats(double a_budgetMs) const noexcept;
	[[nodiscard]] std::vector<double> Samples() const;

private:
	std::size_t _capacity;
	std::deque<double> _samples;
};

// Detects when frametime has settled after a transition.
//
// Settled means: at least MinSamples consecutive frames whose values all lie
// within Tolerance of the running median of that run. Any frame outside the
// band restarts the run. This deliberately requires a *sustained* quiet period
// rather than a single good frame.
class SettleDetector
{
public:
	struct Config
	{
		std::size_t minSamples = 30;
		double toleranceMs = 1.5;
		// Frames ignored immediately after a change, to skip the transition
		// spike itself (DLSS history reset, resource work).
		std::size_t warmupSamples = 5;
	};

	// Two constructors rather than a defaulted argument: GCC rejects
	// `Config a_config = {}` while the enclosing class is still incomplete,
	// though MSVC accepts it.
	SettleDetector() noexcept;
	explicit SettleDetector(Config a_config) noexcept;

	void Reset() noexcept;

	// Returns true on the frame at which settling is first detected.
	// Further calls return false until Reset().
	bool Push(double a_frameTimeMs);

	[[nodiscard]] bool Settled() const noexcept { return _settled; }
	[[nodiscard]] std::size_t SamplesSeen() const noexcept { return _seen; }

private:
	Config _config;
	std::vector<double> _run;
	std::size_t _seen = 0;
	bool _settled = false;
};

}
