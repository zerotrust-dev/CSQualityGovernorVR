#include "GovernorCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace csgov {

namespace {

// snprintf rather than std::format, to match the rest of core. The unit tests
// build under GCC as well as MSVC, and this library stays on the conservative
// side of library support on purpose.
template <class... Args>
[[nodiscard]] std::string Say(const char* a_format, Args... a_args)
{
	char buf[256]{};
	std::snprintf(buf, sizeof(buf), a_format, a_args...);
	return buf;
}

// Ladder ordered cheapest first, by pixel fraction. kPresets is already in that
// order, but relying on that silently would make a future reorder a control bug
// rather than a compile error, so the neighbours are resolved by scale.
[[nodiscard]] std::optional<Preset> NextUp(Preset a_preset) noexcept
{
	const float current = PresetScale(a_preset);
	std::optional<Preset> best;
	float bestScale = std::numeric_limits<float>::max();
	for (const auto& info : kPresets) {
		if (info.scale > current && info.scale < bestScale) {
			bestScale = info.scale;
			best = info.preset;
		}
	}
	return best;
}

[[nodiscard]] std::optional<Preset> NextDown(Preset a_preset) noexcept
{
	const float current = PresetScale(a_preset);
	std::optional<Preset> best;
	float bestScale = std::numeric_limits<float>::lowest();
	for (const auto& info : kPresets) {
		if (info.scale < current && info.scale > bestScale) {
			bestScale = info.scale;
			best = info.preset;
		}
	}
	return best;
}

[[nodiscard]] double Percentile(std::vector<double> a_values, double a_percentile) noexcept
{
	if (a_values.empty()) {
		return 0.0;
	}
	std::sort(a_values.begin(), a_values.end());
	return PercentileSorted(a_values, a_percentile);
}

}

std::string_view GovernorTierName(GovernorTier a_tier) noexcept
{
	return a_tier == GovernorTier::Headroom ? "headroom" : "frametime";
}

std::string_view GovernorActionName(GovernorAction a_action) noexcept
{
	switch (a_action) {
	case GovernorAction::Climb:
		return "climb";
	case GovernorAction::Descend:
		return "descend";
	case GovernorAction::Hold:
	default:
		return "hold";
	}
}

GovernorConfig GovernorConfig::Default()
{
	return GovernorConfig{};
}

GovernorCore::GovernorCore(GovernorConfig a_config) :
	_config(a_config),
	_probeInterval(a_config.probeIntervalSeconds)
{
}

void GovernorCore::Reset(double a_nowSeconds)
{
	_window.clear();
	_timedSamples = 0;
	_lastEvalAt = a_nowSeconds;
	// A scene cut is D-9's other backoff reset: the reason the last probe
	// failed was a place we are no longer in.
	_probeActive = false;
	_probeInterval = _config.probeIntervalSeconds;
}

void GovernorCore::NotifyApplied(Preset a_preset, double a_nowSeconds)
{
	(void)a_preset;
	_lastChangeAt = a_nowSeconds;

	if (_pendingAction == GovernorAction::Climb && _pendingTier == GovernorTier::Frametime) {
		// A blind probe. Whether it holds is judged by what happens next.
		_lastProbeAt = a_nowSeconds;
		_probeActive = true;
	} else if (_pendingAction == GovernorAction::Descend && _probeActive) {
		// The probe did not hold. Double the interval so a scene that cannot
		// afford more quality is not asked the same question every 20 seconds
		// (D-9).
		_probeInterval = std::min(_probeInterval * 2.0, _config.probeIntervalMaxSeconds);
		_probeActive = false;
	}
	_pendingAction = GovernorAction::Hold;

	// Everything in the window describes the preset we just left.
	_window.clear();
	_timedSamples = 0;
}

void GovernorCore::Trim(double a_nowSeconds)
{
	const double cutoff = a_nowSeconds - _config.judgeWindowSeconds;
	while (!_window.empty() && _window.front().t < cutoff) {
		if (_window.front().gpuMs > 0.0 && _timedSamples > 0) {
			--_timedSamples;
		}
		_window.pop_front();
	}
}

