#include "core/TraceReplay.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <string>

using namespace csgov;
using Catch::Approx;

namespace {

// A synthetic capture in the real format: the sweep visits every preset, and
// each preset's GPU time follows t_fixed + t_scaled*f exactly, so the fit has a
// known answer to be checked against.
std::string SyntheticSweep(double a_tFixed, double a_tScaled)
{
	std::ostringstream out;
	out << "wall_ms,time_s,frame_ms,preset_public,state,gpu_us,gpu_frame\n";

	const std::uint32_t order[]{ 4, 3, 2, 1, 6, 5, 0 };
	double t = 0.0;
	std::uint64_t gpuFrame = 0;
	for (const auto value : order) {
		const auto info = FindPresetByPublicValue(value);
		const double f = static_cast<double>(info->scale) * static_cast<double>(info->scale);
		const double gpuMs = a_tFixed + a_tScaled * f;
		for (int i = 0; i < 400; ++i) {
			t += 0.0139;
			++gpuFrame;
			const double frameMs = gpuMs > 13.889 ? gpuMs : 13.889;
			out << static_cast<std::uint64_t>(1786000000000.0 + t * 1000.0) << ',' << t << ','
				<< frameMs << ',' << value << ",Dwelling," << static_cast<std::uint64_t>(gpuMs * 1000.0)
				<< ',' << gpuFrame << '\n';
		}
	}
	return out.str();
}

// The same sweep, but with scene load varying frame to frame.
//
// SyntheticSweep gives every frame at a preset an identical GPU time, which
// makes a P95 equal to the mean and equal to each individual frame. Any two
// definitions of "over budget" agree on it exactly, so it cannot tell a bound
// that counts missed frames from one that counts intervals whose P95 missed -
// and it duly passed while the released report showed a controller at 108% of
// its own optimum. Variance is what makes those two definitions differ, so a
// bound has to be checked against a trace that has some.
//
// The load averages 1.0 across a full cycle, so the fitted cost model is still
// the one the caller asked for.
std::string VaryingSweep(double a_tFixed, double a_tScaled)
{
	std::ostringstream out;
	out << "wall_ms,time_s,frame_ms,preset_public,state,gpu_us,gpu_frame\n";

	const std::uint32_t order[]{ 4, 3, 2, 1, 6, 5, 0 };
	double t = 0.0;
	std::uint64_t gpuFrame = 0;
	for (const auto value : order) {
		const auto info = FindPresetByPublicValue(value);
		const double f = static_cast<double>(info->scale) * static_cast<double>(info->scale);
		const double base = a_tFixed + a_tScaled * f;
		for (int i = 0; i < 400; ++i) {
			t += 0.0139;
			++gpuFrame;
			// A slow swell plus an occasional spike: enough spread that the P95
			// sits well above the median, as it does in a real capture.
			const double load = 1.0 + 0.30 * std::sin(i * 0.05) + (i % 17 == 0 ? 0.35 : 0.0);
			const double gpuMs = base * load;
			const double frameMs = gpuMs > 13.889 ? gpuMs : 13.889;
			out << static_cast<std::uint64_t>(1786000000000.0 + t * 1000.0) << ',' << t << ','
				<< frameMs << ',' << value << ",Dwelling," << static_cast<std::uint64_t>(gpuMs * 1000.0)
				<< ',' << gpuFrame << '\n';
		}
	}
	return out.str();
}

}

TEST_CASE("parses a capture by header name, not by column position", "[replay]")
{
	// Columns deliberately reordered and one extra added: the capture format
	// has gained columns twice already, and a positional parser read the wrong
	// ones without complaining.
	const std::string csv =
		"time_s,gpu_us,frame_ms,extra,preset_public,state,gpu_frame,wall_ms\n"
		"1.5,9000,13.889,ignored,3,Dwelling,42,1786000001500\n";

	std::istringstream stream(csv);
	const auto trace = ParseTrace(stream);

	REQUIRE(trace.size() == 1);
	CHECK(trace[0].timeSeconds == Approx(1.5));
	CHECK(trace[0].gpuUs == 9000);
	CHECK(trace[0].frameTimeMs == Approx(13.889));
	CHECK(trace[0].presetPublicValue == 3);
	CHECK(trace[0].gpuFrameIndex == 42);
	CHECK(trace[0].wallMs == 1786000001500ull);
}

TEST_CASE("a truncated capture still parses", "[replay]")
{
	// Captures get truncated by crashes, which is exactly when you want to read
	// one. A malformed tail must not cost the rows before it.
	const std::string csv =
		"wall_ms,time_s,frame_ms,preset_public,state,gpu_us,gpu_frame\n"
		"1786000000000,1.0,13.9,3,Dwelling,9000,1\n"
		"1786000000014,1.014,13.9,3,Dwelling,9\n";

	std::istringstream stream(csv);
	const auto trace = ParseTrace(stream);
	CHECK(trace.size() == 2);  // second row parses what it has
}

