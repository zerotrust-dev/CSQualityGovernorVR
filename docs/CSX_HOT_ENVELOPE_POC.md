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
one DLSS context had to accept a 3× span of input sizes. CSX does not do that:
each quality is configured into a viewport slot at its own input resolution, so
`extentIn` matches that slot's optimum. Streamline tags resources with extents
precisely so the input may be a sub-rect of a larger texture; texture size and
DLSS input size are independent.

Empirically: with Render Scale Mode on we routinely see UltraQuality, Quality,
Balanced, Performance and UltraPerformance all working, each at its own input
size. The DLSS side already does what this needs.

**A correction to an earlier draft of this section.** We first wrote that CSX
"keeps a viewport context per `qualityMode + dlssPreset`", which overstated it.
`vrDLSSViewportSlots` is `[kVRDLSSViewportRoleCount][kVRDLSSViewportSlotCount]`
with `kVRDLSSViewportSlotCount = 2` — a **two-entry LRU cache per role**, keyed
by `(qualityMode, dlssPreset)` and evicted on `lastUse`. Against four or five
presets in rotation that evicts and recreates on most changes. The conclusion
above survives, because each selected quality still gets configured at its own
input size; but the reason we gave for it was wrong, and the eviction is a real
per-change cost we had attributed to the relatch alone.

### What the DLSS runtime actually accepts

`slDLSSGetOptimalSettings` is loaded by CSX but never called, so we called it
and logged the answer. Measured on this machine, 3494×3558 per eye:

| mode | optimal | accepted input range |
|---|---|---|
| `eMaxQuality` | 2329×2372 | **1747×1779 .. 3494×3558** |
| `eBalanced` | 2027×2064 | 1747×1779 .. 3494×3558 |
| `eMaxPerformance` | 1747×1779 | 1747×1779 .. 3494×3558 |
| `eUltraPerformance` | 1165×1186 | **1165×1186 .. 1165×1186** |

Every mode that has a range accepts down to exactly **half the output** in each
axis, and `eUltraPerformance` has no range at all.

Against our ladder — Quality 2329, Balanced 2054, Performance 1747,
UltraPerformance 1165 — a single `eMaxQuality` context can serve **Quality,
Balanced and Performance** by varying the tagged extent, with Performance
sitting exactly on the floor. UltraPerformance cannot join them.

So the honest answer to "can the reset be removed" is *for most of the ladder*.
Two contexts — one spanning the top three rungs, one for UltraPerformance —
reduce context changes from one per quality change to one per crossing of a
single boundary. That happens to equal the existing slot count, but the two
facts are unrelated: `kVRDLSSViewportSlotCount` is an LRU capacity, not a
designed split. We mention the coincidence only so nobody reads it as evidence.

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

## 8. Results

Two sessions, both on MGO 4.0 beta RC3, envelope boot-latched at Quality
(`qualityMode 3`, 3494×3558 → 2328×2372 per eye), `renderScaleMode` and
`vrHotEnvelope` both read back from disk after the run.

### Run 1 — relaxing `restartRequired` alone (`cff819e3`)

Changed `qualityModeMatches` into `qualityModeMatches || renderSizeFitsAllocation`
in `VRPerfModeRestartState::RequiresRestart`, which is where we assumed the
latched contract was enforced.

**No effect. 28 boot latches in four minutes**, one per quality change, each
recreating the allocation at the new quality's size:

```
[20:28:47] Boot-latched kDLSS quality 3 ... -> render 2328x2372 (generation 1).
[20:28:49] Boot-latched kDLSS quality 6 ... -> render 1164x1186 (generation 2).
[20:28:59] Boot-latched kDLSS quality 5 ... -> render 1746x1778 (generation 3).
                                    ... through generation 28
```

The user-visible symptom was new and, in hindsight, the tell: quality changes
stopped being instant and showed **a few heavily pixelated frames** first.
That is the sub-rect path engaging in the window between the change and the
relatch catching up — rendering into part of a target still sized for the old
quality, then being overwritten. The patch had inserted a slower path in front
of the existing one rather than replacing it.

