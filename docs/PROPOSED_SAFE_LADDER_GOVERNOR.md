# Proposed safe-ladder governor

**Status:** design proposal for review; not a decision and not implemented  
**Audience:** Claude and future maintainers of `CSQualityGovernorVR`  
**Date:** 2026-08-12  
**Purpose:** replace the choice between D-18 and rejected D-24 with a controller
that optimises the seven discrete quality presets while treating actual VR
delivery, model uncertainty, and environment changes as first-class inputs.

This document is intentionally self-contained. It incorporates an independent
second review of the proposal before describing how to implement it. The
controller described here is more conservative about what can be known than the
earlier draft, but it is also more adaptable: it contains no machine-specific
step seeds, no assumed `scale²` cost law, and no Pimax-specific resolution
constants in its decision path.

---

## 1. Executive conclusion

Do not choose between D-18's six independently learnt ratios and D-24's single
linear curve. Both try to make the GPU cost predictor carry more authority than
the evidence permits.

Implement a **safe discrete ladder controller** with four separable parts:

1. A fast application-GPU safety loop, using deduplicated tail measurements.
2. A separately validated VR-delivery loop, using compositor present/drop/
   reprojection data where trustworthy and an explicitly weaker fallback where
   it is not.
3. A session-specific seven-point cost profile, measured directly rather than
   forced onto `scale²` or split into six unrelated seeds.
4. An uncertainty-aware transition predictor that learns its **landing error**
   and makes unmeasured climbs as explicit bounded probes.

The priority order is lexicographic:

1. Keep VR delivery within its configured safety limit.
2. Keep application GPU tail time within its budget.
3. Select the highest-quality safe preset.
4. Minimise preset changes and failed probes.

GPU time proposes what might be safe. A validated delivery signal decides
whether it actually was safe. A climb remains provisional until both signals
accept it.

This is the critical change from the current design. E-49/E-52 show that the
application GPU bracket does not explain all late application intervals. A more
accurate GPU ladder model cannot repair an omitted part of the pipeline.

---

## 2. Corrections made during the second review

The initial safe-ladder proposal was directionally right, but four claims needed
correction before it was suitable as a foundation.

### 2.1 Do not call the existing `frame_ms > 14.5` signal a headset miss

The present capture proves that application frame intervals become late in a
preset-dependent way while measured application GPU work remains below budget.
It does **not**, by itself, prove which images the Pimax runtime scanned out.

OpenVR's `Compositor_FrameTiming` has the fields needed to describe delivery:

- `m_nNumFramePresents`
- `m_nNumDroppedFrames`
- `m_nReprojectionFlags`
- `m_nNumMisPresented`

Valve documents `m_nNumDroppedFrames` as additional scans of the previous frame
and `m_nNumFramePresents > 1` as reuse/reprojection of the scene texture.
However Skyrim is going through OpenComposite, so the implementation may not
populate every field with SteamVR-equivalent semantics. These values are only
eligible to drive decisions after a capability test demonstrates that their
frame indices are fresh and that deliberate overload changes the expected
counters. That test establishes operational fitness for the tested runtime; it
does not turn undocumented OpenComposite behaviour into a universal guarantee.

Core OpenXR provides `predictedDisplayTime`, `predictedDisplayPeriod`, and frame
loop throttling. It does not provide a portable core result saying which
application image was ultimately scanned out. An OpenXR/OpenComposite hook can
therefore improve frame-period and deadline observation but cannot invent
runtime presentation telemetry the runtime does not expose.

The design below uses the following terminology:

- **delivery event:** a validated dropped, repeated, mis-presented, or
  reprojected application frame from compositor/runtime telemetry.
- **late application interval:** a fallback observation derived from the
  application's pacing interval.
- **delivery-valid:** true only when the primary telemetry path has passed its
  capability test.

Logs and reports must never silently rename late application intervals as HMD
misses.

### 2.2 Frames are correlated; raw-frame confidence is false confidence

A Beta-binomial posterior over individual 72 Hz frames assumes independent
trials. VR failures arrive in bursts during scene transitions, scheduling
hitches, relatches, and sustained overload. Treating 720 frames as 720
independent observations would make the controller far more confident than the
data warrants.

Delivery health is therefore aggregated into fixed one-second time blocks.
Decisions use:

- the total delivery-event rate over recent blocks;
- the worst recent block;
- the longest consecutive run of bad intervals;
- clean time since the last event;
- the number of valid blocks, not merely the number of frames.

If a statistical confidence interval is later required, use block bootstrap or
a conservative interval whose effective sample count is the number of blocks.
The initial implementation does not need a probabilistic claim at all; explicit
block thresholds are easier to audit.

### 2.3 Quality order does not prove cost monotonicity

The seven presets have a known image-quality order. That does not logically
guarantee their total GPU cost is monotonic on every driver and DLSS build.
Upscaler inference cost, cache behaviour, pass selection, or a future Community
Shaders change could produce a local inversion.

Do not force isotonic regression into the control input. Preserve the raw
per-preset measurements and their intervals. A statistically significant cost
inversion is a calibration warning, not data to overwrite.

Monotonicity remains a valuable validation test:

- overlapping uncertainty intervals: acceptable ambiguity;
- small inversion inside uncertainty: keep the raw data and lower confidence;
- inversion larger than uncertainty: reject or degrade the calibration.

The controller's quality order and measured cost order remain separate facts.

### 2.4 `max(additive, multiplicative)` is not a mathematical upper bound

The additive and multiplicative forms bracket two plausible scene-reanchoring
assumptions. Their maximum is often conservative relative to either expert, but
there is no proof that reality cannot exceed both.

The actual safety allowance must come from held-out **underprediction error**:

```text
underprediction = max(0, realised_settled_p95 - predicted_p95)
```

Until enough transition residuals exist, confidence is low and climbing is
restricted to one adjacent rung under an enlarged bootstrap allowance. The
word "upper" below means an empirically padded decision estimate, not a formal
probability guarantee.

---

## 3. Required invariants

These should be recorded as design decisions before implementation.

1. **One owner per lever.** The cycler, the governor, and the player must not
   change the preset concurrently.
2. **Delivery and GPU safety are separate.** Neither is synthesized from the
   other.
3. **No stale GPU sample is counted twice.** Deduplicate by `gpuFrameIndex` in
   production, calibration, replay, reports, and tests.
4. **No counterfactual is called an observation.** Predictions, observed
   landings, and runtime delivery events have distinct fields.
5. **A climb is provisional.** It is accepted only after settle and a longer
   delivery assessment.
6. **Exploration is explicit.** An unmeasured rung may be probed, but the log
   must say that a probe—not an informed optimisation—is occurring.
7. **Configuration identity is measured.** Absolute profiles are never reused
   across a changed environment signature.