TEST_CASE("recovers a known cost model from a synthetic sweep", "[replay]")
{
	std::istringstream stream(SyntheticSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	REQUIRE_FALSE(trace.empty());

	const auto model = FitCostModel(trace);
	REQUIRE(model.Valid());
	CHECK(model.presetsFitted == 7);
	CHECK(model.tFixedMs == Approx(9.9).margin(0.05));
	CHECK(model.tScaledMs == Approx(8.0).margin(0.05));
	CHECK(model.worstResidualMs < 0.05);
	CHECK(model.K() == Approx(8.0 / 9.9).margin(0.01));
}

TEST_CASE("the fit deduplicates by GPU frame index", "[replay]")
{
	// The same measurement republished many times must not outvote a single
	// occurrence of another (Rule 8).
	std::ostringstream out;
	out << "wall_ms,time_s,frame_ms,preset_public,state,gpu_us,gpu_frame\n";
	for (int i = 0; i < 100; ++i) {
		out << "0," << 0.014 * i << ",13.9,4,Dwelling,10000,7\n";  // one measurement, 100 rows
	}
	for (int i = 0; i < 100; ++i) {
		out << "0," << 2.0 + 0.014 * i << ",13.9,0,Dwelling," << 18000 + i << ',' << 100 + i << '\n';
	}

	std::istringstream stream(out.str());
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);

	REQUIRE(model.presetsFitted == 2);
	// If the repeated row were counted 100 times it would still be one point,
	// but its value must be the single measurement, not a weighted blend.
	CHECK(model.PredictMs(1.0 / 9.0) == Approx(10.0).margin(0.5));
}

TEST_CASE("replay costs a quality change instead of granting it free", "[replay]")
{
	// The defect this exists to prevent: if the recorded GPU time is fed back
	// regardless of the preset the controller chose, climbing costs nothing and
	// any parameter set that climbs always wins.
	std::istringstream stream(SyntheticSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);
	REQUIRE(model.Valid());

	GovernorConfig config;
	config.minSamples = 10;
	config.judgeWindowSeconds = 1.0;
	config.evalIntervalSeconds = 0.25;
	config.cooldownSeconds = 1.0;

	const auto scaled = Replay(trace, model, config, Counterfactual::Scaled);
	const auto additive = Replay(trace, model, config, Counterfactual::Additive);

	REQUIRE(scaled.frames > 0);
	REQUIRE(additive.frames > 0);

	// With t_fixed 9.9 ms of a 13.889 ms budget there is not room for maximum
	// quality, so a controller that ends up at the top rung is being given its
	// quality for free.
	CHECK(scaled.timeWeightedPixelFraction < 1.0);
	CHECK(additive.timeWeightedPixelFraction < 1.0);

	// And it must not simply sit at the bottom either.
	CHECK(scaled.timeWeightedPixelFraction > 0.111);
}

TEST_CASE("replay reports the objective's terms", "[replay]")
{
	std::istringstream stream(SyntheticSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);

	GovernorConfig config;
	config.minSamples = 10;
	config.judgeWindowSeconds = 1.0;
	config.evalIntervalSeconds = 0.25;

	const auto result = Replay(trace, model, config, Counterfactual::Scaled);

	// Section 1 of the design doc: maximise time-weighted pixel fraction
	// subject to the frame-miss constraint. A sweep that reports neither cannot
	// rank parameter sets.
	CHECK(result.durationSeconds > 0.0);
	CHECK(result.timeWeightedPixelFraction > 0.0);
	CHECK(result.meanGpuMs > 0.0);
	CHECK(result.p95GpuMs >= result.meanGpuMs * 0.5);
	CHECK(result.changesPerMinute >= 0.0);
}

TEST_CASE("the constrained optimum obeys the actuator's limits", "[replay]")
{
	// The perfect-foresight oracle switches every frame and is unreachable.
	// This one is the honest target: same foresight, but one lever and a
	// minimum dwell, so a controller can be scored against it as a percentage
	// rather than judged by eye.
	std::istringstream stream(SyntheticSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);
	REQUIRE(model.Valid());

	const auto plan = ComputeOptimal(trace, model, 13.889, 0.5, 3.0, 0.02);
	REQUIRE(plan.Valid());

	// It cannot beat the ladder's own ceiling, and it must not sit at the floor.
	CHECK(plan.timeWeightedPixelFraction > 0.111);
	CHECK(plan.timeWeightedPixelFraction <= 1.0);

	// With t_fixed 9.9 ms of a 13.889 ms budget, the top rungs never fit, so an
	// optimum that claims no misses must have avoided them.
	CHECK(plan.overBudgetRate < 0.5);

	// The dwell is real: changes cannot exceed one per cooldown.
	CHECK(static_cast<double>(plan.changes) <= plan.durationSeconds / 3.0 + 1.0);
	CHECK(plan.trajectory.size() > 1);
}

