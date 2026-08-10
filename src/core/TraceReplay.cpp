#include "TraceReplay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace csgov {

namespace {

[[nodiscard]] std::vector<std::string> SplitFields(const std::string& a_line)
{
	std::vector<std::string> fields;
	std::string field;
	std::istringstream stream(a_line);
	while (std::getline(stream, field, ',')) {
		fields.push_back(field);
	}
	return fields;
}

[[nodiscard]] double ToDouble(const std::string& a_text)
{
	return std::strtod(a_text.c_str(), nullptr);
}

[[nodiscard]] std::uint64_t ToU64(const std::string& a_text)
{
	return static_cast<std::uint64_t>(std::strtoull(a_text.c_str(), nullptr, 10));
}

}

std::vector<TraceFrame> ParseTrace(std::istream& a_stream)
{
	std::vector<TraceFrame> trace;
	std::string line;

	// Column order is taken from the header rather than assumed, because the
	// capture format has already gained columns twice (gpu_us, then wall_ms)
	// and a positional parser would have read the wrong ones silently.
	// Provenance lines come first, as "# key=value". Skipped rather than parsed
	// here: the capture records what produced it - CS build, API revision, the
	// assumed preset ladder - and a reader that took the first line as the
	// header would read "# cs_build=9" as a column name and then find no
	// columns it recognised, which fails as an empty result rather than as an
	// error.
	std::vector<std::string> header;
	do {
		if (!std::getline(a_stream, line)) {
			return trace;
		}
	} while (!line.empty() && line.front() == '#');
	header = SplitFields(line);

	const auto indexOf = [&header](std::string_view a_name) -> int {
		for (std::size_t i = 0; i < header.size(); ++i) {
			if (header[i] == a_name) {
				return static_cast<int>(i);
			}
		}
		return -1;
	};

	const int iWall = indexOf("wall_ms");
	const int iTime = indexOf("time_s");
	const int iFrame = indexOf("frame_ms");
	const int iPreset = indexOf("preset_public");
	const int iState = indexOf("state");
	const int iGpu = indexOf("gpu_us");
	const int iGpuFrame = indexOf("gpu_frame");

	if (iTime < 0 || iFrame < 0) {
		return trace;
	}

	while (std::getline(a_stream, line)) {
		if (line.empty() || line.front() == '#') {
			continue;
		}
		const auto fields = SplitFields(line);
		const auto has = [&fields](int a_index) {
			return a_index >= 0 && static_cast<std::size_t>(a_index) < fields.size();
		};
		if (!has(iTime) || !has(iFrame)) {
			continue;
		}

		TraceFrame row;
		row.wallMs = has(iWall) ? ToU64(fields[iWall]) : 0;
		row.timeSeconds = ToDouble(fields[iTime]);
		row.frameTimeMs = ToDouble(fields[iFrame]);
		row.presetPublicValue = has(iPreset) ? static_cast<std::uint32_t>(ToU64(fields[iPreset])) : 0;
		row.state = has(iState) ? fields[iState] : std::string{};
		row.gpuUs = has(iGpu) ? ToU64(fields[iGpu]) : 0;
		row.gpuFrameIndex = has(iGpuFrame) ? ToU64(fields[iGpuFrame]) : 0;
		trace.push_back(std::move(row));
	}

	return trace;
}

