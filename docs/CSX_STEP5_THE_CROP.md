# The defect: we are cropping, not scaling

**Written:** 2026-08-18, after the monocular test.
**Supersedes:** `CSX_STEP4_SHADER_UV_MISMATCH.md`, whose hypothesis this
falsifies.

---

## 1. The deduction

Both eyes, viewed **separately**, looked equally wrong and wrong in the same way.

> **With one eye closed you cannot perceive a stereo error.**

So the defect is monocular. It is not disparity, not the eye origin, not the
per-eye UV clamp, and not stereo reconstruction. Every hypothesis this
investigation has pursued since the first session has been about the
relationship *between* the eyes. All of them were looking in the wrong place.

The description names what it is:

> *"like someone holding a picture in front of me and then started tilting and
> yawing it"* — plus, from the earlier session, *"too close"*.

A view that is magnified, flat, and swims with head rotation is a view whose
**field of view does not match what it is presented as covering**. We are taking
a *crop* of each eye and submitting it as the whole eye.

## 2. The arithmetic matches the reported severity exactly

If the engine renders each eye at the full allocation half (2328 px) and we
submit only `expectedEyeWidth` of it, the magnification is
`allocationHalf / renderHalf`:

| preset | crop | magnification | reported |
|---|---|---|---|
| Quality (envelope) | 2328 / 2328 | **1.00×** | perfect, 73 fps, 10% headroom |
| Balanced | 2054 / 2328 | **1.13×** | bad, "a bit less" |
| Performance | 1746 / 2328 | **1.33×** | bad |
| UltraPerformance | 1164 / 2328 | **2.00×** | worst |

The ordering and the relative severity follow directly, and the envelope quality
is exactly 1.00× — perfect, as observed.

## 3. It also explains the origin result

If each eye is drawn full-size in its own half, then taking the **left portion**
of each half yields two consistent crops whose centres correspond, so the eyes
align. That is the allocation-half origin, and it is why it fixed the cross-eye.

The packed origin would take `[1746, 3492]` for eye 1, straddling the boundary
between the two eyes' regions — hence cross-eyed. Both results follow from the
same picture, and neither required the engine to repack.

## 4. What this means

> **The engine is not honouring `dynamicResolutionWidthRatio` for the scene
> viewport under Render Scale Mode. It renders the full allocation, and we crop
> the result.**

Consequences, in order of importance:

1. The image is wrong because a crop is not a downscale.
2. **There is no GPU saving.** If the scene is rendered at full allocation size
   regardless of the selected quality, the envelope costs exactly what the boot
   quality costs. The entire prize is absent.
3. Every downstream fix attempted so far — submit origin, submit extent, depth
   region, validators — was adjusting how we *read* a frame that was already
   rendered at the wrong size. None of them could have worked.

This is consistent with everything measured and contradicts nothing.

## 5. The decisive confirmation, and it is free

The hypothesis makes a sharp quantitative prediction:

> Under an active envelope, **GPU time will not fall as the preset drops.**

Stock behaviour is the opposite — that is the whole reason Render Scale Mode is
worth having. The governor already measures GPU time, and the CS overlay already
shows headroom. Changing preset from Quality to Performance and watching whether
headroom moves settles it in seconds, with no build and no capture.

If headroom is flat, section 4 is confirmed and the approach as built is dead in
its current form. If headroom improves as expected, the engine *is* rendering
smaller, this document is wrong, and the defect is elsewhere.

## 6. If confirmed, where it leaves the approach

The sub-rect must reach the engine's scene viewport. Under RS-off it does — that
path renders smaller and delivers the saving, which is why it is CS's shipped VR
default. Under RS mode it does not, and `state->screenSize` being the allocation
rather than the display is the only structural difference between them.

That is the whole remaining question, and it is now the *first* question rather
than one of many, because nothing downstream matters until the engine renders at
the requested size.

It is also the point at which this may become a change only the upstream author
can make, which the handoff anticipated as a legitimate outcome.

## 7. Corrections this supersedes

- **Step 4** attributed the residual to the shaders' repacked UV assumption,
  predicting a right-eye-only defect. The monocular test falsified it directly.
- **Step 1 §5** attributed the UltraPerformance freeze to reading outside the
  rendered region. Under this picture the read region is inside what the engine
  drew, so the freeze remains unexplained.
- I twice reported the eye origin as "the remaining defect". It was never the
  defect; it was a real but secondary misalignment on top of a frame that was
  the wrong size to begin with.