8. **Unknown means cautious, not impossible.** Missing calibration falls back
   to bounded adjacent probes, not foreign absolute seeds.
9. **Exhausted safe actuator choices are a result.** Report the period as
   ungovernable when no remaining preset has credible evidence of restoring
   safety; do not oscillate in search of a nonexistent solution.
10. **Every hold reaches the timeline and periodically reaches the log.**

---

## 4. Environment signature: the basis of configuration agnosticism

The governor should react to what the running system exposes, not to labels such
as "Pimax ImageQuality 0.75" or "72 Hz Upscale (Lab)". Those labels may change
between Pimax Play versions and may not be available to an SKSE plugin.

Create an `EnvironmentSignature` from observable consequences:

```cpp
struct EnvironmentSignature
{
    std::uint32_t refreshMilliHz = 0;

    // Stable identifiers only; hash normalised runtime/system and GPU/driver
    // data rather than transient handles or localised display strings.
    std::uint64_t runtimeAndSystemHash = 0;
    std::uint64_t gpuAndDriverHash = 0;

    // From IVRSystem::GetRecommendedRenderTargetSize when available.
    std::uint32_t recommendedEyeWidth = 0;
    std::uint32_t recommendedEyeHeight = 0;

    // Canonical per-eye size derived from the submitted texture and bounds.
    std::uint32_t submittedEyeWidth = 0;
    std::uint32_t submittedEyeHeight = 0;

    std::uint32_t csBuild = 0;
    std::uint32_t csApiRevision = 0;
    std::uint32_t upscaleMethod = 0;
    bool renderAtUpscaleRes = false;
    std::uint64_t upscalerImplementationHash = 0;

    // Hash of public preset order, verified scales/profile identifiers where
    // available, and any transition API version that changes actuator behaviour.
    std::uint64_t ladderAndApiHash = 0;

    auto operator<=>(const EnvironmentSignature&) const = default;
};
```

Build `runtimeAndSystemHash` from stable, normalised properties available at the
runtime boundary (for example runtime/provider build plus tracking-system, HMD
model, and hashed serial identifiers). Build `gpuAndDriverHash` from the D3D
adapter identity and driver version. Derive `upscalerImplementationHash` from a
stable build/version identity of the loaded implementation, not from a mutable
path. If a property is unavailable, encode that absence explicitly; do not
substitute a transient pointer, handle, or display name. An incomplete signature
further reduces trust in persisted priors.

### 4.1 Canonicalise texture geometry

E-43 proved that the submitted texture may switch between per-eye and
double-wide layouts within one session. Raw texture width is therefore not an
environment signature.

Compute effective eye dimensions from the submitted bounds:

```cpp
eyeWidth  = round(textureWidth  * abs(bounds.uMax - bounds.uMin));
eyeHeight = round(textureHeight * abs(bounds.vMax - bounds.vMin));
```

Require several stable frames before accepting a geometry change. This treats
`3494x3558` per eye and a `6988x3558` double-wide texture with half-width bounds
as the same environment.

### 4.2 Derive refresh rather than trusting `TargetHz`

Preferred sources, in order:

1. OpenVR `Prop_DisplayFrequency_Float` if valid.
2. OpenXR `predictedDisplayPeriod` if an OpenComposite/OpenXR hook exposes it.
3. Stable median pacing interval after excluding obvious multiples.
4. Configured `TargetHz` as a logged fallback.

The budget is always recomputed:

```text
budget_ms = 1000 / measured_refresh_hz
```

A persistent refresh change invalidates decision windows, calibration, landing
residuals, and failure memories. A transient noisy reading does not.

### 4.3 Invalidation policy

On a signature change:

1. Freeze climbs immediately.
2. Clear the live measurement windows.
3. Mark the session profile and learnt residuals incompatible.
4. Complete or safely abort any transition already in progress.
5. Enter cautious adjacent-probe mode or request a new calibration sweep.
6. Log the old and new signature field-by-field.

Persisted profiles may be loaded only on an exact signature match and must start
with higher uncertainty than a same-session calibration. They are priors, never
silent facts.

Not every vendor control is necessarily observable at this boundary. Pimax Play
could change an internal processing path without changing the recommended or
submitted dimensions or an exposed runtime identifier. Therefore:

- perform or refresh the session calibration after every game start, even on an
  exact signature match;
- treat a persisted exact-match profile as a probationary prior until current-
  session visits or landings agree;
- freeze climbs and invalidate that prior when prediction residuals, delivery
  health, or stable geometry change beyond configured change-detector limits.

No controller can pre-identify a hidden external setting that leaves every
observable unchanged. It can remain configuration-agnostic by refusing to grant
old data permanent authority and by detecting the setting's measured effects.

---

## 5. Telemetry design

### 5.1 Application GPU tail

Extend `GovernorSample` without changing the existing measurement contract:

```cpp
struct GovernorSample
{
    double nowSeconds = 0.0;
    double frameTimeMs = 0.0;

    std::uint64_t gpuTimeUs = 0;
    std::uint64_t gpuFrameIndex = 0;

    DeliverySample delivery{};
    EnvironmentSignature environment{};
};
```

Maintain two GPU views:

- **fast tail:** about two seconds, used for overload descent;
- **stable tail:** about six seconds, divided into one-second blocks, used for
  climbs and trend estimation.

Within each block, calculate nearest-rank P50, P90, and P95 from unique GPU
measurements. Suggested derived values are:

```text
observed_gpu_p95 = max(fast_window_p95, median(recent_block_p95s))
gpu_drift        = slope(recent_block_p50s or p95s)
gpu_variability  = robust spread of recent_block_p95s
```

The exact aggregation should be selected by held-out replay and shadow data.
The invariant is more important than the first formula: a climb must not be
justified by pooling away a recent expensive block.

Timed-sample coverage is part of validity. A stale or sparse GPU stream moves
the controller to degraded mode; it does not turn zero into headroom.

### 5.2 Delivery telemetry hierarchy

Introduce a provider interface:

```cpp
struct DeliverySample
{
    std::uint64_t frameIndex = 0;
    std::uint32_t presents = 0;
    std::uint32_t dropped = 0;
    std::uint32_t misPresented = 0;
    std::uint32_t reprojectionFlags = 0;

    bool fresh = false;
    bool deliveryValid = false;
    bool lateApplicationInterval = false;
};

class IDeliveryTelemetry
{
public:
    virtual ~IDeliveryTelemetry() = default;
    virtual DeliverySample Read() = 0;
    virtual std::string_view ProviderName() const = 0;
};
```

Provider order:

1. `IVRCompositor::GetFrameTiming`, after capability validation.
2. A patched OpenComposite/vendor telemetry path, if it exposes stronger
   runtime facts.
3. Application interval fallback, clearly marked `deliveryValid=false`.

The OpenVR provider must:

- set `m_nSize` correctly;
- deduplicate `m_nFrameIndex`;
- reject frozen/all-zero structures;
- distinguish the asynchronous-reprojection-enabled flag from an actual CPU/
  GPU reprojection reason;
- demonstrate expected counter changes under a controlled overload test.

The application interval fallback can still protect the user. It just cannot
support claims about physical scanout.

### 5.3 Delivery blocks

Aggregate samples into one-second blocks:

```cpp
struct DeliveryBlock
{
    double startedAt = 0.0;
    std::uint32_t expectedIntervals = 0;
    std::uint32_t freshSamples = 0;
    std::uint32_t deliveryEvents = 0;
    std::uint32_t repeatedScans = 0;
    std::uint32_t lateApplicationIntervals = 0;
    std::uint32_t longestBadRun = 0;
    bool deliveryValid = false;
};
```

Create at most one `deliveryEvent` for each fresh compositor frame even when
several fields describe the same failure. Preserve repeated-scan/drop magnitude
separately for severity. Otherwise a single bad frame could be counted two or
three times merely because `presents`, `dropped`, and reprojection flags all
reacted to it. The provider must normalise repeated-scan magnitude from the
fields it has validated, rather than summing overlapping counters.

Publish both `deliveryEvents / freshSamples` (affected application-frame rate)
and `repeatedScans / expectedIntervals` (display-interval harm). Use guarded
denominators and minimum-coverage gates. These rates answer different questions
and should remain separate in logs and thresholds.

The fallback threshold must also be period-relative rather than the current
hard-coded `14.5 ms`. Derive it from the stable measured display/application
period plus a separately validated tolerance, and report intervals near two or
more periods as a distinct severity. This keeps the fallback meaningful at 72,
90, or another refresh rate without pretending it is scanout telemetry.

Over the recent block window, publish:

- event rate;
- late-interval rate;
- worst block rate;
- longest consecutive run;
- seconds clean;
- valid block count;
- provider name and validation state.

Emergency and climb gates should use multiple conditions rather than one noisy
percentage. For example, a consecutive run can trigger immediate recovery while
a low sustained rate can be judged over a longer window.

---

## 6. Session calibration profile

### 6.1 What is stored

The cost model is a direct seven-point profile:

```cpp
struct PresetProfilePoint
{
    Preset preset = Preset::UltraPerformance;

    // Equal-weight median of per-visit GPU P95 values.
    double p95Ms = 0.0;

    // Between-visit spread, within-visit drift, and a measurement floor.
    double uncertaintyMs = 0.0;

    std::size_t visits = 0;
    std::size_t uniqueGpuSamples = 0;
    double minimumCoverage = 0.0;

    bool valid = false;
};

struct CalibrationProfile
{
    EnvironmentSignature signature{};
    std::array<PresetProfilePoint, kPresets.size()> points{};

    bool complete = false;
    bool monotonicWithinUncertainty = false;
    double worstVisitSpreadMs = 0.0;
};
```

There are no shipped absolute milliseconds and no shipped step ratios in the
decision path.

### 6.2 Collect per visit, not per contiguous preset run

Every visit needs an explicit `(sweep, index)` identity from `CyclerCore`.
This prevents the serpentine endpoint bug recorded in Measurement Method Rule
10.

Extend `TransitionRecord` with GPU statistics collected after settle:

```cpp
struct GpuVisitStats
{
    std::size_t uniqueSamples = 0;
    double coverage = 0.0;
    double p50Ms = 0.0;
    double p90Ms = 0.0;
    double p95Ms = 0.0;
    double firstHalfP95Ms = 0.0;
    double secondHalfP95Ms = 0.0;
};

struct TransitionRecord
{
    // Existing fields...
    GpuVisitStats steadyGpu;
    DeliveryBlockSummary steadyDelivery;
};
```

The cleanest implementation is to replace `CyclerCore::Tick(now, frameMs)` with
a `CyclerSample` carrying frame time, GPU identity/value, and delivery sample.
`CyclerCore` remains a pure state machine because all hardware access still
happens in the caller.

### 6.3 Profile estimator

For each preset:

1. Calculate P95 independently for every valid visit.
2. Give visits equal weight; do not pool all frames and reward longer visits.
3. Use the median visit P95 as the profile centre.
4. Estimate uncertainty from the maximum of:
   - a minimum measurement floor;
   - robust spread of visit P95 values;
   - representative first-half/second-half drift;
   - coverage penalties.
5. Preserve the raw visit list in diagnostics.

Illustrative estimator:

```cpp
point.p95Ms = Median(visitP95s);
point.uncertaintyMs = std::max({
    config.profileUncertaintyFloorMs,
    1.4826 * MedianAbsoluteDeviation(visitP95s),
    Median(withinVisitHalfP95Differences),
    CoveragePenalty(visits)
});
```

This is a starting estimator, not a pre-approved formula. With only two visits,
MAD is weak; default four-sweep captures provide a more defensible profile.
Using the full first-half/second-half difference is deliberately conservative;
halving it would underprice observed within-visit movement without evidence.

### 6.4 Calibration acceptance

Reject or downgrade the profile when:

- any required preset lacks enough valid visits or unique samples;
- capture or GPU timing coverage is inadequate;
- visit spread is comparable to or larger than important rung differences;
- a cost inversion is larger than the involved uncertainty intervals;
- refresh, texture geometry, CS build/API, or upscale method changed mid-sweep;
- delivery health changed so sharply that the sweep cannot represent one
  operating regime.

A moving sweep may still provide a weak relative prior, but only a stable,
balanced sweep should unlock multi-rung climbs. This follows Measurement Method
Rule 7: movement can preserve a ranking while destroying absolute levels.

---

## 7. Landing predictor

### 7.1 Prediction experts

For current preset `c`, candidate `p`, live observed GPU P95 `g`, and calibrated
profile `q`, compute:

```text
additive(p)       = g + q[p] - q[c]
multiplicative(p) = g * q[p] / q[c]
```

Optionally retain a recent direct-transition expert:

```text
local_step(p) = g * recent_ratio[c -> p]
```

The direct expert is useful information from D-18, but it is no longer the only
truth and it carries its own age, observation count, and residual history.

An expert is excluded when its inputs are invalid, stale, or from a different
environment signature.

### 7.2 Learn held-out landing residuals

Every completed transition produces:

```cpp
struct LandingObservation
{
    EnvironmentSignature signature{};
    Preset from{};
    Preset to{};
    int rungDistance = 0;
    bool climb = false;

    double predictedAdditiveMs = 0.0;
    double predictedMultiplicativeMs = 0.0;
    std::optional<double> predictedLocalMs;
    double realisedSettledP95Ms = 0.0;

    double sceneDriftDuringAssessmentMs = 0.0;
    double observedAt = 0.0;
};
```

