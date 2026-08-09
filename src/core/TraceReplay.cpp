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

		for (std::size_t i = 0; i < count; ++i) {
			if (frames[i] > 0.0) {
				missed.push_back(std::move(over[i]));
				weight.push_back(frames[i]);
			}
		}
	}
	if (missed.empty()) {
		return plan;
	}

	// State: (preset, intervals held since the last change, capped at the dwell).
	const int dwell = std::max(1, static_cast<int>(a_minDwellSeconds / a_intervalSeconds + 0.5));
	const std::size_t slots = static_cast<std::size_t>(dwell) + 1;
	const std::size_t states = presets * slots;
	constexpr double kUnreachable = -1.0e18;
	const auto index = [slots](std::size_t a_preset, int a_held) {
		return a_preset * slots + static_cast<std::size_t>(a_held);
	};

	struct Pass
	{
		std::vector<std::size_t> path;
		double pixels = 0.0;
		double overRate = 0.0;
	};

	// One dynamic-programming pass at a fixed price for a missed frame. Both
	// terms are counted in frames, so the price is what one missed frame is
	// worth in pixels and the trade is explicit.
	const auto solve = [&](double a_penalty) {
		const auto reward = [&](std::size_t a_interval, std::size_t a_preset) {
			const double f = static_cast<double>(kPresets[a_preset].scale) *
			                 static_cast<double>(kPresets[a_preset].scale);
			return f * weight[a_interval] -
			       a_penalty * static_cast<double>(missed[a_interval][a_preset]);
		};

		std::vector<double> previous(states, kUnreachable);
		std::vector<std::vector<int>> from(missed.size(), std::vector<int>(states, -1));

		for (std::size_t p = 0; p < presets; ++p) {
			previous[index(p, dwell)] = reward(0, p);
		}

		for (std::size_t i = 1; i < missed.size(); ++i) {
			std::vector<double> current(states, kUnreachable);
			for (std::size_t p = 0; p < presets; ++p) {
				for (int held = 0; held <= dwell; ++held) {
					const double value = previous[index(p, held)];
					if (value <= kUnreachable) {
						continue;
					}

					// Hold: the dwell counter advances, capped.
					const int nextHeld = std::min(held + 1, dwell);
					const double holdValue = value + reward(i, p);
					if (holdValue > current[index(p, nextHeld)]) {
						current[index(p, nextHeld)] = holdValue;
						from[i][index(p, nextHeld)] = static_cast<int>(p);
					}

					// Change: only once the dwell has elapsed.
					if (held < dwell) {
						continue;
					}
					for (std::size_t q = 0; q < presets; ++q) {
						if (q == p) {
							continue;
						}
						const double moveValue = value + reward(i, q);
						if (moveValue > current[index(q, 0)]) {
							current[index(q, 0)] = moveValue;
							from[i][index(q, 0)] = static_cast<int>(p);
						}
					}
				}
			}
			previous.swap(current);
		}

		Pass pass;
		std::size_t bestState = 0;
		double bestValue = kUnreachable;
		for (std::size_t s = 0; s < states; ++s) {
			if (previous[s] > bestValue) {
				bestValue = previous[s];
				bestState = s;
			}
		}
		if (bestValue <= kUnreachable) {
			return pass;
		}

		// Walk the choices back to recover the trajectory.
		pass.path.assign(missed.size(), 0);
		std::size_t state = bestState;
		for (std::size_t i = missed.size(); i-- > 0;) {
			pass.path[i] = state / slots;
			if (i == 0) {
				break;
			}
			const int previousPreset = from[i][state];
			if (previousPreset < 0) {
				break;
			}
			const int held = static_cast<int>(state % slots);
			// Held 0 means this interval was the change, so the previous state
			// had waited out the full dwell; otherwise it held one fewer.
			state = index(static_cast<std::size_t>(previousPreset), held == 0 ? dwell : held - 1);
		}

		// Frame-weighted, to match Replay: an interval holding fewer frames must
		// not count as much as a full one.
		double pixels = 0.0;
		double over = 0.0;
		double frames = 0.0;
		for (std::size_t i = 0; i < pass.path.size(); ++i) {
			const double f = static_cast<double>(kPresets[pass.path[i]].scale) *
			                 static_cast<double>(kPresets[pass.path[i]].scale);
			pixels += f * weight[i];
			over += static_cast<double>(missed[i][pass.path[i]]);
			frames += weight[i];
		}
		if (frames > 0.0) {
			pass.pixels = pixels / frames;
			pass.overRate = over / frames;
		}
		return pass;
	};

	// Price over-budget intervals until the plan spends no more time over budget
	// than the controller is allowed to. Zero price maximises pixels and ignores
	// the budget; a large one refuses every risk. Anything in between trades at
	// a rate, and bisection finds the cheapest price that satisfies the
	// allowance.
	Pass best = solve(0.0);
	if (best.overRate > a_overBudgetAllowance) {
		double low = 0.0;
		double high = 100.0;
		for (int iteration = 0; iteration < 32; ++iteration) {
			const double penalty = 0.5 * (low + high);
			const auto pass = solve(penalty);
			if (pass.overRate > a_overBudgetAllowance) {
				low = penalty;
			} else {
				high = penalty;
				best = pass;
			}
		}
	}
	if (best.path.empty()) {
		return plan;
	}

	for (std::size_t i = 0; i < best.path.size(); ++i) {
		plan.trajectory.push_back(kPresets[best.path[i]].preset);
		if (i > 0 && best.path[i] != best.path[i - 1]) {
			++plan.changes;
		}
	}
	plan.timeWeightedPixelFraction = best.pixels;
	plan.overBudgetRate = best.overRate;
	plan.durationSeconds = static_cast<double>(best.path.size()) * a_intervalSeconds;
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
