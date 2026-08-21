# Gating question: does Skyrim VR + CS support dynamic-resolution sub-rect rendering?

**Written:** 2026-08-18, opening the systematic-analysis session.
**Question, from `HOT_ENVELOPE_HANDOFF.md`:** *"`ApplyDynamicResolutionState`
sets `runtimeData.dynamicResolutionWidthRatio` and flags `cameraDataDirty` ->
`UpdateCameraData()`. The ratio mutates camera state, not just a viewport. Does
Skyrim VR + CS support dynamic-resolution sub-rect rendering at all? If that path
is unused and untested in VR, approach (a) is dead."*

**Answer: yes, and stronger than "supported".** Dynamic-resolution sub-rect
rendering is CS's *default and only* VR rendering mode whenever a vendor
upscaler is active and Render Scale Mode is off. It is the path Rik plays on
today. Approach (a) is not dead.

All code references below are the **pristine base**, tag `CSX3.18` = `2051e2ae`,
read via `git show CSX3.18:...`, not our modified branch.

---

## 1. The chain, end to end

### 1.1 The plan owner is literally named for it

`Upscaling.h:67` - the resolution-owner enum has three members, and the middle
one is the answer:

```cpp
enum class ResolutionOwner : uint8_t
{
    Native,
    VendorDynamicResolution,
    VRRenderScaleMode,
};
```

`RefreshRuntimeResolutionPlan` (`Upscaling.cpp:19022`) assigns it. The default
render size is *already* the dyn-res sub-rect:

```cpp
plan.engineRenderSize = state ? Util::ConvertToDynamic(screenSize) : screenSize;
```

and when Render Scale Mode is **not** latched:

```cpp
} else if (plan.vendorMethod && IsUpscalingActive()) {
    if (globals::game::isVR)
        plan.engineRenderSize = resolveVendorDynamicRenderSize(plan.trueHMDDisplaySize);
    plan.owner = ResolutionOwner::VendorDynamicResolution;
```

`resolveVendorDynamicRenderSize` is `displaySize * resolutionScale`. The targets
stay at full display size. **That is a sub-rect, in VR, by construction.**

### 1.2 The ratio is written, unlocked, in VR

`ConfigureUpscaling` computes `resolutionScale` from the quality preset
(`GetQualityModeResolutionScale`, `Upscaling.h:183`): Quality = 1/1.5 = 0.667,
qualityMode 4 = 1/1.7 = 0.588. It then calls `ApplyDynamicResolutionState`
(`Upscaling.cpp:39274`).

Inside, the VR branch is gated by `ShouldUseReducedResolutionForUpscaling`,
whose threshold is `kDynamicResolutionUpscalingScaleThreshold = 0.99f`. Every
ladder rung is far below it, so the branch taken is:

```cpp
runtimeData.dynamicResolutionWidthRatio  = resolutionScale.x;
runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;
runtimeData.dynamicResolutionLock        = 0;   // <- unlocked, on purpose
...
if (cameraDataDirty) UpdateCameraData();
```

Note `SetDynamicResolutionOverrideForUpscaling(ForceDisabled)` immediately
above it: CS force-disables *Skyrim's automatic DRS controller* precisely so it
can own the ratio itself. That is not the shape of an unused path.

### 1.3 The engine honours it in VR

- `UpdateCameraData` is the engine's own function,
  `RELOCATION_ID(75472, 77258)` - **it has a VR address**. The camera-state
  mutation the handoff worried about is a resolved, shipped VR entry point.
- `Util::IsDynamicResolution` reads `REL::RelocationID{ 508794, 380760 }` -
  again, a VR address for the engine's DRS-enabled flag.

### 1.4 The scissor hook scales, and kMAIN is not exempt

`Upscaling::SetScissorRect::thunk` (`Upscaling.cpp:51080`):

```cpp
const bool vrNativeLayoutSubmitProtectedTarget =
    globals::game::isVR && IsCurrentRenderTargetVRNativeLayoutSubmitProtectedTexture();
if (!runtimeData.dynamicResolutionLock && !vrNativeLayoutSubmitProtectedTarget) {
    a_left  = (int)(a_left  * runtimeData.dynamicResolutionWidthRatio);
    a_right = (int)(a_right * runtimeData.dynamicResolutionWidthRatio);
    ...
}
```

The protected list (`kVRNativeLayoutSubmitProtectedTargets`, `:1764`) is five
menu/HUD/fade targets - `kPROJECTEDMENU`, `kHUDMENU`, `kFADERUI`, and the two
`kTEMPORAL_AA_UI_ACCUMULATION` targets. **`kMAIN` is not in it.** The scene
target gets scaled.

`Main_RenderPrecipitation` and `BSFaceGenManager_UpdatePendingCustomizationTextures`
are each wrapped in `RunWithDynamicResolutionLocked` - passes that had to be
carved *out* of the sub-rect transform. You only accumulate those by running the
path for real.

### 1.5 The shaders consume it, with VR-specific stereo code

