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

---

## 10. A second gem, and a contradiction it exposes

Added 2026-08-21, from continued source study while CI built.

### 10.1 The engine's own constants say the eyes are packed

`package/Shaders/Common/FrameBuffer.hlsli:70-85` (stock CSX) clamps sampling to
one eye:

```hlsl
// VR uses side-by-side stereo packing in the shared render target.
bool isRight = screenPosition.x >= 0.5;
float minFactor = isRight ? 1 : 0;
minValue.x = 0.5 * (DynamicResolutionParams2.z * minFactor);
float maxFactor = isRight ? 2 : 1;
maxValue.x = 0.5 * (DynamicResolutionParams2.z * maxFactor);
```

with, from the same file's `packoffset(c86)` comment,
`DynamicResolutionParams2.z = fDynamicResolutionWidthRatio - fDRClampOffset`.

So each eye is clamped to **half of the ratio**, not to half of the target:

```
eye 0 -> DR U in [0,          0.5 * ratio)
eye 1 -> DR U in [0.5 * ratio, ratio)
```

The two active fields are **adjacent, starting at zero** — packed. Those are
constants the *engine* fills, so this is Skyrim's convention, not CS's choice.

The rest of CS agrees. `PreparePerEyeInputs` (`Upscaling.cpp:38068`) takes the
vendor input with `offsetXIn = (i == 1) ? eyeWidthIn : 0`, and the pass
replacement copies `[0, combined render width)`. The whole "where" chain is
internally consistent on packed.

### 10.2 But a measurement in this repository says otherwise

`Upscaling.h:257-262` records, as the justification for the current default:

> **MEASURED 2026-08-18** ... under an active envelope the engine renders each
> eye into its own half of the allocation and shrinks it within that half. It
> does not repack. At quality 4 (ratio 0.882) eye 1 reads correctly from 2328
> and is double-visioned from the packed 2054.

2328 is `A_eye`; 2054 is `R_eye`. That is the allocation-separated layout, and
it directly contradicts §10.1.

### 10.3 Why Render Scale off decides between them

The two conventions are **not** interchangeable in the shipped RS-off flow:

| | eye 1 origin, RS-off Balanced (`A` = 6988, `R` = 4110) |
|---|---:|
| packed (`0.5 * ratio * A`) | **2055** |
| allocation-separated (`A/2`) | **3494** |

They differ by 1439 pixels. RS-off works and its image is correct, so whichever
convention CS's shaders use there must be the engine's — and the shader clamp
above uses packed. Under Render Scale **on** the two coincide exactly, because
`A_eye == R_eye`; that is why nothing ever had to choose.

So §10.1 and §10.2 cannot both describe the same mechanism.

### 10.4 What we are not concluding

Either the 2026-08-18 measurement was confounded, or the engine genuinely places
eyes differently once render-scale mode has changed its notion of screen size.
**We do not know which**, and this document will not guess.

Two things are worth stating plainly, though:

