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
	_probeInterval(a_config.probeIntervalSeconds)
{
	for (std::size_t i = 0; i < kPresets.size(); ++i) {
		_stepRatio[i] = SeedRatio(kPresets[i].preset);
		// D-26: nothing is known about a rung until the sweep measures it, and
		// an unmeasured rung must under-reach rather than guess. SetRungCosts
		// replaces these; the decay walks them down if it never runs.
		_climbThreshold[i] = a_config.unknownRungThresholdFrac;
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

// D-20. Returns the headroom this rung now requires, or 0 when it is free.
double GovernorCore::ClimbBlockedByFailure(Preset a_target, double a_headroomMs,
	double a_nowSeconds) const noexcept
{
	const auto index = RatioIndex(a_target);
	if (_climbFailedAt[index] <= 0.0) {
		return 0.0;  // never failed
	}
	// Old enough to be about a different place. D-9's T_reset, same reasoning:
	// without this, one bad patch of scenery makes the controller timid for the
	// rest of the session.
	if (a_nowSeconds - _climbFailedAt[index] > _config.climbFailForgetSeconds) {
		return 0.0;
	}
	const double required = _climbFailedHeadroom[index] + _config.climbRetryMarginMs;
	return a_headroomMs >= required ? 0.0 : required;
}

void GovernorCore::NoteClimbOutcome(Preset a_newPreset, double a_nowSeconds)
{
	if (!_climbInFlight) {
		return;
	}
	// A move to a preset at or below where the climb started means the climb
	// did not hold. Anything else - another climb, or a descent that stays
	// above it - is not evidence about this rung.
	if (PresetScale(a_newPreset) < PresetScale(_lastClimbTarget) &&
		a_nowSeconds - _lastClimbAt <= _config.climbReversalWindowSeconds) {
		const auto index = RatioIndex(_lastClimbTarget);
		_climbFailedHeadroom[index] = _lastClimbHeadroom;
		_climbFailedAt[index] = a_nowSeconds;
	}
	_climbInFlight = false;
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
}

void GovernorCore::NotifyApplied(Preset a_preset, double a_nowSeconds)
{
	// D-20: judge the climb in flight before recording this change as the new
	// baseline. A move back below where a climb landed, soon after it landed,
	// is that climb having failed - the one piece of evidence about a rung that
	// does not depend on the cost model being right.
	NoteClimbOutcome(a_preset, a_nowSeconds);
	if (_pendingAction == GovernorAction::Climb) {
		_climbInFlight = true;
		_lastClimbAt = a_nowSeconds;
	}

	_lastChangeAt = a_nowSeconds;

	// D-17: hold the pre-change observation so the next settled evaluation can
	// close the two-point calibration. Only worth it when the window actually
	// had GPU data to describe the preset we are leaving.
	if (_lastDecisionP95Gpu > 0.0) {
		_p95BeforeChange = _lastDecisionP95Gpu;
		_presetBeforeChange = _lastDecisionPreset;
		_awaitingCalibration = true;
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

	// D-25: counted per frame rather than per evaluation, so a burst spanning an
	// evaluation boundary is still seen as one run. A windowed rate cannot
	// distinguish six late frames together from six spread across two seconds,
	// and only the first is felt.
	if (entry.frameMs > _config.frameBudgetMs * _config.missToleranceFrac) {
		++_consecutiveMisses;
	} else {
		_consecutiveMisses = 0;
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
	double frameSum = 0.0;
	for (const auto& e : _window) {
		frames.push_back(e.frameMs);
		frameSum += e.frameMs;
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
	decision.meanFrameMs =
		_window.empty() ? 0.0 : frameSum / static_cast<double>(_window.size());
	decision.headroomMs = gpus.empty() ? 0.0 : _config.frameBudgetMs - decision.p95GpuMs;

	// D-25's delivery signal. Deliberately not dropRate, which only counts
	// intervals missed outright at 1.5x budget - far too coarse to steer on.
	const double lateAt = _config.frameBudgetMs * _config.missToleranceFrac;
	std::size_t late = 0;
	for (const double f : frames) {
		if (f > lateAt) {
			++late;
		}
	}
	decision.missRate = frames.empty() ? 0.0 : static_cast<double>(late) / frames.size();
	decision.consecutiveMisses = _consecutiveMisses;
	if (late > 0) {
		_lastMissAt = a_nowSeconds;
	}

	// D-18: close the measurement opened by the last change, now that the window
	// holds frames from the new preset only. This is D-5's free two-point
	// calibration, taken at last, and taken per step rather than fitted.
	if (_awaitingCalibration && decision.p95GpuMs > 0.0) {
		LearnStepRatio(_presetBeforeChange, _p95BeforeChange, a_current, decision.p95GpuMs);
		_awaitingCalibration = false;
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

	if (_config.adaptiveMode) {
		// Relax thresholds only while nothing is going wrong. Decaying during
		// trouble would lower the bar exactly when it should not.
		if (decision.missRate <= 0.0) {
			DecayThresholds(a_nowSeconds);
		}
		return EvaluateAdaptive(a_nowSeconds, a_current, std::move(decision));
	}

	if (_config.simpleMode) {
		return EvaluateSimple(a_nowSeconds, a_current, std::move(decision));
	}

	return decision.tier == GovernorTier::Headroom ?
	           EvaluateHeadroom(a_nowSeconds, a_current, std::move(decision)) :
	           EvaluateFrametime(a_nowSeconds, a_current, std::move(decision));
}

void GovernorCore::SetRungCosts(const std::array<double, kPresets.size()>& a_costMs)
{
	if (_config.frameBudgetMs <= 0.0) {
		return;
	}
	_rungCostsKnown = false;
	for (std::size_t i = 0; i < a_costMs.size(); ++i) {
		// A rung that costs nothing was not measured. A rung that costs more
		// than the whole budget is a measurement taken across a scene change,
		// not a step.
		const bool usable = a_costMs[i] > 0.0 && a_costMs[i] < _config.frameBudgetMs;
		_rungCostFrac[i] = usable ? a_costMs[i] / _config.frameBudgetMs : 0.0;
		_climbThreshold[i] = usable ? _rungCostFrac[i] + _config.climbMarginFrac :
									  _config.unknownRungThresholdFrac;
		_rungCostsKnown = _rungCostsKnown || usable;
	}
}

double GovernorCore::ClimbThreshold(Preset a_from) const noexcept
{
	// No sentinel. Every entry is initialised by the constructor and replaced by
	// SetRungCosts, so the stored value is always the answer.
	//
	// This used to fall back to unknownRungThresholdFrac when the entry was 0,
	// which read as "unset" - but 0 is also where an unmeasured rung's decay
	// legitimately ends up, so a fully relaxed threshold reported itself as the
	// pessimistic default. The controller kept climbing on the real value while
	// every diagnostic showed the wrong one.
	return _climbThreshold[RatioIndex(a_from)];
}

double GovernorCore::RungCostFraction(Preset a_from) const noexcept
{
	return _rungCostFrac[RatioIndex(a_from)];
}

// D-25's escape from a self-confirming threshold.
//
// A rung can only be learned by climbing it, and the threshold is what refuses
// the climb - which is how the governor sat at UltraPerformance with a rung it
// could never reach, no matter how good conditions became (E-54). Relaxing the
// demand after a clean stretch means it eventually tries.
//
// The floor is the measured price of the rung. Bidding below that is what made
// 14% hunt (E-57), and no amount of clean running makes a rung cheaper than it
// is - so the MARGIN can decay away, and the COST cannot.
void GovernorCore::DecayThresholds(double a_nowSeconds)
{
	if (a_nowSeconds - _lastDecayAt < _config.thresholdDecaySeconds) {
		return;
	}
	_lastDecayAt = a_nowSeconds;

	for (std::size_t i = 0; i < _climbThreshold.size(); ++i) {
		// A MEASURED rung already knows its correct resting value - the price
		// plus the margin the player chose - so decay only undoes a raise from a
		// past failure and stops there. Letting it erode the margin as well
		// would walk the threshold down to the bare price, which is exactly the
		// bid that made 14% hunt (E-57), and would silently discard the one
		// setting the player owns.
		//
		// An UNMEASURED rung has no such resting value, so it keeps relaxing
		// until a climb happens and teaches it one. That is the escape from
		// E-54's deadlock, and it is self-limiting: the climb either holds or
		// raises the threshold back.
		// Unmeasured rungs relax toward the margin, not toward zero. Zero would
		// eventually permit a climb on no spare capacity whatever, which is not
		// caution running out - it is the controller forgetting that a rung
		// costs anything at all. The margin is the least it can sensibly demand.
		const double floor = _rungCostFrac[i] > 0.0 ?
		                         _rungCostFrac[i] + _config.climbMarginFrac :
		                         _config.climbMarginFrac;
		_climbThreshold[i] = std::max(floor, _climbThreshold[i] - _config.thresholdDecayFrac);
	}
}

// D-25 + D-26. The controller the project arrived at.
//
// Two rules, in this order, and no predictor between the measurement and the
// decision:
//
//   descend  when frames are actually arriving late
//   climb    when spare capacity exceeds what the next rung is known to cost
//
// Descending on delivery rather than on GPU time is the point. E-49 measured
// that 92% of late frames had spare GPU time by our own instrument, so GPU time
// cannot see most of what makes frames late - and both candidate explanations
// for the difference died (E-55, E-56). Steering on the outcome does not require
// understanding the cause.
GovernorDecision GovernorCore::EvaluateAdaptive(double a_nowSeconds, Preset a_current,
	GovernorDecision a_base)
{
	auto decision = std::move(a_base);

	const auto index = RatioIndex(a_current);
	decision.rungCostFrac = _rungCostFrac[index];
	decision.climbThresholdFrac = ClimbThreshold(a_current);

	// --- down, first and unconditionally ---
	//
	// A burst triggers immediately; a sustained rate needs the window. The two
	// catch different failures: a cliff, and a scene that is merely too heavy.
	const bool burst = decision.consecutiveMisses >= _config.descendConsecutiveMisses;
	const bool sustained = decision.missRate > _config.descendMissRate;
	if (burst || sustained) {
		if (const auto down = NextDown(a_current)) {
			// A rung that could not hold has just told us its price was too
			// low. Raise what it asks next time rather than rediscovering this.
			_climbThreshold[RatioIndex(*down)] =
				std::max(_climbThreshold[RatioIndex(*down)],
					decision.headroomMs / _config.frameBudgetMs + _config.thresholdRaiseFrac);

			decision.action = GovernorAction::Descend;
			decision.target = *down;
			decision.reason = burst ?
				Say("descend: %zu frames late in a row, %.1f%% of the window over budget",
					decision.consecutiveMisses, decision.missRate * 100.0) :
				Say("descend: %.1f%% of the window late, over the %.1f%% limit",
					decision.missRate * 100.0, _config.descendMissRate * 100.0);
		} else {
			decision.reason = Say(
				"hold: %.1f%% of frames late but already at the cheapest preset",
				decision.missRate * 100.0);
		}
		return decision;
	}

	// --- up, only from a clean stretch ---
	const double cleanFor = a_nowSeconds - _lastMissAt;
	if (cleanFor < _config.climbCleanSeconds) {
		decision.reason = Say("hold: clean for %.1fs, want %.1fs before climbing", cleanFor,
			_config.climbCleanSeconds);
		return decision;
	}

	if (decision.p95GpuMs <= 0.0) {
		decision.reason = "hold: no GPU measurement to judge headroom with";
		return decision;
	}

	const auto up = NextUp(a_current);
	if (!up) {
		decision.reason = Say("hold: %.1f%% spare but already at maximum quality",
			100.0 * decision.headroomMs / _config.frameBudgetMs);
		return decision;
	}

	const double headroomFrac = decision.headroomMs / _config.frameBudgetMs;
	if (headroomFrac < decision.climbThresholdFrac) {
		decision.reason = decision.rungCostFrac > 0.0 ?
			Say("hold: %.1f%% spare, need %.1f%% (rung costs %.1f%% + %.1f%% margin)",
				headroomFrac * 100.0, decision.climbThresholdFrac * 100.0,
				decision.rungCostFrac * 100.0,
				(decision.climbThresholdFrac - decision.rungCostFrac) * 100.0) :
			Say("hold: %.1f%% spare, need %.1f%% (this rung has never been measured)",
				headroomFrac * 100.0, decision.climbThresholdFrac * 100.0);
		return decision;
	}

	decision.action = GovernorAction::Climb;
	decision.target = *up;
	_lastClimbTarget = *up;
	_lastClimbHeadroom = decision.headroomMs;
	decision.reason = decision.rungCostFrac > 0.0 ?
		Say("climb: %.1f%% spare clears %.1f%% (rung costs %.1f%%), clean for %.0fs",
			headroomFrac * 100.0, decision.climbThresholdFrac * 100.0,
			decision.rungCostFrac * 100.0, cleanFor) :
		Say("climb: %.1f%% spare clears %.1f%%, clean for %.0fs - probing an unmeasured rung",
			headroomFrac * 100.0, decision.climbThresholdFrac * 100.0, cleanFor);
	return decision;
}

// Simple mode. An instrument, not a controller.
//
// It exists to answer a question no table can: what does "20% overhead" actually
// look like, one preset higher? So it does exactly what a person reading the
// readout would do, and nothing else - no landing prediction, no step ratios, no
// failure memory, no probes.
//
// It reads the MEAN, deliberately, because the mean is what the readout prints
// and therefore what a person forms their intuition from. The rest of the
// controller decides on P95, and the two disagreed by more than a rung's worth
// on 2026-08-12. Matching the number on screen is the whole point here: an
// experiment that acts on a statistic the observer cannot see teaches nothing.
// It is also why this must never become the shipping controller as written - the
// mean hides the tail, and the tail is what a locked framerate is lost to.
GovernorDecision GovernorCore::EvaluateSimple(double a_nowSeconds, Preset a_current,
	GovernorDecision a_base)
{
	auto decision = std::move(a_base);
	(void)a_nowSeconds;

	const double fps = decision.meanFrameMs > 0.0 ? 1000.0 / decision.meanFrameMs : 0.0;

	// Down first, always. A rule that can climb and descend in the same tick
	// should descend.
	if (fps > 0.0 && fps < _config.simpleDescendFps) {
		if (const auto down = NextDown(a_current)) {
			decision.action = GovernorAction::Descend;
			decision.target = *down;
			decision.reason = Say("simple: %.1f fps below %.1f - down one", fps,
				_config.simpleDescendFps);
		} else {
			decision.reason = Say("simple: %.1f fps below %.1f but already at the cheapest preset",
				fps, _config.simpleDescendFps);
		}
		return decision;
	}

	// No GPU measurement means no overhead figure, and the whole rule is stated
	// in terms of one. Hold rather than guess.
	if (decision.meanGpuMs <= 0.0) {
		decision.reason = Say("simple: %.1f fps but no GPU measurement to read overhead from", fps);
		return decision;
	}

	const double headroomFrac =
		_config.frameBudgetMs > 0.0 ? 1.0 - decision.meanGpuMs / _config.frameBudgetMs : 0.0;

	if (headroomFrac >= _config.simpleClimbHeadroomFrac) {
		if (const auto up = NextUp(a_current)) {
			decision.action = GovernorAction::Climb;
			decision.target = *up;
			decision.reason = Say("simple: %.0f%% overhead at or above %.0f%% - up one (%.1f fps, "
								  "mean GPU %.2f ms)",
				headroomFrac * 100.0, _config.simpleClimbHeadroomFrac * 100.0, fps,
				decision.meanGpuMs);
		} else {
			decision.reason = Say("simple: %.0f%% overhead but already at maximum quality",
				headroomFrac * 100.0);
		}
		return decision;
	}

	decision.reason = Say("simple: %.0f%% overhead below %.0f%%, %.1f fps at or above %.1f - hold",
		headroomFrac * 100.0, _config.simpleClimbHeadroomFrac * 100.0, fps,
		_config.simpleDescendFps);
	return decision;
}

// The time parameter was unnamed until D-20, which needs it: a rung's failure
// is remembered with the moment it happened, so the memory can expire.
GovernorDecision GovernorCore::EvaluateHeadroom(double a_nowSeconds, Preset a_current,
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

			// D-20: this rung failed here before, and nothing has improved.
			//
			// The landing check asks whether a climb SHOULD fit, using a cost
			// model whose residual the replay itself warns about. This asks
			// whether one already DID NOT - which is the only evidence that
			// does not depend on the model being right. Keyed on headroom
			// rather than a timer so the rung re-opens when the scene actually
			// gets cheaper.
			if (const auto blocked = ClimbBlockedByFailure(target, decision.headroomMs,
					a_nowSeconds);
				blocked > 0.0) {
				decision.reason = Say(
					"hold: %.2f ms spare but %s failed at %.2f ms spare and needs %.2f",
					decision.headroomMs, PresetName(target).data(),
					_climbFailedHeadroom[RatioIndex(target)], blocked);
				return decision;
			}

			decision.action = GovernorAction::Climb;
			decision.target = target;
			_lastClimbTarget = target;
			_lastClimbHeadroom = decision.headroomMs;
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