CostModel FitCostModel(const std::vector<TraceFrame>& a_trace)
{
	struct Bucket
	{
		double sum = 0.0;
		std::size_t count = 0;
	};
	std::array<Bucket, 7> buckets{};
	std::unordered_set<std::uint64_t> seenGpuFrames;

	for (const auto& row : a_trace) {
		if (row.gpuUs == 0 || row.state != "Dwelling") {
			continue;
		}
		// Rule 8: a published value repeats until the next query completes, so
		// deduplicate by measurement identity before averaging.
		if (row.gpuFrameIndex != 0 && !seenGpuFrames.insert(row.gpuFrameIndex).second) {
			continue;
		}
		if (!FindPresetByPublicValue(row.presetPublicValue) ||
			row.presetPublicValue >= buckets.size()) {
			continue;
		}
		auto& bucket = buckets[row.presetPublicValue];
		bucket.sum += static_cast<double>(row.gpuUs) / 1000.0;
		++bucket.count;
	}

	double sx = 0.0, sy = 0.0, sxy = 0.0, sxx = 0.0;
	std::size_t fitted = 0;
	std::vector<std::pair<double, double>> points;
	for (std::uint32_t value = 0; value < buckets.size(); ++value) {
		const auto& bucket = buckets[value];
		if (bucket.count == 0) {
			continue;
		}
		const auto info = FindPresetByPublicValue(value);
		if (!info) {
			continue;
		}
		const double f = static_cast<double>(info->scale) * static_cast<double>(info->scale);
		const double mean = bucket.sum / static_cast<double>(bucket.count);
		points.emplace_back(f, mean);
		sx += f;
		sy += mean;
		sxy += f * mean;
		sxx += f * f;
		++fitted;
	}

	CostModel model;
	model.presetsFitted = fitted;
	if (fitted < 2) {
		return model;
	}

	const double n = static_cast<double>(fitted);
	const double denominator = n * sxx - sx * sx;
	if (std::abs(denominator) < 1e-12) {
		return model;
	}
	model.tScaledMs = (n * sxy - sx * sy) / denominator;
	model.tFixedMs = (sy - model.tScaledMs * sx) / n;

	for (const auto& [f, mean] : points) {
		model.worstResidualMs =
			std::max(model.worstResidualMs, std::abs(mean - model.PredictMs(f)));
	}
	return model;
}

