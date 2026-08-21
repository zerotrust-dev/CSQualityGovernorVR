# The dynamic-resolution pass replacement: what the code says, and the test that checks it

**Status:** source-derived observation, **not yet confirmed by measurement.**
**Baseline:** tag `CSX3.18` (`2051e2ae`, CSX 3.18-VR build 11). All line numbers
below are that tag's, and every quoted line is **stock CSX** — see §6.
**Date:** 2026-08-21. Experiment `CDO-001`, phase 3 narrowed.

---

## 1. The question

Hot-Envelope renders a lower quality inside a boot-quality allocation, and the
image comes out looking too close and flattened. The leading hypothesis, `H1`,
is a **duplicate spatial transform**:

> If something already expands the complete image from the render extent `R` to
> fill the allocation `A`, and the submit path then crops an `R`-sized region and
> expands *that* to the output `O`, the result is a deterministic zoom of
> `A_eye / R_eye` — with every individual texture dimension numerically legal.

That predicts about 1.13x at Balanced, 1.33x at Performance, exactly 2.00x at
UltraPerformance, and exactly 1.00x on the envelope diagonal — where the image
is in fact correct.

So the question that decides the first half of `H1` is narrow:

> **Under Render Scale Mode, does Skyrim's dynamic-resolution expansion actually
> run — and if something replaces it, what box does the replacement copy?**

We had planned to ask the CS author. He has not been reachable for about a week,
which is entirely reasonable. It turns out the question is answerable from CS's
own control flow without asking anyone.

## 2. The control flow is unambiguous

`FrameAnnotations.cpp` installs vtable hooks on three image-space passes and
routes them all through one CS function:

```cpp
// FrameAnnotations.cpp:51-59  (CSX3.18)
template <RE::ImageSpaceManager::ImageSpaceEffectEnum EffectType>
bool TryReplaceDynamicResolutionUpsample(Upscaling::DynamicResolutionUpsampleStage a_stage)
{
    constexpr const char* passName =
        EffectType == RE::ImageSpaceManager::ISUpsampleDynamicResolution ? "ISUpsampleDynamicResolution" :
        EffectType == RE::ImageSpaceManager::ISFullScreenVR              ? "ISFullScreenVR" :
        EffectType == RE::ImageSpaceManager::ISCopyDynamicFetchDisabled  ? "ISCopyDynamicFetchDisabled" :
                                                                           nullptr;
    return passName &&
           globals::features::upscaling.TryReplaceVanillaDynamicResolutionUpsample(passName, a_stage);
}
```

and the thunk that uses it:

```cpp
// FrameAnnotations.cpp:111  (Render stage; :132 is the Dispatch stage)
if (TryReplaceDynamicResolutionUpsample<EffectType>(Upscaling::DynamicResolutionUpsampleStage::Render))
    return;                       // <- vanilla pass SKIPPED

...
func(imageSpaceShader, shape, param);   // <- vanilla pass RUNS
```

That gives an exact translation of the question:

> **"Did the vanilla expansion happen?"**
> is precisely
> **"Did `TryReplaceVanillaDynamicResolutionUpsample` return `false`, and why?"**

And every reason it can return `false` is a named condition inside one function,
`Upscaling.cpp:45794`. There is no intent to reverse-engineer — only a decision
to record.

## 3. What the replacement actually does: crop, not expand

When it does take over, it performs a straight copy:

```cpp
// Upscaling.cpp:46014-46015  (CSX3.18)
D3D11_BOX sourceBox{ 0, 0, 0, inputWidth, inputHeight, 1 };
context->CopySubresourceRegion(a_targetTexture, 0, 0, 0, 0, sourceTexture, 0, &sourceBox);
```

with

```cpp
inputWidth  = ClampPositiveDimension(resolutionPlan.engineRenderSize.x)   // COMBINED render width
outputWidth = ClampPositiveDimension(resolutionPlan.finalOutputSize.x)    // COMBINED output width
```