Residuals are classified by direction and rung distance and decay with age.
Use a prequential discipline: freeze and log the prediction before applying the
change, score it against the later landing, and only then allow that observation
to update the residual model. Never recompute the historical prediction using
the landing it is supposed to predict.

Do not learn from:

- a scene cut or load during assessment;
- invalid GPU coverage;
- a configuration signature change;
- an apply/readback failure;
- an assessment contaminated by another preset change.

The useful error is one-sided underprediction, because overprediction costs
quality while underprediction can cause an unsafe climb.

### 7.3 Decision estimate

An illustrative result type:

```cpp
struct LandingPrediction
{
    double centreMs = 0.0;
    double decisionUpperMs = 0.0;
    double profileUncertaintyMs = 0.0;
    double residualAllowanceMs = 0.0;
    double driftAllowanceMs = 0.0;

    std::size_t relevantResiduals = 0;
    bool calibrated = false;
    bool multiRungTrusted = false;
};
```

Initial combination:

```text
centre = weighted median of valid experts

decision_upper = max(valid expert predictions)
               + propagated profile uncertainty
               + one-sided residual allowance
               + positive live drift allowance
```

Propagate profile uncertainty conservatively. For the additive expert, a simple
initial rule is `u[p] + u[c]`; do not combine them in quadrature as if the two
profile points were independent. The multiplicative expert should likewise use
a conservative relative-error propagation with guards for small denominators.

Again, `decision_upper` is an empirically conservative controller value, not a
formal confidence bound.

The residual allowance is:

- a deliberately large bootstrap floor with few observations;
- later, a weighted high quantile of positive held-out residuals for the same
  direction/distance;
- never reduced below a measurement floor merely because the last transition
  happened to land well.

Multi-rung trust requires all of:

- a strong same-signature calibration;
- enough held-out residuals at the relevant distance or a conservative
  propagation rule validated on held-out sessions;
- stable live GPU blocks;
- clean delivery history;
- no applicable failure memory.

Otherwise, a climb is one adjacent rung.

---

## 8. Controller state machine

Four explicit states are sufficient. `CandidateClimb` does not need to be a
state; it is a decision calculated while observing.

```cpp
enum class GovernorState
{
    DegradedTelemetry,
    Observing,
    AssessingChange,
    RecoveryLockout,
};
```

### 8.1 `DegradedTelemetry`

Entered when timing coverage is poor, the preset readback is unknown, the
environment is changing, or no valid decision signal exists.

Behaviour:

- never climb;
- hold if there is no evidence of danger;
- descend conservatively if the fallback interval signal is clearly bad;
- periodically explain which telemetry is missing;
- return to `Observing` only after windows refill under one preset and one
  signature.

### 8.2 `Observing`

Normal steady state. Evaluate in this order.

#### A. Emergency recovery

Trigger on one or more of:

- fast GPU P95 clearly over the hard budget;
- a severe consecutive delivery-event run;
- sustained validated delivery rate above the configured limit;
- sustained fallback late-interval overload when delivery telemetry is not
  valid.

GPU overload may justify a multi-rung descent because the application GPU model
directly measures the controlled workload. Delivery-only overload should
normally descend one rung and assess causal response, unless severity demands
an immediate lowest-quality emergency heuristic.

Choose the highest-quality lower rung whose conservative landing prediction is
below a recovery target. If the direct profile contains a credible local cost
inversion, use measured cost rather than assuming that lower image quality must
mean lower total GPU cost. If no trustworthy cross-rung evidence exists, step
down adjacently and assess; a severe emergency may still choose the lowest-
quality rung as an explicitly logged heuristic, not as a guaranteed cheapest
point.

#### B. Hold

Hold when any of these is true:

- current measurements are safe but still filling;
- minimum stable dwell has not elapsed;
- GPU load is trending upward;
- delivery has not been clean long enough;
- the candidate is blocked by recent failure memory;
- the candidate prediction is too uncertain;
- a climb's quality gain does not justify another expensive transition yet.

#### C. Informed climb

Consider candidates from highest quality downward. Select the highest candidate
that satisfies:

```text
decision_upper(candidate) <= landing_target
```

and also:

- stable GPU history;
- adequate timed coverage;
- clean delivery history;
- sufficient dwell;
- compatible calibration signature;
- jump distance allowed by predictor confidence;
- no applicable failure memory.

The change is provisional and enters `AssessingChange`.

#### D. Explicit probe

If no informed climb is possible solely because the adjacent rung is unmeasured,
allow one bounded probe when:

- current delivery has been clean for the longer probe interval;
- GPU headroom exceeds an additional probe reserve;
- load is flat or improving;
- no recent probe failed;
- rollback is armed;
- only one unknown rung is crossed.

The log must use the word `probe` and list which evidence is missing.

### 8.3 `AssessingChange`

Every change, including a descent, is assessed against an immutable record of
the pre-change state.

Use two horizons:

1. **Settle assessment:** starts only after apply/readback and the transition
   disturbance have ended; validates realised GPU landing.
2. **Delivery assessment:** a longer block window validates delivery health and
   catches costs outside the application GPU bracket.

Rollback a climb when:

- GPU tail crosses the hard ceiling;
- validated delivery becomes unsafe;
- the fallback interval signal becomes severely unsafe;
- realised landing materially exceeds its decision estimate;
- a scene change makes the assessment uninterpretable—then recover
  conservatively rather than learning a false residual.

Accept a climb only when both horizons pass. Record the landing observation
whether the prediction was good or bad, provided the assessment was valid.

A recovery change that restores safety enters `RecoveryLockout`. One that does
not restore safety may try another candidate supported by direct session
evidence. Report `ungovernable` only when no remaining actuator choice has
credible evidence of restoring safety (or a bounded emergency attempt has
exhausted them); do not oscillate through presets during the unsafe episode.

### 8.4 `RecoveryLockout`

This prevents an immediate re-climb after a recovery descent.

Exit only after:

- a minimum clean time;
- stable GPU blocks;
- no applicable scene/load transition;
- enough improvement to clear failure-memory requirements.

---

## 9. Failure memory

Generalise D-20 from one headroom number to the evidence that matters:

```cpp
struct FailedClimb
{
    EnvironmentSignature signature{};
    Preset from{};
    Preset target{};

    double attemptedAt = 0.0;
    double preGpuP95Ms = 0.0;
    double preGpuHeadroomMs = 0.0;
    double predictedUpperMs = 0.0;
    double realisedGpuP95Ms = 0.0;

    double preDeliveryRate = 0.0;
    double realisedDeliveryRate = 0.0;
    bool deliveryValid = false;

    std::uint64_t sceneEpoch = 0;
};
```

A retry is allowed when at least one relevant fact improved materially:

- more GPU headroom than the failed attempt plus retry margin;
- better delivery health and clean duration;
- a new scene epoch;
- failure memory decayed past its lifetime;
- a new same-signature calibration or enough new landing evidence changed the
  prediction and its uncertainty.

Time alone should weaken memory, not automatically declare the same conditions
safe.

---

## 10. Decision pseudocode

The core remains small and auditable:

```cpp
GovernorDecision GovernorCore::Evaluate(const GovernorSnapshot& s)
{
    if (EnvironmentChanged(s.environment)) {
        InvalidateSessionKnowledge(s.environment);
        return Hold("environment changed; calibration invalidated");
    }

    if (!s.telemetry.MinimumValid()) {
        _state = GovernorState::DegradedTelemetry;
        return EvaluateDegraded(s);
    }

    if (_state == GovernorState::AssessingChange) {
        return AssessAppliedChange(s);
    }

    if (SevereDeliveryFailure(s) || ClearGpuOverload(s)) {
        const Preset target = SelectRecoveryPreset(s);
        BeginAssessment(s, target, GovernorAction::Descend);
        return Descend(target, ExplainRecovery(s, target));
    }

    if (_state == GovernorState::RecoveryLockout && !RecoveryComplete(s)) {
        return Hold(ExplainRecoveryLockout(s));
    }

    if (!EligibleToClimb(s)) {
        return Hold(ExplainFirstFailedClimbGate(s));
    }

    for (Preset candidate : QualityOrderHighestFirst()) {
        if (QualityRank(candidate) <= QualityRank(s.current)) {
            continue;
        }

        const auto prediction = _predictor.Predict(s, candidate);
        if (!JumpAllowed(s.current, candidate, prediction)) {
            continue;
        }
        if (FailureApplies(s, candidate)) {
            continue;
        }
        if (prediction.decisionUpperMs <= LandingTargetMs(s)) {
            BeginAssessment(s, candidate, GovernorAction::Climb);
            return Climb(candidate, ExplainPrediction(s, prediction));
        }
    }

    if (CanProbeAdjacentUnknownRung(s)) {
        const Preset target = NextUp(s.current).value();
        BeginAssessment(s, target, GovernorAction::Climb);
        return Probe(target, ExplainProbe(s, target));
    }

    return Hold("no higher preset is safe with current evidence");
}
```

The implementation should return structured reason codes alongside human text.
Tests and replay should assert the reason code; logs should render the complete
numeric explanation.

---

## 11. Integration into the current repository

### 11.1 Proposed core files

```text
src/core/EnvironmentSignature.h
src/core/DeliveryStats.h/.cpp
src/core/CalibrationProfile.h/.cpp
src/core/LandingPredictor.h/.cpp
src/core/GovernorCore.h/.cpp       revised state machine
src/core/TraceReplay.h/.cpp        new telemetry and profile inputs
```

Plugin boundary:

```text
src/plugin/CompositorTelemetry.h/.cpp
src/plugin/Reporter.h/.cpp         extended schemas and profile artifact
src/plugin/main.cpp                acquisition and orchestration only
```

Keep hardware/API calls out of `GovernorCore`. It should consume snapshots and
return decisions exactly as it does today.

### 11.2 Construction and profile injection

The current production governor is constructed before the sweep and the sweep's
GPU dwell data is not passed to it. Add an explicit injection boundary:

```cpp
class GovernorCore
{
public:
    explicit GovernorCore(GovernorConfig config);

    void SetEnvironment(EnvironmentSignature signature);
    void SetCalibrationProfile(CalibrationProfile profile);
    void InvalidateCalibration(std::string_view reason);

    std::optional<GovernorDecision> Push(
        const GovernorSample& sample,
        Preset current);
};
```

At sweep completion:

```cpp
auto result = CalibrationProfileBuilder::Build(
    g_cycler->Records(),
    g_environmentTracker.StableSignature());

g_reporter->WriteCalibrationProfile(result);

if (result.usableForAdjacentPrediction) {
    g_governor->SetCalibrationProfile(result.profile);
} else {
    g_governor->InvalidateCalibration(result.reason);
}
```

`Reporter::Finish` should not be the owner of this calculation. Reporting and
control must consume the same already-validated profile object, or they will
eventually disagree about deduplication, segmentation, or currency.

### 11.3 Timeline schema

Add machine-readable columns rather than relying on reason-string parsing:

```text
state
reason_code
environment_hash
delivery_provider
delivery_valid
delivery_event_rate
repeated_scan_rate
late_interval_rate
delivery_clean_s
delivery_worst_block_rate
delivery_longest_bad_run
gpu_fast_p95_ms
gpu_stable_p95_ms
gpu_variability_ms
gpu_drift_ms_s
prediction_additive_ms
prediction_multiplicative_ms
prediction_local_ms
prediction_upper_ms
prediction_residual_allowance_ms
prediction_profile_uncertainty_ms
prediction_observations
change_assessment_id
probe
```

Write a separate profile artifact containing every visit, centre, uncertainty,
acceptance warning, and signature. Do not make `_summary.txt` the only copy.

### 11.4 Transition hold and settle

`transitionHoldFrames = 8` was measured at one configuration. A fixed frame count
does not automatically generalise to a changed refresh rate, runtime upscale
mode, or render resolution.

Retain a hard maximum, but prefer an observed completion condition:

- capture of a valid pre-change frame complete;
- apply/readback complete;
- submitted eye geometry stable;
- relatch/transition block reason cleared;
- application GPU returned from the transition spike;
- minimum hold elapsed, maximum hold not exceeded.

This is actuator work rather than decision-model work, but without it a more
agnostic decision controller could still expose configuration-specific relatch
artefacts.

---

## 12. Configuration parameters

Initial values are experiment starting points, not accepted tuning decisions.
All time constants should be expressed in seconds unless the mechanism is truly
frame-count based.

```cpp
struct GovernorConfig
{
    // Derived from measured refresh; configured value is fallback only.
    double fallbackTargetHz = 72.0;

    double evalIntervalSeconds = 0.5;
    double gpuFastWindowSeconds = 2.0;
    double gpuStableWindowSeconds = 6.0;
    double telemetryBlockSeconds = 1.0;
    double deliveryWindowSeconds = 12.0;

    double minimumStableDwellSeconds = 6.0;
    double climbCleanSeconds = 12.0;
    double probeCleanSeconds = 20.0;

    double settleAssessmentSeconds = 3.0;
    double deliveryAssessmentSeconds = 12.0;
    double recoveryCleanSeconds = 12.0;

    // Base reserve plus empirical uncertainty; no percentage-of-budget margin.
    double baseLandingReserveMs = 0.5;
    double profileUncertaintyFloorMs = 0.20;
    double bootstrapResidualAllowanceMs = 1.0;

    // Delivery thresholds require live validation before becoming decisions.
    double maxDeliveryEventRate = 0.01;
    double maxRepeatedScanRate = 0.01;
    double maxFallbackLateIntervalRate = 0.02;
    std::uint32_t emergencyConsecutiveEvents = 2;

    int maxTrustedClimbRungs = 1; // raised only after evidence enables it
    int maxDescentRungs = 3;

    double failureForgetSeconds = 120.0;
    double retryHeadroomImprovementMs = 0.5;
};
```

