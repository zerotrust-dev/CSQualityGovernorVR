# Hot-Envelope proof of concept for CSX VR Render Scale Mode

**Audience:** ParticleTroned, and whoever maintains this after us.
**Status:** plan, written before any code. Results will be appended here.
**Build under test:** CSX **3.18-VR** (tag `CSX3.18`, `2051e2ae`), API build 11.
**Environment:** MGO 4.0 beta RC3, Pimax Crystal Super, 72.000 Hz, DLSS,
OpenComposite Unleashed 4.2.3.

---

## 1. What this is, and what it is not

You said Render Scale Mode is still work in progress and suggestions are welcome.
This is us taking that seriously enough to do the work rather than only describe
it: a patched build of CSX 3.18-VR that demonstrates one specific change, with
measurements on both sides of it.

**It is not** a request that you adopt our code, a claim that the current design
is a mistake, or something we intend to ship. We will build it, measure it, hand
you the diff and the numbers, and go back to stock. If the demonstration shows
the idea does not work, that is a useful result and we will say so with the same
detail.

We drive quality changes from an external SKSE plugin, through the transition
API you added — `GetVRUpscalingTransitionProfileDecision`, then
`SetVRUpscalingTransitionProfileForMethod`. That API is what makes any of this
possible from outside, and the preflight in particular saved us a great deal of
guesswork.

---

## 2. The measurement that motivates it

Two sessions on the same route, 45 minutes apart, `renderScaleMode` read from
`SettingsUser.json` for both. Sweep dwells, deduplicated by GPU-timer frame
identity, nearest-rank P95:

| preset | Render Scale **on** | Render Scale **off** | cost of turning it off |
|---|---|---|---|
| UltraPerformance | 8.65 ms | 11.02 ms | **+2.37** |
| Performance | 10.81 ms | 13.28 ms | **+2.47** |
| Balanced | 12.65 ms | 14.28 ms | **+1.63** |
| Quality | 13.71 ms | 15.40 ms | **+1.69** |

Against a 13.889 ms budget, that is a large amount of headroom. The saving also
**shrinks as quality rises** — 22% at UltraPerformance where targets are 11% of
full size, 11% at Quality where they are 44% — which is what allocating smaller
targets must do, and is a good sign the numbers are real rather than noise.

The consequence for a governor that changes quality during play:

| | Render Scale **on** | Render Scale **off** |
|---|---|---|
| time-weighted pixel fraction | **0.320** | 0.216 |
| frames later than 1.05× budget | 4.7% | 5.2% |
| frames beyond **two** display periods | **1.35%** | 0.03% |
| worst frame within 0.5 s of a change | 59 ms | 34 ms |

**85% of those severe frames sit within 0.7 s of a preset change.** So the two
configurations are each incomplete: with the feature on we get the quality and
pay a relatch on every change; with it off the frame delivery is clean and the
quality is gone. The controller is identical in both.

**A correction, so you can weigh our numbers properly.** We first measured this
feature as having *no detectable benefit* (+0.12 ms). That comparison was wrong —
we had not verified the setting, and both arms were almost certainly render-scale
on. The tell was our own control: NativeAA, where the feature can do nothing by
construction, showed a 0.88 ms spread. We now read the setting from disk for
every session. We would rather tell you this than have you discover our method
was loose.

---

## 3. Analysis

`Features/Upscaling/PerfMode.cpp`, `UpdateRestartRequiredState`:

```cpp
VRPerfModeRestartState::Refresh(
    restartRequired,
    ActiveBootContractInputs{
        .bootActive = boot.active,
        .requestedNow = requestedNow,
        .displaySizeChanged = displaySizeChanged,
        .eligibleNow = eligibleNow,
        .methodMatches = boot.method == a_method,
        .qualityModeMatches = boot.qualityMode == qualityMode,
    });
```

Because Render Scale Mode replaces the runtime's recommended render-target size
with `HMD size × quality scale`, Skyrim allocates its physical targets at the
boot quality's input resolution. A quality change therefore changes texture
dimensions, and `qualityModeMatches` correctly reports that the latched contract
no longer holds — which cascades into
`RecreateRenderTargetsForVRRenderScale`, `globals::ReInit()` and a rebuild of
every render-target-dependent feature.

The observation we want to put to you is narrow:

> When the new quality is **lower** than the boot quality, its render dimensions
> **fit inside the targets that are already allocated.** Nothing needs
> reallocating; only the logical extent rendered into them needs to change.

That makes the boot quality an **upper bound** rather than a fixed point.
Everything at or below it becomes selectable during play; anything above it still
needs a real relatch and can be deferred to a loading screen.

### Why we no longer think DLSS's dynamic range blocks this