**No scale factor appears anywhere in it.** It is a 1:1 copy of the combined
render region to `(0,0)` of the target.

That matters directly: **wherever this replacement runs, `H1`'s "an earlier pass
expanded R to A" is false for that pass.** CS is specifically preventing the
vanilla expansion, not performing one.

## 4. But it steps aside during ordinary gameplay

The replacement declines in a specific and, for us, very interesting case:

```cpp
// Upscaling.cpp:45968-45972  (CSX3.18)
// In-place/UI-target passes can carry late full-resolution HUD/interactions.
// Let vanilla execute these to avoid submitting cropped low-res prompt frames.
const bool inPlacePass = outputTexture == sourceTexture;
const bool uiRenderTargetPass = IsVRPresentationRenderTargetTexture(sourceTexture) ||
                                IsVRPresentationRenderTargetTexture(outputTexture);
const bool interactionUiContext = !IsKnownGameMenuContextActive();
if ((inPlacePass || uiRenderTargetPass) && interactionUiContext) {
    releaseRefs();
    return false;                 // <- vanilla runs, and vanilla DOES expand
}
```

Read the third line carefully. `interactionUiContext` is true when a game menu is
**not** open — that is, during **ordinary gameplay**, which is exactly when the
defect is observed.

So for any pass that is in-place or targets a VR presentation render target, CS
steps aside during gameplay and Skyrim's own expansion runs.

**That is a live candidate for the `R -> A` expansion `H1` requires.** It is a
candidate, not a finding. Whether those conditions are actually true for any pass
in this configuration is a runtime fact, and reading a control path and declaring
it the answer is a mistake this project has already made once and recorded.

## 5. A second observation: the copy assumes one stereo layout

The box is `[0, inputWidth)` where `inputWidth` is the **combined** render width,
copied to `(0,0)`. That is only correct if the two eyes' active fields are
**adjacent starting at zero** — the packed layout.

Consider the worked case, boot Quality with active Balanced:

| | per eye | combined |
|---|---:|---:|
| allocation `A` | 2328 x 2372 | 4656 x 2372 |
| render `R` | 2054 x 2092 | 4108 x 2092 |

- **If packed** — eye 0 at x=0, eye 1 at x=2054 — then `[0, 4108)` captures both
  fields exactly. Correct.
- **If allocation-separated** — eye 0 at x=0, eye 1 at x=**2328** — then eye 1's
  field occupies `[2328, 4382)`, and `[0, 4108)` takes all of eye 0, 274 columns
  of nothing, and eye 1 **truncated at 4108**, losing its last 274 columns.

Now the important part, and the reason this could never have surfaced before:

> **Under Render Scale Mode as shipped, `A_eye == R_eye`.** There is no unused
> space, so the packed and allocation-separated layouts are *the same layout*.
> The copy is correct under either reading, and the assumption is invisible.
>
> Hot-Envelope is the first configuration where `R_eye < A_eye`, and the two
> layouts diverge.

This is the `A`/`R`/`O` conflation described in `FINDINGS-FOR-CS.md` §4, showing
up in a concrete copy box rather than in a variable name.

**Again: a question, not a conclusion.** We do not know the engine's actual
layout in that target. Two self-consistent eye-origin conventions were already
built and tested against the submitted image, and *both* failed — which is
precisely why we stopped guessing conventions and started recording them.

## 6. This is stock CSX, and we checked

The function is upstream, not ours. Extracted from both the pristine baseline and
our working branch immediately before the instrument commit, the only difference
in 239 lines is our own phase-1a field rename:

```diff
- uint32_t inputWidth = ClampPositiveDimension(resolutionPlan.engineRenderSize.x);
+ uint32_t inputWidth = ClampPositiveDimension(resolutionPlan.engineRenderSize.width);
```

Behaviourally identical — `.x` became `.width` when the render extent got its own
type. **The logic described above is CSX 3.18's, unmodified by any of our work.**

