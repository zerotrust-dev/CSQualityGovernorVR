#pragma once

#include "GovernorCore.h"
#include "Presets.h"

#include <array>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace csgov {

// One recorded frame, as read from a *_frames.csv capture.
struct TraceFrame
{
	std::uint64_t wallMs = 0;
	double timeSeconds = 0.0;
	double frameTimeMs = 0.0;
	std::uint32_t presetPublicValue = 0;
	std::string state;
	std::uint64_t gpuUs = 0;
	std::uint64_t gpuFrameIndex = 0;
};

// The session's cost curve, fitted across the presets the sweep visited.
//
// Deliberately not a per-scene fit: a capture from ordinary play moves through
// many scenes, and Rule 7 in MEASUREMENT_METHOD.md is about exactly that. This
// is a session average and is used only to scale a counterfactual, never to
// state what a scene costs.
struct CostModel
{
	double tFixedMs = 0.0;
	double tScaledMs = 0.0;
	std::size_t presetsFitted = 0;
	double worstResidualMs = 0.0;

	[[nodiscard]] bool Valid() const noexcept { return presetsFitted >= 3 && tFixedMs > 0.0; }
	[[nodiscard]] double K() const noexcept { return tFixedMs > 0.0 ? tScaledMs / tFixedMs : 0.0; }
	[[nodiscard]] double PredictMs(double a_pixelFraction) const noexcept
	{
		return tFixedMs + tScaledMs * a_pixelFraction;
	}
};

// How a frame's cost is projected onto a preset it was not rendered at (D-14).
// Both exist because neither is obviously right, and a conclusion that depends
// on the choice is an artefact rather than a result.
enum class Counterfactual {
	Scaled,    // fixed and scaled costs move together with scene load
	Additive,  // only the fixed cost moves; per-pixel cost stays put
};

// What a replayed session produced. These are the terms of the objective in
// section 1 of the design doc, so a parameter sweep can be ranked by them
// rather than by eye.
struct ReplayResult
{
	std::size_t frames = 0;
	std::size_t decisions = 0;
	std::size_t changes = 0;
	double durationSeconds = 0.0;

	// The thing being maximised: mean rendered pixel fraction, weighted by how
	// long each preset was held.
	double timeWeightedPixelFraction = 0.0;
	// The constraint. Frames whose synthesised GPU time exceeded the budget.
	double overBudgetRate = 0.0;
	// Sustained oscillation shows up here: changes that reversed direction.
	std::size_t reversals = 0;
	double changesPerMinute = 0.0;

	double meanGpuMs = 0.0;
	double p95GpuMs = 0.0;

	// The preset held per decision interval, on the same grid and the same
	// origin as OptimalPlan::trajectory, so the two can be compared slot for
	// slot. Where the controller changed mid-interval this is the preset the
	// most frames were rendered at.
	//
	// Totals say a controller reached 86% of the optimum; only this says
	// whether the missing 14% is climbs it declined or descents it was late to,
	// which is the difference between two opposite fixes.
	std::vector<Preset> trajectory;

	// The frame-weighted mean pixel fraction actually rendered in each of those
	// intervals. Not derivable from `trajectory`: that names the preset the most
	// frames used, and attributing a whole interval to it credits a controller
	// that changed mid-interval with pixels it did not render. Doing exactly
	// that made the first divergence report account for 0.033 of an 0.081 gap.
	std::vector<double> intervalPixelFraction;

	// What the controller believed a one-rung climb cost, by the end of the
	// run, indexed as kPresets is by the preset being LEFT. Paired with whether
	// that belief was ever measured rather than left at its seed.
	//
	// The decision this exists to inform: if the controller declines climbs the
	// optimum takes, either its belief is pessimistic - and the fix is in how
	// the ratio is learned - or the belief is right and the gate on top of it
	// is too tight. Those want opposite changes, and the totals cannot separate
	// them.
	std::array<double, kPresets.size()> learnedStepRatio{};
	std::array<bool, kPresets.size()> learnedStepMeasured{};
};

// What an adjacent step actually cost in the capture, from the dwell buckets:
// the mean GPU time at rung i+1 over the mean at rung i. Indexed by the rung
// being left, so it lines up with ReplayResult::learnedStepRatio.
//
// This is the trace's own answer, independent of the linear cost model and of
// anything the controller believed - which is the point, since the controller's
// belief is the thing under suspicion.
struct StepObservation
{
	double ratio = 0.0;  // 0 when either end was never dwelt at
	std::size_t samplesFrom = 0;
	std::size_t samplesTo = 0;

