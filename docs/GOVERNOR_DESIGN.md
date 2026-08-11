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
| E-24 | **Phase 5 passes on its own terms, and shows two defects.** First live session, 6.5 minutes of free play (mountain path at night, dark interior): the governor reached **f=0.382 at 0.7% over budget**, against fixed Balanced 0.346 at 0.5% and fixed Quality 0.444 at 12.8%. More pixels than the conservative baseline, a fraction of the misses of the ambitious one. 87.4% of frames at ≥71 fps, **0.18% dropped**, and every dropped frame in the session occurred within 2 s of a preset change (42 in total, all from descents; the six climbs cost none). Defect: 8 of 14 changes fired at **0.01–0.27 ms** over the descend line. The one climb that reversed within 8 s was **not** a defect — the player had entered a dark interior with almost nothing to draw and stepped back out into a forest 5 s later; the controller tracked a real scene change in both directions. | Session 2026-08-08 16:34, and the player's account of it |
| E-23 | **Dynamic control is worth up to +83% pixels over the safe fixed preset, and the first fitted parameters exist.** Replaying session 2026-08-08: fixed presets give 0.6% over budget at Balanced (f=0.346) and 2.4% at Quality (f=0.444), while a perfect-foresight oracle averages **f=0.632 at 0.1% over budget**. The parameter sweep's best row under a 2% constraint reaches **f=0.511 — 81% of the oracle** — at `marginUp 2.5 / marginDown 0.0`. The provisional `marginUp 0.4` was far too eager, as labelled. | CI replay report, `tests/data/session-20260808.csv` |
| E-22 | **D-13b was the missing piece: 80% of frames were opening the bracket before the compositor released the application.** The counter added with D-13b reports 49 485 of 61 440 frames (80.5%) drawing before `WaitGetPoses` returned — so for four frames in five, the old start boundary put the pacing wait inside the measurement. With the start moved, the residual against the reference goes **+1.18 ms → −0.30 ms**, flat across load: −0.25 at 7–8 ms (17 s), −0.30 at 8–9 (171 s), −0.26 at 9–10 (227 s), −0.09 at 12–13, −0.15 at 17–18. Ours now reads slightly *below* the reference, which is the correct sign for a bracket that is a strict subset of theirs. | Session 2026-08-08 12:19, 639 matched seconds |
| E-21 | **The linear cost model holds on GPU time, and the fit corroborates D-13a.** Fitting `gpu = t_fixed + t_scaled·f` across all seven presets (deduplicated by `gpu_frame`) gives residuals of −0.67 to +0.57 ms, most under 0.3. Across the two sessions: `t_fixed` 10.85 → **9.88 ms** and `t_scaled` 6.58 → **7.96 ms** after the close boundary moved to the compositor submit — exactly the direction an error that inflates low-load readings would produce, from evidence entirely independent of the reference comparison. `k` = 0.61 and 0.81 respectively. | `20260806_152146_frames.csv`, `20260806_161947_frames.csv` |
| E-20 | **Independent audit: the end boundary is correct, my explanation of the residual was not.** Repeated `End()` on a timestamp query is documented behaviour (last call wins), so the per-eye re-stamp is sound. But upstream OpenComposite stores each eye and calls `SubmitFrames`→`xrEndFrame` only once **both** are in, so the reference bracket is a *superset* of ours — starting earlier at `xrBeginFrame`, ending later after the second-eye copy. Ours being larger therefore cannot be "the reference misses second-eye work". Independent recomputation: correlation **0.977** restricted to 7–20 ms, **0.995** on stable plateaus, residual +1.30 to +1.48 ms. | External review, 2026-08-06, `deliverables/Independent_VR_GPU_Time_Measurement_Audit.md` |
| E-19 | **D-13a fixed most of it, and missed its own acceptance bar.** After moving the close to the compositor submit, the excess over the reference fell from +3.95 ms to **+1.65 ms** in the 7–8 ms bucket and our floor dropped 11.5 → **9.17 ms** (reference floor 7.68 ms, i.e. the user's reported 43%). The excess is now near-constant across load — +1.65 at 7–8 ms, +0.79 at 19–20 — where before it swung by 3.1 ms. The stated bar was sub-millisecond in the 7–9 bucket, and it was not met. | Session 2026-08-06 16:19, 279 matched seconds |
| E-18 | **The bracket is right at the open end and wrong at the close end.** Joined against the toolkit's own `appGPU` log, 382 matched seconds of session 2026-08-06 15:21: **correlation 0.948**, but ours reads **+2.32 ms high** (sd 1.24), and the excess is *load-dependent* — +3.95 ms when their GPU is 7–8 ms, +0.85 ms when it is 15–16. Our reading has a floor near 11.5 ms and never goes below it; theirs reaches 7.41 ms. In headroom terms: ours 5.7%, theirs 22.4%. | `20260806_152146_frames.csv` joined to `stats_20260806_152045.csv` on wall clock |
| E-17 | **The GPU timer works and the signal is uncensored. D-13 passes.** Session 2026-08-06 14:33, 17 538 frames, 99.4% capture. The four cheapest presets all read 13.84–13.96 ms of frametime — indistinguishable, as E-1 says — while their GPU times read 11.73 / 12.47 / 13.19 / 14.00 ms, cleanly separated and monotonic in pixel count. All seven rungs order correctly within a single sweep. | `20260806_143350_frames.csv` |
| E-16 | **The overlay's headroom is `(budget − appGpuTimeUs) / budget`, and it logs itself to disk.** Source: `headroomTime = (1000000/targetFps) − time; headroomPercent = (headroomTime/10)/frameTimeMs` — algebraically identical to ours. Four qualifiers came with it, below. | OpenXR-Toolkit 1.3.2 `menu.cpp:918-947`, `layer.cpp:2251/2344-2346/2569`; live registry and stats CSV on this machine, 2026-08-06 |

### E-22 in detail — the start boundary, and what shadow mode cannot show

**The measurement question is closed.** The residual that three sessions could
not explain was the start boundary, and the counter added with D-13b says so
directly: **80.5% of frames drew before `WaitGetPoses` returned**. D-13's
assumption — that a frame's first draw necessarily follows the compositor wait —
was wrong for four frames in five, and no further reasoning about the *close*
boundary would ever have found it. An independent reviewer pointed at the start;
one session settled it.

| their GPU | seconds | ours − theirs |
|---|---:|---:|
| 7–8 ms | 17 | −0.25 |
| 8–9 ms | 171 | −0.30 |
| 9–10 ms | 227 | −0.26 |
| 12–13 ms | 43 | −0.09 |
| 17–18 ms | 14 | −0.15 |

Within half a millisecond at every load, with 188 seconds of evidence in the
7–9 ms range where earlier claims rested on three. The remaining −0.3 ms has the
right sign and a mechanism: their bracket starts earlier (`xrBeginFrame`, before
our first draw) and ends later (after OpenComposite's second-eye copy), making
it a strict superset of ours.

**Shadow mode has a limit this session made obvious.** Nothing is applied, so a
climb does not make the next frame more expensive, and the controller re-decides
the same climb after every cooldown: 199 climbs against 29 descends, with 1120
of 1179 holds being "cooldown". **The change rate under shadow mode measures
nothing**; only the direction of the first decision after each real preset
change carries information.

That is not a defect in shadow mode — it is precisely why D-14 exists. The
replay harness charges for a quality change by synthesising its cost, which is
what a live run would do and a shadow run cannot.

**The thresholds are visibly wrong, as labelled.** With 71% of frames at ≥30%
headroom in this session, "climb below 13.49 ms" fires almost always. That is
the predicted consequence of translating E-13's numbers through an offset that
has since changed twice. Placeholders until Phase 3 fits them.

### E-19 in detail — after D-13a, and what the residual is not

| their GPU | ours before | ours after | excess before | excess after |
|---:|---:|---:|---:|---:|
| 7–8 ms | 11.72 | 9.50 | +3.95 | **+1.65** |
| 9–10 | 12.01 | 11.09 | +2.42 | +1.75 |
| 12–13 | 13.79 | 13.66 | +1.34 | +1.13 |
| 15–16 | 16.29 | 16.60 | +0.85 | +0.95 |
| 19–20 | — | 19.89 | — | +0.79 |

**The load dependence is much reduced.** That was the disqualifying property: an
error that grew as headroom grew could never be calibrated away, because it was
largest exactly where the decision is made. What remains is roughly 1–1.5 ms.

**Correction (E-20): "essentially gone" overstated it.** The independent audit
points out that the 19–20 ms bucket, which anchors the flat end of that claim,
holds **three samples**. The low-load end is well supported — the audit
reproduces +1.56 ms at 7–8 ms and +1.63 at 8–9 — but the shape across the full
range is not established by this session, and I asserted it as though it were.

**The residual is not the Present fallback.** Frames where our GPU time sits
within 0.3 ms of frametime — 31.4% of the session, which would be the signature
of a frame that never reached the compositor — occur **only** where the reference
also reports high load: 0% of them below 11 ms, 59% at 12–13 ms. They are
genuinely GPU-bound frames, where GPU time legitimately approaches frametime.
The two suppression paths in the submit hook fire on device loss and relatch
only, and log once each.

**Correlation fell from 0.948 to 0.800** between the two sessions. Not a
regression in the instrument: the reference aggregates over its own one-second
windows, which are not aligned to the wall-clock seconds we bucket into, so a
session with faster-changing load correlates worse for reasons that have nothing
to do with either measurement.

### E-18 in detail — the close boundary is in the wrong place

The external comparison was run, and it found what internal evidence could not.

| their GPU | ours | frametime | excess |
|---:|---:|---:|---:|
| 7.77 ms | 11.72 | 13.96 | **+3.95** |
| 8.44 | 11.55 | 13.96 | +3.11 |
| 9.59 | 12.01 | 14.45 | +2.42 |
| 11.44 | 13.17 | 13.88 | +1.73 |
| 12.44 | 13.79 | 13.86 | +1.34 |
| 15.43 | 16.29 | 16.25 | +0.85 |
| 17.53 | 18.57 | 18.53 | +1.04 |

**Correlation 0.948.** The two instruments are measuring the same thing, so D-13's
open boundary is sound and E-17's conclusion — the signal is uncensored — stands.

**But the excess is not a constant offset, and it is worst exactly where it
hurts.** When the GPU is saturated the two agree to within a millisecond; when
there is real headroom, ours over-reads by up to 4 ms. Our value has a floor
around 11.5 ms and cannot go below it, while theirs reaches 7.41 ms. That is
17 percentage points of headroom at a 13.889 ms budget — the difference between
"descend, we are out of budget" and "climb, a fifth of the frame is spare".

**This is the D-13 caveat, which was written down as "conservative" and turns out
to be disqualifying.** The reasoning there was that idle inside the bracket makes
the reading an upper bound, erring toward less apparent headroom. True — but a
governor whose headroom signal saturates at ~2 ms of apparent spare capacity
cannot climb, and would strip quality in exactly the scenes that could afford
more. Conservative in the wrong place is not safe, it is useless.

**Where the excess comes from.** The toolkit stops its timer at `xrEndFrame`,
i.e. when the application hands the frame to the compositor. Ours runs on to
`Present`. Between those two points sits the game's submit to the compositor,
where frame pacing blocks the CPU while the GPU has nothing left to do — and a
timestamp delta counts that idle. When the GPU is saturated there is no idle to
count and the two agree, which is exactly the shape of the table.

### E-17 in detail — the measurement that closes roadmap step 4

**These GPU figures are from the D-13a build and are superseded.** The start
boundary was still wrong (E-22), inflating them by roughly 1.5 ms, worst at the
cheap end. Re-measured on 2026-08-08 the same ladder reads 8.08 / 10.05 / 11.45 /
12.77 / 13.21 / 14.25 / 15.28 ms. The table is kept because the *conclusion* it
supports — frametime separates three rungs, GPU time separates seven — is
unaffected, and because superseded evidence stays visible in this document
rather than being quietly corrected.

Also note what is portable here and what is not: `scale` is Community Shaders'
own and identical everywhere; the timings belong to one GPU, one per-eye
resolution and one mod list.

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

**Read the levels in that table as rankings only.** The session was ordinary
play, not a stationary capture, so each preset's average blends whatever the
player walked through during its four visits. The ordering is evidence; the
headroom percentages are not a statement about any scene. Rule 7 in
`MEASUREMENT_METHOD.md` exists because that distinction was got wrong here
first — an aggregate showing +7.0% at best was set against the player's direct
observation of 34%, and the aggregate was believed. The same file's per-frame
minimum was 9.63 ms, i.e. 30.7%, which agreed with the player all along.

**What is still outstanding** is the external cross-check against the overlay's
own `appGPU` column. It is corroboration for the bracket placement, but for the
*magnitude* it is the only check there is: nothing internal can show whether our
GPU time is the same quantity as theirs, and an inflated reading would make the
governor strip quality believing there is no headroom when there is 34%. The
comparison also calibrates the constant offset between the two brackets (ours
ends before `Present`, theirs at `xrEndFrame`).

It could not be done from the 2026-08-06 capture, because that capture stopped
when the sweep did — 4m38s of a much longer session, with every later
observation unrecorded. Monitor mode fixes that.

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

### D-10c — Thresholds are absolute milliseconds, and they are in *our* units

E-13's `10% climb / 5% descend` were read off the reference overlay. E-19 shows
our GPU time runs about 1 ms higher than that overlay's, so those percentages
cannot be carried across unchanged — 1 ms is 7.2 points of a 13.889 ms budget,
which is most of the gap between the two thresholds.

**Therefore the controller's thresholds are expressed as absolute milliseconds of
P95 GPU time**, not as percentages:

```
climb    when  p95_gpu <  budget - margin_up
descend  when  p95_gpu >  budget - margin_down
hold     otherwise
```

This follows D-3's reasoning for the cost model — a proportional margin is wrong
when a fixed cost dominates the frame — and it makes the offset a property of one
number rather than of every comparison.

**Fitted 2026-08-08 by replay (E-23), superseding the translated placeholders:**

| | value | meaning |
|---|---|---|
| `margin_up` | **3.0 ms** | climb only when P95 GPU is below 10.9 ms |
| `margin_down` | **0.0 ms** | descend when P95 GPU exceeds the budget |

The earlier placeholders — `margin_up 0.4`, `margin_down −0.3`, translated from
E-13 through a measured offset — were far too eager, which the shadow run made
obvious before the sweep confirmed it.

**Why 3.0 and not the sweep's top row.** The best row under the 2% constraint is
`margin_up 2.5`, at f=0.511 against 0.489 for 3.0. It is not chosen, because it
changes preset 5.15 times a minute against 3.13. Replay has no settle latency at
a synthesised preset (E-2 measured ~1.0 s), so it systematically flatters
parameter sets that change often, and D-8 already says success is few correct
changes rather than many small ones. Buying 4% more pixels with 65% more
transitions is the wrong side of that trade, and the replay cannot see the cost
of it.

**Still provisional in one specific respect.** The session behind this fit spent
109 s near the budget out of 5.4 minutes, and 71% of its frames had ≥30%
headroom. That is enough to rank parameters but thin for the region where the
thresholds actually decide. A capture with sustained marginal load should
re-run the same sweep before these are treated as settled.

### D-14 — Replay is counterfactual, and says so

Phase 3 chooses the parameters by replaying recorded traces. That requires
answering a question the trace cannot: **what would this frame have cost at a
preset it was not rendered at?**

The trace records GPU time at whatever preset the cycler had selected. A
controller under replay will want to choose differently, and from that moment
the recorded numbers no longer describe what it is doing. Ignoring this — feeding
the recorded GPU time back regardless of the preset the controller picked —
would produce a replay in which quality changes have no cost, and a parameter
fit that maximises quality for free. It would also look entirely plausible.

**So the replay synthesises the counterfactual from E-21's model:**

```
scaled:    gpu(t, p) = gpu_obs(t) · (t_fixed + t_scaled·f(p)) / (t_fixed + t_scaled·f(p_rec(t)))
additive:  gpu(t, p) = gpu_obs(t) + t_scaled·(f(p) − f(p_rec(t)))
```

`t_fixed` and `t_scaled` are fitted per session from the sweep, which visits
every preset within a few minutes.

**Both forms are implemented, and the replay reports both.** They differ in what
they assume about a scene getting heavier: the scaled form assumes the fixed and
resolution-dependent costs grow together, the additive form assumes only the
fixed part moves. Neither is obviously right. **Any parameter choice that depends
on which one is used is not a parameter choice, it is an artefact**, and running
both is the cheapest way to see that happen.

**Limits, stated because a replay is easy to over-trust:**

- The fit is a *session* average across whatever scenes the player moved
  through, so it inherits Rule 7 in `MEASUREMENT_METHOD.md`. `k` measured 0.61
  and 0.81 in two sessions on the same machine on the same day.
- The model cannot represent a scene where resolution genuinely stops helping
  (D-6's CPU-bound case) other than through a small `k`.
- Settle behaviour after a change (E-2, ~1.0 s) is not in the trace at the
  synthesised preset. Replay therefore over-states how quickly a change takes
  effect, which flatters any parameter set that changes often.

Replay is for **rejecting** parameter sets that oscillate or sit at the wrong
rung, not for certifying one as optimal. Phase 4's shadow mode, where the
decisions are computed against real frames, is what confirms it.

### D-15 — On the headroom tier, climb to where the scene can afford, not one rung at a time

**Supersedes "upward moves are always one rung" (D-4) for the headroom tier
only.** The frametime tier keeps it.

D-4's asymmetry — fall fast, rise slow — was never a preference. It followed
from censoring: with frametime alone, a climb is a blind probe, and the only
safe probe is small. **That reason does not exist on the headroom tier.** GPU
time is uncensored, the cost model is measurable (E-21), so where a climb lands
can be predicted instead of felt for.

One rung per `T_cooldown` costs more than patience. From UltraPerformance to
Quality is three rungs and about nine seconds spent below what the scene
affords — and **three preset changes instead of one**, each with its own history
reset and, on builds before the upstream fix, its own white flash. Fewer, larger,
correct changes is what D-8 asks for.

**Choose the highest rung whose predicted P95 still lands inside the hold band:**

```
predicted_p95(target) = p95_now · (1 + k·f_target) / (1 + k·f_now)
target = highest rung with predicted_p95 ≤ budget − margin_down − landing_margin
```

capped at `max_climb_rungs` in one move.

**Three deliberate conservatisms**, because a climb that overshoots costs a
descent immediately afterwards and that is the oscillation D-1 exists to
prevent:

- `k` defaults to **1.3**, the *highest* value measured across sessions (0.61,
  0.81, 0.95, 1.29). A high `k` over-predicts the cost of resolution, so the
  jump under-reaches rather than overshoots.
- `landing_margin` keeps the predicted landing off the descend edge, so an
  imperfect model does not immediately trigger the descent it just caused.
- The cap bounds the damage from a bad `k` to a known number of rungs.

**This does not touch descending**, which already jumps, nor the frametime
tier, where the probe remains the only honest option.

### D-16 — Every climb is landing-checked (the descend half was withdrawn)

**Withdrawn before it shipped: "a descent must be earned".** I proposed
requiring consecutive evaluations over the line, on the grounds that 8 of 14
changes fired at 0.01–0.27 ms over and that this was noise. Checking the
sequences that preceded each descent falsified it:

```
376.8s  13.24 → 13.06 → 13.00 → 14.14   descend
423.1s  13.38 → 13.21 → 13.30 → 14.12   descend
714.9s  12.98 → 13.10 → 13.37 → 14.90   descend
```

Seven of eight were **rising trends crossing the line once**, not a P95
flickering across it. Confirmations would have delayed every one of them by
half a second and then fired anyway. The mechanism addressed a failure mode
that was not present, and it was removed rather than kept "in case".

**What the same data does show** is why those descents cost so much quality.
A rung is worth about 1.45 ms here, but the hold band is 3.0 ms wide. Descending
at 14.1 lands near 12.6, and climbing back then requires P95 below 10.889 —
**1.7 ms beyond where the scene was when it descended**. Every descent strands
the controller a rung low until the scene lightens far more than it darkened.
That is Q-11 seen from the other side, and the fix is the band, not the trigger.

### D-16 (retained) — Every climb is landing-checked

**Every rung of a climb is now landing-checked, including the first.** D-15
exempted it, on the reasoning that the climb threshold had already been met.
That is not the same question: *"is there spare capacity now"* and *"will there
still be spare capacity after paying for this rung"* differ by exactly the cost
of the rung.

**This change is prophylactic, and the evidence I first cited for it was
wrong.** The climb-then-reverse at 683.5 s looked like an overshoot in the
timeline; the player's account is that they had walked into an unlit interior
with almost nothing to draw and back out into a forest five seconds later. The
controller was right twice, not wrong once. Checking the same prediction on that
climb would have permitted it anyway — 10.82 ms predicts a 12.13 ms landing
against a 12.89 ms limit — so the fix costs nothing there and still closes the
case it was meant for: a climb whose own cost puts it past the descend line.

The lesson is the recurring one. A timeline shows what the controller did, never
what the room looked like, and inferring the second from the first produced a
confident wrong reading for the third time in this project.

Not addressed here: `margin_down` stays at 0.0. The band width is the live
question (Q-11), and it is being answered by a sweep rather than by argument.

### D-23a — The frame hold fails open, never closed

**Why this is a decision and not an implementation detail.** The hold substitutes
a frame we captured for one the game rendered. That is only safe while every
property of the submit is one we recorded, and **the submit path belongs to
Community Shaders, not to us.** It has already changed underneath us once inside
a single session — per-eye textures becoming a combined double-wide atlas the
moment the render-scale path latched (E-43) — and their source carries an
upscale-method switch, foveated upscaling, frame generation and a DX12 swapchain,
any of which could present something we cannot copy.

So the rule: **when anything about a submit is not exactly what was captured,
hand the original through untouched.** Not withheld, not substituted, not
guessed at.

The worst outcome is then the relatch being visible — which is where we started,
and which is merely ugly. Withholding shows black (E-41) and substituting the
wrong thing shows corruption (E-42, E-43, E-44), both of which are worse than
the problem.

Concretely: an unknown texture type, an extended submit struct, a failed capture
or a missing copy all pass through. Each is reported once, so a hold that
quietly stops working explains itself in the log instead of looking like it
never ran.

**This is what keeps a future CS option from turning the governor back into a
comic-book generator.** It cannot keep the hold working across every change they
make; nothing outside their process can. It can guarantee that a change we do
not understand costs us the mitigation and nothing else.

### D-23 — Hide the relatch by withholding the frame, not by fading

**The problem, stated properly.** Changing the quality mode reallocates every
render target: `PerfMode.cpp` sets `restartRequired` whenever `qualityMode`
differs from the boot latch. For a frame or two there is nothing valid to
present, and the headset shows whatever is in that memory — the gridded panel
the player reports, which he identified as resembling CS's own interface.

**Three things ruled out first, each by evidence rather than argument.**

- *The fade.* CS's own constants say it does not drive `Game.FadeOutGame`; the
  caller must, and the intended shape is 1 s out, up to 6 s of black, 1 s in.
  For a controller changing quality every 20-30 s that is far worse than the
  artefact. Implemented and measured in D-22: the artefact survived it.
- *The menu path.* There isn't one. `PerfMode` makes **no distinction** between
  a change from CS's UI and one through the plugin API. The menu looks smooth
  because its panel covers the view while the world is paused — the same glitch
  happens, on a static image, behind an overlay.
- *Going back to the white flash.* That flash was a bug in
  `ClearHMDMaskCS.hlsl`, which now correctly clears the hidden-area mask to
  black. It happened to paint over the relatch gap. It is upstream, it is fixed,
  and it is not ours to reintroduce. **The artefact is not new — it is what the
  bug was covering.**

**The decision.** We already hook `IVRCompositor::Submit` for D-21. For a short
window after we apply a preset change, **do not call through**. An application
that does not submit gets its previous frame reprojected to the current head
pose by the runtime — that is what reprojection exists for. The result reads as
a brief hitch rather than a garbage panel.

**Why withholding beats re-submitting the old texture.** The obvious version is
to keep the last good frame and hand it back. That needs a reference to a
texture the game recycles, or a full-resolution copy every frame against the
chance we might need it — about 7 GB/s at this resolution, to insure against an
event that happens twice a minute. Withholding achieves the same visual result
because the runtime already holds the previous frame. Nothing is copied, nothing
is kept alive, and the code is a counter.

**Expected effect, stated in advance.** The gridded panel is replaced by a
2-6 frame hold, which at 72 Hz is 28-83 ms. Dropped-frame counts in the capture
will rise around each change, and that is the mechanism working rather than
failing — the frames were already unusable. Pixel fraction and the governor's
decisions are untouched; this changes only what reaches the headset.

**Refutation condition.** If the artefact still appears, the bad frame is not
the one we withheld — either the window is wrong or the garbage arrives through
a path that is not `Submit` — and the hold length is the first thing to check
before the idea is abandoned. If the hold is visible as a stutter worse than the
artefact, it is the wrong trade and gets withdrawn.

**Cost if wrong.** A few withheld frames per change. The runtime is designed for
exactly this, and it is bounded by a counter with an ini switch to disable it.

### D-22 (PARTLY REFUTED — the artefact survived; preflight retained) — Apply presets through CS's transition-profile API, not `SetUpscalePreset`

**Challenges:** nothing decided. It replaces plumbing written when the only
available path was `SetUpscalePreset()`, which was true of PL3.15 and is no
longer true of CSX 3.18.

**The evidence: E-38.** Every governor-applied change on RC3 produced a visible
gridded panel for a split second. It stopped when changes stopped, and it never
happened during calibration. Meanwhile `kTransitionPending` was reported 11
times — we asked CS to change while a transition was already in flight.

**What CS now expects.** Its header states the contract for external
controllers: preflight with
`GetVRUpscalingTransitionProfileDecision(method, renderScaleMode, preset, profile)`,
then act on the answer.

- `kBlocked` — buffer the desired profile and retry later.
- `kNoChange` — settings and the physical render-scale contract already match,
  so the caller **must not schedule a fade**.
- `kApply` — schedule the door fade, then call
  `SetVRUpscalingTransitionProfileForMethod`.

We do none of this. We call the old setter, which changes the render scale with
no fade to hide the relatch — hence the artefact.

**The proposal.** Route every apply through the preflight, and use the fade path
on `kApply`. Three things follow, and only the first is about the artefact:

1. The transition is hidden, as CS intends.
2. `kNoChange` removes applies that were never going to change anything — the
   flash-for-nothing case, at zero cost in quality.
3. Our hand-rolled block/retry plumbing is replaced by the authority's own
   answer, so it stops being ours to keep correct across release candidates.
   This is the direct payoff of the RC plan's step 0: the release notes never
   mentioned this API; the header did.

**Fallback, and why it must stay.** `SetUpscalePreset` remains the path when the
preflight is unavailable — the API is revision-gated, and PL3.15 does not have
it. E-34 applies with full force: the new methods must be called **only** on a
build confirmed to have them, by exact match, never by a revision or build
number being "at least" something.

**Expected effect, stated in advance.** The artefact disappears; the number of
applies falls slightly as `kNoChange` filters no-op changes; the pixel fraction
is unchanged, because this decides *how* a change is made, not *whether*. If
pixel fraction moves materially, something else changed and the result should be
distrusted.

**Refutation condition.** If the artefact survives the fade path, it is not the
missing fade, and the cause is elsewhere — most likely the relatch itself, in
which case this is withdrawn rather than tuned.

### D-21 — Measure GPU time from our own plugin; keep the fork only as the upstream patch

**Challenges:** D-11's choice of *where* to bracket. It does **not** challenge
D-11a — the upstream case stands unchanged, and upstreaming remains the end
state. What changes is the interim.

**What D-11 decided, and why.** Three places can bracket correctly: an OpenXR
API layer, hooking from the plugin, or Community Shaders itself. It chose CS,
and rejected the plugin with one sentence: *"no extra install, but the hook
points are guesswork without local test capability."* That was correct when
written. It is no longer true, and the phrasing hid a distinction that matters.

**Why the objection has lapsed.**

- The hook points are no longer guesswork. They are `IVRCompositor` **vfunc 2**
  (`WaitGetPoses`) and **vfunc 5** (`Submit`) — the two the fork already
  detours, empirically validated across E-18 to E-22.
- They are not "Skyrim's renderer" either. `IVRCompositor` is OpenVR's
  **published vtable ABI**, versioned and stable, and the fork already hooks it
  successfully *through OpenComposite's* `openvr_api.dll` — which is this
  stack's actual implementation. The thing D-11 called risky is the one part of
  the stack with a documented contract.

**Two costs D-11 did not price, both now realised.**

1. **The measurements describe a configuration nobody plays.** Running our fork
   means running a CS the modlist does not ship. Every cost model, step ratio
   and score is then taken against a build the player would never have. D-11
   priced the fork as "a rebase before the PR" and warned the patch "should not
   be left to age for months"; five weeks on, CS has moved three versions, been
   renamed to CSX, and reworked the render-scale path twice.
2. **The version handshake is structurally unsafe.** E-34: two forks both
   answer "revision 4" with different vtables, because build numbers are not
   namespaced. That crashed the game at startup. As long as we ship a private
   fork, every RC is another chance for the same class of collision.

**The decision.** Move the timer into the plugin. `FrameGpuTimer` is already
standalone — `<atomic>`, `<cstdint>`, `<d3d11.h>` and a logger — so this is a
relocation of proven code, not a rewrite. The device and immediate context come
from the texture handed to `Submit`, which is the device that rendered the
frame by construction and needs no renderer-internals lookup.

**Acceptance criterion, stated before the code, as D-13 did.** The plugin-side
bracket must reproduce the fork's agreement with the reference: a residual
against OpenXR Toolkit's `appGPU` of about **−0.2 to −0.3 ms, flat across load
buckets**, on a matched-wall-clock join of several hundred seconds. Anything
load-dependent means the bracket has moved, and it is not a substitute.

**The known risk, named in advance.** The fork opens the bracket at *the first
draw after* `WaitGetPoses` returns, using a signal only available inside CS's
render path. From the plugin we open at `WaitGetPoses` **return** instead. If
the application does not begin drawing promptly, GPU idle falls inside the
bracket — which is exactly the error E-22 removed (+1.18 ms → −0.30 ms). The
criterion above is what detects it. If it fails, the fork remains the fallback
and this decision is withdrawn rather than tuned.

**What stays.** The fork continues to exist as the vehicle for the upstream
patch (D-11a). It is no longer what Rik runs.

### D-20 (PROPOSED, not implemented) — A climb that was just reversed is not re-tried at the same headroom

**Challenges:** nothing previously decided. It adds a missing piece rather than
replacing one. D-9 gave the *frametime* tier a backoff after a failed probe, on
the grounds that a blind climb which did not hold should not be attempted again
straight away. The headroom tier never got the equivalent, because its climbs are
informed rather than blind — and E-33 shows an informed climb can fail just as
repeatably.

**The evidence: E-33.** Three climbs to UltraQuality inside 85 seconds, on
headroom of 2.60, 2.66 and 2.62 ms, each reversed after 15.2 s, 9.2 s and
29.4 s. Six preset changes, six flashes, and the controller ended where it
started. Nothing in it was wrong on its own terms: each climb was justified by
the headroom at the time, and each descent by a real rise in scene cost. What is
missing is any memory that this rung, in this scene, at about this much headroom,
has just been shown not to hold.

**Not a time-based lockout.** The obvious form — "don't re-try for N seconds" —
picks an arbitrary N and gets it wrong in both directions: too short and it
changes nothing, too long and it refuses a climb that has genuinely become
affordable. The observed reversals span 9 to 29 seconds, so no single N is even
descriptive of this one session.

**The proposal, in the spirit of D-18: remember the number, not the clock.**

On a climb to rung X, record the headroom it was taken on. If the controller
descends below X within `climbReversalWindowSeconds`, store that headroom as the
level at which X is known to fail. A later climb to X then requires

    headroom >= failedHeadroom[X] + climbRetryMarginMs

so the rung unlocks itself exactly when the scene has actually improved, rather
than when a timer expires. A `climbFailForgetSeconds` bound, mirroring D-9's
`T_reset`, drops the memory once it is old enough to be about somewhere else.

Proposed: `climbReversalWindowSeconds` 30 (covers all three observed reversals),
`climbRetryMarginMs` 0.5, `climbFailForgetSeconds` 120 (as `probeResetSeconds`).

**Expected effect, stated in advance.** On E-33's session the second and third
climbs would have been refused — their headroom, 2.66 and 2.62 ms, does not clear
2.60 + 0.5. That is **four fewer preset changes** out of eight, with no quality
given up that actually held, and no additional time over budget. It should cost a
small amount of pixel fraction on captures where a re-try would have succeeded.

**This one is checkable in replay, unlike D-19.** The mechanism is decision
logic, not transient measurement, so the replay's blindness to settle latency
does not hide it. Both terms are visible: changes per minute and time-weighted
pixel fraction.

**Refutation condition.** If replay across both committed captures shows a
material loss of pixel fraction — say more than 2% relative — *without* a matching
reduction in changes, the hysteresis is costing quality rather than churn, and
the proposal should be withdrawn rather than tuned.

**Cost if wrong.** The controller becomes sticky: a scene that improves slowly
sits one rung lower than it could until the forget timer expires. That is the
same trade D-9 already accepted on the other tier, and it fails in the safe
direction — fewer frames over budget, not more.

### D-19 (WITHDRAWN — refuted by E-32, code reverted) — A step is only measured once the transition has settled

> **Outcome: implemented, tested against its own pre-registered condition, and
> withdrawn.** The condition below said the proposal should be withdrawn if the
> well-sampled step's ratio did not move toward its observation. It moved
> *away* — 1.146 over 7 observations to 1.184 over 5, against an observed 1.092
> — while costing observations everywhere and leaving the pixel outcome a wash
> (E-32). The code is reverted; D-18's learning stands unchanged.
>
> The argument below is kept in full rather than deleted, because it is a good
> argument that happens to be wrong, and the next person to notice that the
> post-change P95 can be taken inside the settle window deserves to find out
> that this was tried, how, and what it cost.
>
> **What it rules out and what it points at.** The settle transient is not the
> source of the bias. Excluding the first second after a change *raised* the
> measured ratio, which is the signature of load genuinely rising after the
> climb — so the contamination is scene drift across the transition, or a stale
> `_p95BeforeChange`, and the after-half was never the suspect it looked like.


**Challenges:** the learning half of D-18 (and D-17 before it). Not the per-step
table itself, which E-31 vindicates — the table is the reason the residual error
is 5% rather than E-26's factor of four. What is challenged is *when* the second
half of the two-point measurement is taken.

**The evidence: E-31.** `Quality → UltraQuality` is believed 5% dearer than the
capture shows, across 7 observations, on the step the divergence table says is
under-used. Seven observations converging high is not scatter. Every other step
sits on a single observation and lands on both sides, which is what scene
movement does to one measurement.

**The mechanism.** `NotifyApplied` clears the window, and an evaluation may
proceed once `minSamples` = 30 frames are in it — about **0.42 s** at 72 fps.
Settle is **~1.0 s** (E-2). So the post-change P95 can be taken entirely inside
the transition transient. Settle only ever *adds* GPU time, so the error has a
sign: every ratio measured this way is inflated, always. That is the shape E-31
shows — a well-sampled step biased one way while poorly-sampled ones scatter.

Note what this does NOT claim: it is not that the transient is large, but that
its contribution never cancels, so smoothing more observations converges on the
wrong number rather than the right one. More data makes this worse, not better.

**The proposal, and what was built.** Close the two-point calibration only when
the measurement contains no frames from before the transition settled, using the
settle detector the sweep already has rather than a fixed delay — the mechanism
exists, is tested, and measures the thing instead of assuming a constant.

Two details were settled during implementation and are recorded because neither
was obvious from the proposal:

- **The detector is fed GPU time, not frametime.** Frametime is censored flat at
  the compositor cap (E-1), so it holds perfectly still *during* the transient
  and would report "settled" immediately — the detector would agree to
  everything and the decision would be inert.
- **The detector is not sufficient on its own; it needs a floor.** It looks for
  a sustained *quiet* run, so a transient that holds a raised cost **steady** —
  the shape of a history rebuild keeping GPU time high for about a second — is
  maximally quiet and gets declared settled almost immediately. Stability is not
  the same as having returned to baseline, and "measure rather than assume" does
  not rescue a measurement of the wrong property. The measurement now starts at
  the later of the detector's verdict and `_lastChangeAt + 1.0 s`, that floor
  being E-2's measured settle. The detector still decides everything beyond it,
  which is where it earns its place: a transition that takes longer than 1.0 s
  is caught by the detector, and one that merely looks quiet is caught by the
  floor.
- **The P95 is taken over the settled frames only, rather than by waiting for
  the whole judge window to be clear of them.** Waiting is the more obvious
  reading of "no frames from before settle", and it does not work: settle plus a
  full window is about 3.2 s at the shipped values, longer than the 3.0 s
  cooldown, so a controller changing at its normal cadence would have every
  calibration pre-empted by the next change and would learn *nothing at all*.
  Filtering expresses the same requirement without that side effect.

**Expected effect, stated in advance so it can be checked.** The refusal band
11.25–11.80 ms covers ~10% of the light session, so this should recover part of
the 20 unfollowed climbs and some of the 0.083 pixel gap. It should **not**
close that gap: at `landingMargin` 0.0 — a far larger relaxation than correcting
5% — the light capture still only reaches 0.533, and does it at 5.4% over budget.
If a replay after this change shows the gap closing substantially, that is a
reason to distrust the change, not to celebrate it.

**Cost if wrong.** The controller learns more slowly, since some transitions
produce no measurement at all. Against that, D-18's seeds are deliberately
conservative and a step that is never measured keeps one — so the failure mode
is staying at the seed, not adopting a worse number.

**What would refute this.** A replay in which the well-sampled step's learned
ratio does not move toward its observed value once settle frames are excluded.
That would mean the bias comes from somewhere else — the P95 window length, or
`_p95BeforeChange` being stale rather than the after-value being early — and the
proposal should be withdrawn rather than tuned.

**Not yet decided:** the evidence is 7 observations on one step, from two
captures taken on one machine on one evening. That is enough to establish a
sign, not to size a correction. It is written here first because the procedure
above requires it, and because a change to how the controller learns is exactly
the kind that gets made twice if the reasoning is not recorded once.

### D-18 — A measured cost ratio per step, not a fitted `k`

**Supersedes D-17's single learnt `k`.** Same idea — measure rather than assume —
applied to the right quantity.

D-2 allowed for this from the start: *"if the residual is too large, the fallback
is a per-preset measured lookup table, which needs no functional form at all."*
E-27 is that residual. The implied `k` runs from 0.21 to 2.27 across the ladder,
so the linear form cannot describe both ends, and fitting one number to it
guarantees error at whichever end is not being fitted — under-predicting the
cheap rungs, which is the direction that causes E-26's hunting.

**Six numbers, one per adjacent step:**

```
ratio[from → to] = p95_after / p95_before      measured on every adjacent change
predicted landing = p95_now × ratio[current → candidate]
multi-rung        = the product of the ratios along the way
```

No model, no `k`, no clamp range, and the prediction is in the same units as the
thing it predicts.

**Seeded from measurement, at the pessimistic end — and the seed is a starting
point, not a claim.** The defaults are the *larger* of two observed values per
step, so an unlearnt step over-states its cost and the first climb under-reaches.

They were measured on one GPU, one per-eye resolution and one mod list, so on
anyone else's machine they are a guess. The **first real measurement of a step
therefore replaces its seed outright**, and only subsequent ones are smoothed.
Blending from the start would leave 70% of this machine's hardware in a stranger's
estimate after their first transition, which is a prior nobody has any reason to
defend. Smoothing is right between observations of the same thing; it is wrong
between a measurement and a guess.

The practical consequence is worth stating plainly for anyone reading this as a
description of the design: **the table self-corrects within a few transitions**,
and the numbers below are where it starts, not what it believes.

**What it gives up.** A single `k` generalises: one transition anywhere on the
ladder informs every step. The table does not — each step must be seen at least
once, so early in a session it runs on seeds. Given the seeds are measured and
deliberately conservative, that is a better failure than the one E-26 produced.

**How this was decided.** Not by argument: the ratio is stable across sessions
where the `k` fitted to it is not (E-27), and the constrained optimum now lets
both be scored on the same captures rather than compared by impression.

### D-17 (superseded by D-18) — Measure `k`, do not assume it

**D-5 has prescribed this since the beginning** — *"the governor changes presets
anyway, so each change is a free two-point calibration"* — and it was
implemented only for the frametime tier. The headroom tier assumed a constant,
and E-26 is what that cost: fourteen transitions in a session, every one of them
a measurement, all discarded, while the controller hunted on a value that was
wrong by a factor of four.

**On every applied change, solve for `k` from the two observations:**

```
r = p95_after / p95_before
k = (r − 1) / (f_after − r · f_before)
```

then EMA it. Rejected when the arithmetic is unstable or the answer is
implausible — `|Δf|` too small, a non-positive denominator, `k` outside
[0.2, 12] — because a bad estimate is worse than a stale one.

**It starts pessimistic, at `k = 3.0`.** Higher `k` over-states what a rung
costs, so early climbs under-reach. The session averages were 0.61–1.29 and this
one scene showed 5.6; starting at the low end would repeat E-26 for the first
minutes of every session, which is precisely when nothing has been learnt yet.

**Why this rather than raising `margin_up` back to 3.0.** That would restore the
net without fixing the hole: the prediction would still be wrong, it would just
be overruled more often, and the quality left on the table by E-25 would come
back. A controller that measures the thing it depends on is also simpler to
reason about than one carrying a constant that is right for some scenes.

**What it does not fix.** `k` is learnt from the scene you are in, so the first
climb into a genuinely different scene is still a prediction from stale data.
The cooldown and the landing margin remain the protection there.

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

**What the submission must contain, and must not.** The author-facing document
lives in the fork at `docs/development/frame-gpu-time.md` and is written to be
read by someone who owes us nothing. It carries a "What has NOT been
established" section, and that section is not optional:

- the unexplained ~1.3–1.5 ms residual against the reference at low load, and
  the fact that the obvious explanation for it was **checked and disproved**;
- that the reference is a sanity check, not an equality oracle, because its
  bracket, statistic and aggregation all differ from ours;
- that the load-independence claim rests on three samples at the high end;
- that intra-frame idle is inside the bracket, so the reading is an upper bound;
- that single-render-thread execution is assumed.

**Nothing may be presented to the author as verified that an independent review
downgraded.** Two claims in this document have already been withdrawn after
measurement, and one after review; a patch offered upstream inherits that
history whether or not it mentions it. Stating the limits is also the cheapest
way to be trusted about the parts that *are* established — the boundaries, the
query lifecycle, and a signal that separates seven rungs where frametime
separates three.

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

### D-13a — Close the bracket at the submit to the compositor, not at Present

**Supersedes the close boundary in D-13.** Evidence: E-18.

D-13 chose `Present` because everything the application submits is enclosed by
it and the vsync wait is not. The first half is right; the second is not. In
this stack the pacing block sits at the game's **submit to the compositor**,
which happens *before* `Present`, so the wait was inside the bracket after all —
just at the other end from the one being guarded.

```
first draw ────────────── render work ──────────────┐
                                                    │  ← close here (submit)
                        compositor submit / pacing block   ← was inside, must be out
                                                    │
                                                 Present   ← old close boundary
```

**New close boundary:** the game's compositor submit, reached through the
`BSOpenVR` vtable — the same object CS already hooks at slot `0x12` for
`GetRenderTargetSize`, so the mechanism is proven in-repo and needs no new
dependency.

**Open boundary is unchanged.** E-18's correlation of 0.948 says it is right.

**Acceptance is the same test, repeated.** Re-run the join. Success is the
excess falling to well under a millisecond *at low GPU load* — the high-load end
already agrees and proves nothing. Anything that only improves the mean while
leaving the 7–8 ms bucket over-reading by 3 ms has not fixed this.

**If the submit hook proves unreliable**, the fallback is to bracket only what
Community Shaders itself can see — first draw to the end of CS's final
present-stage pass — and re-measure. That under-counts by whatever the game
draws afterwards, which is the opposite error and equally must be measured
rather than assumed.

**Why not simply subtract the offset.** Because it is not an offset. It varies
from +0.85 to +3.95 ms with load, in the direction that destroys the signal:
the correction would be largest exactly where the measurement is needed most,
and it would be fitted to one session in one set of scenes.

### D-13b — The bracket opens where the runtime releases the application

**Refines D-13's open boundary.** Evidence: E-20.

D-13 opened the bracket at the frame's first draw, armed at Present, on the
reasoning that the compositor wait precedes the first draw. E-20 removes the
explanation that let the residual be blamed elsewhere, and leaves the start as
the outstanding suspect: **if the game issues any draw before `WaitGetPoses`
returns, the bracket opens ahead of the pacing block** and counts the wait as
work. That is precisely how a nominally narrower bracket ends up longer than a
wider one.

`IVRCompositor::WaitGetPoses` is where the runtime releases the application, and
its return is this stack's `xrBeginFrame` — the reference's own start. The timer
therefore moves its start timestamp forward when that call returns with the
bracket already open, using the same documented last-`End`-wins rule that lets
the close move per eye.

**It counts how often that happens and logs the share.** "The game never draws
before WaitGetPoses" is a claim, and this is the number that settles it. If the
count is ~0 the start was never the problem and the residual lies elsewhere; if
it is high, this change should move the measurement.

**The acceptance test changes with it**, per the audit's recommendation and
D-10c: the reference is a sanity check on shape and scale, not an equality
oracle, because its bracket, its statistic and its aggregation all differ from
ours. What must hold is that the residual stops depending on load. Exact
equality was never the requirement, and setting it as one was my error.

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
| **4** ✅ | **Fork CS at `eb54a72c`**, add the GPU timer, expose `GetLastFrameGpuTimeUs()` at interface revision 4. | The measurement that makes control correct rather than conservative. | **Closed 2026-08-06 with a stated residual.** The signal is uncensored (E-17), tracks the reference (E-18), and after D-13a carries a near-constant ~1 ms offset instead of a load-dependent one (E-19). It missed the sub-millisecond bar and is accepted anyway: Phase 3 fits thresholds by replaying our own traces, so it works in our units (D-10c), and the reference's job — proving the signal is real — is done |: GPU time separates all seven rungs where frametime separates three, and the 2.16 ms gap at UltraPerformance shows the wait is outside the bracket. External cross-check against the overlay's `appGPU` remains, now for offset calibration rather than verdict |
| **5** | **Controller**, tiered: headroom loop (D-10) when GPU time is present, cost model (D-2/D-3) when not. Parameters chosen in CI by replaying recorded traces. | Both tiers must work; the first shipped version runs against unmodified CS. | Phase 3 simulation passes on all captured traces |
| **6** | **Shadow mode**, then live. | First live run must not also be the first test. | Phase 4 and 5 pass |
| **7** | **Upstream the CS patch** (D-11a), with this document and the measurements. | Removes the fork from the distribution path entirely. | Patch offered; governor ships against stock CS. Author-facing rationale is already written — `docs/development/frame-gpu-time.md` in the fork — and the fork-only build workflow must be dropped from the PR |

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

| E-45 | **We are in the right place in the chain, holding the right texture — and were capturing it one frame too late.** The install-time diagnostic answers two structural questions at once: *"the Submit we chain to belongs to `openvr_api.dll` — we are BEHIND (good) it"*, so CS patched after us and the order is CS → us → runtime; we see and replay CS's **final** frame. And `submit flags 0x0`: no pose, no depth, a plain `Texture_t`. Both standing hypotheses dead. The capture is also demonstrably correct — eye 0 `[0.000,0.000]-[0.500,1.000]` and eye 1 `[0.500,0.000]-[1.000,1.000]` on the same `6988×3558` combined texture. What remained unchecked was whether the captured frame was *good*: the capture was taken on the first submit **after** the change was requested, and E-40 established only that those frames have normal frametime, which is not the same as valid content. If CS begins tearing down its targets at once, we copy the relatch and then replay it faithfully — a steady panel for the whole window rather than a flash, which is exactly what was reported. Fixed by splitting `ArmCapture` from `HoldFrames` and deferring the change until a capture exists. | `CSQualityGovernorVR.log`, session 2026-08-11 20:02 |
| E-44 | **A replayed frame must carry its own bounds; they belong to the frame, not the eye.** With the per-arming capture fixed, transitions were clean — *"a tiny freeze followed by a visible quality change, no black spot, no comic page"* — and then the pattern returned partway through the session and stayed for every change after. It was not the caller: calibration and governor both apply through the same `LiveCSApi::SetPreset`. It was the layout switch (E-43). Our copy was being submitted with the **current** frame's `VRTextureBounds_t`, and the relatch is precisely what flips the submission between per-eye and double-wide — so during a hold we paired a pre-relatch per-eye texture with post-relatch half-width bounds. Fixed by capturing `bounds`, `eType` and `eColorSpace` with the pixels and replaying all of them, taking nothing from the current frame. **The pattern across E-42 to E-44 is one mistake repeated three times: treating part of a submit as constant when the whole submit is a per-frame parameter** — first the descriptor, then the geometry, then the bounds. | Player report + log, session 2026-08-10 21:2x |
| E-43 | **The submitted texture geometry is not constant, and a stale copy is the wrong shape rather than an old picture.** The capture logging added after E-42 shows CS submitting **3494×3558 per-eye** textures at one point and a **6988×3558 double-wide** atlas later in the same session, with bind flags changing `0xA8` → `0x28`; in the double-wide case `bounds` select each eye's half. It also exposed the defect behind the black right eye: the capture-complete test was `g_held[0].valid && g_held[1].valid`, and `valid` persists from the previous change — so the flag cleared after the **first** eye was captured and the second kept a copy from an earlier hold. When the layout had changed in between, the right eye replayed a per-eye texture while being submitted with double-wide right-half bounds, which is a black half. Fixed with a per-arming captured flag, distinct from "a texture exists", and both eyes forced to re-capture on every hold. **The general error: I treated the submitted texture as a fixed property of the session when it is a per-frame parameter.** | `CSQualityGovernorVR.log`, session 2026-08-10 21:00 |
| E-42 | **A substitute frame has to match the original's description exactly, sharing flags included.** With the copy-and-replay form of D-23, the counters read **184 submits replayed, 0 withheld** — the mechanism ran perfectly — and the artefact got *worse*: the player reported a view "divided into rectangles, like a comic book page" plus a black region in the right eye at the start of the window. That is a texture being read with the wrong memory layout, and the cause was mine: the copy was created with `MiscFlags = 0` and hand-picked `BindFlags`, on the reasoning that we only needed something the compositor could read. OpenComposite passes these textures on to an OpenXR runtime, which needs the sharing flags the game created them with. Fixed by taking the source `D3D11_TEXTURE2D_DESC` verbatim. The general rule this cost us a session to learn: **a copy handed back in place of an original must BE the original in every respect the receiver can observe** — matching what `CopyResource` demands is not the same as matching what the consumer requires. | Player report + `transition hold: 184 replayed, 0 withheld`, session 2026-08-10 20:39 |
| E-41 | **OpenComposite does not reproject a skipped submit — it shows black.** D-23's first form simply did not call through to `Submit` during the relatch, on the assumption that a runtime with nothing new to show reprojects what it showed last. It does not: the player reported *"black flashes like the white ones but black"*. So the gridded panel became a black one — no better, arguably no worse, and not the frozen image intended. The assumption was flagged as the risk before the run and is now measured. Revised to capture the frame once when a change is armed — still a valid old-preset frame at that point (E-40) — and hand that copy back for the hold, so the runtime always receives a frame and reprojects it normally. One copy per change, not the ~7 GB/s a speculative per-frame copy would cost. | Player report, session 2026-08-10 20:07 |
| E-40 | **The relatch is the same event on RC2 and RC3; only its appearance changed — and it is 10 frames, not 6.** Measured around applied preset changes in both captures. RC2: stalls of 78 and 94 ms, then a frame doing **76.9 and 83.6 ms** of GPU work, back to normal frametime after 9-10 frames. RC3: stalls of 77-91 ms, spike **66.5-86.1 ms**, normal after 10-11. Identical signature, which confirms from our own data what the source said: PL3.15's white flash was `ClearHMDMaskCS.hlsl` clearing the hidden-area mask to the wrong value, painting over a gap that was always there. Counting frames from the apply: **1-3 are still valid old-preset frames, 4-6 are the stall and the relatch, 7-10 are valid but slow.** D-23's hold was set to 6 by guesswork and corrected to 8 on this measurement — enough to cover the bad frames with margin, not so much that it withholds the recovery. | `20260810_091513_frames.csv` (RC2), `20260810_160355_frames.csv` (RC3) |
| E-39 | **RC3's cost curve is much steeper than RC2's, and D-21's timer is what showed it.** First honest 4-sweep calibration on CSX 3.18 (2 000 deduplicated samples per rung): UltraPerformance **10.28 ms** → NativeAA **19.48 ms**, fitting `t_fixed` **5.00 ms**, `t_scaled` **13.76 ms**, **k = 2.75**, worst residual 0.72 ms. Against RC2's 1.29 (light) and 0.95 (marginal), **k has roughly doubled**: RC3 is far more resolution-sensitive, so a rung is worth more and the ladder is a stronger lever than it was. Also confirms the sweep-count decision empirically — the same capture run at `Sweeps = 1` fitted `t_fixed = −0.18 ms`, a negative fixed cost, purely from the ladder-position bias that serpentine needs an even count to cancel. | `20260810_160355_frames.csv` |
| E-38 | **We drive CS 3.18 through an API it has moved past, and it shows on screen.** With `ApplyGovernor = 1` on RC3, the player reported a flat gridded panel flashing for a split second **on each governor preset change**, absent during the calibration sweep and absent at the end of the session. The timeline matches exactly: 8 applies between t=286 s and t=440 s, then every later decision **deferred** — so no preset actually changed after 447 s and the artefact stopped. Block reasons observed across the run: `0x0` ×820, **`0x4` kLoadingMenu ×43**, **`0x10` kTransitionPending ×11**, `0x8` kRelatchPending ×1. Our `SetPreset` calls `SetUpscalePreset()`, the PL3.15 path, while CSX 3.18 documents that external controllers should preflight with `GetVRUpscalingTransitionProfileDecision` and, on `kApply`, *"schedule the existing door fade and immediately call `SetVRUpscalingTransitionProfileForMethod`"*. We schedule no fade, so the un-faded intermediate frame is visible; and `kTransitionPending` ×11 shows we asked while a transition was already in flight. The player's own diagnosis — *"like in cs2 but in cs3 that causes a side effect"* — was correct. | `20260810_160355_timeline.csv`, `_apistate.csv` |
| E-37 | **The plugin-side hook works; a flag meaning two things crashed the game anyway.** Third startup crash on RC3, and the log shows the hook succeeding exactly as designed: *"compositor found: IVRCompositor_022"*, *"hooked WaitGetPoses (slot 2) and Submit (slot 5) after 2 attempt(s)"*, *"GPU timing is live"* — then an access violation at that same timestamp, stack `CommunityShaders.dll ← CSQualityGovernorVR.dll ×3`. Cause: `ApiSnapshot::Capture(g_CSInterface, g_gpuTiming)`. `g_gpuTiming` means *"we have GPU timing from somewhere"*; `Capture` reads its argument as *"the CS interface has the timing methods"* and calls them. Our own timer going live flipped that flag true and the next snapshot called `GetLastFrameGpuTimeUs` on build 11. **This is E-34's root cause at a second call site**, and it survived the E-34 fix because that fix hardened the capability *test* while leaving a caller that never consulted it. Tally across the three RC3 crashes: **two were this same one-flag-two-meanings error, one was the interface-version walk (E-36); none were the hooking mechanism**, which the log shows working on its second attempt. | `crash-2026-08-10-15-30-22.log`, `CSQualityGovernorVR.log` |
| E-36 | **An OpenVR interface version is not a version number, it is a different object.** D-21's first working hook crashed the game at the main menu. `FindCompositor` walked interface versions newest-first and took whatever answered; OpenComposite exports up to `IVRCompositor_024`, so it returned a **024 wrapper**, and slots 2 and 5 were patched on that. Wrong twice: vtable indices do not carry the same meaning across interface versions, and the game never calls through that object, so nothing would have been measured even had it survived. Scanning `SkyrimVR.exe` shows it references exactly one compositor version — **`IVRCompositor_022`** — which is also the interface the fork's slot 2 / slot 5 were validated against. Now requested by name. The general lesson is E-34's, arriving from the other direction: a version number that is not an exact, checked match is not evidence, whether it comes from a fork's build number or from an ABI string. | `SkyrimVR.exe` string scan; crash at menu on `a1a611e` |
| E-35 | **What CSX 3.18 changed, read from source rather than release notes.** MGO 4.0beta RC3 ships **CSX 3.18-VR** (`csx-3-VR`, archive `2026-08-09T14-05Z`, build 11) — the Particle Lights fork renamed to "Community Shaders Expanded", same Nexus 166950. Four findings, each of which would have cost a session to learn the hard way. **(1) The ladder is unchanged:** `GetQualityModeResolutionScale` returns exactly our seven values with the same internal indices, so every pixel fraction, the cost model and the optimum survive the move; `VerifiedCsBuild` is therefore 11. **(2) CS still does not measure GPU time.** Their own `VR_PERFORMANCE_HANDOVER.md`: *"This is a static code analysis, not a profiler capture… must be validated with RenderDoc, Tracy, or the in-game performance overlay."* The overlay is for humans, not an API — so there is nothing to defer to and D-21 stands. **(3) They added a preflight FOR external controllers:** `GetVRUpscalingTransitionProfileDecision()` returning `kBlocked` / `kNoChange` / `kApply`, documented *"External transition controllers should query this before applying VR upscaling profiles"*, where `kNoChange` means the caller **must not schedule a fade**. That supersedes our hand-rolled block/retry/redundant-apply plumbing and directly removes flashes that achieve nothing. **(4) Foveation reduces shader work per pixel, not pixel count** — *"Center: full current quality… Periphery: reduce or skip only expensive detail terms"* — so `f = scale²` remains valid, but `t_scaled` moves with it, which makes foveation settings a confound that must be held fixed across a measured session. | `csx-3-VR` source; `VR_PERFORMANCE_HANDOVER.md`; `MGO-Presets/CSX NVIDIA- Quality/SettingsUser.json` |
| E-34 | **Two forks both call it "revision 4", and our capability test crashed the game.** MGO 4.0beta RC3 ships **CSX 3.18-VR** — the Particle Lights fork renamed to "Community Shaders Expanded", same Nexus 166950 — reporting **revision 4, build 11**. Their revision 4 adds `GetVRUpscalingTransitionProfileDecision()`; **ours** adds `GetLastFrameGpuTimeUs()`. Same number, different vtables, and CSX 3.18 has no GPU-timing method at all (confirmed by reading `csx-3-VR` on GitHub, not by guessing from the binary). `GpuTimingAvailable()` tested `revision >= 4 && build >= 9`, build 11 passed, and the first call jumped off the end of their vtable: `EXCEPTION_ACCESS_VIOLATION at CommunityShaders.dll+0FB2E38`, "tried to execute memory", with `RCX = (CSPluginAPI::CSInterface001*)` — a virtual call on the interface. Reproduced twice, both at the logo. **Phase 0's build check fired correctly and refused to apply, then the same unverified build was trusted for a vtable call** — the guard protected the pixel fractions and not the crash. Fixed by requiring the build to MATCH `VerifiedCsBuild` exactly rather than exceed it; two forks can agree on a number by accident, and only an exact match against a build somebody checked is evidence. | `crash-2026-08-10-13-08-53.log`, `CSQualityGovernorVR.log` |
| E-33 | **First real live run of the actuator: the controller is correct, and it re-tries a rung that just failed.** Session 2026-08-10 09:15, governor holding the lever from 170 s to 311 s, **8 applied changes, no circuit-breaker trip**, every decision explicable from its own logged numbers. It climbed Quality → UltraQuality three times — at 170.4 s, 203.4 s, 223.7 s, on headroom of **2.60, 2.66 and 2.62 ms** — and was driven back down each time after 15.2 s, 9.2 s and 29.4 s. **The landing check was not at fault:** it predicted "landing ~12.66 ms" and the measured p95 was 12.51 ms within one second and flat at ~12.6 ms for ten seconds, accurate to about 0.05 ms. The reversals came from the scene genuinely getting dearer — 12.69 → 12.76 → 12.90 → 13.01 → 13.53 → 14.14 ms over six seconds. This also **refutes my own guess** that the descents were the controller reacting to its own transient; the post-change measurement was stable and correct. Ending state: 58 s held at Quality with p95 11.96–13.09 ms and headroom 0.80–1.93 ms, below `marginUpMs` 2.5 — a deliberate hold, not a stall. | `20260810_091513_timeline.csv` |
| E-32 | **D-19's own refutation condition fired: excluding settle frames moved the best-sampled belief FURTHER from the truth.** Replay after implementing it, against E-31's figures. Light capture `Quality → UltraQuality`: **1.146 over 7 observations → 1.184 over 5**, against an observed 1.092 — error +5% → **+8%**, in the wrong direction. Marginal: 1.129 over 2 → 1.080 over 1, closer but now resting on a single transition. Observations were lost across the board — the marginal capture's `Performance → Balanced` stopped being measured at all and fell back to its seed. Pixels: light **0.504 → 0.499**, marginal 0.568 → 0.572; a wash, and the light capture's gap widened from 0.083 to 0.089. **The settle transient is therefore not the source of the bias.** The direction is itself informative: excluding the first second after a change *raises* the measured ratio, which is what happens when load genuinely rises after the climb — so the contamination is scene drift across the transition, or a stale `_p95BeforeChange`, not a transient in the after-half. Honest limit: 5 against 7 EMA-weighted observations cannot separate "refuted" from "underpowered", and the pre-registered rule was acted on rather than reinterpreted. | CI run on `9447744`, both captures |
| E-31 | **The one step ratio with real evidence behind it is biased high, and the poorly-sampled ones are just noise.** Learned ratios at the end of a replay, against the same step measured from the dwell buckets: `Quality → UltraQuality` reads **1.146 believed vs 1.092 observed on 7 observations** (light) and **1.129 vs 1.034 on 2** (marginal) — pessimistic in both, and the only step that is. Every other step rests on **exactly one** observation: Perf→Bal 1.056 vs 1.114 and 1.024 vs 1.139, Bal→Qual 1.047 vs 1.087 and 1.165 vs 1.116 — scattering both directions, which is what a single transition contaminated by scene movement looks like. Averaging seven cancels the scene and leaves what is systematic. Consequence: with the gate at `budget − landingMargin` = 12.89 ms, believing 1.146 instead of 1.092 refuses every climb whose P95 lies in **11.25–11.80 ms**, about **10%** of the light session — and `Quality → UltraQuality` is exactly the step the divergence table shows under-used (20.9% of intervals against the optimum's 40.2%). | CI run on `3f226d5`, both captures |
| E-30 | **The optimum was never reporting the value its own table had found — the trajectory was reconstructed by guesswork.** With E-29's exact DP in place the monotonicity check still failed, and wider: **0.382** at a 1.0 s dwell against **0.410** at 5.0 s. Cause, present since the first version and therefore underneath E-28's published figures too: the backpointer stored only the predecessor's *preset*, and the dwell counter was re-derived as `held == 0 ? dwell : held - 1`. That derivation is not unique — `nextHeld = min(held + 1, dwell)` means a state at `held == dwell` has two possible predecessors, `dwell - 1` and `dwell` itself, already capped. On a wrong guess the walk lands on a cell that was never written, breaks out, and leaves the whole earlier prefix at preset index 0 — the cheapest rung — so the reported figure sat below the optimum the table had actually computed, by an amount varying with the dwell. Now stores the full predecessor state; nothing is re-derived except the budget, which genuinely is unique. **Lesson: two of these three defects were in the reporting path, not the search.** A DP that computes the right answer and then misreads its own table is indistinguishable, from the outside, from one that computes the wrong answer. | CI run 31324364163, commit `adc7b70` |
| E-29 | **Pricing the miss allowance cannot solve this problem exactly — the constraint has to be carried.** With E-28 fixed, the monotonicity check failed: a **1.0 s** dwell scored **0.392** where a **5.0 s** dwell scored **0.401**, though every trajectory available to the slower solver is available to the faster one. Cause: penalty bisection is Lagrangian relaxation of an integer programme and has a duality gap. At a given price the DP returns *some* optimum of `pixels − λ·missed`; with more freedom it can step straight past the allowance into the interior of the feasible region, spending fewer permitted misses and taking fewer pixels with them, and no λ enumerates the skipped points. Replaced with an exact DP carrying consumed misses as a third state dimension, so the constraint binds by construction. The flat synthetic trace had too few distinct trade-off points to land in the gap; `VaryingSweep` exposed it immediately. Also retired the companion assertion `tight.changes <= loose.changes` — it is not a theorem, since a near-constant trajectory can be optimal under the loose dwell and use fewer changes than the tight optimum. | CI run 31319876314, commit `654da2f` |
| E-28 | **The first published constrained optimum was not a bound, and the report said so on its face.** CI run 31317210118 ranked the best parameter row at **108% of optimum** on the light capture and **110%** on the marginal one — a controller above its own ceiling, which is arithmetically impossible and therefore a defect in the optimiser. Cause: `ComputeOptimal` counted an interval as over budget when its windowed **P95** exceeded the budget, while `Replay` counts individual **frames**. A window whose P95 only just fits still misses one frame in twenty, so the same nominal 2% allowance bought the optimiser almost no slack and the controller a real one. Both now count frames. The bad figures were 0.470 (against an achieved 0.506) and 0.517 (against 0.570); **no conclusion had been drawn from them**, which is the only reason this is an erratum and not a retraction. The synthetic test could not have caught it: `SyntheticSweep` gives every frame at a preset an identical GPU time, so P95, mean and each frame coincide and the two definitions are indistinguishable. A `VaryingSweep` with real within-preset spread now backs the invariant. | CI run 31317210118, commit `41cdab9` |
| E-27 | **`k` is not a property of the scene — it varies 10× across the ladder, while the per-step ratio is stable.** Implied `k` per adjacent step, two sessions: 2.18/1.76, 2.27/1.35, 1.98/1.56, 0.26/1.35, 0.94/0.21, 0.32/0.35 — from 0.21 to 2.27, so no single value fits both ends of the ladder and the linear cost model misfits by construction. The same transitions expressed as **cost ratios** repeat across sessions: Balanced→Quality 1.116 vs 1.100, Hoshipa→NativeAA 1.072 vs 1.077, Perf→Balanced 1.139 vs 1.097. | Sweep dwells, sessions 2026-08-08 14:24 and 16:34 |
| E-26 | **The assumed `k` is wrong by a factor of four in a resolution-bound scene, and it makes the controller hunt.** Standing still, the second live session cycled Quality→UltraQuality→Quality four times. Each climb predicted a landing near 12.65 ms and actually landed at 13.97–14.85, so it descended immediately. The implied `k` for that step is **≈5.6** against the assumed **1.3**. With `margin_up` at 2.5 the landing check is the only gate (E-25), so a wrong `k` translates directly into oscillation — the failure mode the safety net had been hiding at 3.0. | Session 2026-08-09 14:01, and the player standing still while it flashed |
| E-25 | **`margin_up` is inert below 2.0 — the landing check binds first.** Re-swept across both captures with D-16 active, every value from 2.0 down to −1.0 produces an *identical* replay (marginal: f=0.568, 1.1% over, 3.73 changes/min). Above that it acts purely as a conservatism limiter: 2.5 gives f=0.564/0.513 at 1.0%/1.8% over, and 3.0 gives f=0.546/0.495 at 0.9%/1.2%. At 2.0 the light capture **breaches the 2% constraint** (2.4%), so the limiter is still load-bearing and cannot simply be deleted. Set to **2.5**: ~3.5% more pixels than 3.0 for ~30% more changes. | CI replay report, both captures |
| Q-11 | ~~**Is `margin_up` now redundant, and costing quality?**~~ **Answered by E-25**: redundant as a climb *criterion* below 2.0, still load-bearing as a safety limiter above it. Original text: | With D-16's landing check the controller asks the climb question twice: "is there spare capacity now" (`margin_up`) and "will it still fit after paying for the rung" (the prediction). The second is the real question and the first is a proxy for it. Measured on the first live session, **16–25% of hold decisions would have climbed safely by the landing test** and were blocked by the threshold, worth about +0.02 pixel fraction — 5–7% more pixels. | Re-sweeping `margin_up` in CI against both captures, which now exercises the landing check that did not exist when 3.0 was chosen. No new session needed: a smaller margin is safer than it was, so the old answer may not survive |
| Q-10 | **Is there a *leading* indicator of scene cost — interior/exterior, weather, actor count — worth feeding the controller?** | **Considered and declined, 2026-08-08.** Measured GPU time is already the best *lagging* indicator, and a second signal would have to either predict a cost change or separate a transient from a structural one; merely correlating with cost adds state without adding information. The case that prompted the question — two changes in eight seconds around a doorway — turned out to be the controller responding correctly to a real scene change, so there is no failure for prediction to fix. Telemetry for it was written and deleted unused. Revisit only if a failure appears that a leading signal would have prevented |
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

**2026-08-10 — First live run of the actuator (E-33). The controller is correct;
D-20 proposed for the one thing it lacks. No code written.**

Governor holding the lever for 141 seconds: 8 applied changes, no circuit-breaker
trip, every decision explicable from its own numbers. The landing check was
vindicated — it predicted a 12.66 ms landing and got 12.51 ms within a second,
flat for ten seconds. The player's report of "flash, and back again" was the
controller tracking a scene that genuinely got dearer by 1.5 ms in six seconds,
and the session's final state — 58 s held at Quality on 0.80–1.93 ms of headroom
against a 2.5 ms climb threshold — was a deliberate hold, not a stall.

This also refutes a guess I had put to the player the day before: that those
descents might be the controller reacting to its own transient. The post-change
measurement was stable and accurate. It was the scene.

What is missing is any memory of failure. Three climbs to the same rung in 85
seconds, on 2.60, 2.66 and 2.62 ms of headroom, each reversed. D-9 gave the
frametime tier a backoff after a failed blind probe; the headroom tier never got
one, on the reasoning that its climbs are informed. E-33 shows an informed climb
can fail just as repeatably.

D-20 proposes remembering the headroom a rung failed at rather than starting a
timer, so it unlocks when the scene actually improves. Written up first, with its
expected effect and refutation condition, per this document's procedure — and
noting that unlike D-19 this one *is* visible in replay, because it is decision
logic rather than transient measurement.

**2026-08-09 (later still) — D-19 withdrawn and reverted; its refutation
condition fired (E-32).**

The replay it was waiting for went the wrong way. `Quality → UltraQuality` on
the light capture moved from 1.146 over 7 observations to **1.184 over 5**,
against an observed 1.092 — the best-evidenced belief got *worse*, not better.
Observations were lost across both captures, one step falling back to its seed
entirely, and pixels were a wash (0.504 → 0.499 light, 0.568 → 0.572 marginal).

So the settle transient is not the source of the bias. The direction says where
to look next: excluding the first second after a change **raises** the measured
ratio, which is what happens when load genuinely rises after the climb. That
points at scene drift across the transition, or at `_p95BeforeChange` being
stale — the before-half, not the after-half. Both were listed in D-19 as
alternatives and neither has been tested.

The rule was written in advance precisely so this outcome would not be
reinterpreted, and it was acted on rather than tuned around. What cannot be
claimed is a clean refutation: 5 against 7 EMA-weighted observations cannot
separate "the hypothesis is wrong" from "the test is underpowered". A change
that costs observations while showing no benefit is the worse thing to keep,
which decides it on cost rather than on proof.

The argument is kept in the decision, marked withdrawn, rather than deleted.

**2026-08-09 (earlier) — D-19 approved and implemented.**

Calibration now waits for the settle detector, fed GPU time, and takes its P95
over the settled frames only. Two things surfaced in the building that the
proposal had not anticipated, both recorded in D-19 itself:

Feeding the detector *frametime* would have made the change inert — frametime is
censored flat at the cap (E-1), so it is at its quietest precisely during the
transient. And requiring the whole judge window to be free of pre-settle frames,
which is the obvious reading of the proposal, would have delayed every
calibration to ~3.2 s against a 3.0 s cooldown: a controller changing at its
normal cadence would have learned nothing whatever. The measurement is filtered
instead.

Tested with a transition that costs 15% extra for a second on top of a rung that
truly costs 20%: measured through the transient the ratio reads about 1.38, and
the test requires 1.20. A second test pins the accepted cost — GPU time that
never holds still teaches nothing, and the step keeps its seed.

**The first implementation failed that test at 1.3799 — the exact
measured-through-the-transient value — and the failure was worth more than the
pass would have been.** The settle detector looks for a sustained *quiet* run,
and the test's transient is a steady elevated plateau, which is maximally quiet;
it was declared settled about 0.2 s in. Stability is not a return to baseline.
So the decision as originally argued was insufficient on its own, and a floor of
E-2's ~1.0 s was added beneath the detector. Had the test used a decaying spike —
the shape I first reached for, and the more flattering one — it would have passed
and shipped a mechanism that any plateau-shaped transition defeats.

**Expected effect still stands as written and has NOT yet been checked against a
replay:** this should recover part of the gap and must not close it.

**2026-08-09 — D-19 proposed, awaiting a ruling: learn a step ratio only after
the transition has settled (E-31). No code written.**

Instrumenting what the controller believes a rung costs, beside what the capture
shows, found one step with real evidence behind it and a bias on it.
`Quality → UltraQuality` reads 1.146 against an observed 1.092 over **seven**
observations on the light capture, and 1.129 against 1.034 over two on the
marginal one. Every other step rests on a single observation and scatters both
ways, so those are noise and were nearly read as findings before the observation
counts were added.

The mechanism is that the post-change P95 can be taken ~0.42 s after a change
(`minSamples` 30 at 72 fps) while settle takes ~1.0 s (E-2), so the measurement
lands inside the transient. Settle only adds GPU time, so the error has a sign
and never cancels — which is why the well-sampled step is biased while the
one-shot steps are merely noisy. More observations converge on the wrong number.

Consequence: with the gate at 12.89 ms, the inflated belief refuses every climb
whose P95 falls in 11.25–11.80 ms — about 10% of the light session, on the step
the divergence table shows most under-used (20.9% of intervals against the
optimum's 40.2%).

**Not implemented.** The evidence is seven observations of one step from two
captures on one machine on one evening: enough to establish a sign, not to size
a correction. Written up first because this document requires the argument
before the code, and recorded with its expected effect and its refutation
condition so the follow-up cannot be graded on a moving target.

**2026-08-06 (later) — The external comparison found what internal evidence
could not (E-18). Close boundary moves to the compositor submit (D-13a); step 4
reopens.**

Joined against the toolkit's own log — 382 matched seconds, correlation 0.948 —
our GPU time reads high by +2.32 ms on average, and the excess scales with
*available headroom*: +3.95 ms when true GPU work is 7–8 ms, +0.85 ms when it is
15–16. Our reading cannot go below ~11.5 ms; theirs reaches 7.41. As headroom,
ours 5.7% against theirs 22.4%.

The cause is the close boundary. The pacing block sits at the game's submit to
the compositor, before `Present`, so the wait D-13 was written to exclude was
inside the bracket at the other end. D-13's open boundary is vindicated by the
correlation; its close boundary is superseded.

Two lessons worth keeping. First, the internal argument for D-13 was sound and
insufficient: "GPU time differs from frametime at the cheap presets" ruled out
the whole wait being enclosed, and said nothing about *part* of it being
enclosed. Second, the caveat in D-13 called this error "conservative" and
therefore acceptable. It is not: a headroom signal that saturates at ~2 ms of
apparent spare capacity cannot climb, and would strip quality precisely in the
scenes that could afford more.

The user's report of 34–39% overhead, against our best reading of 7%, is what
prompted the check. Repeatedly now, the direct observation has been right and
the derived number wrong.

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
