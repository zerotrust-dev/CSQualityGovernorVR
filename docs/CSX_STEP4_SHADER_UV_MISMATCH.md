# The residual defect: the shaders assume a repack the engine does not perform

**Written:** 2026-08-18, after the second controlled session (log 16:09–16:18).
**Status:** hypothesis, precise and falsifiable. Not yet confirmed by measurement.

---

## 1. It is not the projection

Nothing in the camera or projection path differs between stock RS mode and an
active envelope in a way that could cause this:

- `state->screenSize` is the **allocation** under RS mode, and the envelope does
  not change it. Aspect ratio, and anything derived from it such as
  `Util::GetVerticalFOVRad`, is therefore identical to stock RS mode.
- `projectionPosScaleX/Y` carries only the sub-pixel **jitter** offset. The
  envelope changes its denominator from the allocation to the render extent,
  which perturbs jitter by a fraction of a pixel. It cannot move the world.
- Per-eye projection matrices in VR come from the HMD, not from the ratio.

The log confirms the frame-level bookkeeping is clean throughout: every quality
change `FITS`, no relatch, no deferral, no error, `matches=true` on every submit
line at every preset.

## 2. What actually differs

Only four things change when the envelope engages:

1. `engineRenderSize < engineAllocationSize`
2. `resolutionScale < 1.0`
3. `dynamicResolutionWidthRatio/HeightRatio < 1.0` with `dynamicResolutionLock = 0`
4. the submit source box

We have now fixed (4) empirically. (3) is the one that reaches every shader.

## 3. The contradiction

`DynamicResolutionParams1/2` is the **engine's** per-frame constant buffer,
filled from `runtimeData.dynamicResolution*Ratio`. Every CS screen-space shader
converts UVs through `FrameBuffer::GetDynamicResolutionAdjustedScreenPosition`,
whose VR path is:

```hlsl
float2 screenPositionDR = DynamicResolutionParams1.xy * screenPosition;
// right eye:
minValue.x = 0.5 * (DynamicResolutionParams2.z * 1);   // 0.5·r
maxValue.x = 0.5 * (DynamicResolutionParams2.z * 2);   // 1.0·r
```

That is the **repacked** layout, hard-coded: with ratio `r`, the right eye is
assumed to occupy `[0.5r, r]` of the target.

But the sweep measured that under RS mode the engine leaves each eye in its own
half of the allocation. Those two statements cannot both be true, and the shader
side is not something the submit box can correct.

## 4. The arithmetic, at Performance

Allocation 4656 wide, `r = 0.75`, render extent 3492.

| | left eye | right eye |
|---|---|---|
| where the engine draws (measured) | `[0, 1746]` | `[2328, 4074]` |
| where the shaders sample | `[0, 1746]` | `[1746, 3492]` |
| error | **none** | **shifted 582 px left** |

The left eye is sampled correctly. The right eye is sampled 582 px too far
left, and the outer 582 px of the true right-eye image is never sampled at all.
582 is exactly `allocationHalf − renderHalf`, the packed/allocation gap.

## 5. Why this fits every symptom

| observation | explained |
|---|---|
| envelope quality perfect | gap is 0, shader window is exactly right |
| worsens as quality drops | gap grows: 274 → 582 → 1164 px |
| "too close", "cardboard" | one eye's screen-space depth cues are spatially wrong, so disparity is corrupted rather than merely offset |
| changes when looking up or down | screen-space effects sample by screen position, so the error moves through the scene as the view moves |
| fixing the submit origin did not help | the submit box is downstream of the shading; the damage is already in the frame |
| cross-eye tunable, 3D not | rasterised eye position is a separate thing from where the shaders sample |

Affected consumers are everything that resolves screen UVs: screen-space
shadows, volumetric lighting, dynamic cubemaps, water refraction and the
underwater mask, plus the DLSS depth and motion inputs.

## 6. What this means for the approach

`dynamicResolutionWidthRatio` **encodes a repacked layout**. Using it to
describe a sub-rect that is not repacked is a contradiction no downstream fix
can resolve. So there is only one coherent route:

> Make the engine repack under RS mode, as it demonstrably does under RS-off.

Then shaders, submit box and DLSS all agree, and the packed origin becomes
correct everywhere. The open question from step 2 is therefore the *whole*
remaining problem, and it is now sharp:

**Under RS-off the engine repacks; under RS mode it does not. `state->screenSize`
is the display in the first case and the allocation in the second. What consumes
that difference to place the eye?**

Likely candidate: a VR eye offset cached when the render targets are created or
resized, which the ratio never revisits. Not yet established — do not record it
as established.

## 7. A correction

I told Rik mid-session that the frozen image at UltraPerformance was evidence
that the engine repacks, because the allocation-half read region would fall
outside the rendered area. Under the conclusion above that is wrong: with the
engine drawing at allocation halves, `[2328, 3492]` at UltraPerformance is
exactly where it draws, and should be live. The freeze is **unexplained** and
remains the standing UltraPerformance anomaly.

## 8. How to test it cheaply

The hypothesis predicts something specific and visible: the defect is **entirely
in the right eye**. Left-eye-only viewing should look correct at every preset
below the envelope; right-eye-only should be visibly wrong, and wrong by more at
lower presets.

Closing one eye costs nothing, needs no build, and no numbers. If both eyes look
equally wrong, this hypothesis is dead and section 6 does not follow.