OptimalPlan ComputeOptimal(const std::vector<TraceFrame>& a_trace, const CostModel& a_model,
	double a_budgetMs, double a_intervalSeconds, double a_minDwellSeconds,
	double a_overBudgetAllowance)
{
	OptimalPlan plan;
	if (a_trace.empty() || !a_model.Valid() || a_intervalSeconds <= 0.0) {
		return plan;
	}

	// Deduplicated samples with the pixel fraction they were recorded at, so a
	// rolling window can be taken over them exactly as the controller takes one.
	struct Sample
	{
		double t = 0.0;
		double gpuMs = 0.0;
		double f = 1.0;
	};
	std::vector<Sample> samples;
	std::unordered_set<std::uint64_t> seen;
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
		samples.push_back({ row.timeSeconds, static_cast<double>(row.gpuUs) / 1000.0,
			static_cast<double>(info->scale) * static_cast<double>(info->scale) });
	}
	if (samples.size() < 2) {
		return plan;
	}

	// How many frames each preset would have missed, per decision interval.
	//
	// Counted per frame, deliberately, because that is the currency Replay
	// reports the controller in. An earlier version judged an interval by its
	// windowed P95 instead, which is far stricter: a window whose P95 only just
	// fits still has one frame in twenty over budget. Given the same nominal 2%
	// allowance the bound was solving a harder problem than the controller, and
	// duly came out below it - 0.470 against an achieved 0.506 on the light
	// capture, 0.517 against 0.570 on the marginal one. A bound under an
	// achievable trajectory is a statement about the optimiser, not the
	// controller.
	//
	// The frames counted are the ones the choice actually governs - the interval
	// ahead of the decision, not the window behind it. The controller must infer
	// from the past because that is all it has; this has foresight by
	// construction, and charging it for frames it could not affect would understate
	// the ceiling.
	const std::size_t presets = kPresets.size();
	std::vector<std::vector<std::size_t>> missed;  // [interval][preset]
	std::vector<double> weight;                    // frames in that interval
	{
		const double start = samples.front().t;
		const auto slot = [&](double a_t) {
			return static_cast<std::size_t>((a_t - start) / a_intervalSeconds);
		};
		// Not assumed monotonic. ParseTrace skips malformed rows rather than
		// throwing, precisely so a capture truncated by a crash is still usable,
		// and a clock that steps backwards there would index this negatively -
		// which, cast to an unsigned, is an out-of-bounds write rather than a
		// wrong answer.
		double finish = start;
		for (const auto& sample : samples) {
			finish = std::max(finish, sample.t);
		}
		const std::size_t count = slot(finish) + 1;
		std::vector<std::vector<std::size_t>> over(count, std::vector<std::size_t>(presets, 0));
		std::vector<double> frames(count, 0.0);

		for (const auto& sample : samples) {
			const double denominator = a_model.PredictMs(sample.f);
			if (denominator <= 0.0 || sample.t < start) {
				continue;
			}
			const std::size_t i = slot(sample.t);
			if (i >= count) {
				continue;
			}
			frames[i] += 1.0;
			for (std::size_t p = 0; p < presets; ++p) {
				const double f = static_cast<double>(kPresets[p].scale) *
				                 static_cast<double>(kPresets[p].scale);
				if (sample.gpuMs * a_model.PredictMs(f) / denominator > a_budgetMs) {
					++over[i][p];
				}
			}
		}

		// Empty intervals are kept rather than compacted away, so index i is the
		// i'th interval of wall clock and the trajectory can be diffed against
		// ReplayResult::trajectory slot for slot. They cost the DP nothing: no
		// frames to win and none to miss, so every preset scores zero and only
		// the dwell counter advances - which is right, because time passed.
		missed = std::move(over);
		weight = std::move(frames);
	}
	if (missed.empty()) {
		return plan;
	}

	// State: (preset, intervals held since the last change, capped at the dwell).
	//
	// Clamped to the trace length: a dwell longer than the capture means "never
	// change", which the table already expresses, and it keeps the state index
	// inside the 16 bits the predecessor array stores.
	// 9000 keeps presets * (dwell + 1) inside 16 bits; missed.size() is at least
	// 1 here, so the upper bound is never below the lower.
	const int dwellLimit = static_cast<int>(std::min<std::size_t>(missed.size(), 9000));
	const int dwell =
		std::clamp(static_cast<int>(a_minDwellSeconds / a_intervalSeconds + 0.5), 1, dwellLimit);
	const std::size_t slots = static_cast<std::size_t>(dwell) + 1;
	const std::size_t states = presets * slots;
	constexpr double kUnreachable = -1.0e18;
	const auto index = [slots](std::size_t a_preset, int a_held) {
		return a_preset * slots + static_cast<std::size_t>(a_held);
	};

	// The miss allowance is carried as a dimension of the state, not priced.
	//
	// It was priced first - maximise pixels minus lambda times missed frames,
	// and bisect lambda until the plan fits the allowance. That is Lagrangian
	// relaxation of an integer programme, and it has a duality gap: at a given
	// price the solver returns SOME optimum of the priced objective, and with
	// more freedom it can step straight past the allowance into the interior of
	// the feasible region - spending fewer of its permitted misses and taking
	// fewer pixels with them. No price enumerates the points it skips over.
	//
	// It showed up as a violated monotonicity check: a 1.0 s dwell scored 0.392
	// where a 5.0 s dwell scored 0.401, though every trajectory available to the
	// slower solver is available to the faster one. Carrying the budget makes
	// the constraint exact and the invariant hold by construction.
	double totalFrames = 0.0;
	for (const auto value : weight) {
		totalFrames += value;
	}
	if (totalFrames <= 0.0) {
		return plan;
	}
	std::size_t allowed = static_cast<std::size_t>(a_overBudgetAllowance * totalFrames);

	// A trace can be unaffordable at every preset - the cheapest rung still
	// misses more than the allowance. Rather than report nothing, raise the
	// budget to the least any trajectory could achieve, so the plan stays the
	// best available and overBudgetRate reports what it truly cost.
	std::size_t floorMisses = 0;
	for (const auto& row : missed) {
		floorMisses += *std::min_element(row.begin(), row.end());
	}
	allowed = std::max(allowed, floorMisses);

	// Bounds the table. With the allowance at a few percent of a session this is
	// never reached; if it were, quantising rounds each interval's misses UP, so
	// the plan stays feasible against the true allowance rather than cheating.
	constexpr std::size_t kMaxLevels = 1024;
	const std::size_t quantum = std::max<std::size_t>(1, (allowed + kMaxLevels - 1) / kMaxLevels);
	const std::size_t levels = allowed / quantum + 1;
	const auto charge = [quantum](std::size_t a_misses) {
		return (a_misses + quantum - 1) / quantum;
	};
	const auto at = [levels](std::size_t a_state, std::size_t a_budget) {
		return a_state * levels + a_budget;
	};

	const auto gain = [&](std::size_t a_interval, std::size_t a_preset) {
		const double f = static_cast<double>(kPresets[a_preset].scale) *
		                 static_cast<double>(kPresets[a_preset].scale);
		return f * weight[a_interval];
	};

	std::vector<double> previous(states * levels, kUnreachable);
	std::vector<double> current(states * levels, kUnreachable);

	// The FULL predecessor state, not just its preset.
	//
	// Storing the preset alone and re-deriving the dwell counter is wrong, and
	// was wrong here from the first version. `nextHeld = min(held + 1, dwell)`
	// means a state at `held == dwell` has two possible predecessors - dwell-1,
	// or dwell again, already capped - and re-derivation can only guess one. On
	// a wrong guess the walk lands on a cell that was never written, breaks, and
	// leaves the entire earlier prefix at preset index 0. The plan then reports
	// a figure BELOW the optimum the table actually found, by an amount that
	// varies with the dwell, which is what broke monotonicity: 0.382 at a 1.0 s
	// dwell against 0.410 at 5.0 s.
	constexpr std::uint16_t kNoPredecessor = 0xFFFFu;
	std::vector<std::vector<std::uint16_t>> from(
		missed.size(), std::vector<std::uint16_t>(states * levels, kNoPredecessor));

	for (std::size_t p = 0; p < presets; ++p) {
		const std::size_t spend = charge(missed[0][p]);
		if (spend < levels) {
			previous[at(index(p, dwell), spend)] = gain(0, p);
		}
	}

	for (std::size_t i = 1; i < missed.size(); ++i) {
		std::fill(current.begin(), current.end(), kUnreachable);
		for (std::size_t p = 0; p < presets; ++p) {
			for (int held = 0; held <= dwell; ++held) {
				const std::size_t state = index(p, held);
				for (std::size_t b = 0; b < levels; ++b) {
					const double value = previous[at(state, b)];
					if (value <= kUnreachable) {
						continue;
					}

					// Hold: the dwell counter advances, capped.
					const std::size_t holdSpend = b + charge(missed[i][p]);
					if (holdSpend < levels) {
						const int nextHeld = std::min(held + 1, dwell);
						const std::size_t to = at(index(p, nextHeld), holdSpend);
						const double holdValue = value + gain(i, p);
						if (holdValue > current[to]) {
							current[to] = holdValue;
							from[i][to] = static_cast<std::uint16_t>(state);
						}
					}

					// Change: only once the dwell has elapsed.
					if (held < dwell) {
						continue;
					}
					for (std::size_t q = 0; q < presets; ++q) {
						if (q == p) {
							continue;
						}
						const std::size_t spend = b + charge(missed[i][q]);
						if (spend >= levels) {
							continue;
						}
						const std::size_t to = at(index(q, 0), spend);
						const double moveValue = value + gain(i, q);
						if (moveValue > current[to]) {
							current[to] = moveValue;
							from[i][to] = static_cast<std::uint16_t>(state);
						}
					}
				}
			}
		}
		previous.swap(current);
	}

	std::size_t bestCell = 0;
	double bestValue = kUnreachable;
	for (std::size_t cell = 0; cell < previous.size(); ++cell) {
		if (previous[cell] > bestValue) {
			bestValue = previous[cell];
			bestCell = cell;
		}
	}
	if (bestValue <= kUnreachable) {
		return plan;
	}

	// Walk the choices back to recover the trajectory.
	std::vector<std::size_t> path(missed.size(), 0);
	std::size_t state = bestCell / levels;
	std::size_t budget = bestCell % levels;
	for (std::size_t i = missed.size(); i-- > 0;) {
		const std::size_t preset = state / slots;
		path[i] = preset;
		if (i == 0) {
			break;
		}
		const std::uint16_t previousState = from[i][at(state, budget)];
		if (previousState == kNoPredecessor) {
			break;
		}
		// The budget is derivable - this interval's own charge came out of it -
		// but the state is not, so it is read back rather than reconstructed.
		const std::size_t spend = charge(missed[i][preset]);
		budget = budget >= spend ? budget - spend : 0;
		state = previousState;
	}

	// Frame-weighted, to match Replay: an interval holding fewer frames must not
	// count as much as a full one. Reported from the true miss counts rather
	// than the quantised ones, so the figure is what the plan actually costs.
	double pixels = 0.0;
	double over = 0.0;
	double frames = 0.0;
	for (std::size_t i = 0; i < path.size(); ++i) {
		const double f = static_cast<double>(kPresets[path[i]].scale) *
		                 static_cast<double>(kPresets[path[i]].scale);
		pixels += f * weight[i];
		over += static_cast<double>(missed[i][path[i]]);
		frames += weight[i];
		plan.trajectory.push_back(kPresets[path[i]].preset);
		plan.intervalFrames.push_back(weight[i]);
		if (i > 0 && path[i] != path[i - 1]) {
			++plan.changes;
		}
	}
	if (frames > 0.0) {
		plan.timeWeightedPixelFraction = pixels / frames;
		plan.overBudgetRate = over / frames;
	}
	plan.durationSeconds = static_cast<double>(path.size()) * a_intervalSeconds;
	return plan;
}