std::optional<GovernorDecision> GovernorCore::Push(const GovernorSample& a_sample, Preset a_current)
{
	Entry entry;
	entry.t = a_sample.nowSeconds;
	entry.frameMs = a_sample.frameTimeMs;

	// A GPU reading only counts if it is NEW. A repeated frame index means the
	// timer produced nothing this frame, and reusing the previous value would
	// invent samples - which is exactly how the frame sampler once biased a
	// whole ladder (E-10, E-11).
	if (a_sample.gpuTimeUs > 0 && a_sample.gpuFrameIndex != _lastGpuFrameIndex) {
		entry.gpuMs = static_cast<double>(a_sample.gpuTimeUs) / 1000.0;
		_lastGpuFrameIndex = a_sample.gpuFrameIndex;
		++_timedSamples;
	}

	_window.push_back(entry);
	Trim(a_sample.nowSeconds);

	// The tier follows the window, so it recovers on its own when the timer
	// starts or stops - during a load, say - without anyone toggling anything.
	_tier = _timedSamples * 2 >= _window.size() && _timedSamples > 0 ? GovernorTier::Headroom
																	: GovernorTier::Frametime;

	if (a_sample.nowSeconds - _lastEvalAt < _config.evalIntervalSeconds) {
		return std::nullopt;
	}
	_lastEvalAt = a_sample.nowSeconds;

	// A probe that has survived this long is not a failed probe. Forgetting the
	// backoff here is what lets the controller climb again after a scene that
	// genuinely could not afford it has been left behind (D-9).
	if (_probeActive && a_sample.nowSeconds - _lastProbeAt >= _config.probeResetSeconds) {
		_probeActive = false;
		_probeInterval = _config.probeIntervalSeconds;
	}

	if (_window.size() < _config.minSamples) {
		return std::nullopt;
	}

	auto decision = Evaluate(a_sample.nowSeconds, a_current);
	_pendingAction = decision.action;
	_pendingTier = decision.tier;
	return decision;
}

GovernorDecision GovernorCore::Evaluate(double a_nowSeconds, Preset a_current)
{
	std::vector<double> frames;
	std::vector<double> gpus;
	frames.reserve(_window.size());
	gpus.reserve(_timedSamples);
	double gpuSum = 0.0;
	for (const auto& e : _window) {
		frames.push_back(e.frameMs);
		if (e.gpuMs > 0.0) {
			gpus.push_back(e.gpuMs);
			gpuSum += e.gpuMs;
		}
	}

	GovernorDecision decision;
	decision.target = a_current;
	decision.tier = _tier;
	decision.atSeconds = a_nowSeconds;
	decision.samples = _window.size();
	decision.p95FrameMs = Percentile(frames, 95.0);
	decision.p95GpuMs = gpus.empty() ? 0.0 : Percentile(gpus, 95.0);
	decision.meanGpuMs = gpus.empty() ? 0.0 : gpuSum / static_cast<double>(gpus.size());
	decision.headroomMs = gpus.empty() ? 0.0 : _config.frameBudgetMs - decision.p95GpuMs;

	const auto stats = ComputeStats(frames, _config.frameBudgetMs);
	decision.dropRate = stats.dropRate;
	decision.censored = decision.p95FrameMs <= _config.frameBudgetMs * (1.0 + _config.capTolerance) &&
	                    stats.dropRate <= _config.dropFloor;

	// E-2 measured settle at ~1.0 s. Acting inside that means measuring the
	// previous change rather than the scene (D-8).
	if (a_nowSeconds - _lastChangeAt < _config.cooldownSeconds) {
		decision.reason = Say("hold: cooldown, %.1fs since last change",
			a_nowSeconds - _lastChangeAt);
		return decision;
	}

	return decision.tier == GovernorTier::Headroom ?
	           EvaluateHeadroom(a_nowSeconds, a_current, std::move(decision)) :
	           EvaluateFrametime(a_nowSeconds, a_current, std::move(decision));
}

