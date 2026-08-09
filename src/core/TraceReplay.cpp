#include "TraceReplay.h"

#include <algorithm>
#include <array>
#include <cmath>
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
	std::vector<std::string> header;
	if (!std::getline(a_stream, line)) {
		return trace;
	}
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
		if (line.empty()) {
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
	double a_budgetMs, double a_intervalSeconds, double a_minDwellSeconds)
{
	OptimalPlan plan;
	if (a_trace.empty() || !a_model.Valid() || a_intervalSeconds <= 0.0) {
		return plan;
	}

	// One cost estimate per decision interval, per candidate preset. The P95 is
	// used rather than the mean because that is what the controller judges on,
	// so the target is computed on the same statistic it will be compared to.
	struct Interval
	{
		std::array<double, kPresets.size()> cost{};
	};
	std::vector<Interval> intervals;

	const double start = a_trace.front().timeSeconds;
	double windowEnd = start + a_intervalSeconds;
	std::vector<double> bucket;
	std::vector<double> bucketF;
	std::unordered_set<std::uint64_t> seen;

	const auto flush = [&]() {
		if (bucket.empty()) {
			return;
		}
		std::sort(bucket.begin(), bucket.end());
		const double p95 = PercentileSorted(bucket, 95.0);
		const double fRecorded = bucketF.empty() ? 1.0 : bucketF.front();
		Interval interval;
		for (std::size_t i = 0; i < kPresets.size(); ++i) {
			const double f =
				static_cast<double>(kPresets[i].scale) * static_cast<double>(kPresets[i].scale);
			const double denominator = a_model.PredictMs(fRecorded);
			interval.cost[i] =
				denominator > 0.0 ? p95 * a_model.PredictMs(f) / denominator : p95;
		}
		intervals.push_back(interval);
		bucket.clear();
		bucketF.clear();
	};

	for (const auto& row : a_trace) {
		while (row.timeSeconds > windowEnd) {
			flush();
			windowEnd += a_intervalSeconds;
		}
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
		bucket.push_back(static_cast<double>(row.gpuUs) / 1000.0);
		bucketF.push_back(static_cast<double>(info->scale) * static_cast<double>(info->scale));
	}
	flush();

	if (intervals.empty()) {
		return plan;
	}

	// State: (preset, intervals held since the last change, capped). The cap is
	// the dwell requirement, so "held long enough to move again" is a single
	// state rather than an unbounded counter.
	const int dwell = std::max(1, static_cast<int>(a_minDwellSeconds / a_intervalSeconds + 0.5));
	const std::size_t presets = kPresets.size();
	const std::size_t states = presets * static_cast<std::size_t>(dwell + 1);
	const double negative = -1.0e18;

	const auto index = [dwell, presets](std::size_t a_preset, int a_held) {
		(void)presets;
		return a_preset * static_cast<std::size_t>(dwell + 1) +
		       static_cast<std::size_t>(std::min(a_held, dwell));
	};

	std::vector<double> previous(states, negative);
	std::vector<std::vector<int>> choice(intervals.size(), std::vector<int>(states, -1));

	// Reward for an interval: the pixel fraction, but only when the preset fits
	// the budget. A preset that does not fit is allowed solely when nothing
	// fits, because a trajectory that knowingly runs over budget is not a
	// target worth comparing against.
	const auto reward = [&](std::size_t a_intervalIndex, std::size_t a_preset) -> double {
		const auto& interval = intervals[a_intervalIndex];
		const double f =
			static_cast<double>(kPresets[a_preset].scale) * static_cast<double>(kPresets[a_preset].scale);
		if (interval.cost[a_preset] <= a_budgetMs) {
			return f;
		}
		bool anyFits = false;
		for (std::size_t i = 0; i < presets; ++i) {
			if (interval.cost[i] <= a_budgetMs) {
				anyFits = true;
				break;
			}
		}
		// Nothing fits: take the cheapest and accept the miss (D-6).
		return anyFits ? negative : f - 1000.0;
	};

	for (std::size_t p = 0; p < presets; ++p) {
		const double value = reward(0, p);
		if (value > negative) {
			previous[index(p, dwell)] = value;
		}
	}

	for (std::size_t i = 1; i < intervals.size(); ++i) {
		std::vector<double> current(states, negative);
		for (std::size_t p = 0; p < presets; ++p) {
			for (int held = 0; held <= dwell; ++held) {
				const double from = previous[index(p, held)];
				if (from <= negative) {
					continue;
				}
				// Hold.
				const double holdValue = from + reward(i, p);
				auto& holdSlot = current[index(p, std::min(held + 1, dwell))];
				if (holdValue > holdSlot) {
					holdSlot = holdValue;
					choice[i][index(p, std::min(held + 1, dwell))] = static_cast<int>(p);
				}
				// Change, only once the dwell has elapsed.
				if (held < dwell) {
					continue;
				}
				for (std::size_t q = 0; q < presets; ++q) {
					if (q == p) {
						continue;
					}
					const double moveValue = from + reward(i, q);
					if (moveValue <= negative) {
						continue;
					}
					auto& slot = current[index(q, 0)];
					if (moveValue > slot) {
						slot = moveValue;
						choice[i][index(q, 0)] = static_cast<int>(p);
					}
				}
			}
		}
		previous.swap(current);
	}

	std::size_t bestState = 0;
	double bestValue = negative;
	for (std::size_t s = 0; s < states; ++s) {
		if (previous[s] > bestValue) {
			bestValue = previous[s];
			bestState = s;
		}
	}
	if (bestValue <= negative) {
		return plan;
	}

	// Walk the choices back to recover the trajectory.
	std::vector<std::size_t> path(intervals.size());
	std::size_t state = bestState;
	for (std::size_t i = intervals.size(); i-- > 0;) {
		const std::size_t preset = state / static_cast<std::size_t>(dwell + 1);
		path[i] = preset;
		if (i == 0) {
			break;
		}
		const int from = choice[i][state];
		if (from < 0) {
			break;
		}
		const int held = static_cast<int>(state % static_cast<std::size_t>(dwell + 1));
		state = index(static_cast<std::size_t>(from), held == 0 ? dwell : held - 1);
	}

	double pixels = 0.0;
	std::size_t over = 0;
	for (std::size_t i = 0; i < path.size(); ++i) {
		const auto& info = kPresets[path[i]];
		pixels += static_cast<double>(info.scale) * static_cast<double>(info.scale);
		if (intervals[i].cost[path[i]] > a_budgetMs) {
			++over;
		}
		plan.trajectory.push_back(info.preset);
		if (i > 0 && path[i] != path[i - 1]) {
			++plan.changes;
		}
	}

	const double n = static_cast<double>(path.size());
	plan.timeWeightedPixelFraction = pixels / n;
	plan.overBudgetRate = static_cast<double>(over) / n;
	plan.durationSeconds = n * a_intervalSeconds;
	return plan;
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

	for (const auto& row : a_trace) {
		const auto recorded = FindPresetByPublicValue(row.presetPublicValue);
		if (!recorded || row.gpuUs == 0) {
			continue;
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
