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

---

## Update 2026-08-13 — the diagnosis is confirmed in source, and there is a workaround

This draft asked for pre-allocation plus a sub-rect instead of a relatch. That
request is now backed by the source and by measurement, and an independent review
(Codex) arrived at the same architecture in far more detail — it calls it
**Hot-Envelope Render Scale Mode**: keep Skyrim's physical render targets at one
immutable allocation size, and switch quality by changing only the *logical*
rendering extent inside them via the dynamic-resolution ratios.

### What the source proves

`Features/Upscaling/PerfMode.cpp`:

```cpp
restartRequired =
    boot.active &&
    (!requestedNow || displaySizeChanged || !eligibleNow ||
     boot.method != a_method ||
     boot.qualityMode != qualityMode);   // a quality change relatches
```

Because VR Render Scale Mode replaces the runtime's recommended render-target
size with `HMD size x quality scale`, Skyrim allocates its physical targets at
that quality's input resolution. Changing quality therefore changes physical
texture dimensions, which cascades into `RecreateRenderTargetsForVRRenderScale`,
`globals::ReInit()`, and a rebuild of every render-target-dependent feature. The
stall is a rendering-graph reconstruction, not an upscaler switch.

The single line to change is `boot.qualityMode != qualityMode`. Quality must
become hot state; only display size, method, render-scale mode and the
*allocation* scale should remain cold.

### There is a workaround today, and it should shape the ask

Booting with **VR Render Scale Mode disabled** already avoids this entirely:
`EnsureBootLatch` returns before arming the latch when `IsEligible` is false, so
`restartRequired` can never be set by a quality change. Measured on RC3, worst
frame within 0.5 s of a change fell from **59 ms to 34 ms**, with late frames per
preset and preset distribution unchanged.

So the honest framing for the author is not "the freeze makes external control
impossible" — it is:

> External VR quality control already works, but only by giving up Render Scale
> Mode's allocation savings. Hot-Envelope would give both.

### Two things to check before proposing an implementation

1. **DLSS dynamic-resolution range.** The ladder spans scale 0.333 to 1.0, a 3x
   linear span. Streamline defines a valid input range per output resolution via
   `slDLSSGetOptimalSettings`. CSX resolves that entry point but does not appear
   to use the returned min/max anywhere. If the range is narrower than the ladder,
   "all seven levels hot" is impossible and the envelope must be chosen to fit.
   **This is the most likely failure of the whole idea and costs nothing to
   check.**
2. **The VR DLSS viewport slot cache** holds two slots per role, keyed on quality
   mode plus preset (`Upscaling/Streamline.cpp`). Cycling seven rungs will evict
   and recreate. Once the relatch is gone this is the next candidate hitch, and
   re-keying on performance mode + preset + output dimensions + HDR state would
   let qualities that share a DLSS mode share a context.

### Provenance

The public `csx-3-VR` head declares API build 10; RC3 ships build 11. The relatch
path matches, but any patch should be based on the exact source for the installed
binary, identified by build artifact or PDB rather than by nearest public head.

---

## Second, smaller ask: a ~3-frame hidden-area-mask artefact on preset change

Independent of the render-scale architecture, and much cheaper to fix.

**What is seen.** On every VR upscale preset change, a rectangle appears in the
**nose region of both eyes** — white in the right eye, empty in the left. It
**starts wide, extending toward the edges, and contracts to the nose over about
three frames** before disappearing. The nose is exactly what the hidden area mask
covers.

**How the length was established.** An external controller (this plugin) conceals
transitions by replaying the last known-good submitted frame for N frames. Binary
searching N against what a player can actually see:

| N | result |
|---|---|
| 8 | clean |
| 3 | clean |
| 2 | nose rectangle visible |
| 1 | nose rectangle visible |
| 0 | **widest** — extends to the edges |

**It is not the controller's doing.** Setting N = 0 removes the capture and
replay entirely, and the artefact becomes *worse*, not better. So this is
produced by Community Shaders' own transition path and merely papered over
downstream.

**Build.** CSX **3.18-VR**, archive dated **2026-08-09**, on MGO 4.0 beta RC3,
Pimax Crystal Super, 72 Hz. Applied through
`SetVRUpscalingTransitionProfileForMethod` — i.e. the documented transition API
with the door fade, not `SetUpscalePreset` underneath the renderer. Reproduced
with **VR Render Scale Mode disabled**, so no render-target relatch is involved.

Note this appears to survive the 2026-07-25 upstream flash fix, which predates
this build by three weeks — so either that fix does not cover this path, or this
is a distinct artefact with a similar appearance.

**Why it matters to an external controller.** It sets a hard floor on how short
a transition can be made. Every quality change costs at least three frames of
deliberately frozen image to hide it — about 42 ms at 72 Hz. Remove the artefact
and the concealment can go away entirely, which is the difference between a
governor that can change quality freely and one that must ration changes.
