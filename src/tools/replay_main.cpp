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
// pixels spent because it was slow to come down. This walks the two
// trajectories slot for slot and splits the difference between them.
void ReportDivergence(const OptimalPlan& a_plan, const ReplayResult& a_controller,
	double a_intervalSeconds)
{
	PrintHeader("Where the gap is");

	const std::size_t n = std::min(a_plan.trajectory.size(), a_controller.trajectory.size());
	if (n == 0) {
		std::cout << "  No comparable intervals.\n";
		return;
	}

	// Time share per rung, in ladder order rather than by name, so the table
	// reads cheapest to most expensive like every other one in this report.
	const auto rung = [](Preset a_preset) {
		for (std::size_t i = 0; i < kPresets.size(); ++i) {
			if (kPresets[i].preset == a_preset) {
				return i;
			}
		}
		return std::size_t{ 0 };
	};
	std::array<std::size_t, kPresets.size()> shareOptimum{};
	std::array<std::size_t, kPresets.size()> shareOurs{};
	double deficit = 0.0;   // pixels the optimum had and we did not
	double surplus = 0.0;   // pixels we held and the optimum did not
	std::size_t below = 0;  // intervals we sat under the optimum
	std::size_t above = 0;
	std::size_t same = 0;

	for (std::size_t i = 0; i < n; ++i) {
		const double optF = static_cast<double>(PresetPixelFraction(a_plan.trajectory[i]));
		const double ourF = static_cast<double>(PresetPixelFraction(a_controller.trajectory[i]));
		++shareOptimum[rung(a_plan.trajectory[i])];
		++shareOurs[rung(a_controller.trajectory[i])];
		if (ourF < optF) {
			deficit += optF - ourF;
			++below;
		} else if (ourF > optF) {
			surplus += ourF - optF;
			++above;
		} else {
			++same;
		}
	}

	const double total = static_cast<double>(n);
	std::cout << std::fixed;
	std::cout << "  " << n << " intervals of " << std::setprecision(1) << a_intervalSeconds
			  << " s compared, on the same grid and origin.\n\n";

	std::cout << "  preset            optimum      ours\n";
	for (std::size_t i = 0; i < kPresets.size(); ++i) {
		if (shareOptimum[i] == 0 && shareOurs[i] == 0) {
			continue;
		}
		std::cout << "  " << std::left << std::setw(18) << PresetName(kPresets[i].preset)
				  << std::right << std::setw(6) << std::setprecision(1)
				  << 100.0 * shareOptimum[i] / total << "%" << std::setw(9)
				  << 100.0 * shareOurs[i] / total << "%\n";
	}
	std::cout << std::left;

	std::cout << "\n  agreed on " << std::setprecision(1) << 100.0 * same / total
			  << "% of intervals; below the optimum on " << 100.0 * below / total
			  << "%, above on " << 100.0 * above / total << "%\n";
	std::cout << "  mean pixel fraction given up by sitting low:  " << std::setprecision(3)
			  << deficit / total << "\n";
	std::cout << "  mean pixel fraction held that it would not:   " << surplus / total << "\n";

	// Which lever. An interval below the optimum while the optimum is climbing
	// or already up is a climb we did not take; one above while the optimum has
	// come down is a descent we were late to.
	std::size_t lateClimb = 0;
	std::size_t lateDescent = 0;
	for (std::size_t i = 1; i < n; ++i) {
		const double optF = static_cast<double>(PresetPixelFraction(a_plan.trajectory[i]));
		const double optPrev = static_cast<double>(PresetPixelFraction(a_plan.trajectory[i - 1]));
		const double ourF = static_cast<double>(PresetPixelFraction(a_controller.trajectory[i]));
		if (optF > optPrev && ourF < optF) {
			++lateClimb;
		}
		if (optF < optPrev && ourF > optF) {
			++lateDescent;
		}
	}
	std::cout << "\n  of the optimum's moves, we failed to follow " << lateClimb
			  << " climbs and " << lateDescent << " descents within the same interval\n";

	if (deficit > surplus * 2.0) {
		std::cout << "  => the gap is dominated by pixels NOT TAKEN. The climb path is the one\n"
					 "     to look at, not the descend path.\n";
	} else if (surplus > deficit * 2.0) {
		std::cout << "  => the gap is dominated by pixels HELD TOO LONG. The descend path is the\n"
					 "     one to look at.\n";
	} else {
		std::cout << "  => neither direction dominates, so this is timing rather than a\n"
					 "     threshold: both trajectories visit similar rungs, at different times.\n";
	}

	std::cout << "\n  Caveat that does not go away: the optimum has foresight. Some of what it\n"
				 "  gains is entering a scene it already knows is cheap, which no causal\n"
				 "  controller can do. This says which lever, not how much is reachable.\n";
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
		ReportDivergence(optimum, shipped, base.evalIntervalSeconds);
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