Important points:

- `baseLandingReserveMs` is not the whole safety margin; profile, residual, and
  drift allowances are added separately.
- The bootstrap residual allowance must be validated and should initially make
  unknown multi-rung climbs impossible.
- Delivery thresholds apply to validated delivery events. The fallback late-
  interval threshold is a separate setting because the signals have different
  meanings.
- Start with one-rung climbs. Increasing `maxTrustedClimbRungs` is a later
  evidence-backed decision, not part of the first implementation.

---

## 13. Why this is agnostic to user configuration

The controller does not need to know why the workload changed. It measures the
budget, the environment signature, the session ladder, and the realised outcome.

| User/system change | What actually changes | Controller response |
|---|---|---|
| Pimax ImageQuality/render quality | Recommended/submitted render dimensions, runtime processing, or GPU cost can change | Invalidate on an observable signature change; otherwise the mandatory session refresh, prediction-residual detector, and delivery assessment prevent an old prior from retaining authority |
| Native 72 Hz to 72 Hz Upscale (Lab) | Refresh may remain 72 Hz, while input/render dimensions or a hidden runtime processing path changes | Keep the measured time budget if frequency remains 72 Hz; refresh the session profile regardless; invalidate on exposed signature changes, while delivery assessment catches runtime-side cost outside the app GPU timer |
| 72 Hz to 90 Hz | Display period changes from about 13.889 ms to 11.111 ms | Refresh is measured; budget and all windows/profile knowledge are reset |
| Different HMD or optical engine | Recommended size, submitted geometry, refresh, and runtime identity change | No absolute cost is reused across the changed signature |
| Driver/DLSS update | Preset cost shape or inference overhead may change without obeying `scale²` | Driver/binary identity invalidates compatible knowledge when exposed; the mandatory session profile and residual detector still cover version changes the plugin cannot identify |
| MGO/Community Shaders update | Scene workload, preset implementation, scales, transition API may change | CS/API/ladder hash invalidates incompatible knowledge; public quality order still requires verification |
| Weather, interior/exterior, combat | Live scene level and possibly fixed/scaled mix change | Live anchoring, drift detection, two prediction experts, and scene epochs adapt without named scene classes |
| Thermal/background load | Overall GPU level and variability change | Live tail anchors every prediction; increased residuals/variability reduce climb confidence |

Official Pimax documentation describes Render Quality as a performance/visual
control and Upscale Mode as processing a lower-resolution image into a higher-
definition output. That is exactly why neither a fixed cost table from another
configuration nor a universal `scale²` coefficient is acceptable.

The design is agnostic within an important boundary: it assumes Community
Shaders' public presets retain a verified image-quality order. If a future build
changes the meaning/order of the enum, the ladder hash/build gate must reject it.
No controller can maximise an order it does not know.

---

## 14. Why this is better than D-18

D-18 has useful instincts—same-currency P95, live adaptation, conservative
uncertainty—but makes six independent transition ratios the full model.

Problems addressed here:

- **Self-confirming seeds:** removed. Unknown rungs are explicit probes.
- **Each rung must be visited:** still physically true, but a session profile
  informs all rungs while uncertainty prevents pretending this is certainty.
- **Scene drift contaminates transitions:** landing residuals are rejected on
  detected scene changes and are not blindly assigned to a rung.
- **Multi-rung product error:** direct profile predictions do not multiply six
  independently noisy numbers.
- **No model health:** profile spread and one-sided residual allowance publish
  how uncertain the prediction is.
- **GPU-only safety:** delivery assessment can reject a climb that lands inside
  the GPU budget but still harms frame delivery.

The good part of D-18 survives as an optional recent local-transition expert and
as failure/outcome evidence. It no longer has unilateral authority.

---

## 15. Why this is better than D-24

D-24's global fit correctly identified that pairwise slope estimation was
ill-conditioned, but it replaced that estimator with an assumption the captures
did not validate.

Problems addressed here:

- **No forced linear `scale²` form.** The seven points are cheap enough to
  measure directly.
- **P95 is used throughout.** No mean-to-P95 currency change.
- **No one-time frozen shape.** Live held-out transition residuals update the
  uncertainty and expert performance.
- **Additive versus multiplicative remains open.** Both D-14 forms are retained.
- **No in-sample stability claim.** Predictor quality is judged on subsequent
  transitions and held-out sessions.
- **Configuration changes are explicit.** A changed resolution or runtime mode
  invalidates the absolute profile instead of relying on reanchoring to repair
  an incompatible curve.
- **The unexplained interval signal matters.** A better GPU fit is not mistaken
  for an end-to-end frame-delivery guarantee.

The design is more elaborate than one equation, but its complexity corresponds
to real, independently observed failure modes. Each component remains small,
testable, and separately falsifiable.

---

## 16. Why not use a Kalman filter, reinforcement learning, or a bandit library

A latent state model such as `t = A_scene + B_scene*f` is attractive, but from
one currently selected preset the two scene coefficients are not observable.
Transitions add information while also adding scene drift and a relatch
disturbance. A Kalman filter would move the uncertainty into process-noise
tuning without resolving the missing observation.

Reinforcement learning or a generic contextual bandit would also be a poor fit:

- seven actions do not require a black-box optimiser;
- unsafe exploration occurs in a headset and has visible cost;
- the available sessions are far too few for broad generalisation claims;
- decisions must be reconstructible from logs;
- deterministic safety and rollback rules are easier to test.

This proposal is a safe discrete controller with bandit-like explicit
exploration, not an opaque learning system.

---

## 17. Test plan

### 17.1 Unit tests

Add deterministic tests for:

1. GPU identity deduplication in every collector.
2. Two consecutive endpoint visits remaining distinct by `(sweep,index)`.
3. Equal visit weighting rather than pooled-frame weighting.
4. Per-frame delivery-event deduplication when multiple compositor fields react.
5. A significant non-monotonic profile being preserved and rejected/downgraded,
   not silently corrected.
6. Signature changes freezing climbs and invalidating profile/residual state.
7. Exact-match persisted profiles remaining probationary until current-session
   evidence agrees.
8. Same effective per-eye geometry for per-eye, double-wide, and inverted
   texture bounds.
9. Refresh changes recomputing the budget.
10. No profile causing adjacent probes only.
11. Valid profile plus insufficient residuals still refusing multi-rung climbs.
12. Validated delivery failure causing descent despite spare application GPU.
13. Fallback late intervals producing a different reason code from validated
    delivery events.
