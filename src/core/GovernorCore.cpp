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

namespace {

// What a single upward rung has cost, measured. Index by the preset being left.
//
// Seeded from sweeps on 2026-08-08 (E-27), taking the LARGER of the two observed
// values per step so an unlearnt step over-states its cost and the first climb
// under-reaches rather than overshooting. Every adjacent change replaces its
// entry with the real thing.
//
// A single fitted k cannot do this job: the implied k across these same steps
// runs from 0.21 to 2.27, so the linear form misfits whichever end it is not
// fitted to - and it under-predicted the cheap rungs, which is what made the
// controller hunt (E-26).
[[nodiscard]] double SeedRatio(Preset a_from) noexcept
{
	switch (a_from) {
	case Preset::UltraPerformance:
		return 1.243;  // -> Performance
	case Preset::Performance:
		return 1.139;  // -> Balanced
	case Preset::Balanced:
		return 1.116;  // -> Quality
	case Preset::Quality:
		return 1.125;  // -> UltraQuality
	case Preset::UltraQuality:
		return 1.079;  // -> Hoshipa
	case Preset::Hoshipa:
		return 1.077;  // -> NativeAA
	case Preset::NativeAA:
	default:
		return 1.0;  // nothing above it
	}
}

[[nodiscard]] std::size_t RatioIndex(Preset a_preset) noexcept
{
	for (std::size_t i = 0; i < kPresets.size(); ++i) {
		if (kPresets[i].preset == a_preset) {
			return i;
		}
	}
	return 0;
}

}

GovernorCore::GovernorCore(GovernorConfig a_config) :
	_config(a_config),
	_probeInterval(a_config.probeIntervalSeconds),
	_calibrationSettle(SettleDetector::Config{ a_config.minSamples,
		a_config.calibrationSettleToleranceMs, 5 })
{
	for (std::size_t i = 0; i < kPresets.size(); ++i) {
		_stepRatio[i] = SeedRatio(kPresets[i].preset);
	}
}

double GovernorCore::StepRatio(Preset a_from) const noexcept
{
	return _stepRatio[RatioIndex(a_from)];
}

bool GovernorCore::StepMeasured(Preset a_from) const noexcept
{
	return _stepMeasured[RatioIndex(a_from)];
}

std::size_t GovernorCore::StepObservations(Preset a_from) const noexcept
{
	return _stepObservations[RatioIndex(a_from)];
}

void GovernorCore::LearnStepRatio(Preset a_from, double a_p95Before, Preset a_to,
	double a_p95After)
{
	if (a_p95Before <= 0.0 || a_p95After <= 0.0) {
		return;
	}
	// Only an adjacent upward step measures a step. A multi-rung move measures
	// the product of several, and attributing it to one would corrupt them all.
	const auto up = NextUp(a_from);
	if (!up || *up != a_to) {
		return;
	}

	const double ratio = a_p95After / a_p95Before;
	// Outside this range it is not a step, it is a scene that changed while the
	// preset did. Keeping the previous number beats adopting that one.
	if (!std::isfinite(ratio) || ratio < _config.stepRatioMin || ratio > _config.stepRatioMax) {
		return;
	}

	const auto index = RatioIndex(a_from);
	auto& stored = _stepRatio[index];

	// The first real measurement REPLACES the seed rather than blending with
	// it. The seeds were measured on one machine with one mod list and one
	// resolution; they are a starting point, not a claim, and treating them as
	// a prior worth defending would leave 70% of somebody else's hardware in
	// the estimate after their first transition. Later observations smooth
	// against each other, where averaging is what one wants.
	++_stepObservations[index];
	if (!_stepMeasured[index]) {
		stored = ratio;
		_stepMeasured[index] = true;
		return;
	}
	stored = (1.0 - _config.stepRatioAlpha) * stored + _config.stepRatioAlpha * ratio;
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

	// A scene cut invalidates the pending calibration too: the "before" half
	// describes a place we are no longer in, so the pair would measure the cut
	// rather than the rung.
	_awaitingCalibration = false;
	_calibrationSettle.Reset();
	_calibrationSettledAt = -1.0;
}