std::vector<StepObservation> ObserveStepRatios(const std::vector<TraceFrame>& a_trace)
{
	// Same bucketing rules as FitCostModel: dwelling rows only, deduplicated by
	// measurement identity (Rule 8). A transient counted here would be a
	// transition's cost, not a rung's.
	std::array<double, kPresets.size()> sum{};
	std::array<std::size_t, kPresets.size()> count{};
	std::unordered_set<std::uint64_t> seen;

	for (const auto& row : a_trace) {
		if (row.gpuUs == 0 || row.state != "Dwelling") {
			continue;
		}
		if (row.gpuFrameIndex != 0 && !seen.insert(row.gpuFrameIndex).second) {
			continue;
		}
		const auto info = FindPresetByPublicValue(row.presetPublicValue);
		if (!info) {
			continue;
		}
		for (std::size_t i = 0; i < kPresets.size(); ++i) {
			if (kPresets[i].preset == info->preset) {
				sum[i] += static_cast<double>(row.gpuUs) / 1000.0;
				++count[i];
				break;
			}
		}
	}

	std::vector<StepObservation> steps(kPresets.size());
	for (std::size_t i = 0; i + 1 < kPresets.size(); ++i) {
		steps[i].samplesFrom = count[i];
		steps[i].samplesTo = count[i + 1];
		if (count[i] == 0 || count[i + 1] == 0) {
			continue;
		}
		const double from = sum[i] / static_cast<double>(count[i]);
		const double to = sum[i + 1] / static_cast<double>(count[i + 1]);
		if (from > 0.0) {
			steps[i].ratio = to / from;
		}
	}
	return steps;
}

