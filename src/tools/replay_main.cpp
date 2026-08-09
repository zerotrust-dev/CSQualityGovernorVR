// Offline analysis of a captured session.
//
// Phase 3 chooses the controller's parameters here rather than in a headset:
// deterministically, from a recorded trace, with the reasoning visible.
//
// It is built to answer as much as possible from ONE session, and - just as
// importantly - to say what that session cannot answer. A parameter fitted on
// 17 seconds of data at some load is not a fitted parameter, and the report
// says so rather than printing a confident number.
//
//   csgov_replay <frames.csv> [--fine] [--csv <out.csv>]

#include "core/TraceReplay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace csgov;

namespace {

struct SweepPoint
{
	double marginUpMs = 0.0;
	double marginDownMs = 0.0;
	ReplayResult scaled;
	ReplayResult additive;
};

// Deduplicated GPU samples: a published value repeats until the next query
// completes, and weighting it by how long it stayed published is Rule 8.
struct Distilled
{
	std::vector<double> gpuMs;
	std::map<std::uint32_t, double> secondsPerPreset;
	double durationSeconds = 0.0;
	std::size_t rows = 0;
};

Distilled Distill(const std::vector<TraceFrame>& a_trace)
{
	Distilled out;
	std::unordered_set<std::uint64_t> seen;
	double last = a_trace.empty() ? 0.0 : a_trace.front().timeSeconds;
	for (const auto& row : a_trace) {
		++out.rows;
		const double dt = std::clamp(row.timeSeconds - last, 0.0, 1.0);
		last = row.timeSeconds;
		out.durationSeconds += dt;
		out.secondsPerPreset[row.presetPublicValue] += dt;
		if (row.gpuUs == 0) {
			continue;
		}
		if (row.gpuFrameIndex != 0 && !seen.insert(row.gpuFrameIndex).second) {
			continue;
		}
		out.gpuMs.push_back(static_cast<double>(row.gpuUs) / 1000.0);
	}
	return out;
}

void PrintHeader(const std::string& a_title)
{
	std::cout << "\n" << a_title << "\n" << std::string(a_title.size(), '=') << "\n";
}

// What the trace can support a conclusion about. The controller's thresholds
// live near the budget, so coverage there is what decides whether a sweep is
// measuring anything.
void ReportCoverage(const Distilled& a_distilled, double a_budgetMs)
{
	PrintHeader("Coverage");
	std::cout << "  " << a_distilled.rows << " rows, " << a_distilled.gpuMs.size()
			  << " distinct GPU measurements, " << std::fixed << std::setprecision(1)
			  << a_distilled.durationSeconds / 60.0 << " minutes\n\n";

	std::map<int, std::size_t> histogram;
	for (const double ms : a_distilled.gpuMs) {
		histogram[static_cast<int>(ms)]++;
	}

	const double perSample =
		a_distilled.gpuMs.empty() ? 0.0 : a_distilled.durationSeconds / static_cast<double>(a_distilled.gpuMs.size());

	std::cout << "  GPU ms    samples   approx seconds\n";
	for (const auto& [bucket, count] : histogram) {
		if (bucket > 30) {
			continue;
		}
		const double seconds = static_cast<double>(count) * perSample;
		std::cout << "  " << std::setw(3) << bucket << "-" << std::setw(2) << bucket + 1 << "  "
				  << std::setw(9) << count << "   " << std::setw(8) << std::setprecision(1)
				  << seconds;
		if (seconds < 20.0 && count > 0) {
			std::cout << "   <- thin";
		}
		std::cout << "\n";
	}

	// The band the thresholds actually operate in.
	std::size_t nearBudget = 0;
	for (const double ms : a_distilled.gpuMs) {
		if (ms > a_budgetMs - 3.0 && ms < a_budgetMs + 1.0) {
			++nearBudget;
		}
	}
	const double nearSeconds = static_cast<double>(nearBudget) * perSample;
	std::cout << "\n  near the budget (" << std::setprecision(1) << a_budgetMs - 3.0 << "-"
			  << a_budgetMs + 1.0 << " ms): " << nearBudget << " samples, " << nearSeconds
			  << " s\n";
	if (nearSeconds < 60.0) {
		std::cout << "  WARNING: under a minute spent where the thresholds decide. A sweep on\n"
					 "  this trace ranks parameters mostly on scenes that were never marginal,\n"
					 "  and the winner will be whichever climbs hardest. Capture a session with\n"
					 "  sustained load near the cap before trusting a threshold from this.\n";
	}
}

// The ceiling and the floor, so a sweep result can be read as "how much of the
// available quality did this parameter set actually get".
void ReportBounds(const std::vector<TraceFrame>& a_trace, const CostModel& a_model,
	double a_budgetMs)
{
	PrintHeader("Bounds: fixed presets, and a perfect-foresight oracle");

	std::unordered_set<std::uint64_t> seen;
	struct Sample
	{
		double gpuMs;
		double f;
	};
	std::vector<Sample> samples;
	for (const auto& row : a_trace) {
		if (row.gpuUs == 0) {
			continue;
		}
		if (row.gpuFrameIndex != 0 && !seen.insert(row.gpuFrameIndex).second) {
			continue;
		}
		const auto info = FindPresetByPublicValue(row.presetPublicValue);
		if (!info) {
			continue;
		}
		samples.push_back({ static_cast<double>(row.gpuUs) / 1000.0,
			static_cast<double>(info->scale) * static_cast<double>(info->scale) });
	}
	if (samples.empty() || !a_model.Valid()) {
		std::cout << "  not enough data\n";
		return;
	}

	std::cout << "  preset            f      over budget    (a fixed-preset baseline)\n";
	for (const auto& info : kPresets) {
		const double f = static_cast<double>(info.scale) * static_cast<double>(info.scale);
		std::size_t over = 0;
		for (const auto& sample : samples) {
			// Project this frame onto the fixed preset, same model the replay
			// uses (D-14, scaled form).
			const double projected =
				sample.gpuMs * a_model.PredictMs(f) / a_model.PredictMs(sample.f);
			if (projected > a_budgetMs) {
				++over;
			}
		}
		std::cout << "  " << std::left << std::setw(18) << info.name << std::right << std::fixed
				  << std::setprecision(3) << f << "   " << std::setw(9) << std::setprecision(1)
				  << 100.0 * static_cast<double>(over) / static_cast<double>(samples.size())
				  << "%\n";
	}

	// Perfect foresight: the best rung that fits, frame by frame. No controller
	// can beat this, so it is the ceiling a parameter set is measured against.
	double oracleF = 0.0;
	std::size_t oracleOver = 0;
	for (const auto& sample : samples) {
		double best = 0.0;
		bool any = false;
		for (const auto& info : kPresets) {
			const double f = static_cast<double>(info.scale) * static_cast<double>(info.scale);
			const double projected =
				sample.gpuMs * a_model.PredictMs(f) / a_model.PredictMs(sample.f);
			if (projected <= a_budgetMs && f > best) {
				best = f;
				any = true;
			}
		}
		if (!any) {
			++oracleOver;
			best = static_cast<double>(kPresets.front().scale) *
			       static_cast<double>(kPresets.front().scale);
		}
		oracleF += best;
	}
	const double n = static_cast<double>(samples.size());
	std::cout << "\n  perfect foresight: mean pixel fraction " << std::setprecision(3)
			  << oracleF / n << ", over budget " << std::setprecision(1) << 100.0 * oracleOver / n
			  << "%\n";
	std::cout << "  (unreachable: it changes preset every frame and knows the future)\n";

	// The honest target: the same foresight, but obeying our actuator's limits.
	GovernorConfig config;
	const auto plan = ComputeOptimal(a_trace, a_model, a_budgetMs, config.evalIntervalSeconds,
		config.cooldownSeconds, 0.02);
	if (plan.Valid()) {
		std::cout << "\n  CONSTRAINED OPTIMUM (one lever, " << std::setprecision(1)
				  << config.cooldownSeconds << " s minimum dwell, " << config.evalIntervalSeconds
				  << " s cadence):\n";
		std::cout << "    pixel fraction " << std::setprecision(3)
				  << plan.timeWeightedPixelFraction << ", over budget " << std::setprecision(1)
				  << 100.0 * plan.overBudgetRate << "%, " << plan.changes << " changes ("
				  << std::setprecision(2) << plan.changes * 60.0 / plan.durationSeconds
				  << " per minute)\n";
		std::cout << "    This is the number to score against. A controller making many more\n"
					 "    changes than this is churning; many fewer is sluggish.\n";
	}
}

void ReportModel(const CostModel& a_model)
{
	PrintHeader("Cost model");
	if (!a_model.Valid()) {
		std::cout << "  NOT FITTED: fewer than three presets dwelt at. A trace without a sweep\n"
					 "  cannot support a counterfactual, so no parameter conclusion follows.\n";
		return;
	}
	std::cout << std::fixed << std::setprecision(2);
	std::cout << "  t_fixed  " << a_model.tFixedMs << " ms   (resolution-independent)\n";
	std::cout << "  t_scaled " << a_model.tScaledMs << " ms   (at full resolution)\n";
	std::cout << "  k        " << std::setprecision(3) << a_model.K() << "\n";
	std::cout << "  presets fitted " << a_model.presetsFitted << ", worst residual "
			  << std::setprecision(2) << a_model.worstResidualMs << " ms\n";
	if (a_model.worstResidualMs > 0.5) {
		std::cout << "  WARNING: residual over 0.5 ms. The linear model does not describe this\n"
					 "  session well, so every synthesised cost below inherits that error.\n";
	}
}

// Where the gap between the controller and the optimum actually is.
//
// The totals say a controller reached some percentage of the achievable
// optimum. They cannot say which mechanism to touch to close it, and the two
// candidates want opposite fixes: pixels lost because it declined to climb, or
// pixels spent because it was slow to come down.
//
// The arithmetic lives in ComputeDivergence so the identity it rests on -
// deficit minus surplus equals the gap - is guaranteed by a test rather than by
// this function happening to get it right.
void ReportDivergence(const Divergence& a_gap, double a_intervalSeconds,
	double a_controllerHeadline)
{
	PrintHeader("Where the gap is");

	if (!a_gap.Valid()) {
		std::cout << "  No comparable intervals.\n";
		return;
	}

	const double total = static_cast<double>(a_gap.intervals);
	std::cout << std::fixed;
	std::cout << "  " << a_gap.intervals << " intervals of " << std::setprecision(1)
			  << a_intervalSeconds << " s compared, on the same grid and origin.\n\n";

	std::cout << "  preset            optimum      ours\n";
	for (std::size_t i = 0; i < kPresets.size(); ++i) {
		if (a_gap.optimumRungs[i] == 0 && a_gap.controllerRungs[i] == 0) {
			continue;
		}
		std::cout << "  " << std::left << std::setw(18) << PresetName(kPresets[i].preset)
				  << std::right << std::setw(6) << std::setprecision(1)
				  << 100.0 * a_gap.optimumRungs[i] / total << "%" << std::setw(9)
				  << 100.0 * a_gap.controllerRungs[i] / total << "%\n";
	}
	std::cout << std::left;

	std::cout << "\n  level with the optimum on " << std::setprecision(1)
			  << 100.0 * a_gap.level / total << "% of intervals; below on "
			  << 100.0 * a_gap.below / total << "%, above on " << 100.0 * a_gap.above / total
			  << "%\n\n";

	// Printed as an equation because it is one. If these ever stop adding up,
	// the breakdown is wrong and should not be read.
	std::cout << std::setprecision(3);
	std::cout << "  optimum        " << a_gap.optimumPixelFraction << "\n";
	std::cout << "  ours           " << a_gap.controllerPixelFraction << "\n";
	std::cout << "  gap            " << a_gap.optimumPixelFraction - a_gap.controllerPixelFraction
			  << "  =  " << a_gap.deficit << " not taken  -  " << a_gap.surplus
			  << " held too long\n";

	// The sweep reports the controller time-weighted over its own samples; this
	// is frame-weighted on the plan's grid. They are close but not identical,
	// and saying so is cheaper than someone finding the discrepancy later.
	const double drift = a_gap.controllerPixelFraction - a_controllerHeadline;
	if (std::abs(drift) > 0.005) {
		std::cout << "\n  (the sweep's time-weighted figure for the same run is "
				  << a_controllerHeadline << ", a\n   difference of " << std::abs(drift)
				  << " from weighting and from rows the plan deduplicates)\n";
	}

	std::cout << "\n  of the optimum's moves, we failed to follow " << a_gap.unfollowedClimbs
			  << " climbs and " << a_gap.unfollowedDescents
			  << " descents within the same interval\n";

	if (a_gap.deficit > a_gap.surplus * 2.0) {
		std::cout << "  => the gap is dominated by pixels NOT TAKEN. The climb path is the one\n"
					 "     to look at, not the descend path.\n";
	} else if (a_gap.surplus > a_gap.deficit * 2.0) {
		std::cout << "  => the gap is dominated by pixels HELD TOO LONG. The descend path is the\n"
					 "     one to look at.\n";
	} else {
		std::cout << "  => neither direction dominates by 2x, so read the two terms above\n"
					 "     directly rather than taking a verdict from this line.\n";
	}

	std::cout << "\n  Caveat that does not go away: the optimum has foresight. Some of what it\n"
				 "  gains is entering a scene it already knows is cheap, which no causal\n"
				 "  controller can do. This says which lever, not how much is reachable.\n";
}

// What the controller believes a rung costs, against what the capture shows.
//
// The landing check refuses a climb when p95 * ratio would not clear the
// budget. If the ratio it uses is inflated, it refuses climbs that would have
// fitted - and the fix belongs in D-18's learning, not in the gate. If the
// ratio matches the capture and the climb is still refused, the gate is the
// problem. The two want opposite changes, so the belief has to be visible.
void ReportStepRatios(const ReplayResult& a_result,
	const std::vector<StepObservation>& a_observed, const GovernorConfig& a_config)
{
	PrintHeader("Step ratios: believed vs observed");
	std::cout << std::right << std::fixed;
	std::cout << "  What a one-rung climb multiplies P95 GPU time by.\n\n";
	std::cout << "  step                             believed  from       observed  frames\n";

	for (std::size_t i = 0; i + 1 < kPresets.size(); ++i) {
		const std::string step = std::string(PresetName(kPresets[i].preset)) + " -> " +
		                         std::string(PresetName(kPresets[i + 1].preset));
		// Wide enough for the longest pair on the ladder. The first version cut
		// off at 28 and every column after it stepped right by two.
		std::string source = "seed";
		if (a_result.learnedStepMeasured[i]) {
			source = std::to_string(a_result.learnedStepObservations[i]) + " obs";
		}
		std::cout << "  " << std::left << std::setw(33) << step << std::right << std::setprecision(3)
				  << std::setw(8) << a_result.learnedStepRatio[i] << "  " << std::left
				  << std::setw(9) << source << std::right;

		if (i < a_observed.size() && a_observed[i].Valid()) {
			std::cout << std::setw(9) << a_observed[i].ratio << std::setw(8)
					  << std::min(a_observed[i].samplesFrom, a_observed[i].samplesTo);
			// Pessimism is what blocks a climb. Flag it only when the belief
			// exceeds the capture by more than the noise in a dwell mean.
			const double excess = a_result.learnedStepRatio[i] - a_observed[i].ratio;
			if (excess > 0.05) {
				std::cout << "   <- believes it costs " << std::setprecision(0)
						  << 100.0 * excess / a_observed[i].ratio << "% more than it did";
			}
		} else {
			std::cout << std::setw(9) << "-" << std::setw(8) << "-";
		}
		std::cout << "\n";
	}

	std::cout << "\n  A belief above the observation makes the landing check refuse climbs that\n"
				 "  would have fitted; below it, the check lets through climbs that will not.\n"
				 "  'from' is what the belief rests on: 'seed' means this step was never\n"
				 "  measured in this run, so the number is the shipped starting point and says\n"
				 "  nothing about this machine; 'N obs' is how many transitions fed the EMA.\n"
				 "  'frames' is the sample size of the OBSERVATION, not of the belief - the two\n"
				 "  are not comparable and a single-observation belief is not a measurement of\n"
				 "  a rung so much as a measurement of one moment.\n";
	std::cout << "  The gate: a climb needs the predicted landing under " << std::setprecision(2)
			  << a_config.frameBudgetMs - a_config.landingMarginMs << " ms (budget "
			  << a_config.frameBudgetMs << " minus landingMargin " << a_config.landingMarginMs
			  << ").\n";
}

// Is the landing check what is holding the controller a rung low?
//
// The divergence table says the gap is pixels never taken, concentrated on the
// upper-middle rungs. D-16's landing check is the gate on exactly those: it
// predicts where a climb lands using the learned step ratio and refuses the
// climb unless the landing clears the budget by landingMarginMs. E-26 measured
// the Quality -> UltraQuality step as the one where the cost model was most
// wrong, which is the step this would block first.
//
// Swept separately rather than as a third dimension of the main table: that
// would multiply sixty rows by five and bury the two parameters that are
// already understood.
void ReportLandingCheck(const std::vector<TraceFrame>& a_trace, const CostModel& a_model,
	const GovernorConfig& a_base, const OptimalPlan& a_optimum, double a_overBudgetLimit)
{
	PrintHeader("Landing check sensitivity");

	// ReportDivergence leaves the stream left-aligned for its preset column, and
	// a numeric table inherits it - which is how the first run of this printed
	// its percent signs adrift from their numbers.
	std::cout << std::right;
	std::cout << "  At the shipped marginUp/marginDown. landingMarginMs is the clearance a\n"
				 "  predicted landing must have; maxClimbRungs bounds one change's reach.\n\n";
	std::cout << "  landing rungs  | pixels  over% chg/min  UQ+time  | of opt\n";

	const double optimumPixels = a_optimum.timeWeightedPixelFraction;
	for (const double landing : { -1.0, 0.0, 0.5, 1.0, 1.5, 2.0 }) {
		for (const int rungs : { 1, 3, 6 }) {
			GovernorConfig config = a_base;
			config.landingMarginMs = landing;
			config.maxClimbRungs = rungs;
			const auto result = Replay(a_trace, a_model, config, Counterfactual::Scaled);

			// Time spent at UltraQuality or above - the rungs the controller is
			// not reaching. Moving this is the point of the exercise; moving
			// only the total could be a wash across the whole ladder.
			std::size_t high = 0;
			for (const auto preset : result.trajectory) {
				if (PresetPixelFraction(preset) > 0.5) {
					++high;
				}
			}
			const double highShare = result.trajectory.empty() ?
				0.0 :
				100.0 * static_cast<double>(high) / static_cast<double>(result.trajectory.size());

			std::cout << "  " << std::fixed << std::setprecision(2) << std::setw(6) << landing
					  << std::setw(6) << rungs << "  | " << std::setprecision(3) << std::setw(6)
					  << result.timeWeightedPixelFraction << std::setprecision(1) << std::setw(6)
					  << 100.0 * result.overBudgetRate << "%" << std::setprecision(2)
					  << std::setw(8) << result.changesPerMinute << std::setprecision(1)
					  << std::setw(9) << highShare << "%  |";
			if (optimumPixels > 0.0) {
				std::cout << std::setprecision(0) << std::setw(6)
						  << 100.0 * result.timeWeightedPixelFraction / optimumPixels << "%";
			}
			if (result.overBudgetRate > a_overBudgetLimit) {
				std::cout << "  (violates constraint)";
			}
			std::cout << "\n";
		}
	}

	std::cout << "\n  UQ+time is the share of intervals at UltraQuality or above - the rungs the\n"
				 "  controller under-uses. The shipped row is landing 1.00 / rungs 3. A row\n"
				 "  that lifts UQ+time without breaching the constraint is evidence the gate is\n"
				 "  too tight; one that lifts pixels only by going over budget is not - that is\n"
				 "  the check doing its job.\n";
}

void ReportSweep(const std::vector<SweepPoint>& a_sweep, double a_oracleGuard)
{
	PrintHeader("Parameter sweep");
	std::cout << "  marginUp marginDown | pixel_frac  over%  chg/min  rev | additive pixel_frac "
				 "over%\n";
	for (const auto& point : a_sweep) {
		std::cout << std::fixed << std::setprecision(2) << "  " << std::setw(8)
				  << point.marginUpMs << " " << std::setw(10) << point.marginDownMs << " | "
				  << std::setw(10) << std::setprecision(3) << point.scaled.timeWeightedPixelFraction
				  << " " << std::setw(6) << std::setprecision(1)
				  << 100.0 * point.scaled.overBudgetRate << " " << std::setw(8)
				  << std::setprecision(2) << point.scaled.changesPerMinute << " " << std::setw(4)
				  << point.scaled.reversals << " | " << std::setw(11) << std::setprecision(3)
				  << point.additive.timeWeightedPixelFraction << " " << std::setw(6)
				  << std::setprecision(1) << 100.0 * point.additive.overBudgetRate;

		// A ranking that flips between the two counterfactual forms is an
		// artefact of the synthesis, not a property of the parameters (D-14).
		const double gap = point.scaled.timeWeightedPixelFraction -
		                   point.additive.timeWeightedPixelFraction;
		if (std::abs(gap) > 0.05) {
			std::cout << "  <- forms disagree";
		}
		if (point.scaled.overBudgetRate > a_oracleGuard) {
			std::cout << "  (violates constraint)";
		}
		std::cout << "\n";
	}
}

}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::cerr << "usage: csgov_replay <frames.csv> [--fine] [--csv out.csv]\n";
		return 2;
	}

	const std::string path = argv[1];
	bool fine = false;
	std::string csvOut;
	for (int i = 2; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--fine") {
			fine = true;
		} else if (arg == "--csv" && i + 1 < argc) {
			csvOut = argv[++i];
		}
	}

	std::ifstream file(path);
	if (!file) {
		std::cerr << "cannot open " << path << "\n";
		return 2;
	}

	const auto trace = ParseTrace(file);
	if (trace.empty()) {
		std::cerr << "no usable rows in " << path << "\n";
		return 2;
	}

	GovernorConfig base;
	const auto distilled = Distill(trace);
	const auto model = FitCostModel(trace);

	std::cout << "Replay of " << path << "\n";
	ReportCoverage(distilled, base.frameBudgetMs);
	ReportModel(model);
	if (!model.Valid()) {
		return 1;
	}
	ReportBounds(trace, model, base.frameBudgetMs);

	std::vector<SweepPoint> sweep;
	const double upStep = fine ? 0.25 : 0.5;
	const double downStep = fine ? 0.25 : 0.5;
	for (double up = -1.0; up <= 3.001; up += upStep) {
		for (double down = -1.5; down <= 1.001; down += downStep) {
			GovernorConfig config = base;
			config.marginUpMs = up;
			config.marginDownMs = down;

			SweepPoint point;
			point.marginUpMs = up;
			point.marginDownMs = down;
			point.scaled = Replay(trace, model, config, Counterfactual::Scaled);
			point.additive = Replay(trace, model, config, Counterfactual::Additive);
			sweep.push_back(point);
		}
	}

	// Objective from section 1 of the design doc: maximise time-weighted pixel
	// fraction subject to the frame-miss constraint. Ranking is done here so
	// the recommendation cannot quietly differ from the printed table.
	constexpr double kOverBudgetLimit = 0.02;
	auto ranked = sweep;
	std::sort(ranked.begin(), ranked.end(), [](const SweepPoint& a, const SweepPoint& b) {
		const bool aOk = a.scaled.overBudgetRate <= kOverBudgetLimit;
		const bool bOk = b.scaled.overBudgetRate <= kOverBudgetLimit;
		if (aOk != bOk) {
			return aOk;
		}
		return a.scaled.timeWeightedPixelFraction > b.scaled.timeWeightedPixelFraction;
	});

	ReportSweep(sweep, kOverBudgetLimit);

	PrintHeader("Best under the constraint");
	std::cout << "  constraint: over budget <= " << 100.0 * kOverBudgetLimit << "% of frames\n\n";

	const auto optimum = ComputeOptimal(trace, model, base.frameBudgetMs, base.evalIntervalSeconds,
		base.cooldownSeconds, kOverBudgetLimit);
	for (std::size_t i = 0; i < std::min<std::size_t>(5, ranked.size()); ++i) {
		const auto& point = ranked[i];
		std::cout << std::fixed << std::setprecision(2) << "  " << i + 1 << ". marginUp "
				  << point.marginUpMs << "  marginDown " << point.marginDownMs << "  -> pixels "
				  << std::setprecision(3) << point.scaled.timeWeightedPixelFraction << ", over "
				  << std::setprecision(1) << 100.0 * point.scaled.overBudgetRate << "%, "
				  << std::setprecision(2) << point.scaled.changesPerMinute << " changes/min";
		if (optimum.Valid() && optimum.timeWeightedPixelFraction > 0.0) {
			const double ofOptimum =
				100.0 * point.scaled.timeWeightedPixelFraction / optimum.timeWeightedPixelFraction;
			const double changeRatio =
				optimum.changes > 0 ?
					point.scaled.changesPerMinute /
						(optimum.changes * 60.0 / optimum.durationSeconds) :
					0.0;
			std::cout << "  [" << std::setprecision(0) << ofOptimum << "% of optimum, "
					  << std::setprecision(1) << changeRatio << "x its changes]";
		}
		std::cout << "\n";
	}

	std::cout << "\n  Read this as a shortlist, not an answer. Replay has no settle latency at\n"
				 "  the synthesised preset (E-2 measured ~1.0 s), so it flatters parameters that\n"
				 "  change often - prefer the lowest changes/min among comparable rows.\n";

	// Diffed against the shipped parameters, not the sweep's winner: the
	// question is where THIS controller loses ground, and the winner is a
	// shortlist entry rather than a decision.
	if (optimum.Valid()) {
		const auto shipped = Replay(trace, model, base, Counterfactual::Scaled);
		ReportDivergence(ComputeDivergence(optimum, shipped), base.evalIntervalSeconds,
			shipped.timeWeightedPixelFraction);
		ReportStepRatios(shipped, ObserveStepRatios(trace), base);
		ReportLandingCheck(trace, model, base, optimum, kOverBudgetLimit);
	}

	if (!csvOut.empty()) {
		std::ofstream out(csvOut);
		out << "margin_up_ms,margin_down_ms,pixel_fraction,over_budget_rate,changes_per_min,"
			   "reversals,pixel_fraction_additive,over_budget_rate_additive\n";
		for (const auto& point : sweep) {
			out << point.marginUpMs << ',' << point.marginDownMs << ','
				<< point.scaled.timeWeightedPixelFraction << ',' << point.scaled.overBudgetRate
				<< ',' << point.scaled.changesPerMinute << ',' << point.scaled.reversals << ','
				<< point.additive.timeWeightedPixelFraction << ','
				<< point.additive.overBudgetRate << '\n';
		}
		std::cout << "\n  sweep written to " << csvOut << "\n";
	}

	return 0;
}