void GovernorCore::NotifyApplied(Preset a_preset, double a_nowSeconds)
{
	(void)a_preset;
	_lastChangeAt = a_nowSeconds;

	// D-17: hold the pre-change observation so the next settled evaluation can
	// close the two-point calibration. Only worth it when the window actually
	// had GPU data to describe the preset we are leaving.
	if (_lastDecisionP95Gpu > 0.0) {
		_p95BeforeChange = _lastDecisionP95Gpu;
		_presetBeforeChange = _lastDecisionPreset;
		_awaitingCalibration = true;
		// D-19: the transition starts here, so the settle run starts here too.
		_calibrationSettle.Reset();
		_calibrationSettledAt = -1.0;
	}

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

	// D-19: watch the transition settle, on GPU time rather than frametime. The
	// detector wants a sustained quiet run, so a transient keeps restarting it
	// and the moment it reports is measured rather than assumed - which is why
	// this is the settle detector and not a fixed delay.
	if (_awaitingCalibration && _calibrationSettledAt < 0.0 && entry.gpuMs > 0.0) {
		if (_calibrationSettle.Push(entry.gpuMs)) {
			_calibrationSettledAt = a_sample.nowSeconds;
		}
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

	// D-18: close the measurement opened by the last change, now that the window
	// holds frames from the new preset only. This is D-5's free two-point
	// calibration, taken at last, and taken per step rather than fitted.
	//
	// D-19 adds the second condition: the window must also hold no frame from
	// before the transition settled. "Full" was never the same as "quiet" - 30
	// samples arrive in 0.42 s and settle takes ~1.0 s - and since settle only
	// ever adds GPU time, a P95 taken early is inflated every single time. That
	// bias does not average out, so smoothing more of them converged on the
	// wrong number (E-31).
	// The measurement is taken over the settled frames ONLY, rather than by
	// waiting for the whole judge window to be free of them. Waiting would push
	// the calibration past settle plus a full window - about 3.2 s at the
	// shipped values - which is longer than the 3.0 s cooldown, so a controller
	// changing at its normal cadence would have every calibration pre-empted by
	// the next change and would learn nothing at all. Filtering says the same
	// thing without that side effect.
	if (_awaitingCalibration && _calibrationSettledAt >= 0.0) {
		std::vector<double> settledGpu;
		settledGpu.reserve(_timedSamples);
		for (const auto& e : _window) {
			if (e.gpuMs > 0.0 && e.t >= _calibrationSettledAt) {
				settledGpu.push_back(e.gpuMs);
			}
		}
		if (settledGpu.size() >= _config.minSamples) {
			LearnStepRatio(_presetBeforeChange, _p95BeforeChange, a_current,
				Percentile(settledGpu, 95.0));
			_awaitingCalibration = false;
		}
	}
	_lastDecisionP95Gpu = decision.p95GpuMs;
	_lastDecisionPreset = a_current;

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
		if (const auto down = NextDown(a_current)) {
			decision.action = GovernorAction::Descend;
			decision.target = *down;
			decision.reason = Say(
				"descend: p95 GPU %.2f ms over %.2f ms (%.2f ms into the budget)",
				decision.p95GpuMs, descendAt, -decision.headroomMs);
		} else {
			// D-6: nothing cheaper exists. Say so rather than logging nothing,
			// because "ungovernable" is a result.
			decision.reason = Say(
				"hold: p95 GPU %.2f ms over budget but already at the cheapest preset",
				decision.p95GpuMs);
		}
		return decision;
	}

	if (decision.p95GpuMs < climbAt) {
		if (const auto up = NextUp(a_current)) {
			// D-15: with an uncensored signal there is no reason to feel for
			// the ceiling one rung at a time. Predict where each rung lands and
			// take the highest that still sits inside the hold band - one
			// change instead of three, each of which would cost a history reset
			// and a visible transition.
			const double landAt = descendAt - _config.landingMarginMs;
			Preset target = *up;
			int rungs = 0;
			double predictedTarget = 0.0;
			// D-18: walk the ladder multiplying by each step's measured cost.
			// No model to be wrong at one end - a multi-rung landing is just the
			// product of the rungs it passes through.
			double predicted = decision.p95GpuMs;
			Preset from = a_current;
			for (auto candidate = up; candidate && rungs < _config.maxClimbRungs;
				candidate = NextUp(*candidate)) {
				predicted *= StepRatio(from);
				from = *candidate;
				// D-16: every rung is checked, including the first. "Is there
				// spare capacity now" and "will there still be after paying for
				// this rung" differ by exactly the cost of the rung.
				//
				// Prophylactic, not a fix for an observed failure: the one
				// climb-then-reverse in the first live session turned out to be
				// the player walking into an unlit interior and back out, which
				// this check would have permitted anyway.
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
			decision.reason = Say(
				"climb: p95 GPU %.2f ms leaves %.2f ms spare, %d rung(s) fit, landing ~%.2f ms "
				"(step costs %.3fx)",
				decision.p95GpuMs, decision.headroomMs, rungs, predictedTarget,
				StepRatio(a_current));
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