Divergence ComputeDivergence(const OptimalPlan& a_plan, const ReplayResult& a_controller)
{
	Divergence out;
	const std::size_t n =
		std::min(a_plan.trajectory.size(), a_controller.intervalPixelFraction.size());
	if (n == 0 || a_plan.intervalFrames.size() < n) {
		return out;
	}

	out.optimumRungs.assign(kPresets.size(), 0);
	out.controllerRungs.assign(kPresets.size(), 0);

	const auto rung = [](Preset a_preset) {
		for (std::size_t i = 0; i < kPresets.size(); ++i) {
			if (kPresets[i].preset == a_preset) {
				return i;
			}
		}
		return std::size_t{ 0 };
	};

	// One set of weights for both sides - the plan's, which are the ones behind
	// its headline. Weighting each side by its own would make the difference of
	// the means stop equalling the mean of the differences, and the identity
	// this exists to guarantee would quietly fail.
	double frames = 0.0;
	double optimumPixels = 0.0;
	double controllerPixels = 0.0;
	double deficit = 0.0;
	double surplus = 0.0;

	for (std::size_t i = 0; i < n; ++i) {
		const double w = a_plan.intervalFrames[i];
		const double optF = static_cast<double>(PresetPixelFraction(a_plan.trajectory[i]));
		const double ourF = a_controller.intervalPixelFraction[i];

		frames += w;
		optimumPixels += w * optF;
		controllerPixels += w * ourF;
		if (ourF < optF) {
			deficit += w * (optF - ourF);
		} else if (ourF > optF) {
			surplus += w * (ourF - optF);
		}

		// Rung shares stay interval-counted rather than frame-weighted: the
		// question they answer is "how much of the session did it sit here",
		// and an interval is the unit the controller actually decides in.
		++out.optimumRungs[rung(a_plan.trajectory[i])];
		if (i < a_controller.trajectory.size()) {
			++out.controllerRungs[rung(a_controller.trajectory[i])];
		}

		constexpr double kLevel = 1e-9;
		if (ourF < optF - kLevel) {
			++out.below;
		} else if (ourF > optF + kLevel) {
			++out.above;
		} else {
			++out.level;
		}

		if (i > 0) {
			const double optPrev =
				static_cast<double>(PresetPixelFraction(a_plan.trajectory[i - 1]));
			if (optF > optPrev && ourF < optF) {
				++out.unfollowedClimbs;
			}
			if (optF < optPrev && ourF > optF) {
				++out.unfollowedDescents;
			}
		}
	}

	out.intervals = n;
	if (frames > 0.0) {
		out.optimumPixelFraction = optimumPixels / frames;
		out.controllerPixelFraction = controllerPixels / frames;
		out.deficit = deficit / frames;
		out.surplus = surplus / frames;
	}
	return out;
}

