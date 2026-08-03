# CS Quality Governor VR

An SKSE plugin that adapts Community Shaders' upscaling quality to the scene, so
SkyrimVR holds a locked framerate without being tuned permanently for its worst
moment.

Status: **cycler built and producing data; governor designed, not yet built.**

The design is settled in **[`docs/GOVERNOR_DESIGN.md`](docs/GOVERNOR_DESIGN.md)**,
which is the authority for what the governor does and how each phase is tested.
Changes to it follow the procedure at the top of that file: argument and evidence
first, then code.

## The Problem

Tuning graphics settings for the worst-case scene means paying for that worst
case during the 95% of play that is nowhere near it. Tuning for the average means
stutter whenever the scene gets heavy.

Every modern engine solves this with dynamic resolution. SkyrimVR through
Community Shaders has the lever but no controller.

## Why This Is A Small Project

Community Shaders' *Particle Lights Fork* — the build MGO 4.0 ships — already
contains everything below the policy layer:

| Layer | Status |
|---|---|
| Live preset switching, no restart | **exists**, measured working |
| Render-target relatch, resource lifecycle, epoch ownership | **exists** |
| Anti-oscillation guards, cooldowns, stable-frame counting | **exists** |
| Transition blocking with documented reasons | **exists** |
| Published inter-plugin API for external controllers | **exists** |
| **Frametime policy — when to change quality** | **missing. This repo.** |

CS API revision 3 was written for exactly this use, instructing external
controllers to *"buffer its latest desired profile and try again later"* when
blocked.

**No fork of Community Shaders is required.** This plugin links
`CSinterface001.h` and calls the published API.

## Approach

Two phases.

**Phase 1 — the cycler.** A minimal plugin that steps through upscale presets on
a hotkey and logs what happens: request timestamp, readback, block reasons, and
time until frametime settles.

Its real value is that it observes transitions **without opening the CS menu**.
The CS overlay costs 1.5–2 ms while open, which invalidated most of a day's
measurements on 2026-07-31. See `docs/MEASUREMENT_METHOD.md`.

Phase 1 answers: what is the true transition latency, how disruptive is a change
visually, and how often is an apply refused.

**Phase 2 — the governor.** Only if Phase 1 says transitions are cheap enough.
Frametime policy with hysteresis, asymmetric up/down timing, and cooldowns,
configured through MCM.

## Scope

**In scope**

- Frametime measurement and policy
- Driving `SetUpscalePreset` through the CS plugin API
- Respecting `IsVRUpscalingProfileApplyAllowed()` and buffering when blocked
- MCM configuration

**Out of scope**

- Any change to Community Shaders itself
- Render-target or resource management — CS owns that
- A second layer of anti-oscillation on top of CS's own guards
- Toggling `renderScaleModeEnabled` at runtime; it is static configuration,
  not a lever

## Documentation

| Doc | What it is |
|---|---|
| `docs/CS_PLUGIN_API.md` | The API this builds against, including the two mismatched preset enums |
| `docs/MEASURED_BASELINE.md` | The measured quality ladder and the verified stack it was measured on |
| `docs/GOVERNOR_DESIGN.md` | **The design contract**: algorithm, parameters, test plan, decision log |
| `docs/MEASUREMENT_METHOD.md` | How to measure without fooling yourself |

## Background

The research that led here lives in
[`zerotrust-dev/custom_OpenXR_Toolkit`](https://github.com/zerotrust-dev/custom_OpenXR_Toolkit):
Pimax lens geometry, crop tuning, the foveation comparison across four systems,
and why PrimaShock VRS and Pimax Center Rendering were both set aside.

Upstream: [`ParticleTroned/skyrim-community-shaders`](https://github.com/ParticleTroned/skyrim-community-shaders)
(branch `cs-1.7-PL-VR`, GPL-3.0).

## Target Configuration

Pimax Crystal Super · 72 Hz · RTX 5090 · SkyrimVR + MGO 4.0 beta ·
OpenComposite · PrimaShock FOV crop · CS DLSS with foveated upscaling.