TEST_CASE("a shorter dwell can only help the optimum", "[replay]")
{
	// The invariant that actually pins the dynamic programme: relaxing a
	// constraint cannot make the optimum worse. A controller allowed to change
	// every second has every trajectory available to one allowed to change
	// every five, and more besides.
	//
	// This replaces a test that asserted "the optimum beats the best fixed
	// preset". That is false as stated: a preset whose average fits the budget
	// can still exceed it in individual intervals, and the optimum refuses
	// those - so it can legitimately score below a fixed preset that would have
	// been over budget part of the time. The old test was measuring my
	// misunderstanding, not the code.
	std::istringstream stream(SyntheticSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);
	REQUIRE(model.Valid());

	const auto loose = ComputeOptimal(trace, model, 13.889, 0.5, 1.0, 0.02);
	const auto tight = ComputeOptimal(trace, model, 13.889, 0.5, 5.0, 0.02);
	REQUIRE(loose.Valid());
	REQUIRE(tight.Valid());

	CHECK(loose.timeWeightedPixelFraction >= tight.timeWeightedPixelFraction - 1e-9);

	// Each plan obeys its own dwell. Deliberately NOT "tight.changes <=
	// loose.changes", which was asserted here and is not a theorem: if a nearly
	// constant trajectory happens to be optimal under the loose dwell, its
	// change count can sit below what the tight optimum uses, and nothing in the
	// constraint forbids that. Only the per-plan limit is actually implied.
	CHECK(static_cast<double>(loose.changes) <= loose.durationSeconds / 1.0 + 1.0);
	CHECK(static_cast<double>(tight.changes) <= tight.durationSeconds / 5.0 + 1.0);
}

TEST_CASE("both trajectories land on the same grid", "[replay]")
{
	// The divergence report diffs them slot for slot. If the two grids had
	// different origins or lengths, every conclusion it draws about which lever
	// to touch would be drawn from a comparison offset in time - and it would
	// still print confident-looking percentages.
	std::istringstream stream(VaryingSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);
	REQUIRE(model.Valid());

	GovernorConfig config;
	const auto controller = Replay(trace, model, config, Counterfactual::Scaled);
	const auto plan = ComputeOptimal(trace, model, config.frameBudgetMs,
		config.evalIntervalSeconds, config.cooldownSeconds, 0.02);
	REQUIRE(plan.Valid());

	REQUIRE_FALSE(controller.trajectory.empty());
	REQUIRE_FALSE(plan.trajectory.empty());

	// Same span of wall clock, so slot i means the same seconds in both. One
	// interval of slack for the final partial slot.
	const auto difference = plan.trajectory.size() > controller.trajectory.size() ?
		plan.trajectory.size() - controller.trajectory.size() :
		controller.trajectory.size() - plan.trajectory.size();
	CHECK(difference <= 1);

	// And the trajectory must reflect the controller's own changes rather than
	// being a constant left behind by the empty-slot fallback. Stated as a
	// consistency check rather than "it changed at least once", which would be
	// an assumption about this trace instead of an invariant of the code.
	const auto distinct =
		std::set<Preset>(controller.trajectory.begin(), controller.trajectory.end());
	if (controller.changes > 0) {
		CHECK(distinct.size() > 1);
	} else {
		CHECK(distinct.size() == 1);
	}
}

TEST_CASE("the optimum is never beaten by an actual controller", "[replay]")
{
	// The invariant that matters, and the one whose absence let a broken
	// optimum ship: a bound below an achievable trajectory is not a bound.
	//
	// Got wrong twice, both times by making the optimiser stricter than the
	// controller. First it judged feasibility on a 0.5 s P95 while the controller
	// uses 2 s, and forbade any interval over budget. Then, with those fixed, it
	// still counted an interval as missed whenever its windowed P95 exceeded the
	// budget - while the controller counts individual frames. A window whose P95
	// only just fits still misses one frame in twenty, so the same nominal 2%
	// allowance bought the optimiser almost no slack and the controller a real
	// one. That shipped, and the report showed the controller at 108% and 110%
	// of its own upper bound.
	//
	// This runs on the varying trace on purpose: the flat one cannot distinguish
	// the two definitions, which is why this test passed while it was wrong.
	std::istringstream stream(VaryingSweep(9.9, 8.0));
	const auto trace = ParseTrace(stream);
	const auto model = FitCostModel(trace);
	REQUIRE(model.Valid());

	GovernorConfig config;
	const auto controller = Replay(trace, model, config, Counterfactual::Scaled);
	const auto plan = ComputeOptimal(trace, model, config.frameBudgetMs,
		config.evalIntervalSeconds, config.cooldownSeconds,
		std::max(controller.overBudgetRate, 0.02));

	REQUIRE(plan.Valid());
	REQUIRE(controller.timeWeightedPixelFraction > 0.0);

	// Allowed the same over-budget rate the controller actually used, the
	// optimum must be at least as good. A small tolerance covers the interval
	// grid, not a systematic gap.
	CHECK(plan.timeWeightedPixelFraction >= controller.timeWeightedPixelFraction - 0.02);
}
