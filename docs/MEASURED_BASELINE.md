# Measured Baseline

Date: 2026-07-31
All figures measured in-headset with the **CS menu closed**, reading PrimaShock's
overlay. Same save, same spot, outdoor scene.

This is the response curve the policy will be built on.

## The Quality Ladder

Foveated upscaling **on**, visible scale `0.60`, 72 Hz target.

| Preset | Scale | Render px | FPS | Headroom | Stutter turning |
|---|---:|---:|---|---|---|
| Performance | 0.50 | 25% | 72–74 | 20–25% | no |
| Balanced | 0.588 | 34% | 72–74 | 14–18% | no |
| Quality | 0.667 | 45% | 71–73 | 7–13% | no |
| Ultra Quality | 0.769 | 59% | 70–74 | 6–11% | no |
| Hoshipa | 0.85 | 72% | 64–72 | 7% | below 71 fps |

Cost scales with the square of the ratio, so the rungs are **not** evenly spaced.
Performance → Balanced is cheap (+36% pixels); Ultra Quality → Hoshipa is not
(+22% on an already large base).

**The knee is between Ultra Quality and Hoshipa.** Everything up to `0.769` holds
a steady 72.

### Effect of foveated upscaling

Same ladder measured before enabling it:

| Preset | Headroom before | Headroom after | Stutter before |
|---|---|---|---|
| Performance | 15–20% | 20–25% | no |
| Balanced | 7–13% | 14–18% | no |
| Quality | 6–10% | 7–13% | at 69 fps |
| Ultra Quality | 7% | 6–11% | yes, 60–72 fps |
| Hoshipa | none | 7% | yes, 61–65 fps |

Foveated upscaling is worth **5–7 points of headroom at every rung** and moved
the usable ceiling from `0.588` to `0.769`. It should be considered mandatory
context for any policy built on the ladder above.

## Live Switching — The Key Property

**Preset changes apply without a game restart**, observed directly. This is what
makes a governor possible at all, and it is a feature of the fork
("reliable live preset changes").

Transition latency is **not yet measured** — that is Phase 1.

Important distinction from `CS_PLUGIN_API.md`: preset changes are cheap;
toggling `renderScaleModeEnabled` forces a render-target relatch and is not.
The governor moves presets only.

## Verified Stack

Read from the live registry and the PVR config API, not from memory. **One owner
per lever** — this is what made the measurements interpretable.

| Layer | Setting | Value |
|---|---|---|
| PrimaShock | FOV crop | `fov_type 1`, up/down `85`, `ll/rr 88`, `lr/rl 80` |
| PrimaShock | render target | `override_resolution 1` → `3494 × 3558` per eye |
| PrimaShock | VRS foveation | **off** (`vrs = 0`) — artifact source, see background repo |
| PrimaShock | sharpener | `sharpness = 20`, the only sharpener |
| Pimax | Center Rendering | **off** (`enable_foveated_rendering = 0`) |
| Pimax | render scale | `pixels_per_display_pixel_rate = 0.75` |
| Pimax | sharpener | `gpu_upscaling_sharpness = 0` |
| OCU | DLSS | **off** — required; it blocks CS upscaling entirely |
| CS | upscaler | NVIDIA DLSS, profile K, sharpener off |
| CS | foveated upscaling | **on**, visible scale `0.69` after calibration |
| CS | FOV + TAA | off, untested |
| CS / Reflex | FPS limit | **disabled** (was 70 against a 72 Hz display) |

### Two caveats on these numbers

**The ladder was measured at visible scale `0.60`; it was later calibrated to
`0.69`.** A larger sharp region means less saving, so the headroom figures are
optimistic for the current configuration. **Re-measure before tuning policy
thresholds against them.**

**Nothing was measured in a worst-case scene.** All figures come from a quiet
outdoor location. A busy town, heavy combat, or rain will be worse — and the
worst case is exactly what a governor exists to handle. Phase 1 should collect
the ladder again in the nastiest place available.

## Frame Budget

At 72 Hz the budget is **13.89 ms**. Headroom percentages above are read from
PrimaShock's overlay; one point of headroom is roughly **0.14 ms**.

For a locked framerate, what matters is the **floor**, not the mean — misses
happen at the tail. Policy thresholds should be set on a high percentile (P95 or
worse), never on an average.