`package/Shaders/Common/FrameBuffer.hlsli` declares
`DynamicResolutionParams1/2` at `packoffset(c85)/(c86)` **inside the
`#if defined(VR)` block** - the engine populates them in the VR per-frame
buffer. `src/Globals.h:151` mirrors that layout in `FrameBufferVR`, and
`GetDynamicResolutionParams1()` branches on `REL::Module::IsVR()`.

`ClampDynamicResolutionAdjustedScreenPosition` has a dedicated VR path that
clamps each eye inside its own half **of the scaled region**. That is bespoke
stereo-aware sub-rect code; it is not boilerplate that happens to compile.

Consumers span the codebase, not just Upscaling: `ScreenSpaceShadows.cpp:479`,
`VolumetricLighting.cpp:874`, `DynamicCubemaps/UpdateCubemapCS.hlsl`,
`DepthRefractionUpscalePS.hlsl`, `UnderwaterMaskUpscalePS.hlsl`,
`Util::GetScreenDispatchCount`.

---

## 2. The decisive practical proof

With `renderScaleMode: 0` and a vendor upscaler, CS *cannot* take any other
path: `owner = VendorDynamicResolution`, `dynamicResolutionLock = 0`, ratio
0.588. That configuration is:

- what Rik plays today, described in the handoff as *"It works and he can play
  it"*;
- the **"Render Scale off"** column of the PoC's own measurement table - all
  four presets, both sessions.

Every number in that column was captured with VR dynamic-resolution sub-rect
rendering live at a ratio between 0.33 and 0.67. The question is answered by
data we already own.

---

## 3. What this does *not* license

The gate is open for the geometry. It is not open for the old implementation.

Approach (a) was described as *"breaking CS's `render size == allocation size`
invariant"*. That framing is now wrong, and the wrongness matters: **stock CS
already violates it in VR, on the default path.** What Hot-Envelope actually
changes is narrower - it makes the *allocation* the boot quality's size instead
of the full display size. The ratio's denominator moves; the mechanism does not.

That is a materially smaller change than the audit assumed, and it means the
right question for the mapping is no longer "who assumes render == allocation"
but **"who assumes the allocation is `state->screenSize`"**.

---

## 4. A hard contradiction the mapping must resolve first

`CSX_HOT_ENVELOPE_AUDIT.md` lists as *proven*:

> The engine renders each eye into **its own half of the allocation**, shrunken
> within that half - not repacked at the active size.
> *evidence: two runs: origin 2328 aligned, origin 2054 cross-eyed*

**The stock code says the opposite.** Three independent sites, all base:

1. `ResolveVRSideBySideStereoLayout(eyeWidth, eyeHeight)` (`:1323`) sets
   `eyes[1].minX = eyeWidth` - contiguous at *whatever eye width it is handed*.
   At `:44470` it is handed `sourceEyeWidthIn`, the **render** eye width. So the
   submit source is anchored at the repacked origin.
2. `PreparePerEyeInputs` (`:37302`) computes `eyeWidthIn = renderSize.x / 2`
   and then `offsetXIn = (i == 1) ? eyeWidthIn : 0`. The right eye is copied
   from `renderSize.x / 2`, **not** from `screenSize.x / 2`.
3. The HLSL clamp: for the right eye,
   `minValue.x = 0.5 * DynamicResolutionParams2.z` - i.e. `0.5 * ratio`, not
   `0.5`.

Stock geometry is **contiguous, repacked at the render size**: content occupies
`[0, r*W]`, left eye `[0, r*W/2]`, right eye `[r*W/2, r*W]`.

At Balanced inside a Quality envelope, `r = 2054/2328 = 0.882`, and the repacked
right-eye origin in the 4656-wide target is `2328 * 0.882 = 2054` - **exactly
the origin the audit recorded as cross-eyed**, and 2328 is exactly the origin
stock would never use.

Commit `4c308aee` on the failed branch is titled *"clamp the submit box, do not
repack the eye origin"*. That inverted the engine's own convention. It is the
most likely explanation for the unexplained cardboard-depth symptom, and it fits
the one clue nobody could place: the image was correct **only at the envelope
quality**, which is exactly the case `r = 1` where the repacked origin and the
allocation-half origin coincide.

I am not calling the audit's observation false - it was two real runs. But the
runs are confounded: at that time colour was pinned to the allocation half while
depth still resolved against `sourceStereoLayout` at packed origins, so
"aligned" was judged on a build where the two disagreed. **This contradiction,
not the gating question, is the first thing the systematic read must settle**,
and it is settleable from code plus one capture rather than by another play
session.

---

## 5. Verdict

| | |
|---|---|
| Gate | **open** |
| Approach (a) | **viable** - the geometry is CS's shipped VR default |
| Invariant `render == allocation` | **does not exist in stock VR**; the audit's central framing needs revising |
| Real invariant to map | *"the allocation is `state->screenSize`"* |
| Immediate next thread | the repacked-vs-allocation-half eye origin (section 4) |

The handoff called this a cheap early exit. It was: the answer was in
`ResolutionOwner`'s middle enumerator.