ReplayResult Replay(const std::vector<TraceFrame>& a_trace, const CostModel& a_model,
	const GovernorConfig& a_config, Counterfactual a_mode)
{
	ReplayResult result;
	if (a_trace.empty() || !a_model.Valid()) {
		return result;
	}

	GovernorCore governor(a_config);
	Preset current = Preset::NativeAA;
	if (const auto start = FindPresetByPublicValue(a_trace.front().presetPublicValue)) {
		current = start->preset;
	}

	double lastTime = a_trace.front().timeSeconds;
	double pixelFractionTime = 0.0;
	double heldSeconds = 0.0;
	double gpuSum = 0.0;
	std::size_t gpuCount = 0;
	std::size_t overBudget = 0;
	std::vector<double> gpuSamples;
	gpuSamples.reserve(a_trace.size());
	int lastDirection = 0;

	// Frames rendered at each preset, per decision interval. The origin is the
	// first accepted row, which is the same row ComputeOptimal starts its grid
	// at - it applies the same two filters, and the first row can never be the
	// duplicate its extra dedupe drops.
	const double gridInterval =
		a_config.evalIntervalSeconds > 0.0 ? a_config.evalIntervalSeconds : 0.5;
	double gridStart = 0.0;
	bool haveGrid = false;
	std::vector<std::array<double, kPresets.size()>> slotFrames;

	const auto presetIndex = [](Preset a_preset) -> std::size_t {
		for (std::size_t i = 0; i < kPresets.size(); ++i) {
			if (kPresets[i].preset == a_preset) {
				return i;
			}
		}
		return 0;
	};

	for (const auto& row : a_trace) {
		const auto recorded = FindPresetByPublicValue(row.presetPublicValue);
		if (!recorded || row.gpuUs == 0) {
			continue;
		}

		if (!haveGrid) {
			gridStart = row.timeSeconds;
			haveGrid = true;
		}
		if (row.timeSeconds >= gridStart) {
			const std::size_t slot =
				static_cast<std::size_t>((row.timeSeconds - gridStart) / gridInterval);
			if (slot >= slotFrames.size()) {
				slotFrames.resize(slot + 1, std::array<double, kPresets.size()>{});
			}
			slotFrames[slot][presetIndex(current)] += 1.0;
		}

		const double dt = std::clamp(row.timeSeconds - lastTime, 0.0, 1.0);
		lastTime = row.timeSeconds;

		const double recordedF =
			static_cast<double>(recorded->scale) * static_cast<double>(recorded->scale);
		const double currentF = static_cast<double>(PresetPixelFraction(current));
		const double observed = static_cast<double>(row.gpuUs) / 1000.0;

		// D-14: what this frame would have cost at the preset the controller
		// actually holds.
		double synthesised = observed;
		if (current != recorded->preset) {
			if (a_mode == Counterfactual::Scaled) {
				const double denominator = a_model.PredictMs(recordedF);
				synthesised = denominator > 0.0 ? observed * a_model.PredictMs(currentF) / denominator
												: observed;
			} else {
				synthesised = observed + a_model.tScaledMs * (currentF - recordedF);
			}
			synthesised = std::max(synthesised, 0.1);
		}

		++result.frames;
		gpuSum += synthesised;
		gpuSamples.push_back(synthesised);
		++gpuCount;
		if (synthesised > a_config.frameBudgetMs) {
			++overBudget;
		}
		pixelFractionTime += currentF * dt;
		heldSeconds += dt;

		GovernorSample sample;
		sample.nowSeconds = row.timeSeconds;
		// Frametime is synthesised too, or the frametime tier would be replayed
		// against the recorded preset while the headroom tier sees the chosen
		// one. A frame cannot take less than its GPU work, and while it fits,
		// the compositor holds it at the budget - which is E-1's censoring,
		// reproduced deliberately so the fallback tier is tested against it.
		sample.frameTimeMs = std::max(synthesised, std::min(row.frameTimeMs, a_config.frameBudgetMs));
		sample.gpuTimeUs = static_cast<std::uint64_t>(synthesised * 1000.0);
		sample.gpuFrameIndex = row.gpuFrameIndex != 0 ? row.gpuFrameIndex : result.frames;

		if (auto decision = governor.Push(sample, current)) {
			++result.decisions;
			if (decision->action != GovernorAction::Hold) {
				const int direction = decision->action == GovernorAction::Climb ? 1 : -1;
				if (lastDirection != 0 && direction != lastDirection) {
					++result.reversals;
				}
				lastDirection = direction;
				current = decision->target;
				governor.NotifyApplied(current, row.timeSeconds);
				++result.changes;
			}
		}
	}

	// One preset per interval: the one the most frames were rendered at. An
	// interval with no frames inherits the previous one, because the controller
	// did not stop holding a preset just because nothing was recorded.
	result.trajectory.reserve(slotFrames.size());
	result.intervalPixelFraction.reserve(slotFrames.size());
	Preset held = kPresets.front().preset;
	if (const auto start = FindPresetByPublicValue(a_trace.front().presetPublicValue)) {
		held = start->preset;
	}
	for (const auto& counts : slotFrames) {
		std::size_t best = kPresets.size();
		double bestCount = 0.0;
		double frames = 0.0;
		double pixels = 0.0;
		for (std::size_t i = 0; i < counts.size(); ++i) {
			if (counts[i] > bestCount) {
				bestCount = counts[i];
				best = i;
			}
			frames += counts[i];
			pixels += counts[i] * static_cast<double>(kPresets[i].scale) *
			          static_cast<double>(kPresets[i].scale);
		}
		if (best < kPresets.size()) {
			held = kPresets[best].preset;
		}
		result.trajectory.push_back(held);
		// An interval with no frames inherits the held preset's fraction: the
		// controller did not stop holding it just because nothing was recorded.
		result.intervalPixelFraction.push_back(
			frames > 0.0 ? pixels / frames : static_cast<double>(PresetPixelFraction(held)));
	}

	// What the controller ended up believing, so the report can put its beliefs
	// beside what the capture actually shows.
	for (std::size_t i = 0; i < kPresets.size(); ++i) {
		result.learnedStepRatio[i] = governor.StepRatio(kPresets[i].preset);
		result.learnedStepMeasured[i] = governor.StepMeasured(kPresets[i].preset);
		result.learnedStepObservations[i] = governor.StepObservations(kPresets[i].preset);
	}

	result.durationSeconds = heldSeconds;
	result.timeWeightedPixelFraction = heldSeconds > 0.0 ? pixelFractionTime / heldSeconds : 0.0;
	result.overBudgetRate =
		gpuCount > 0 ? static_cast<double>(overBudget) / static_cast<double>(gpuCount) : 0.0;
	result.meanGpuMs = gpuCount > 0 ? gpuSum / static_cast<double>(gpuCount) : 0.0;
	result.changesPerMinute =
		heldSeconds > 0.0 ? static_cast<double>(result.changes) * 60.0 / heldSeconds : 0.0;

	if (!gpuSamples.empty()) {
		std::sort(gpuSamples.begin(), gpuSamples.end());
		result.p95GpuMs = PercentileSorted(gpuSamples, 95.0);
	}

	return result;
}

}