GovernorDecision GovernorCore::EvaluateHeadroom(double, Preset a_current,
	GovernorDecision a_base)
{
	auto decision = std::move(a_base);

	// Both directions are informed here, which is the whole point of D-10: with
	// an uncensored signal there is no blind probe and no two-regime split.
	const double descendAt = _config.frameBudgetMs - _config.marginDownMs;
	const double climbAt = _config.frameBudgetMs - _config.marginUpMs;

	if (decision.p95GpuMs > descendAt) {
		// D-16: a descent has to be earned. Crossing by 0.01 ms is noise, and
		// acting on it costs about six dropped frames for the transition plus a
		// rung of quality for the next half-minute.
		++_overBudgetEvals;
		if (_overBudgetEvals < _config.descendConfirmations) {
			decision.reason = Say("hold: p95 GPU %.2f ms over %.2f ms, %d of %d confirmations",
				decision.p95GpuMs, descendAt, _overBudgetEvals, _config.descendConfirmations);
			return decision;
		}

		if (const auto down = NextDown(a_current)) {
			_overBudgetEvals = 0;
			decision.action = GovernorAction::Descend;
			decision.target = *down;
			decision.reason = Say(
				"descend: p95 GPU %.2f ms over %.2f ms for %d evaluations (%.2f ms into the budget)",
				decision.p95GpuMs, descendAt, _config.descendConfirmations, -decision.headroomMs);
		} else {
			// D-6: nothing cheaper exists. Say so rather than logging nothing,
			// because "ungovernable" is a result.
			decision.reason = Say(
				"hold: p95 GPU %.2f ms over budget but already at the cheapest preset",
				decision.p95GpuMs);
		}
		return decision;
	}

	// Anything not over the line breaks the run: a trend has to be continuous.
	_overBudgetEvals = 0;

	if (decision.p95GpuMs < climbAt) {
		if (const auto up = NextUp(a_current)) {
			// D-15: with an uncensored signal there is no reason to feel for
			// the ceiling one rung at a time. Predict where each rung lands and
			// take the highest that still sits inside the hold band - one
			// change instead of three, each of which would cost a history reset
			// and a visible transition.
			const double fNow = static_cast<double>(PresetPixelFraction(a_current));
			const double landAt = descendAt - _config.landingMarginMs;
			Preset target = *up;
			int rungs = 0;
			double predictedTarget = 0.0;
			for (auto candidate = up; candidate && rungs < _config.maxClimbRungs;
				candidate = NextUp(*candidate)) {
				const double fTarget = static_cast<double>(PresetPixelFraction(*candidate));
				const double predicted = decision.p95GpuMs * (1.0 + _config.costK * fTarget) /
				                         (1.0 + _config.costK * fNow);
				// D-16: every rung is checked, including the first. "Is there
				// spare capacity now" and "will there still be after paying for
				// this rung" differ by exactly the cost of the rung, and taking
				// the first on the climb test alone sent one session to
				// UltraQuality and back within eight seconds.
				if (predicted > landAt) {
					break;
				}
				target = *candidate;
				predictedTarget = predicted;
				++rungs;
			}

			if (rungs == 0) {
				decision.reason = Say(
					"hold: %.2f ms spare but one rung would land past %.2f ms",
					decision.headroomMs, landAt);
				return decision;
			}

			decision.action = GovernorAction::Climb;
			decision.target = target;
			decision.reason =
				Say("climb: p95 GPU %.2f ms leaves %.2f ms spare, %d rung(s) fit, landing ~%.2f ms",
					decision.p95GpuMs, decision.headroomMs, rungs, predictedTarget);
		} else {
			decision.reason = Say("hold: %.2f ms spare but already at maximum quality",
				decision.headroomMs);
		}
		return decision;
	}

	decision.reason = Say("hold: p95 GPU %.2f ms inside the band [%.2f, %.2f]",
		decision.p95GpuMs, climbAt, descendAt);
	return decision;
}

GovernorDecision GovernorCore::EvaluateFrametime(double a_nowSeconds, Preset a_current,
	GovernorDecision a_base)
{
	auto decision = std::move(a_base);

	// The fallback tier, and not a stub: the first shipped version runs against
	// an unmodified Community Shaders, which has no GPU timer (D-10).
	if (decision.dropRate > _config.dropMax) {
		if (const auto down = NextDown(a_current)) {
			decision.action = GovernorAction::Descend;
			decision.target = *down;
			decision.reason = Say("descend: drop rate %.1f%% over %.1f%% (p95 %.2f ms)",
				decision.dropRate * 100.0, _config.dropMax * 100.0, decision.p95FrameMs);
		} else {
			decision.reason = Say("hold: dropping %.1f%% at the cheapest preset",
				decision.dropRate * 100.0);
		}
		return decision;
	}

	// D-4: censored means the frametime is a lower bound, so there is no way to
	// compute the spare capacity - only to try. Hence a slow, backing-off probe
	// rather than the informed climb the headroom tier gets.
	if (decision.censored && a_nowSeconds - _lastProbeAt >= _probeInterval) {
		if (const auto up = NextUp(a_current)) {
			decision.action = GovernorAction::Climb;
			decision.target = *up;
			decision.reason = Say(
				"climb: probing, frametime censored at the cap (p95 %.2f ms, drops %.1f%%)",
				decision.p95FrameMs, decision.dropRate * 100.0);
		} else {
			decision.reason = "hold: censored at maximum quality, nothing to probe";
		}
		return decision;
	}

	decision.reason = decision.censored ?
	                      Say("hold: censored, %.0fs until the next probe",
							  _probeInterval - (a_nowSeconds - _lastProbeAt)) :
	                      Say("hold: p95 %.2f ms, drops %.1f%%", decision.p95FrameMs,
							  decision.dropRate * 100.0);
	return decision;
}

}
