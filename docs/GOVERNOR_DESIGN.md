# Governor Design

Status: **agreed design, not yet implemented.** Supersedes the threshold-based
policy in the 2026-08-01 revision of this file. See [Decision Log](#decision-log)
for what changed and why.

This document is the contract, and it has two readers.

**For us**, it is the reason we stop improvising: everything the governor does
should be traceable to a decision recorded here, and every decision to a
measurement.

**For the Community Shaders author**, it is the evidence that the patch we will
eventually offer (D-11a) came from analysis rather than from bundling something
that happened to work. The evidence table cites its sources, the decision log
records what we got wrong and why we changed it, and superseded decisions are
kept rather than quietly deleted. A reviewer should be able to check our
reasoning, not just our results.

Conclusions we later falsified are listed in `MEASUREMENT_TRAPS.md`. They stay
there deliberately.

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
| E-11 | **E-10 is not cosmetic: it inverted the ladder.** In Markarth, Quality (f=0.444) measured *faster* than Balanced (f=0.346) in every clean sweep of two sessions, which is not physical. Cause: within the same 8 s dwell we captured 209 frames of Balanced and 433 of Quality (36% vs 75%). The steadier preset is sampled less, and what survives is disproportionately jitter, biasing it upward. Riverwood escaped this only because its rungs were far enough apart. | Sessions 2026-08-05 20:52 and 21:02 |
| E-12 | **A true headroom signal exists and is uncensored.** PrimaShock's overhead figure derives from `appGpuTimeUs`, measured with `D3D11_QUERY_TIMESTAMP_DISJOINT` + start/end `D3D11_QUERY_TIMESTAMP` (`d3d11.cpp:777-781`, accumulated `layer.cpp:2251`). Measured in Markarth it is monotonic — 37/25/16/10/5% → GPU 8.75/10.42/11.67/12.50/13.20 ms — over exactly the range where our frametimes were flat and disordered. | OpenXR-Toolkit 1.3.2 source; user readings 2026-08-05 |
| E-13 | **Observed control thresholds.** ≥10% overhead holds a solid 72; ~5% yields 70–71; 0% drops below. Reported as a stable perceptual rule across many sessions: whenever overhead is displayed at all, turning is smooth. | User observation, 2026-08-05 |
| E-14 | **The CS VR API exists specifically to be driven by external governors.** The mod page states it "allows external mods like Shizof's VR FPS Stabilizer to dynamically toggle shadows, SSGI, and upscaler quality modes based on performance, weather and location conditions." | Nexus 166950 description, captured from MO2 `meta.ini` |
| E-15 | Installed baseline identified exactly: **PL3.15 = release RC74 = commit `eb54a72c`**, published 2026-07-09T21:03Z, downloaded 2026-07-09T21:13Z. | MO2 `meta.ini`, GitHub releases API |
| E-17 | **The GPU timer works and the signal is uncensored. D-13 passes.** Session 2026-08-06 14:33, 17 538 frames, 99.4% capture. The four cheapest presets all read 13.84–13.96 ms of frametime — indistinguishable, as E-1 says — while their GPU times read 11.73 / 12.47 / 13.19 / 14.00 ms, cleanly separated and monotonic in pixel count. All seven rungs order correctly within a single sweep. | `20260806_143350_frames.csv` |
| E-16 | **The overlay's headroom is `(budget − appGpuTimeUs) / budget`, and it logs itself to disk.** Source: `headroomTime = (1000000/targetFps) − time; headroomPercent = (headroomTime/10)/frameTimeMs` — algebraically identical to ours. Four qualifiers came with it, below. | OpenXR-Toolkit 1.3.2 `menu.cpp:918-947`, `layer.cpp:2251/2344-2346/2569`; live registry and stats CSV on this machine, 2026-08-06 |

### E-17 in detail — the measurement that closes roadmap step 4

| preset | scale | frametime (ms) | **GPU (ms)** | GPU P95 | headroom P95 |
|---|---:|---:|---:|---:|---:|
| UltraPerformance | 0.333 | 13.89 | **11.73** | 12.92 | +7.0% |
| Performance | 0.500 | 13.96 | **12.47** | 13.70 | +1.4% |
| Balanced | 0.588 | 13.85 | **13.19** | 14.02 | −1.0% |
| Quality | 0.667 | 13.84 | **14.00** | 14.76 | −6.3% |
| UltraQuality | 0.769 | 14.61 | **14.61** | 15.70 | −13.0% |
| Hoshipa | 0.850 | 15.71 | **15.73** | 17.38 | −25.2% |
| NativeAA | 1.000 | 16.24 | **16.24** | 19.84 | −42.9% |

**The bracket excludes the wait, and this table proves it without reference to
any other tool.** That was the one thing D-13 could get wrong, and the test for
it is internal: if the bracket had enclosed the compositor wait, GPU time would
equal frametime at *every* preset. It does at the expensive end — 16.24 = 16.24,
a genuinely GPU-bound frame with no idle to exclude — but at UltraPerformance it
reads 11.73 against a frametime of 13.89. That 2.16 ms gap is the wait, sitting
outside the bracket where D-13 put it.

Read the two halves of the table together: **frametime separates only the three
rungs that miss the cap; GPU time separates all seven.** The bottom four are the
entire problem this project exists to solve, and they are now distinguishable.

Two further results fall out:

- **The E-11 inversion is gone.** Quality no longer measures faster than
  Balanced. All seven rungs order by pixel count, within a single sweep as well
  as in aggregate, so it is not an averaging artefact.
- **`gpu frames 73 of 73 (0 repeated)`** — every frame produced a fresh reading.
  The multi-buffered readback never fell behind and never stalled.

**What is still outstanding** is the external cross-check against the overlay's
own `appGPU` column. It is now corroboration rather than the verdict, and it
serves a different purpose: calibrating the constant offset between the two
brackets (ours ends before `Present`, theirs at `xrEndFrame`).

### E-16 in detail — the reference signal, and its four qualifiers

The overlay we have been reading is `XR_APILAYER_MBUCCHIA_toolkit`, the only
implicit OpenXR layer registered on this machine; its log records
`Application name: 'OpenComposite_SkyrimVR'`, `Pimax OpenXR`, `Pimax Crystal
Super`.

**Caveat on the source reading.** The installed binary announces itself as
`OpenXR Toolkit - Primashock combo (v1.4.0)`, while the source we have on disk
is vanilla OpenXR-Toolkit **1.3.2**. Everything below is read from 1.3.2 and is
very likely unchanged, but it is not verified against the binary in use — and
one behaviour demonstrably differs: setting `record_stats = 1` in the registry
before launch did **not** produce a stats CSV on the combo build, though the
same key is present and two CSVs from earlier sessions exist. Enable it from the
in-headset menu instead (CTRL+ALT+Down → "Record statistics to file"), which is
the path known to have worked. Its settings live in the registry under
`HKCU\SOFTWARE\OpenXR_Toolkit\OpenComposite_SkyrimVR`, and reading them settles
several things that were previously assumed:

1. **`target_rate = 72`.** Their denominator is our denominator, 13 889 µs. The
   two headroom figures are directly comparable without rescaling.
2. **Their figure is a one-second *mean*** — `appGpuTimeUs /= numFrames` once per
   window (`layer.cpp:2344-2346`). D-7 requires the governor to decide on the
   tail, so the mean is what we compare against, **not** what we control on. Our
   per-frame CSV keeps both available; the comparison must average our GPU time
   the same way or it will disagree for reasons that have nothing to do with the
   bracket.
3. **The overlay only *displays* headroom when `fps + 2 >= targetFps`.** Below
   that it shows "CPU bound"/"GPU bound (+X ms)" instead. This is the mechanism
   behind E-13's "whenever overhead is displayed at all, turning is smooth" — the
   *presence* of the number already implies ≥ 70 fps. E-13's thresholds are
   therefore conditioned on that, which is an argument for their conservatism,
   not against it.
4. **`turbo = 1`.** Turbo Mode makes their `appCpuTimeUs` unreliable by their own
   account, and they suppress the CPU line because of it. `appGpuTimeUs` is
   unaffected, and it is the only column we use.

Their bracket, for comparison with D-13: `appGpuTimer->start()` at `xrBeginFrame`
— after `xrWaitFrame`, i.e. after the throttle — and `->stop()` at `xrEndFrame`
(`layer.cpp:2253`, `2569`), read back one frame later through a rotating set of
timers. Ours opens at the first draw and closes before `Present`. Same intent,
same exclusion, different seam.

**And it writes a CSV.** `record_stats = 1` opens
`%LOCALAPPDATA%\OpenXR-Toolkit\stats\stats_<timestamp>.csv`:

```
time,FPS,appCPU (us),renderCPU (us),appGPU (us),VRAM (MB),VRAM (%)
2026-07-30 11:53:40 +0200,72.0,13885,2553,12048,10591,33
```

That sample, from an earlier session, is the whole argument in one row: `appCPU`
pinned at 13 885 µs — the censored quantity of E-1 — while `appGPU` reads
12 048 µs, which is 13% headroom. One column saturated, the other not.

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

Given a frametime target `T = budget − margin_ms`:

**The margin is absolute, not a fraction of budget.** With `t_fixed ≈ 11 ms`
against a 13.889 ms budget, only ~2.8 ms is available for resolution-dependent
work; a 10% proportional margin is 1.39 ms, or **half of all the headroom there
is**, and would drive the governor two rungs below what the scene supports.
Measured 2026-08-05 in clear-weather Riverwood.

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

### D-10 — Headroom is the primary signal; frametime is the fallback

Where GPU time is available, control on **headroom** — how far GPU work sits
below the frame budget — rather than on the cost model of D-2/D-3.

This is not a refinement of D-1 through D-6; it removes most of their reason to
exist. Those decisions are engineering around a **missing measurement**:

| Decision | Why it existed | With headroom |
|---|---|---|
| D-4, two regimes | frametime is censored at the cap | **obsolete** — both directions informed |
| D-4, blind upward probe | no way to see spare capacity | **obsolete** — spare capacity is read directly |
| D-5, `k` from transitions | a cost sample only arrived when the preset changed | **obsolete** — every frame is a sample |
| D-2/D-3, cost model | the only way to infer the cheapest safe rung | **optional** — needed only to jump several rungs at once |

What remains is a far simpler loop, and it is the user's own formulation: climb
while there is headroom to spare, descend when there is not.

```
if headroom < descend_floor   → step down
if headroom > climb_floor     → step up one rung
otherwise                     → hold
```

**Both tiers must be retained.** The governor has to work with no headroom
signal at all, because the first shipped version will run against an unmodified
Community Shaders. Frametime plus the cost model is that fallback: today's data
shows it converges on the same rung, just one rung conservative near the
boundary and tens of seconds slower to climb (D-4's probe). Headroom is an
**enhancement that removes those two weaknesses**, never a hard dependency.

### D-10a — Thresholds come from measurement, not theory

E-13 gives them directly: `climb_floor ≈ 10%`, `descend_floor ≈ 5%`. The user's
"climb until overhead reaches zero" is the true maximum-quality point but has
zero tolerance for a load spike; 10% is where 72 was observed to hold reliably
and 5% is where it was observed to slip.

### D-10b — Headroom is judged on the tail, and the reference mean is only for cross-checking

D-10a gives thresholds as percentages but does not say *of which statistic*, and
the two available answers differ in exactly the regime that matters.

`GetLastFrameGpuTimeUs()` is per-frame. The OpenXR-Toolkit figure that E-13's
thresholds came from is a **mean** over its stats window. A scene whose mean
headroom is 10% but whose worst frames reach 0% drops frames while reporting
comfort, and D-7 already settled this argument for frametime: decide on the tail.

**Therefore:**

- Control on **P95 GPU time** over `W_judge` — i.e. `headroom_p95 = 1 −
  p95(gpu_us) / budget_us`. Both the climb and the descend test use it, so the
  controller cannot be optimistic in one direction and pessimistic in the other.
- Log the **mean** as well, and only for comparison against the toolkit's
  column. Any disagreement between our mean and theirs is an instrument
  question; any disagreement between our mean and our P95 is a scene question.

**This carries a known risk, stated rather than discovered later.** E-13's
10%/5% were read off a *mean*, so applying them to a P95 makes the governor
more conservative than the observation that produced them — by however much the
GPU-time distribution is skewed, which nobody has measured yet. The numbers are
therefore provisional until Phase 3 replays real traces and re-fits them. **Do
not tune them in-game to compensate**; that is precisely the trap Phase 3 exists
to avoid.

### D-11 — Fork Community Shaders; do not ship the fork

Measuring GPU time correctly requires brackets around the frame's render work.
A bracket that accidentally encloses the vsync wait re-measures frametime and
reproduces the censoring we are trying to escape. Three places can bracket
correctly:

1. **An OpenXR API layer** — correct by construction, but layers install via
   registry keys that mod managers cannot write, load into *every* OpenXR
   application on the machine, and interact with PrimaShock's own layer. A bad
   trade for a mod whose promise is "it just holds 72".
2. **Hooking Skyrim's renderer from the plugin** — no extra install, but the
   hook points are guesswork without local test capability.
3. **Community Shaders itself** — already holds the device, already brackets its
   render passes, and is already a hard requirement of this project.

**Choose 3.** Its distribution story is the only good one: once upstreamed, the
dependency is "CS ≥ version X", which MGO picks up on its own. Nothing extra is
ever installed.

**The fork is scaffolding, not a product.** We do not ask users to replace their
CS. The patch is developed in `zerotrust-dev/skyrim-community-shaders`, proven
against a working governor, and then offered upstream.

**Baseline pinned at PL3.15 / RC74 / `eb54a72c`** (E-15) — the exact build every
measurement in this document was taken against. Pinning costs us a rebase onto a
fast-moving `Upscaling.cpp` before the PR; that is accepted deliberately, in
exchange for not invalidating the evidence base mid-project. It does mean the
patch should not be left to age for months.

### D-11a — The upstream case is an extension, not a request

E-14 matters more than it first appears. The CS VR API was **built to be driven
by exactly this kind of external controller** — the mod page names dynamic
upscaler-quality switching driven by performance as an intended use.

So the eventual conversation with the author is not "please add a feature we
want". It is:

> Your API was designed for external mods to drive quality from performance. We
> built one. It works, and here are the measurements. The one thing the API
> cannot currently supply is the signal that makes the decision correct rather
> than merely conservative — GPU time, which you already measure the ingredients
> for and which nothing outside CS can obtain honestly. Here is the patch.

That is why this document exists in the form it does: an author receiving a
patch is entitled to know whether it came from analysis or from guesswork. The
evidence table, the decision log and the superseded decisions are the answer.

### D-12 — The run must be readable from the logs alone

Testing must not depend on the user narrating what they saw. Every session
records, continuously and time-aligned:

- frametime, and GPU time / headroom once available
- the current preset, and every transition with its trigger and latency
- what the controller decided and **why** — including decisions to hold
- block reasons, readback results, and any refused apply

The target is that the user walks around normally and the entire session is
reconstructible afterwards from the artifacts. This is not convenience: every
wrong conclusion recorded in `MEASUREMENT_TRAPS.md` came from reasoning about
something nobody was measuring at the time.

### D-13 — Where the GPU timestamps bracket the frame

D-11 chose Community Shaders as the place to measure GPU time. *Where inside the
frame* the bracket opens and closes decides whether the number is worth having.

**The failure mode is specific.** A `D3D11_QUERY_TIMESTAMP` pair measures elapsed
time on the GPU timeline between two points in the command stream — **including
any GPU idle between them.** If the bracket opens before the compositor releases
the CPU (`WaitGetPoses` at frame start), the GPU sits idle inside the bracket for
exactly as long as the throttle holds, and the measurement becomes
frametime again: pinned at ~13.9 ms regardless of load, censored per E-1, and
worthless for the same reason. Reproducing the censoring inside a new instrument
would be the worst outcome available, because the number would still look
plausible — exactly the shape of the trap already recorded as E-4.

**Placement.**

| Boundary | Where | Why there |
|---|---|---|
| open | the frame's **first draw**, via the existing `BSGraphics::SetDirtyStates` hook | The CPU has already passed the compositor throttle — the first draw cannot be submitted before it. Idle spent waiting is therefore outside the bracket. |
| close | in the `IDXGISwapChain::Present` hook, **immediately before** the swap-chain call, after `Menu::DrawOverlay` | Everything the application submits for the frame is enclosed; the present/vsync wait is not. |

The open boundary is *armed* at Present and *fired* by the first draw that
follows, rather than issued at a fixed point. A fixed frame-start point would
have to be either before the throttle (censoring) or at a CS-specific pass such
as the deferred opaque pass, which would silently exclude shadow-map generation —
a resolution-independent cost that `t_fixed` in D-2 exists to represent.

**Known and accepted limitation.** When the frame is CPU-bound, gaps between
draws where the GPU starves *are* inside the bracket, so the reading is an upper
bound on real GPU work. This is the same limitation the reference implementation
carries (OpenXR-Toolkit brackets `xrBeginFrame`→`xrEndFrame`), and it is
conservative in the safe direction: it can make the governor believe it has less
headroom than it does, never more. D-6 already covers the CPU-bound case.

**Readback never stalls.** Four buffered query sets, read with
`D3D11_ASYNC_GETDATA_DONOTFLUSH`; a set that is not ready is left for a later
frame, and a frame whose set is still in flight is simply not timed. Results
with the disjoint flag set are discarded, per the D3D11 contract.

**Why not CS's existing profiler.** Community Shaders already contains
`src/Profiler.cpp`, which does D3D11 timestamp queries properly. It was not
reused, and an upstream reviewer will reasonably ask why:

- It is **opt-in twice over** — `IsEnabled()` requires both a user toggle and an
  active capture request. A governor needs a signal that is always there,
  including on a machine whose owner never opens the CS menu.
- It measures **per-pass CS work**, up to 128 named timers per frame, not the
  frame. Summing its passes would miss everything the game renders outside CS,
  which is most of `t_fixed`.
- It exists to drive a UI, and its cost is sized for that.

The frame timer added here is three queries per frame, always on, and answers a
different question: what did the whole frame cost the GPU. The two are
complementary rather than redundant, which is also the argument for carrying
both upstream.

**Exposed as** `ICSInterface001::GetLastFrameGpuTimeUs()` at **interface
revision 4**, appended to the vtable (never inserted — the ABI note in the header
is binding), alongside `GetLastFrameGpuTimeFrameIndex()` so a consumer can tell a
stale reading from a stable one rather than inferring it from an unchanging
value. `0` means "no measurement available", which is the D-10 fallback tier's
trigger.

**Validation is external and is the point.** The reading is compared against the
OpenXR-Toolkit overlay's `appGpuTimeUs` across a preset sweep. If ours tracks
theirs, the bracket is right; if ours is flat while theirs moves, the bracket
enclosed the wait and this decision is wrong. Nothing about the implementation
being "obviously correct" substitutes for that comparison.

**The comparison is log-against-log, not eye-against-HUD** (E-16). Setting
`record_stats = 1` makes the toolkit write its own per-window CSV, so both sides
of the test come off disk and are joined on wall-clock time. Two consequences:
the acceptance test no longer depends on anyone watching at the right moment,
and our GPU time must be averaged over their window before comparing, because
theirs is a mean and ours is per-frame.

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
| `margin_ms` | absolute safety margin below budget | 0.35 ms | Phase 3 sweep |
| `climb_floor` | headroom above which to raise quality | 10% | E-13, refined in Phase 3 |
| `descend_floor` | headroom below which to lower quality | 5% | E-13, refined in Phase 3 |
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

## 7a. Roadmap

Ordered by dependency. Each step has an artifact and a stop condition; a step
that fails sends us back to Section 4 rather than being worked around.

| # | Step | Why now | Done when |
|---|---|---|---|
| **1** | **Fix the frame sampler.** Thread for timing, one-shot `SKSE::AddTask` per frame, no dedup. | E-11: the bias inverted the ladder. Every number is currently suspect. Independent of everything else and the smallest change. | Capture rate ≈100%; Markarth ladder monotonic on re-run |
| **2** | **Telemetry (D-12).** Continuous time-aligned log of frametime, preset, transitions, decisions and block reasons — during *free play*, not only during a scripted sweep. | Makes every later step testable without the user narrating, and without staging scenes. | A session is fully reconstructible from artifacts alone |
| **3** | **Gather cost curves by playing**, not by staging. Walk normally; heavy scenes arrive on their own and telemetry records them. | Re-establishes the cost curves on trustworthy data. | `k` agrees within 15% across comparable segments; residuals < 0.5 ms |
| **4** ✅ | **Fork CS at `eb54a72c`**, add the GPU timer, expose `GetLastFrameGpuTimeUs()` at interface revision 4. | The measurement that makes control correct rather than conservative. | **Done 2026-08-06 (E-17).** Passed on internal evidence: GPU time separates all seven rungs where frametime separates three, and the 2.16 ms gap at UltraPerformance shows the wait is outside the bracket. External cross-check against the overlay's `appGPU` remains, now for offset calibration rather than verdict |
| **5** | **Controller**, tiered: headroom loop (D-10) when GPU time is present, cost model (D-2/D-3) when not. Parameters chosen in CI by replaying recorded traces. | Both tiers must work; the first shipped version runs against unmodified CS. | Phase 3 simulation passes on all captured traces |
| **6** | **Shadow mode**, then live. | First live run must not also be the first test. | Phase 4 and 5 pass |
| **7** | **Upstream the CS patch** (D-11a), with this document and the measurements. | Removes the fork from the distribution path entirely. | Patch offered; governor ships against stock CS |

Steps 1–3 are on the governor repo and unblock everything. Step 4 is the only
one touching Community Shaders.

## 7b. Phase T — Telemetry

**Procedure.** Extend the existing capture set so a free-roaming session — not
just a scripted sweep — is fully readable afterwards:

- `*_timeline.csv` — one row per decision interval: time, preset, frametime P50
  and P95, drop rate, GPU time and headroom when available, controller state,
  the decision taken, and the reason it was taken.
- Transition rows keep their existing detail (latency, settle, block reasons,
  readback).
- The existing `apistate.csv` continues sampling the whole readable API surface,
  so drift in anything we are *not* controlling stays visible.

**Passes if** a session where the user simply walks around can be reconstructed
— what the scene cost, what the controller saw, what it did, and why — without
asking them a single question.

**This phase exists because of a measured failure**, not tidiness. The Markarth
ladder inversion (E-11) was invisible in the summary and only became explicable
after inspecting per-visit sample counts. Anything not logged is something we
will later reason about wrongly.

### It also exists to stop asking the user to stage scenes

Until this phase lands, answering a question means sending the user to a
specific location, having them stand still for five minutes, and — where the
signal is censored — read numbers off another mod's overlay by eye. That is slow,
it does not scale, and it makes the data depend on someone's attention at the
right moment.

**The requirement is therefore explicit: the user plays normally, and the data
comes to us.** Heavy scenes arrive on their own during ordinary play; they do not
need to be visited on request. Any future question that can only be answered by
staging a scene should first be treated as a gap in what is logged.

There is **no** unavoidable manual step, and the belief that there was one was
wrong. This document previously recorded reading GPU headroom off the overlay by
eye as the single exception. E-16 removes it: the overlay is OpenXR-Toolkit, and
`record_stats = 1` makes it log `appGPU (us)` per window to
`%LOCALAPPDATA%\OpenXR-Toolkit\stats\`. The reference signal comes off disk like
everything else, and is joined to our own capture on wall-clock time.

The lesson is the one this section already states, applied to itself: a question
that seems to need a human observer is a gap in instrumentation, and that
includes questions about somebody else's instrument.

## 8. Open Questions

| # | Question | Resolved by |
|---|---|---|
| Q-1 | Is a true GPU-time signal available? | **Answered 2026-08-06: yes.** GPU time separates all seven rungs while frametime separates only three (E-17). Phase 1b passes |
| Q-2 | Do the rungs separate under real load? | **Answered 2026-08-04: yes, decisively** — 13.57 → 25.63 ms across the ladder under load (E-7) |
| Q-3 | Is `t = t_fixed + t_scaled·f` accurate enough? | **Answered 2026-08-04: yes** (E-7) |
| Q-4 | Why did `LoadingMenu` block a whole session once? | **Answered 2026-08-04**: it blocks for >30 s after every load (E-9). `StartDelaySeconds` raised 20 → 45 |
| Q-5 | Are our frametimes real, or is the delta-dedup undercounting? | **Answered 2026-08-04: undercounting, ~54% capture, biased against smooth frames** (E-10). Fix is the one-shot `AddTask` marshalling |
| Q-6 | Does `k` differ enough between scene classes to need per-class seeding? | Phase 1 across the four classes; one data point so far (`k ≈ 2.10`, rainy exterior) |
| Q-8 | **Where does headroom actually stop holding 72?** E-13 says ≥10% holds and ~5% slips, but that came from a tool that hides the figure below `target − 2` fps (E-16), so the failing half of the curve has never been observed. | The first capture with our own timer, which has no such gate: log headroom through the drops as well as through the clean running |
| Q-9 | How skewed is per-frame GPU time? This decides how much more conservative D-10b's P95 is than the mean E-13's thresholds were read from. | Phase 3, from the captured traces |
| Q-7 | How much does weather move `k` and `t_fixed`? The 2026-08-04 run roughly **doubled** in cost mid-session (NativeAA 15.4 → 25.7 ms) as rain set in. | Phase 1, by capturing the same spot in different weather |

Answered since the 2026-08-01 revision: transition latency is ~1.0 s (E-2);
visual disruption is an upstream bug, not inherent (E-5); API revision 3 is
available (E-6).

---

## Decision Log

**2026-08-06 — D-13 holds; the signal is uncensored (E-17). Roadmap step 4 closes.**

First run against the forked Community Shaders. GPU time separates all seven
presets, monotonically in pixel count and within a single sweep, over a range
where frametime reads 13.84–13.96 ms for four of them. Q-1 is answered and
Phase 1b passes.

The bracket question settled itself internally, which was not the plan: the
acceptance test was to be a comparison against the overlay. It was not needed,
because enclosing the wait would force GPU time to equal frametime at *every*
preset, and at UltraPerformance the two differ by 2.16 ms. The comparison is
still worth doing — it calibrates the offset between the two brackets — but it
is corroboration now, not the verdict.

What this does not yet tell us: whether the thresholds are right. In this
session only UltraPerformance had positive P95 headroom (+7.0%); everything
above it was already over budget. That is one scene during ordinary play, and
D-10a's numbers stay provisional until Phase 3 (Q-8, Q-9).

**2026-08-06 — The reference signal is readable from disk (E-16); the last
"unavoidable" manual step was not unavoidable.**

The overlay whose overhead figure this project has been treating as ground truth
is OpenXR-Toolkit, it stores its settings in HKCU, and it can log
`appGPU (us)` to CSV. Reading the source and the live settings confirmed the
headroom formula is identical to ours and that `target_rate = 72` makes the
denominators match — but also produced three qualifiers that were being assumed
away: their figure is a one-second mean (so it is what we compare against, never
what we control on, per D-7); the overlay only displays headroom at all when fps
is within 2 of target, which is the mechanism behind E-13 rather than a
coincidence; and Turbo Mode invalidates their CPU column but not the GPU one.

Section 7b's claim that reading the overlay by eye was "the one genuinely
unavoidable manual step" is withdrawn. It was never verified — the setting had
been sitting in the registry the whole time, and two stats CSVs from earlier
sessions were already on disk.

**2026-08-06 — Bracket placement fixed for the CS GPU timer (D-13).**

Roadmap step 4 needed one decision recorded before code: where the timestamp
pair sits. The constraint is that a bracket enclosing the compositor throttle
measures GPU idle as GPU work and reproduces the censoring of E-1 inside the
instrument meant to escape it. Opening the bracket at the frame's first draw
(armed at Present, fired by the first `SetDirtyStates`) puts the throttle
outside it while keeping shadow-map generation inside; closing it immediately
before the swap-chain call keeps the present out. The residual error — GPU
starvation gaps in CPU-bound frames — is inside the bracket, is shared with the
reference implementation, and errs toward *less* apparent headroom. Exposed at
interface revision 4 with a frame index so staleness is observable rather than
inferred. PrimaShock's overlay remains the acceptance test, not code review.

**2026-08-05 — Headroom becomes the primary signal (D-10), CS is forked rather
than a second layer built (D-11), telemetry becomes a first-class phase (D-12).**

Three findings in one session. First, E-11: the frame sampler's bias was not
cosmetic — it inverted Quality against Balanced in Markarth, because we captured
36% of one preset's frames and 75% of another's in the same dwell. Second, E-12:
a true uncensored headroom signal exists and is already measured on this stack by
PrimaShock, via D3D11 timestamp queries, and it is monotonic exactly where our
frametimes were flat and disordered. Third, E-13: the user's long-standing
observation supplies the control thresholds directly (10% holds, 5% slips).

Together these demote most of D-1 through D-6 to a fallback tier. Those decisions
were engineering around a missing measurement; supplying the measurement removes
the two-regime split, the blind upward probe, and the need to estimate `k` only
at transitions. The remaining loop is the user's own: climb while headroom
exists, descend when it does not. The cost model is retained for the
no-GPU-time case and for multi-rung jumps.

The delivery vehicle changed with it. A second OpenXR layer was proposed and
rejected on distribution grounds (registry install, loads into every OpenXR
application, layer-ordering interactions with PrimaShock). Patching CS is chosen
instead because it is the only route whose end state adds nothing to a user's
install, and because E-14 shows the CS VR API was built for exactly this kind of
external controller — making the eventual upstream conversation an extension of
stated intent rather than a feature request. Baseline pinned at PL3.15 / RC74 /
`eb54a72c` (E-15), the build every measurement here was taken against.

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