We raised this in our first message and want to retract it. It would matter if
one DLSS context had to accept a 3× span of input sizes. CSX does not do that —
it keeps a viewport context per `qualityMode + dlssPreset`, so each context is
created at its own input resolution and `extentIn` matches its optimum. Streamline
tags resources with extents precisely so the input may be a sub-rect of a larger
texture; texture size and DLSS input size are independent.

Empirically: with Render Scale Mode on we routinely see UltraQuality, Quality,
Balanced, Performance and UltraPerformance all working. Those are five contexts
at five input sizes. The DLSS side already does what this needs.

---

## 4. What we will build

Minimal, and reusing machinery that already exists rather than adding a rendering
path.

**1. Rebase our fork onto `CSX3.18`.** It currently sits on a PL3.15 branch. No
patch is meaningful until it applies to the code that actually ships.

**2. Make the relatch condition a fits-check.**
`qualityModeMatches` becomes something closer to
`renderSizeFitsAllocation`: the latched contract holds while
`ScaleDimension(displayEye, activeScale) <= boot.renderEye` in both axes. A
quality above the envelope still sets `restartRequired`, unchanged.

**3. Drive the sub-rect through the existing dynamic-resolution path.** For a
quality below the envelope, `ConfigureUpscaling` uses
`ApplyDynamicResolutionState` with

```
resolutionScale = activeScale / bootScale
```

instead of `ApplyLockedFullResolutionDynamicResolutionState`. Both functions are
already in the file.

**4. Viewport slots, if needed.** Two slots per role against five presets in
rotation means eviction on nearly every change. If that becomes the dominant
hitch once the relatch is gone, we will try raising the count and report the VRAM
cost rather than guessing at a number.

**5. Choose the envelope.** For the demonstration, boot-latch at Quality
(scale 0.667). That covers ~99% of our observed play and keeps most of the
allocation saving. NativeAA and Hoshipa become loading-screen-only, which for us
is no loss — they measure 21.9 ms and 18.4 ms against a 13.889 ms budget.

---

## 5. How we will verify it

Stated before building, so the result cannot be chosen afterwards.

**Method, fixed now:** deduplicate GPU samples by frame identity; segment sweep
visits by `(sweep, index)` rather than by contiguous preset, because the
serpentine turnaround visits the endpoint twice in a row; nearest-rank P95;
`renderScaleMode` and the envelope read from disk and recorded with every
capture.

**Three runs on one route:** stock with Render Scale Mode off, stock with it on,
and the patched build. Same save, same route, same plugin build.

**Correctness before performance.** A patched build that is faster and wrong is
worse than no patch, so before any timing number we check: both eyes; the
hidden-area mask; water, underwater and refraction; precipitation; menus and
loading screens; TAA and water history across a change; and the per-eye against
double-wide submit layouts. Any visual regression stops the exercise and gets
reported as-is.

---

## 6. Expected outcome, stated in advance

**Success looks like the patched build matching Render-Scale-on for quality and
Render-Scale-off for delivery:**

| | RS off | RS on | patched (predicted) |
|---|---|---|---|
| pixel fraction | 0.216 | 0.320 | **≈ 0.320** |
| beyond two display periods | 0.03% | 1.35% | **≈ 0.03%** |
| worst frame near a change | 34 ms | 59 ms | **≈ 34 ms** |

**Partial success**, which we would still consider worth reporting: quality at or
near the render-scale-on level, and severe frames materially below 1.35% but not
at 0.03% — most likely because the viewport cache becomes the next hitch.

**Failure modes we consider plausible, and will report as readily:**

- Passes that do not consult the dynamic-resolution ratio keep working at
  allocation size, so the saving is much smaller than the table predicts.
- Features caching extent-derived state — dispatch sizes, history validity,
  jitter — produce artefacts on a change even though no texture was recreated.
- The viewport slot cache thrashes badly enough that trading the relatch for it
  is not a win.
- Something in the engine depends on the target size matching the render size in
  a way we have not found.

Any of these is a real answer to the question, and we will write it up with the
same numbers either way.

---

## 7. What we are not claiming

We have not measured this on any hardware but one machine — a 5090 at
3494×3558 per eye, 72 Hz. The size of the prize will differ elsewhere, and on a
GPU where the fixed cost dominates it may differ a lot.

We also do not know your reasons for the current design. A boot latch is the
obviously correct thing if quality changes are expected to come from the menu,
where a restart prompt is acceptable; everything above only matters because we
are changing quality several times a minute from outside. If there is a reason
this cannot work that we have not seen, we would genuinely like to know, and that
answer is as useful to us as a patch.

---

## 8. Provenance

Source identified by tag `CSX3.18` = `2051e2ae`, `CSX_VERSION "3.18-VR"`,
`CSBuildNumber = 11` — matching the build number our plugin reads at runtime, and
tagged 24 minutes before the Nexus archive was uploaded. Measurements,
methodology and the full history of this investigation, including the parts we
got wrong, are in the same repository as this file.
