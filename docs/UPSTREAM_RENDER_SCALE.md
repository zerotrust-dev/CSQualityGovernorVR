# For the Community Shaders author: making render scale genuinely hot

Draft, not yet sent. Written 2026-08-10 against **CSX 3.18-VR (build 11)**.

Companion to `docs/development/frame-gpu-time.md` in the fork, which asks for GPU
timing. This one asks for something different and, for an external controller,
more important.

## The ask, in one paragraph

Render-scale mode is documented as hot-switchable. It isn't: a quality-mode
change re-latches the render targets, costs about **85 ms** of stalled frames
plus one frame of invalid content, and takes **10 frames** to recover. DLSS
supports dynamic resolution natively — create the feature at the maximum render
size and vary the rendered sub-rectangle per frame. If the targets were
allocated at the top of the ladder and the sub-rect moved instead, a quality
change would cost nothing, which is what an external controller like VR FPS
Stabilizer or ours needs in order to be useful at all.

## The measurements

Taken with our own D3D11 timestamp brackets, opened when
`IVRCompositor::WaitGetPoses` returns and closed at the compositor `Submit`, so
the compositor's pacing wait is outside the measurement. Validated against
OpenXR Toolkit's `appGPU` over 444 matched seconds: **−0.18 ms mean, flat across
load buckets** (+0.11 / +0.06 / −0.12 / −0.11 ms from 11 to 15 ms of GPU work).

Around every applied quality change, in two different builds:

| | PL3.15 (build 9) | CSX 3.18 (build 11) |
|---|---|---|
| stalled interval, no frames produced | 78, 94 ms | 77, 88, 91 ms |
| GPU time on the frame after the stall | 76.9, 83.6 ms | 66.5, 86.1, 83.9 ms |
| frames until frametime returns to normal | 9–10 | 10–11 |

**The same event in both builds.** What changed between them is only its
appearance: PL3.15's `ClearHMDMaskCS.hlsl` cleared the hidden-area mask to a
value that painted the whole view white, which covered the gap. With that fixed,
the underlying invalid frame is visible — users describe a flat gridded panel,
which several have taken for a UI element flashing on screen.

So the artefact is not a regression in 3.18. It is what the mask bug was hiding.

## Why this matters for external controllers

The mod page states the VR API exists so external mods can *"dynamically toggle
shadows, SSGI, and upscaler quality modes based on performance"*, and CSX 3.18
adds `GetVRUpscalingTransitionProfileDecision` specifically for external
transition controllers. We built one: it measures GPU headroom and moves the
upscale preset to hold 72 Hz.

It works — on a 5.9-minute capture it held **86–88% of a perfect-foresight
optimum** that is constrained to the same one-lever, 3-second-cooldown actuator,
against 0.346 pixel fraction for the best fixed preset that stays inside budget.

But every correction costs 85 ms and a visible artefact, so the controller is
forced to be timid: change rarely, and accept a worse picture the rest of the
time. **The transition cost, not the control logic, is what limits it.**

## What we tried first, so you know what is already ruled out

- **The documented transition path.** Preflight with
  `GetVRUpscalingTransitionProfileDecision`, then
  `SetVRUpscalingTransitionProfileForMethod` on `kApply`. Implemented; the
  artefact was unchanged. `kNoChange` is genuinely useful and we kept it.
- **The fade.** `CSVRRenderScaleTransition*` describes 1 s out, up to 6 s of
  black, 1 s in, driven by the caller via `Game.FadeOutGame`. For a controller
  changing quality once or twice a minute this is far worse than the artefact.
- **Withholding frames.** We hook `IVRCompositor::Submit` for our own timing, so
  we now suppress ~8 frames across the relatch and let the runtime reproject.
  This is a workaround on our side of the boundary, and it costs a visible hitch.

None of these remove the 85 ms. Only pre-allocation would.

## The proposal

Allocate the upscaler's targets at the **maximum** render size on the ladder
(NativeAA) and render into a sub-rectangle sized for the current quality mode,
telling DLSS/FSR the rendered extent per frame. Quality changes then become a
change of viewport and a parameter, with no reallocation and no restart.

Cost: VRAM held at the top of the ladder regardless of the mode in use.

## What we do not know, and would not want to assert

We have not read enough of the renderer to know whether the constraint is CS's
own targets or the game's. If Skyrim's render targets are themselves sized at
initialisation and CS reconfigures them, the sub-rect approach may require the
game's passes to respect a valid region too — clamping every sample to the
rendered rectangle, or accepting bleeding from stale pixels outside it. That is
a much larger change than adjusting an allocation size, and it is the sort of
thing that looks simple from outside and is not.

So this is a question, not a prescription: **is the reallocation forced by the
game's pipeline, or is it a choice in CS's?** If the latter, the payoff is large
and directly serves the use case the API was built for.

## What we can offer

- The measurements above, and the captures behind them.
- A working external controller to test against.
- A GPU-timing patch, already written and validated (`frame-gpu-time.md`), if
  timing is wanted in the API — though our own hooks now make that optional for
  us, and it may be more useful to you than to us.