That is stated because it changes what the observation means. It is not "our
patch does something odd"; it is "a shipped code path makes an assumption that
only a configuration nobody has shipped can violate."

It is also **not a bug report against CSX.** In every configuration CS actually
ships, the assumption holds and the code is correct. Our feature is the first
thing that could make it matter.

## 7. The test that checks it

Reading the code produced two candidates. Neither is evidence. The instrument
turns both into a runtime record.

**What it records**, once per distinct decision, then silent:

```
[DynResPass] pass=<name> stage=Render|Dispatch decision=REPLACED|FELL-THROUGH
  reason=<named condition> | plan A=..x.. R=..x.. O=..x..
  | source=..x.. target=..x.. | box=[0,w)x[0,h) | vanillaRuns=yes|no
```

Every `return false` in the function now carries a distinct reason string —
`not-vr`, `cs-menu-open`, `plan-owner-not-render-scale-submit`,
`input-not-smaller-than-output`, `in-place-or-ui-render-target`,
`no-suitable-source-srv`, `copy-declined`, and so on.

**Behaviour-null.** The only control-flow edit splits one multi-condition gate
into sequential `if`s so the record can name which clause fired; predicates and
short-circuit order are unchanged. Metadata only — resource descriptions and
boxes, never contents. No readback, no mutation. Behind `vrDynResPassTrace`,
default `0`, with every `GetDesc` gated on it so the cost is zero when off.
Bounded by the number of distinct states rather than by frame count.

**Three sessions**, ordinary play, no scenes to visit and nothing to read on
screen:

| session | Render Scale | Hot-Envelope | quality | purpose |
|---|---|---|---|---|
| **A** | on | off | Balanced | working control: `A == R` |
| **B** | off | off | Balanced | working control: `A == O` |
| **C** | on | **on** | boot Quality, then Balanced | the case under test |

**What each outcome would mean:**

1. **A pass reports `vanillaRuns=YES` in C but not in A.** The expansion `H1`
   needs exists, and `reason=` names why CS stepped aside. `in-place-or-ui-render-target`
   is the predicted reason.
2. **`source=` differs between A and C for the same `box=`.** If A shows
   `source=4108x…` and C shows `source=4656x…` with `box=[0,4108)`, the copy is
   taking 4108 columns out of a 4656-wide target, and §5's layout question
   becomes the next thing to settle.
3. **B bails early at `plan-owner-not-render-scale-submit`.** Expected — the
   replacement is render-scale-only. It means vanilla expansion runs in a
   configuration whose image is *correct*, so expansion by itself is not the
   fault. Whatever is wrong is in the combination.
4. **No `[DynResPass]` lines at all.** The hooks are not reached in this VR
   configuration, and that is a different investigation.

## 8. What this test cannot do

Stated plainly because it would be easy to forget once the log arrives:

> **A recorded box says what a pass was *asked* to do. It does not say what the
> pixels are.**

A texture's dimensions never describe the coordinate state of the image inside
it. A source that is 4656 wide might hold a raw 4108-wide field, or a field
already resampled to 4656 — identical descriptions, opposite correct next
operations. Only image evidence separates those, and that is what the later
phases of `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md` exist for.

So this test **scopes** the remaining work — it can tell us which branch of the
protocol's decision table we are on, and let the analyzer, capture and fiducial
phases be built for that branch instead of for all of them. It **cannot**
localize the defect, and no result from it may be reported as confirming `H1`.

## 9. Status and provenance

| | |
|---|---|
| observation | source-derived, from CSX3.18 stock code |
| confirmed | **no** — instrument built, sessions not yet run |
| instrument | `csx318-hot-envelope-diag`, commit `ebef7f442` |
| setting | `vrDynResPassTrace`, default `0` |
| protocol phase | `CDO-001` phase 3, narrowed to one question |
| parked as | P-2 in `CDO_EXECUTION_LOG.md` |

This document will be updated with the measured result — including if the
measurement contradicts both readings above, which is a real possibility and
would be reported as such rather than reworded.