	[[nodiscard]] bool Valid() const noexcept { return ratio > 0.0; }
};

[[nodiscard]] std::vector<StepObservation> ObserveStepRatios(
	const std::vector<TraceFrame>& a_trace);

// The best a controller with OUR actuator could have done on this trace.
//
// Not the perfect-foresight oracle, which switches every frame and is therefore
// an unreachable bound. This obeys the real constraints - one lever, a minimum
// dwell between changes, and a decision cadence - and is computed by dynamic
// programming over the preset ladder, so it is the tightest honest target for
// "how well did the controller do".
//
// It also answers a question no controller run can: how many changes the scene
// itself demanded. A design making twice that many is churning; one making half
// is sluggish. Both are visible without anyone judging a session by feel.
struct OptimalPlan
{
	double timeWeightedPixelFraction = 0.0;
	std::size_t changes = 0;
	double overBudgetRate = 0.0;
	double durationSeconds = 0.0;
	// The chosen preset per decision interval, so a controller's trajectory can
	// be diffed against it rather than only its totals.
	std::vector<Preset> trajectory;
	// Frames in each of those intervals. These are the weights behind
	// timeWeightedPixelFraction, exposed so a comparison against a controller
	// can use the same ones and reconcile with the headline instead of
	// approximating it.
	std::vector<double> intervalFrames;

	[[nodiscard]] bool Valid() const noexcept { return durationSeconds > 0.0; }
};

// The difference between a controller and the optimum, decomposed.
//
// deficit and surplus are the two halves of the same gap and satisfy
//   optimumPixelFraction - controllerPixelFraction == deficit - surplus
// exactly, by construction. That identity is the point: a breakdown that does
// not add up to the headline it explains invites picking whichever number
// suits, and the first version of this accounted for 0.033 of an 0.081 gap
// without saying so.
struct Divergence
{
	std::size_t intervals = 0;
	double optimumPixelFraction = 0.0;
	double controllerPixelFraction = 0.0;
	// Pixels the optimum rendered and the controller did not, and vice versa.
	double deficit = 0.0;
	double surplus = 0.0;
	// Intervals where the controller sat below / above / level with the plan.
	std::size_t below = 0;
	std::size_t above = 0;
	std::size_t level = 0;
	// Moves of the plan the controller did not match within the same interval.
	std::size_t unfollowedClimbs = 0;
	std::size_t unfollowedDescents = 0;
	// Interval count at each rung, indexed as kPresets is.
	std::vector<std::size_t> optimumRungs;
	std::vector<std::size_t> controllerRungs;

	[[nodiscard]] bool Valid() const noexcept { return intervals > 0; }
};

// Both must come from the same trace and the same config, or the grids do not
// correspond and every number below is a comparison of different moments.
[[nodiscard]] Divergence ComputeDivergence(const OptimalPlan& a_plan,
	const ReplayResult& a_controller);

// Computes it. a_intervalSeconds is the decision cadence, a_minDwellSeconds the
// cooldown, and a_overBudgetAllowance the fraction of FRAMES it may miss -
// the same quantity ReplayResult::overBudgetRate reports, so the two are
// comparable.
//
// All three must match what the controller actually does, or the result is not
// a bound. This has been got wrong twice, both times by making the optimiser
// stricter than the controller and so producing an "optimum" below an
// achievable trajectory: first by judging a 0.5 s window against the
// controller's 2 s, then by counting an interval as missed when its windowed
// P95 exceeded budget while the controller counted individual frames. Frames
// are now the only currency here.
[[nodiscard]] OptimalPlan ComputeOptimal(const std::vector<TraceFrame>& a_trace,
	const CostModel& a_model, double a_budgetMs, double a_intervalSeconds,
	double a_minDwellSeconds, double a_overBudgetAllowance);

// Parses a *_frames.csv capture. Unknown or malformed rows are skipped rather
// than throwing: a capture truncated by a crash is still worth replaying, and
// that is exactly when one gets truncated.
[[nodiscard]] std::vector<TraceFrame> ParseTrace(std::istream& a_stream);

// Fits the cost model across every preset the trace dwelt at, deduplicating by
// GPU frame index first - an asynchronously published value would otherwise be
// weighted by how long it stayed published (Rule 8).
[[nodiscard]] CostModel FitCostModel(const std::vector<TraceFrame>& a_trace);

// Replays a trace through a controller, synthesising the cost of presets the
// controller chooses and the capture did not.
[[nodiscard]] ReplayResult Replay(const std::vector<TraceFrame>& a_trace,
	const CostModel& a_model, const GovernorConfig& a_config, Counterfactual a_mode);

}