14. Climb rollback after unsafe delivery with a safe GPU landing.
15. Scene cut during assessment preventing residual learning.
16. Positive underprediction widening future allowance.
17. Overprediction not shrinking safety allowance prematurely.
18. Failure memory unlocking only after evidence improves or the scene changes.
19. No credible safe actuator choice entering `ungovernable` without oscillation.
20. Holds remaining visible in timeline/log output.

### 17.2 Telemetry validation tests

Before allowing delivery telemetry to actuate:

1. Verify `GetFrameTiming` returns a changing frame index.
2. Verify frame counters are not frozen/all-zero.
3. Introduce a controlled application overload and confirm the documented
   present/drop/reprojection fields react.
4. Compare them with application intervals, GPU time, and visible behaviour.
5. Repeat on both normal 72 Hz and 72 Hz Upscale (Lab).
6. Record the provider/build/runtime version with the result.

If this fails, keep the fallback signal and label it accurately. Do not fabricate
delivery validity to unblock the design.

### 17.3 Replay tests

Replay remains useful for rejecting obvious failures, but it is counterfactual
and cannot synthesize genuine runtime delivery at a preset not recorded.

Use replay for:

- state-machine determinism;
- rate limits and oscillation;
- profile/residual bookkeeping;
- comparing logged predictions with realised recorded transitions;
- fault injection: stale GPU frames, missing telemetry, environment changes,
  apply deferrals, scene cuts.

Do not use replay to certify delivery safety at counterfactual presets.

### 17.4 Live rollout

Stage the work:

1. **Telemetry only:** no decision changes; validate compositor fields and
   environment signature.
2. **Shadow controller:** log proposed decisions, profile, uncertainty, and
   assessments.
3. **Live recovery only:** allow descents, no climbs.
4. **Adjacent informed climbs:** no multi-rung climbs; rollback active.
5. **Explicit probes:** only after probe gates are validated.
6. **Multi-rung climbs:** separate later decision requiring held-out evidence.

The first live build must not also be the first exercise of new telemetry,
prediction, and multi-rung behaviour.

---

## 18. Acceptance criteria

Pre-register exact capture parsing, block definition, settle/assessment windows,
and held-out sessions before scoring.

Compare the new controller with the currently shipped controller on:

### Primary safety

- validated delivery-event rate, if available;
- validated repeated-scan rate, if available;
- otherwise separately reported late-application-interval rate;
- longest consecutive bad run;
- GPU-over-budget rate;
- false-safe climb count: predicted safe, realised unsafe;
- rollback duration and severity.

### Quality

- time-weighted pixel fraction/preset quality;
- time below the highest retrospectively safe rung;
- false refusal count: a held climb that subsequent direct evidence shows was
  safe.

### Stability

- changes per minute;
- reversals;
- probe failure rate;
- time spent in degraded and ungovernable states.

### Model health

- median, P90, P95, and worst underprediction;
- results by rung distance and operating region;
- additive, multiplicative, and local expert errors separately;
- profile visit spread and configuration signature.

A reasonable promotion bar is not merely lower MAE. Require no degradation in
worst/P95 underprediction and a material reduction in false-safe climbs or
delivery harm. Multi-rung climbs require their own stronger bar.

---

## 19. Expected objections and answers

### "This is more complex than six ratios."

It represents two measured feedback channels, configuration identity,
prediction uncertainty, and transition assessment because all four have already
produced real failures. The complexity is divided into pure modules with narrow
interfaces. Removing a module removes the corresponding safety claim.

### "Delivery health is too slow for control."

That is why the GPU loop remains the fast recovery signal. Delivery health is a
climb gate and outcome validator, plus an emergency signal for severe failures.
The loops have different jobs.

### "The session profile overfits one scene."

It is not trusted as an absolute scene cost. Live GPU P95 reanchors it; additive
and multiplicative experts expose the reanchoring ambiguity; uncertainty and
held-out residuals price the remaining error. A weak/moving sweep only enables
adjacent predictions.

### "A direct table cannot generalise to a different HMD resolution."

That is intentional. An absolute table should not generalise across a changed
resolution. The signature invalidates it, and a new session profile measures the
actual configuration. Cautious probes operate when calibration is unavailable.

### "The controller may leave quality unused because it is conservative."

Initially yes. Reliability requires under-reaching before evidence exists. The
controller earns larger jumps and smaller residual allowances from successful
held-out transitions. That is controlled relaxation rather than a foreign seed
silently granting confidence.

---

## 20. Recommended decisions for Claude to argue and record

Before code, add numbered proposals to `GOVERNOR_DESIGN.md` for:

1. The lexicographic objective and dual safety channels.
2. Capability-validated delivery terminology and provider fallback.
3. Environment signature and invalidation rules.
4. Direct seven-point session P95 profile with per-visit uncertainty.
5. Additive/multiplicative/local prediction experts.
6. One-sided held-out landing-error allowance.
7. Explicit bounded exploration of unknown adjacent rungs.
8. Provisional changes with settle and delivery assessment.
9. Generalised failed-climb memory.
10. Degraded and ungovernable behaviour.
11. Multi-rung climbs disabled until a separate refutation test passes.
12. Configuration-independent transition completion rather than a permanently
    fixed hold-frame count.

Each decision should state its evidence, fallback, diagnostics, and refutation
condition before implementation.

---

## 21. Primary references

- Project evidence and rejected alternative:
  [`REVIEW_2026-08-12_D24.md`](REVIEW_2026-08-12_D24.md),
  [`GOVERNOR_DESIGN.md`](GOVERNOR_DESIGN.md), and
  [`MEASUREMENT_METHOD.md`](MEASUREMENT_METHOD.md).
- Valve OpenVR `Compositor_FrameTiming` documentation:
  <https://github.com/ValveSoftware/openvr/wiki/Compositor_FrameTiming>
- Khronos OpenXR frame synchronisation specification:
  <https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#frame-synchronization>
- NVIDIA Streamline DLSS guide, showing output dimensions and quality mode as
  separate inputs and render dimensions as returned settings:
  <https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md>
- Pimax Play overview, including Render Quality and Upscale Mode:
  <https://pimax.com/blogs/crystal-light-tutorials/pimax-play-tips-and-features-overview>
- Pimax explanation of Upscale Mode:
  <https://pimax.com/blogs/crystal-light-tutorials/set-up-upscale-mode-vr>
- Pimax Crystal Super optical-engine specifications, including native/upscale
  refresh modes:
  <https://pimax.com/blogs/blogs/which-optical-engines-does-the-crystal-super-have>

---

## 22. Final recommendation

Proceed with this architecture, but implement it in the staged order above.

The first engineering priority is not the predictor. It is establishing a
validated delivery signal and an environment signature. Without those, the
governor cannot honestly claim end-to-end 72 Hz safety or configuration
agnosticism.

