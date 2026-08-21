# Ask for StereoTrace v2: settle the VR eye-origin question

For Codex, on `stereofusion/trace-csx318`. Written 2026-08-18.

---

## The question

Under Skyrim VR's dynamic-resolution sub-rect rendering, where does the engine
place the **right eye** inside the double-wide scene target?

Two candidates, and everything in a separate investigation currently turns on
which is true:

- **Repacked** — the right eye starts directly after the left eye's *rendered*
  region, so content occupies `[0, r·W]` with the seam at `r·W/2`.
- **Allocation halves** — each eye stays in its own half of the *allocated*
  target and shrinks within it, so the seam stays at `W/2`.

Your v2 milestone already records viewport and render-target bindings and
derives eye markers from their transitions. That is exactly the measurement.
This note is only about **which configuration to capture in**, because the
obvious one cannot distinguish the two answers.

## The trap in the current configuration

The v1 run was `renderScaleMode: 1`, `qualityMode: 3` — Render Scale on, at the
boot quality. There the allocation *is* the render size, the ratio is 1.0, and
there is **no sub-rect at all**. Both candidates predict the same pixel.

A v2 capture in that configuration would look authoritative and discriminate
nothing. Three builds in the other investigation were spent on exactly this
mistake, each time reading a result that was inert by construction.

## The configuration that discriminates

```
renderScaleMode: 0        <- OFF
upscaleMethod:   3        (DLSS)
qualityMode:     3        (Quality)
```

This is **stock CS's default VR path**, no experimental code, on your existing
pristine `csx318-base` build. With Render Scale off and a vendor upscaler, CS
takes `ResolutionOwner::VendorDynamicResolution`: the scene target is allocated
at the full display size and the engine renders into a sub-rect at the quality
scale, with `dynamicResolutionLock = 0`. The sub-rect is live and the two
candidates diverge.

On this machine, display is 3494×3558 per eye, so the scene target is 6988 wide
and the rendered region is ~4658 wide at Quality (ratio ≈ 0.667).

**Right-eye viewport `TopLeftX`:**

| answer | expected | 
|---|---|
| repacked | **≈ 2329** |
| allocation halves | **3494** |

Width should be ≈2329 either way, and the left eye should be `TopLeftX = 0` in
both — only the right eye's origin discriminates. The two candidates are ~1165
px apart, so rounding is irrelevant and a one-pixel disagreement doesn't matter.

## Worth a second point

A second capture at `qualityMode: 6` (UltraPerformance, ratio 1/3) costs little
and rules out coincidence: repacked predicts `TopLeftX ≈ 1164`, allocation halves
still predicts `3494`. If the observed origin *tracks the ratio*, that is
conclusive in a way a single point is not.

## Fields that matter

Beyond what v2 already plans, for draws bound to the scene colour target
(`kMAIN`):

1. `D3D11_VIEWPORT` — `TopLeftX/Y`, `Width`, `Height`.
2. The scissor rect, if cheap. Under DR these may disagree, and that would
   itself be a finding.
3. The bound render-target identity, enough to filter to `kMAIN`.

And at capture start — this is your own v1 recommendation, applied to the thing
that matters here:

4. `runtimeData.dynamicResolutionWidthRatio` / `HeightRatio` and
   `dynamicResolutionLock`, plus the scene target's allocated dimensions.

Without (4) the capture cannot prove the sub-rect was actually engaged, and an
inert capture is the one failure mode that has repeatedly looked like a result.

## Why it is worth doing this way

This settles the question on **unmodified CSX**, with no experimental branch and
no visual judgement call. The other investigation's alternative is a runtime
slider that a human eyeballs for correct stereo — which can only report *whether*
some origin looks right, never *what the engine actually did*. Your capture
answers the second question, which is the one that generalises.

The two are complementary rather than redundant, so both are worth having.
