# Governor Design

Status: **agreed design, not yet implemented.** Supersedes the threshold-based
policy in the 2026-08-01 revision of this file. See [Decision Log](#decision-log)
for what changed and why.

This document is the contract. The point of writing it before building is that we
stop improvising. Everything the governor does should be traceable to a decision
recorded here.

## How To Change This Document

Anything in [Decisions](#decisions) may be revised, but **the argument comes
first, in writing, before the code**. The procedure:

1. State which decision is being challenged, by its `D-n` identifier.
2. State the evidence that contradicts it — a measurement, a log, a source
   citation. Not an intuition, and not a preference.
3. Record the replacement in the Decision Log with the date and the evidence.
4. Then change the code.

If a measurement surprises us, that is a reason to revise a decision, not a
reason to skip this. Two of this project's worst detours came from acting on a
conclusion that was never written down and therefore never checked.

---

## 1. Objective

Hold a locked 72 FPS at the best image quality the scene allows.

Stated precisely, because "best quality" and "locked framerate" are in tension
and a governor is exactly the thing that trades them:

> **Maximise the time-weighted mean pixel fraction, subject to the frame-miss
> **drop** rate staying below `drop_max` in every rolling `W_judge` window.**

The constraint is primary. A governor that delivers higher average quality while
letting a fight in Whiterun drop frames has failed, regardless of the average.

This objective is what Phase 5 measures the governor against. It is also what
makes "better" a number instead of an opinion.

## 2. Scope

**One lever:** the Community Shaders upscale preset, via
`ICSInterface001::SetUpscalePreset`. Seven discrete rungs, `scale` 0.333 → 1.0,
pixel fraction `scale²` — so **0.111 → 1.000, a 9× range in pixels.**

The CS API also exposes SSGI, screen-space shadows, volumetric lighting and
contact shadows. They stay out of v1. Two levers square the state space and
multiply the oscillation modes.

### The governor must not

- **Duplicate CS's anti-oscillation.** CS has its own rapid-transition guard,
  memory-relief arming, submit-stage cooldowns and stable-frame counting. The
  governor paces *slower* than those guards and never races them. "Two owners,
  one lever" has bitten this project repeatedly.
- **Toggle `renderScaleModeEnabled`.** That forces a render-target relatch.
  Static configuration only.
- **Act while blocked.** Check `IsVRUpscalingProfileApplyAllowed()` first; buffer
  the latest desired target and retry, as the API documentation prescribes.
- **Retry forever on a terminal block.** `kOpenCompositeUpscaling` means OCU DLSS
  has taken the lever for the session. Disable with one clear log line.
- **Degrade when degrading cannot help.** See D-6.

---

## 3. Evidence Base

Everything in Section 4 derives from these. Each carries its provenance so a
future reader can tell measurement from assumption.

| # | Finding | Source |
|---|---|---|
| E-1 | Four presets spanning 6× in pixels all measured 13.7–13.9 ms — indistinguishable, because all four were pinned at the 72 Hz cap. | Cycler session 2026-08-03 14:04, 21/21 transitions clean |
| E-2 | Settle latency after a preset change is **~1.0 s**, consistent across all seven presets (0.94–1.18 s). | Same session |
| E-3 | CS exposes no arbitrary render scales — only the seven discrete quality modes. | `GetQualityModeResolutionScale`, `docs/CS_VR_RENDER_SCALE.md` |
| E-4 | The CS Performance Tuning "GPU" field does **not** measure GPU work: it read 9.72 ms at 0.50×, 0.77× and 0.85×. | Measured 2026-08-02; recorded as a trap in `MEASUREMENT_TRAPS.md` |
| E-5 | The white flash on preset change is an upstream CS bug (uncleared hidden-area mask), fixed 2026-07-25 in `71cc4c76`; the installed build is 2026-07-09. Not a governor constraint. | CS source trace, 2026-08-03 |
| E-6 | API revision 3 is available; block reasons and apply-allowed are readable. | Startup API probe, same session |
| E-7 | **The linear cost model holds.** Riverwood in heavy rain, sweep 3 (uniformly heavy, 4 uncensored points): `t_fixed ≈ 8.31 ms`, `t_scaled ≈ 17.45 ms`, `k ≈ 2.10`. Residuals ≤ 0.48 ms. Solving for budget gives `f ≤ 0.320` → **Performance**, and Performance measured 13.57 ms (under) while Balanced measured 14.83 (over). The model chose correctly on data it had not seen. | Session 2026-08-04 21:28 |
| E-8 | **`drop%` reproduces perception, `miss%` does not.** Capped visits: `miss 0.44–0.50, drop 0.00`. Genuinely heavy visits: `miss 1.00, drop 1.00`. | Same session |
| E-9 | CS reports `kLoadingMenu` for **over 30 s** after a save finishes loading. A 20 s start delay loses the first transition entirely. | Same session |
| E-10 | The frame source captures only about **half** the frames — 9,947 from 205,885 polls over 307 s — and the ones it drops are the perfectly-paced ones, because the dedup skips a frame whose delta equals its predecessor. Samples are therefore biased toward jitter and mean/P95 read pessimistic. | Same session |

### E-1 is the finding that drives the whole design

Those four presets differ by 6× in pixels and produced the same number. **When
the framerate is capped, FPS carries no information about headroom.** It reads 72
whether 40% is spare or 2%.

This is not noise to be filtered out. It is *censoring*: a capped sample tells us
the true cost is **≤ budget**, never what it actually is. Censored samples cannot
be fitted, averaged, or differenced like real ones, and any part of the design
that forgets this will produce confident nonsense.

---

## 4. Decisions

### D-1 — Control on scene cost, not on framerate

Do not map measured FPS (or frametime) onto a preset through thresholds.

Two independent failures:

- **Saturation** (E-1). A capped measurement contains no gradient, so a
  threshold controller can learn that it is failing but never that it is
  over-delivering.
- **Coupled feedback.** The action changes the measurement. Drop to Performance
  because frametime rose; frametime recovers; the recovered value is now in the
  "raise quality" band; quality rises; frametime rises again. That is a limit
  cycle, and it is structural — no threshold tuning removes it.

Instead, estimate a quantity that **does not move when the preset moves**, and
choose the preset from that. See D-2.

### D-2 — The cost model

Model frametime at preset `p` as linear in pixel fraction:

```
t(p) = t_fixed + t_scaled · f(p)          f(p) = scale(p)²
```

- `t_fixed` — CPU work plus resolution-independent GPU work (shadow map
  generation, some post, compositor overhead).
- `t_scaled` — the resolution-dependent part; the only part quality actually
  buys.

Define the scene's **resolution sensitivity**:

```
k = t_scaled / t_fixed
```

`k` is the one scene-dependent number the controller must track. `t_fixed`
follows from any single uncensored observation:

```
t_fixed = t_obs / (1 + k · f_cur)
```

**Why this form.** `t_fixed` and `k` describe the *scene*, not the preset. They
do not jump when quality changes, which is precisely what breaks the limit cycle
in D-1.

Linearity in pixel count is an approximation — it ignores fixed per-pass
overheads and cache behaviour. Phase 2 tests it (§6) rather than assuming it. If
the residual is too large, the fallback is a per-preset measured lookup table,
which needs no functional form at all.

### D-3 — Preset selection

Given a frametime target `T = budget · (1 − margin)`:

```
t_fixed · (1 + k · f) ≤ T
⇒  f ≤ (T / t_fixed − 1) / k
```

Choose the **highest-quality preset whose `f` satisfies this.**

If no preset satisfies it, the scene cannot be governed into budget. Select the
**highest** quality rung, not the lowest, and log it — see D-6.

### D-4 — Two regimes, because the measurement is censored

The controller behaves differently depending on whether the current measurement
carries information. This asymmetry is forced by E-1, not chosen.

**Uncapped** (not censored per D-7b): the frametime is real. The cost model is
observable. Predict and **jump directly to the selected preset** — do not step
one rung at a time. Four sequential one-rung steps at ~1 s settle each (E-2) is
four seconds of stutter to escape a spike.

**Capped** (`miss_rate ≈ 0`): the frametime is a lower bound and the cost model
is unobservable. There is no way to compute how much headroom exists, so the
only way to find out is to **try**. Probe upward one rung, wait, and keep it if
it holds.

Hence: **fall fast, rise slow.** Not a tuning preference — the down path has
information and the up path does not.

### D-5 — Every transition is a measurement

Two observations at different presets, close together in time, determine both
model parameters:

```
t_scaled = (t₁ − t₂) / (f₁ − f₂)
t_fixed  = t₁ − t_scaled · f₁
k        = t_scaled / t_fixed
```

The governor changes presets anyway, so **each change is a free two-point
calibration.** Update `k` by EMA with factor `α_k`, and reject the update when:

- **either observation is censored** (at the cap) — the difference would be
  meaningless;
- `|f₁ − f₂|` is below `df_min` — dividing by a small number amplifies noise;
- the residual against the current model is implausibly large — the scene changed
  between the two samples, so they do not describe the same scene.

This makes the controller self-calibrating, and it is why `k` needs only a rough
offline starting value.

### D-6 — The CPU-bound guard falls out of the model

If `k → 0`, the scene is insensitive to resolution. D-3 then admits every preset,
so the governor selects maximum quality — correctly refusing to strip image
quality for nothing in exactly the scenes (dense towns, script storms) where
resolution is powerless.

If additionally `t_fixed > T`, no preset meets budget. D-3 says select the
highest rung and log `CPU-bound: ungovernable`.

The 2026-08-01 design carried this as a special case ("if a step down does not
improve frametime, step back up"). It is now a consequence of the model rather
than a patch on top of it. **Do not re-add the special case.**

### D-7 — Decide on the tail, never the mean

All decisions use **P95 frametime and drop rate** over a rolling window. A locked
framerate is lost at the tail; a mean hides exactly the failures that matter.

### D-7a — "Over budget" is not "dropped"

A frame 0.01 ms over budget is not a stutter. Under vsync a frame either makes
its interval (~13.9 ms at 72 Hz) or waits for the next one (~27.8 ms), so the
honest detector sits **between** those two populations:

```
drop  ⟺  frametime > budget × 1.5
```

This matters because the naive definition is actively misleading in the regime
we care about. When the compositor holds you at refresh, frametime sits *exactly
on* budget, and symmetric jitter puts about half the samples fractionally above
it. The 2026-08-03 run reported 33–44% "miss" for the four capped presets in a
scene that was subjectively flawless — and the user's independent observation
("stutter always tracked OpenXR Toolkit showing below 72; 72 always felt smooth")
is the ground truth that the drop definition reproduces and the miss definition
does not.

`missRate` is retained, but only as a **margin gauge**: ~0% means real headroom,
~30–50% means sitting on the cap, >80% means genuinely over budget.

### D-7b — Detecting censoring

A sample is censored (D-4) when the compositor is holding us, i.e.

```
censored  ⟺  p95 ≤ budget × (1 + cap_tol)   and   drop_rate ≈ 0
```

Do not test this with `missRate`; per D-7a it is ~50% in exactly this state.

### D-8 — Smoothness comes from the controller, not the actuator

The lever is discrete and cannot be made continuous (E-3). Continuous *quality*
is not available.

What is available, and what "fluent" should mean here: keep the cost estimate and
the derived target continuous internally, and quantise to a rung only when the
target has moved far enough to justify the change. Smooth internal state, rare
discrete output.

Frequent small adjustments are the failure mode, not the goal. E-2 makes this
concrete: at ~1 s settle, changing more than roughly once per 2–3 s means
measuring your own transients. **Success is few correct changes, not many small
ones.**

### D-9 — Backoff instead of a failure memory

When an upward probe fails, double the probe interval up to `T_up_max`. Reset it
on a cell change (the scene genuinely changed) or after `T_reset` of clean
running.

This replaces the 2026-08-01 "remember what failed at rung *n*" rule, which it
subsumes with one mechanism and fewer parameters.

---

## 5. The Algorithm

Evaluated every `T_eval`, on the game thread, driven by the existing frame pump.

```
state: k, t_fixed, preset_cur, t_last_change, probe_interval, f_last, t_last

every T_eval:
    if not ApplyAllowed():            # D-2 of §2
        buffer target; return
    if now - t_last_change < T_cooldown:
        return                        # let the transition settle (E-2)

    p95, drop = window_stats(W_judge)
    censored  = (p95 <= budget * (1 + cap_tol)) and (drop <= drop_floor)   # D-7b

    # ---- update the model (D-5) ----
    if not censored:
        t_fixed = p95 / (1 + k * f(preset_cur))
        if have_previous_uncensored_sample_at_different_preset():
            k_obs = derive_k(f_last, t_last, f(preset_cur), p95)
            if plausible(k_obs):
                k = (1 - alpha_k) * k + alpha_k * clamp(k_obs, 0, k_max)
        remember (f(preset_cur), p95)

    # ---- decide ----
    if drop > drop_max sustained for T_down:            # falling: informed
        target = highest preset with f <= (T/t_fixed - 1)/k     # D-3
        apply(target)                                    # jump, do not step
        probe_interval = T_up_min
        return

    if censored and now - t_last_change > probe_interval:  # rising: blind
        target = next rung up from preset_cur            # one rung only
        apply(target)
        pending_probe = target
        return

    if pending_probe and probe failed (drop > drop_max within T_probe_judge):
        apply(previous rung)
        probe_interval = min(probe_interval * 2, T_up_max)   # D-9
```

**Invariants the implementation must preserve.** These are the parts that are
easy to break silently:

1. `k` is never updated from a censored sample (D-5).
2. Downward moves may jump multiple rungs; upward moves are always one rung
   (D-4).
3. No change is ever made inside `T_cooldown` of the previous one (E-2).
4. `ApplyAllowed()` is checked before every apply, and a refused target is
   buffered rather than retried in a spin.

---

## 6. Parameters

Initial values are starting points. The "Determined by" column says what fixes
each — **none of them are to be tuned by feel in-game.**

| Symbol | Meaning | Initial | Determined by |
|---|---|---|---|
| `budget` | frame budget | 13.89 ms (72 Hz) | headset refresh |
| `margin` | safety margin below budget | 0.10 | Phase 3 sweep |
| `drop_max` | drop rate that triggers a step down | 0.02 | Phase 3 |
| `drop_floor` | drop rate below which running counts as clean | 0.005 | Phase 1 |
| `cap_tol` | P95 within this fraction of budget means capped | 0.05 | Phase 1 |
| `W_judge` | decision window | 2.0 s | Phase 3 |
| `T_eval` | evaluation cadence | 0.5 s | — |
| `T_cooldown` | minimum gap between changes | 3.0 s | E-2 (≥ 2× settle) |
| `T_down` | sustained misses before falling | 1.0 s | Phase 3 |
| `T_up_min` | initial upward probe interval | 20 s | Phase 3 |
| `T_up_max` | maximum probe interval after backoff | 300 s | Phase 3 |
| `T_probe_judge` | window to judge a probe | 3.0 s | E-2 + `W_judge` |
| `T_reset` | clean running before backoff resets | 120 s | Phase 3 |
| `k` | resolution sensitivity, initial | from Phase 1 | Phase 1, then online |
| `k_max` | clamp on `k` | 10.0 | Phase 1 |
| `α_k` | EMA factor for `k` | 0.2 | Phase 3 |
| `df_min` | minimum pixel-fraction gap for a `k` update | 0.05 | Phase 1 |

---

## 7. Test Plan

Each phase produces an artifact and has a stated pass condition. **A phase that
does not pass is not worked around — it sends us back to Section 4.**

### Phase 0 — Instrument ✅ complete

Cycler runs, sweeps all seven presets, writes transitions/frames/apistate/summary.
API probe confirms revision 3 (E-6).

### Phase 1 — Cost-curve capture

**Procedure.** Run the cycler (**4 sweeps**, serpentine — the count must be even,
see the decision log) in three scene classes, standing still with a roughly fixed
view:

- **Light** — small interior, few actors. Already captured 2026-08-03.
- **Medium** — town exterior, moderate actor count.
- **Heavy-CPU** — dense city; Markarth or Riften. Many objects, lights and
  actors. Expect **high `t_fixed`, low `k`** — resolution barely helps. This is
  the scene that exercises D-6, the ungovernable case.
- **Heavy-GPU** — dense forest exterior with grass, canopy and volumetrics.
  Expect **high `k`** — resolution helps a great deal. This is the main control
  path.

Splitting "heavy" this way is deliberate: total load is not the axis that
matters to this controller, `k` is, and the two heavy scenes sit at opposite
ends of it. A governor fitted against only one will be badly calibrated for the
other. The four classes also subsume the simpler Light/Medium/Heavy framing, so
this is four runs rather than two sets of three.

Use a **named hard save per class** so the run is repeatable — Phase 2 predicts
one sweep from another, which only means anything if the scene is genuinely the
same. A save pins time of day, weather *and* actor positions; console commands
do not pin the last of those.

**Produces.** Per scene class: `t_fixed`, `t_scaled`, `k`, and the spread/drift
columns now in `_summary.txt`.

**Passes if:**

- The heavy scene yields **≥ 3 uncensored presets**. Fewer means the scene is not
  heavy enough to fit a curve, and Phase 1 repeats with a heavier one.
- `spread` is below half the gap between adjacent presets — otherwise those
  presets are not distinguishable in that scene and the ranking is noise.
- `|drift|` is small and not same-signed across all presets; a large common drift
  means the session moved under us and the absolute numbers are not comparable.

**This phase also answers the premise.** If the rungs do not separate under load,
there is nothing to govern and the project stops here.

### Phase 1b — Headroom signal probe

E-1 forces the blind upward-probe regime (D-4). That regime disappears entirely
if a *true* GPU-time signal exists, since headroom would be directly observable
even while capped.

**The mechanism is known and proven on this exact stack.** PrimaShock's overlay
already displays it, and the source shows how: `D3D11_QUERY_TIMESTAMP_DISJOINT`
plus a start/end `D3D11_QUERY_TIMESTAMP` pair bracketing the frame
(`d3d11.cpp:777-781`), accumulated into `appGpuTimeUs` (`layer.cpp:2251`).

A GPU timestamp measures GPU **work**, not present-to-present time, so it is
unaffected by vsync and **does not saturate at the cap**. Observed directly by
the user on 2026-08-05: FPS pinned at 72 across three presets while the overhead
figure read 10%, 15% and 30%.

Prefer this over `vr::IVRCompositor::GetFrameTiming`: it does not depend on
OpenComposite implementing anything honestly, and E-4 is precisely the case of a
plausible-looking timing field that measured nothing.

**Procedure.** Create the queries on the game's D3D11 device, bracket the frame,
read back a few frames later to avoid stalling the pipeline, and log the result
alongside the existing frametime across a cycler sweep.

**Passes if** GPU time varies with preset while frametime is pinned at the cap.

**If it passes, D-4 changes substantially** and must go through the Section 0
procedure: the censoring that forces the blind upward probe disappears, rising
becomes model-driven like falling, and `k` can be estimated continuously from
every frame rather than only at transitions. The probe survives only as a
fallback for when the query is unavailable.

### Phase 2 — Model validation, offline

**Procedure.** Fit `t_fixed`/`k` on sweeps 0 and 2; predict sweep 1; compare.

**Passes if** predicted P95 is within `spread` of actual for every preset. If the
linear form fails, fall back to the measured lookup table (D-2).

### Phase 3 — Controller in simulation

**Procedure.** Replay the recorded per-frame traces through `CyclerCore`'s sibling
controller as **unit tests in CI**. Synthesise step changes, spikes and slow
drifts from the captured cost curves. Sweep the parameters of Section 6.

**Passes if,** across all traces: no sustained oscillation; the controller reaches
the correct rung within `T_cooldown + W_judge` of a step change; transition rate
stays under `1/minute` in steady scenes.

**This is where the parameters get chosen** — deterministically, in CI, with no
headset and no subjective judgement. It is the phase that keeps this project out
of the tune-by-feel trap.

### Phase 4 — Shadow mode

**Procedure.** Run the controller live, computing every decision and logging what
it *would* do — but **do not apply anything.** The cycler's instrumentation keeps
running underneath.

**Passes if** the shadow decisions over real gameplay match what the recorded
frametimes justify, with no decision storms.

Zero risk, real gameplay, and it validates the policy against scenes we never
thought to capture. This phase exists specifically so the first live run is not
also the first test.

### Phase 5 — Live, closed loop

**Procedure.** Enable application. Compare against two fixed-preset baselines
over the same route: one tuned for the worst case (quality floor), one tuned for
the average (stutter).

**Passes if** the governor beats both on the Section 1 objective — higher
time-weighted mean pixel fraction than the worst-case baseline, and a lower miss
rate than the average-case baseline.

---

## 8. Open Questions

| # | Question | Resolved by |
|---|---|---|
| Q-1 | Is a true GPU-time signal available? | Phase 1b |
| Q-2 | Do the rungs separate under real load? | **Answered 2026-08-04: yes, decisively** — 13.57 → 25.63 ms across the ladder under load (E-7) |
| Q-3 | Is `t = t_fixed + t_scaled·f` accurate enough? | **Answered 2026-08-04: yes** (E-7) |
| Q-4 | Why did `LoadingMenu` block a whole session once? | **Answered 2026-08-04**: it blocks for >30 s after every load (E-9). `StartDelaySeconds` raised 20 → 45 |
| Q-5 | Are our frametimes real, or is the delta-dedup undercounting? | **Answered 2026-08-04: undercounting, ~54% capture, biased against smooth frames** (E-10). Fix is the one-shot `AddTask` marshalling |
| Q-6 | Does `k` differ enough between scene classes to need per-class seeding? | Phase 1 across the four classes; one data point so far (`k ≈ 2.10`, rainy exterior) |
| Q-7 | How much does weather move `k` and `t_fixed`? The 2026-08-04 run roughly **doubled** in cost mid-session (NativeAA 15.4 → 25.7 ms) as rain set in. | Phase 1, by capturing the same spot in different weather |

Answered since the 2026-08-01 revision: transition latency is ~1.0 s (E-2);
visual disruption is an upstream bug, not inherent (E-5); API revision 3 is
available (E-6).

---

## Decision Log

**2026-08-03 — "Over budget" replaced by "dropped" as the failure metric (D-7a,
D-7b).**
Prompted by the user's observation that stutter tracked OpenXR Toolkit reporting
below 72 exactly, and that 72 always felt smooth. That is the correct ground
truth, and our metric did not reproduce it: the 2026-08-03 run reported 33–44%
"miss" for the four capped presets in a scene that felt flawless. Cause is
mechanical — held at refresh, frametime sits *on* the budget, so symmetric jitter
puts about half the samples fractionally above a strict `> budget` test. A frame
either makes its interval or waits for the next, so the threshold belongs between
the two populations at `1.5 × budget`. `missRate` is kept as a margin gauge only.
`miss_max = 0.05` would have triggered a step-down continuously in a scene that
needed none; replaced by `drop_max`, plus `cap_tol` for censoring detection.

**2026-08-03 — Sweep count must be even.**
Serpentine traversal was added so that reversing alternate sweeps cancels
session drift. It only does so when both directions are equally represented.
With `n` presets, a preset at ladder position `i` is visited at global time
`s·n + j`, where `j = i` on even sweeps and `n−1−i` on odd. Over four sweeps the
positions sum to `54`, independent of `i`; over three they sum to `27 + i`, which
still depends on where the preset sits in the ladder. The default of 3 therefore
left a residual bias proportional to ladder position — exactly the artefact
serpentine exists to remove. Default is now 4, and `DwellSeconds` drops 12 → 8
to keep the run under five minutes (575 frames at 72 Hz is ample for a P95).

**2026-08-03 — Replaced the threshold policy with the cost model.**
The 2026-08-01 design mapped P95 frametime onto presets through fixed step-up
and step-down thresholds. E-1 falsified the premise: four presets spanning 6× in
pixels produced identical frametimes because all were capped, so the control
signal is saturated and censored precisely in the regime the thresholds were
meant to govern. Replaced by D-1 through D-6.

Carried forward unchanged, because they survived contact with the measurements:
the single lever, the prohibition on fighting CS's own guards, the block-reason
handling, tail-not-mean (D-7), and the asymmetry between falling and rising —
though the asymmetry now has a cause (D-4) rather than being a rule of thumb.

Absorbed rather than deleted: the CPU-bound guard is now a consequence of the
model (D-6), and the failure memory is now backoff (D-9).