Once telemetry is trustworthy, implement the direct session profile, prediction
experts, empirical one-sided residual allowance, and explicit probe state. Keep
multi-rung climbs disabled until held-out live evidence shows they improve
quality without worsening the dangerous tail.

The resulting system is not agnostic because it guesses a universal cost law.
It is agnostic because it detects the running environment, measures the current
ladder, distrusts incompatible history, and validates every quality increase
against the outcome the user actually experiences.

---

## Appendix A — Response, and what was taken from this document (2026-08-12)

Recorded here so this proposal is not read later as either accepted whole or
dismissed. The architecture is right about the thing both D-18 and D-24 got
wrong: **§1's "GPU time proposes, a validated delivery signal decides"** is the
correct shape, and its diagnosis — that a more accurate GPU ladder model cannot
repair an omitted part of the pipeline — is E-49 stated precisely.

**What was adopted into D-25 immediately**, because both are correct
independently of any controller design:

- **§4.2**, derive the budget from measured refresh rather than trusting
  `TargetHz`. This is a live latent bug: at 90 Hz every threshold silently means
  the wrong thing.
- **§4.1**, canonical per-eye geometry from submitted bounds, so a headset
  resolution change is detected and session knowledge is reset. This is what
  makes configuration agnosticism real rather than asserted.

**What was deferred, and why.** D-25 takes the crudest possible delivery signal —
the application's own frame interval — instead of this document's validated
compositor channel. The reasoning is not that the channel is wrong; it is that
**the entire design rests on an assumption nobody has tested yet**: that
`Compositor_FrameTiming` is meaningfully populated *through OpenComposite*.
§2.1 and §17.2 are honest about this, but ~1400 lines of architecture then sit on
top of it. If the capability test fails, the provider degrades to the fallback
signal — which is what D-25 uses directly.

**That test should happen before any of this is built.** We already hook this
vtable for D-21, so reading `GetFrameTiming` for one session and checking that the
frame index advances and the counters react under deliberate overload is small and
isolated. Its result decides whether this is a redesign or a much smaller patch.

**Three unresolved points in this document**, noted for whoever picks it up:

1. **The central bet is unstated.** The design assumes delivery health responds to
   the preset — otherwise the ladder is the wrong actuator and the delivery
   channel is an alarm with no remedy. Our data supports it (misses 0.3–1.0% at
   UltraPerformance against 7–11% at Performance), but it should be written down
   as a precondition, with "stop descending and report ungovernable" as the
   response if a capture ever contradicts it.
2. **Calibration deliberately violates the safety signal it introduces.** The
   sweep drives the ladder to NativeAA, measured at **24.22 ms P95 against a
   13.889 ms budget**, twice per session. Delivery becomes a first-class safety
   channel and calibration is a first-class violation of it. Truncating the sweep
   once a rung measures unsafe would resolve both this and the session thrash —
   a precise cost for a rung that will never be selected is not worth buying.
3. **The predictor is built for the channel that is not binding.** Three experts,
   weighted-median combination, residual allowances keyed by direction and
   distance — all layered on GPU P95, which this same document argues does not
   decide safety. §22 already says the predictor is not the first priority; the
   first implementation should carry one expert and a fixed conservative
   allowance, and earn the rest from held-out residuals.

**A caution on §20.** Writing twelve numbered decisions before the telemetry
answer risks repeating D-24's failure in a larger form: a well-argued structure
the measurement later refuses to support. The invariants in §3 are commitments
rather than predictions and can be recorded now. Each decision should be written
as its stage becomes real.

**Where this document remains the reference.** If the capability test passes, the
staged rollout in §17.4, the prequential residual discipline in §7.2, the
per-visit `(sweep, index)` identity in §6.2, and the acceptance criteria in §18
are better than anything currently in `GOVERNOR_DESIGN.md`, and D-25's simpler
controller should be measured against this one rather than replacing it by
default.

---

## Appendix B — The capability test ran, and it failed (2026-08-12, same day)

Appendix A said the whole design rests on an untested assumption and that the
test should happen before anything is built. It was built, run, and the result is
negative and decisive. **E-53** records it.

    calls 18663 | returned true 18663 (100.0%) | frame index advancing YES (100.0% fresh)
    fields ever varied: presents NO | dropped NO | misPresented NO
                      | reprojection NO | clientInterval yes

Against a session containing **5478 late frames (31%)**, 121 of them longer than
two display periods, with individual frames at **35 ms**. A 35 ms frame at 72 Hz
means the previous image was scanned out at least twice — the documented meaning
of `m_nNumDroppedFrames`. It reported zero, every time.

The entry point is genuine: `GetFrameTiming` answers every call, the frame index
is live, and `m_flClientFrameIntervalMs` and `m_flCompositorRenderGpuMs` both
vary, so the runtime fills what it can compute for itself. The four delivery
counters are inert defaults.

**This is structural.** §2.1 of this document already noted that core OpenXR
provides no portable result identifying which application image was scanned out.
OpenComposite translates OpenVR onto OpenXR, so there is no source for those
fields to carry. **No patch to OpenComposite can supply them** without a vendor
extension that does not exist today.

### What this does to the design

- **§5.2's provider hierarchy has no primary provider on this stack.** Provider 1
  answers but tells us nothing; provider 2 cannot exist for the same structural
  reason; only provider 3, the application interval fallback, has data.
- **§5.3, and the delivery gating throughout §8, have no input.** Every threshold
  expressed in validated delivery events — `maxDeliveryEventRate`,
  `maxRepeatedScanRate`, `emergencyConsecutiveEvents` — is unreachable.
- **§8.3's delivery assessment cannot validate a climb**, so the two-horizon
  acceptance reduces to the settle horizon alone.
- **The terminology in §2.1 becomes more important, not less.** With no validated
  channel, everything the controller sees is a late *application* interval. Logs
  and reports must keep saying so.

The rest of the document is unaffected and remains the reference: the
environment signature (§4), per-visit `(sweep, index)` identity (§6.2), the
prequential residual discipline (§7.2), the staged rollout (§17.4), and the
acceptance criteria (§18) are all independent of where the delivery signal comes
from.

### What was actually gained

The negative result is worth more than the code that produced it. It converts
D-25's frame-interval signal from a simplification that had to be defended into
the only option available, and it retires an entire architectural branch for the
cost of one session. That is what the test was for.

**One gap in how the test was built, for whoever repeats it:**
`m_flClientFrameIntervalMs` is the runtime's *own* measurement of the frame
period and would be the ideal input to §4.2's refresh derivation — better than our
own frame timing, which turns out to be quantised to 1/6 ms steps. It was logged
only in the periodic summary, not written per-frame to the CSV. Add it to the
per-frame columns next time.