- The measurement's stated basis — *"reads correctly"* versus *"double-visioned"*
  — is a **visual judgement**. `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md` §1
  rules that out as an oracle for exactly this class of question, and this is a
  good illustration of why: it was recorded as settled ("the default is now the
  answer rather than a candidate") and it disagrees with the engine's constants.
- It also **did not fix the image**. `README.md` §3 records that both origin
  conventions were built and both failed. So whichever is right, the eye origin
  is not what makes the world look too close.

That last point matters for `H1`. If the entire "where" chain is consistent and
still wrong, the defect is more likely to be a **"what"** problem — the
coordinate state of the pixels rather than their placement — which is precisely
what `H1` describes and what the contract model in phase 1 was built to express.

### 10.5 The cheap way to settle it

Record the **actual per-eye viewport and scissor** in effect during the scene
render, alongside the plan's `A`, `R` and the published ratio. Metadata only,
same class as the instrument in §7. `RSGetViewports` is already used elsewhere in
this file, so the mechanism exists.

That would replace a visual judgement with an integer, and it is the same three
sessions — no extra headset round.

**Note for the pending sessions:** `TryReplaceVanillaDynamicResolutionUpsample`
does not read `vrHotEnvelopeEyeOrigin`, so sessions A, B and C remain valid for
the question in §7. The contradiction above is a separate question that those
sessions do not answer.

### 10.6 Added to the instrument

Commit `679a453a0`. The same deduped record now also carries:

```
viewports={N} vp0=[x{} y{} {}x{}] scissor0=[{},{}) | eyeOriginMode={}
```

`RSGetViewports` and `RSGetScissorRects`, read-only, taken before the function
touches any state, gated on the same `vrDynResPassTrace` setting. No D3D state is
modified and the cost is zero when off.

**How to read it, for the worked case** (boot Quality, active Balanced,
`A_eye` = 2328, `R_eye` = 2054):

| observed `vp0` x-origin for the second eye | means |
|---|---|
| `2054` | **packed** — the engine's constants are right, the August measurement was confounded |
| `2328` | **allocation-separated** — the measurement is right and the shader clamp does not describe this configuration |
| a full-target viewport, one per frame | **inconclusive at this boundary** — the DR pass does not run per eye, and a scene-time observation is needed |

The third row is a real outcome, not a failure. It would tell us this boundary
cannot decide the question, which is worth knowing before building anything
larger.

**Supersedes:** the CI build for `ebef7f442` no longer matters; use the artifact
for `679a453a0`, which contains both instruments.

---

## 11. A third gem: the vendor input is allowed to be oversized

Found 2026-08-21 while CI built. Stock CSX, `Upscaling.cpp:38187`,
`AreActiveVRIntermediateTexturesCompatible`.

### 11.1 The asymmetry

Two predicates decide whether the per-eye vendor resources can be reused:

```cpp
const auto coversInput = [allocationWidth, allocationHeight](...) {
    return ... &&
           a_texture->desc.Width  >= allocationWidth &&      // >=
           a_texture->desc.Height >= allocationHeight &&
           a_texture->desc.Format == a_format;
};

const auto matchesOutput = [=](...) {
    return ... &&
           a_texture->desc.Width  == a_outputWidth &&        // ==
           ...
};
```

**Inputs are accepted when they are at least large enough. Outputs must match
exactly.** That asymmetry is deliberate and it is guarded by:

```cpp
if (!stableFSRInputBounds && vrIntermediateTextureGeneration != a_contractGeneration)
    return false;
```

### 11.2 Why only Hot-Envelope can reach it

In the shipped build a quality change bumps the contract generation, so that
guard fires, everything is recreated, and the `>=` never has to be a `>`. It is
dead slack.

Hot-Envelope's entire purpose is to hold the contract generation stable across a
quality change. So the guard passes, and a per-eye input texture created at boot
Quality — `2328 x 2372` — is judged compatible for active Balanced, which needs
`2054 x 2092`. **It is not resized.**

The result is a texture whose description says `2328` holding a field that is
`2054` wide, with the remaining 274 columns carrying whatever the previous
quality left there. In the phase 1 contract model that is exactly:

```
resourceExtent = 2328 x 2372
coverage       = { fieldExtent 2054 x 2092, covered 2054 x 2092 }
```

A perfectly legal state, and the one where a resource description tells you
nothing about what the pixels are.

### 11.3 And CS already knows about it — in one place

This is the part that makes it interesting rather than alarming.
`StretchSubmitStageEyeOutput` (`Upscaling.cpp:39217-39222`) passes **both**
numbers to its shader:

```cpp
stretchData.inputSize        = { inputWidth, inputHeight };                 // the logical field
stretchData.outputSize       = { outputWidth, outputHeight };
stretchData.sourceTextureSize = { vrIntermediateColorIn[eyeIndex]->desc.Width,
                                  vrIntermediateColorIn[eyeIndex]->desc.Height };  // the resource
```

That is coverage-aware code: it separates *how big the texture is* from *how much
of it is the picture*. DLSS is handled the same way — `sl::Extent extentIn{ 0, 0,
eyeWidthIn, eyeHeightIn }` describes the field, not the resource
(`Streamline.cpp:1985`).

So the oversized-input state is **modelled**, at least here. Two consumers get it
right.

### 11.4 The question that leaves

If a vendor input texture can legitimately be larger than the field inside it,
then **every** consumer of `vrIntermediateColorIn`, `vrIntermediateDepth`,
`vrIntermediateMotionVectors`, `vrIntermediateReactiveMask` and
`vrIntermediateTransparencyMask` has to know which of the two numbers it wants —
and there are 109 references to those arrays across the upscaling sources.

Two are demonstrably correct. The rest are unaudited, and this is the first
configuration in which being wrong changes the answer.

Note also that the slack applies to **depth and motion vectors** as well as
colour. A motion vector field written for a `2054`-wide eye and read as though
it filled `2328` would not crop the image — it would misregister reprojection,
which is a different symptom class and worth keeping separate.

### 11.5 The cheap confirmation

Log `vrIntermediateColorIn[i]->desc` next to `eyeWidthIn` in the same sessions.

- Under **RS-on**, description should equal the field: no slack is ever taken.
- Under **Hot-Envelope after a downward quality change**, description should
  exceed the field — `2328` against `2054`.

That confirms the mechanism is live rather than theoretical, costs one log line,
and needs no extra session. **Not yet added** — it is a second instrument in a
different function, and the build for `679a453a0` is already running.

### 11.6 Status

Source-derived, unconfirmed. It does **not** by itself explain a zoom: both
audited consumers handle the slack correctly, and an unhandled one would more
likely make the world look *smaller* with stale edge columns than closer. It is
recorded because it is a real, Hot-Envelope-only state that a resource
description cannot describe — which is the defect class this whole protocol is
built around.