### The audit: five triggers, not one

`restartRequired` turns out to drive the *menu's* restart prompt. Four further
paths force the relatch on a quality change and **none of them consults it**:

| # | site | what it does |
|---|---|---|
| 1 | `PerfModeRestartState.h` `RequiresRestart` | the restart-required flag |
| 2 | `Upscaling.cpp` `ShouldStageVRRenderScaleTransition` | stages a transition whenever the mode is latched |
| 3 | `Upscaling.cpp` `ApplyCSMenuUpscalingTransition` | calls `RequestPerfModeRenderTargetRecreate` directly |
| 4 | `Upscaling.cpp` `ApplyPendingVRUpscalingTransition` | same, under "VR render-scale profile change" |
| 5 | `Upscaling.cpp` `IsVRRenderScalePhysicalContractConverged` | defines convergence as `boot.qualityMode == requested` |

Number 5 is the load-bearing one. It asks whether the physical targets support
the requested quality, and answers with equality — which was exactly right for
as long as render size always equalled allocation size. Under an envelope they
differ by design, so the contract reports un-converged permanently, which makes
`activeMatchesRequest` false in `SetPerfModeRequested` and reaches an
**unconditional** recreate. Guarding the other four changes nothing while this
one stands.

### Run 2 — all five behind the fits-check (`3912b783`)

**The envelope held.** Eleven latches, all `quality 3`, all inside the first
three seconds of startup, and **none for the remaining four minutes** across
every quality change the governor and the CS menu made:

```
[10:27:48] Boot-latched kDLSS quality 3 ... -> render 2328x2372 (generation 1).
                                    ... through generation 11 at 10:27:51
                                    (nothing further)
```

Against 28 relatches in the previous run, on the same route with the same
controller. **The boot quality can be made an upper bound rather than a fixed
point, and the render-target allocation will hold across quality changes.**

### And the reason that is not yet enough

Everything else got worse, from a single stranded flag.

A quality change legitimately marks the vendor resources dirty — the DLSS input
extent really has changed. But `MarkVendorRuntimeResourcesReady` is only ever
reached **inside the relatch routine**, keyed to `relatchContractGeneration`
(`Upscaling.cpp` ~25943, ~26009, ~26016). Remove the relatch and nothing ever
marks them ready. `pendingDLSSReset` latches on, and because
`GetVRUpscalingApplyBlockReasonsForAPI` folds it into `relatchPending`, every
subsequent request is refused:

```
[governor] giving up on Performance after 30.5s blocked (RelatchPending)
[governor] giving up on Balanced    after 30.5s blocked (RelatchPending)
[VRRenderScale] Deferred request id=26 origin=CSMenu until the unresolved
                physical recovery publishes coherently.
```

Those `CSMenu` deferrals are the user's own clicks in the Community Shaders
menu. One flag blocked the external API and the menu alike, DLSS was never
reconfigured for the new extent so the image was upscaled from the wrong
region, and the controller never got past its first change.

### What we take from this

The relatch is not only how the allocation is resized. It is also **the only
path by which vendor resources become ready again.** That coupling, not the
boot latch itself, is what currently prevents the envelope from moving — and it
is not visible from outside the renderer. It is the part of this exercise we
would most want you to have.

We are continuing on a separate branch, where the aim is to create the DLSS
feature once with dynamic-resolution support spanning the envelope, so an
extent change needs no teardown and the reset dance disappears rather than
being routed around. Results will be appended here on the same terms as these,
whichever way they go.

---

## 9. Provenance

Source identified by tag `CSX3.18` = `2051e2ae`, `CSX_VERSION "3.18-VR"`,
`CSBuildNumber = 11` — matching the build number our plugin reads at runtime, and
tagged 24 minutes before the Nexus archive was uploaded. Measurements,
methodology and the full history of this investigation, including the parts we
got wrong, are in the same repository as this file.
