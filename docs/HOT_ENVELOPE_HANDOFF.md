# Hot-Envelope: handoff for a fresh session

**Written:** 2026-08-18, closing a long session. Read this first, then
`CSX_HOT_ENVELOPE_AUDIT.md`, then `CSX_HOT_ENVELOPE_POC.md`.

---

## The goal

Hold locked 72 FPS in SkyrimVR at the best image quality each scene allows, by
driving Community Shaders' DLSS preset from an external SKSE plugin
(`CSQualityGovernorVR`).

Rik has a working setup today: **Render Scale Mode OFF**, no big freeze, only a
~42 ms microfreeze on a preset change. It works and he can play it.

The prize we are chasing: Render Scale Mode ON is worth **1.6-2.5 ms** of GPU
headroom (measured, E-62), which buys real image quality - time-weighted pixel
fraction 0.320 against 0.216. But with it ON, every quality change costs a
**96-130 ms** relatch stall.

**We want both.** That is the whole exercise.

---

## What is PROVEN (measured, reproduced)

- The boot latch **can** hold across quality changes: **1 relatch per session
  against 171**, two consecutive runs.
- The relatch has **five** independent triggers; four never consult
  `restartRequired`. The original "one boolean" theory was falsified in run 1.
- **Vendor-resource readiness is reachable only through the relatch**
  (`MarkVendorRuntimeResourcesReady` is called only inside the relatch routine).
  This is the real reason the boot latch cannot currently move.
- A guard must never abandon an **in-flight** relatch, or the physical mutation
  epoch strands and blocks every later request.
- The freeze itself is `RecreateAndSetupRenderTargetResources` (the engine's own
  render-target creation for ALL Skyrim targets) plus `globals::ReInit()`.
- DLSS accepted input ranges at 3494x3558 per eye: every mode accepts down to
  half the output per axis; `eUltraPerformance` is fixed at 1165x1186 with no
  range at all.
- The engine renders each eye into **its own half of the allocation**, shrunken
  within that half - not repacked at the active size.

## What FAILED, and why

To make a quality change cheap you must either **(a)** not resize the targets -
which means rendering into a sub-rect, breaking CS's `render size == allocation
size` invariant - or **(b)** resize them without rebuilding everything.

I chose (a) without flagging it as a choice. That invariant is assumed
throughout CS, so every session surfaced a new consumer of it: the submit
bounds watchdog, the eye origin, then camera/projection data. The change
footprint became unmergeable, which is why we stopped.

Last observed symptom: **wrong stereo depth** - "cardboard layers", or the world
far too close - at every preset except the envelope quality. Never explained.

Two things I got wrong late, recorded so they are not repeated:
- `region.depth*` in `ResolveVRSubmitSourceRegion` drives **`DispatchHMDMaskClear`**
  (the hidden-area mask), NOT stereo or DLSS depth input. A "fix" there was
  irrelevant to the symptom and has been reverted.
- The 1164 vs 1165 "off by one" at UltraPerformance is **not a defect**. Stock
  CSX computes the same 1164 and works.

## The open question that gates everything

`ApplyDynamicResolutionState` sets `runtimeData.dynamicResolutionWidthRatio` and
flags `cameraDataDirty` -> `UpdateCameraData()`. The ratio mutates camera
state, not just a viewport.

> **Does Skyrim VR + CS support dynamic-resolution sub-rect rendering at all?**

If that path is unused and untested in VR, approach (a) is dead and no analysis
rescues it. If it is viable, the geometry is sound and the rest is engineering.
**Answer this before any mapping work.** It is a cheap early exit.

---

## The plan Rik has chosen

A full, systematic read of ParticleTroned's CS - flows and sequences mapped
until we understand it as if we had written it - then design the merge properly.
No guessing. Codex reviews the findings periodically.

Constraint that must shape everything: **the change has to be small enough that
the author can merge it** without re-testing years of community work. If the
answer turns out to be a design change only he can make, that is a legitimate
result - he is actively working on Render Scale Mode and our findings are
valuable to him regardless.

---

## Repo and tooling facts

- CS fork: `C:\Data\cs-gputime`, remote `fork` = `zerotrust-dev/skyrim-community-shaders`.
  **`origin` is ParticleTroned's repo - never push there.**
- `csx318-hot-envelope` (PR #2) - the minimal proposal, 3 commits. **Provably
  does not work alone**; describe it as a demonstration of findings, not a patch.
- `csx318-hot-envelope-full` (PR #3) - the failed rewrite. Keep as evidence.
- `csx318-base` - pristine tag `CSX3.18` = `2051e2ae`, CSX 3.18-VR, build 11.
- Portable tools (not on PATH):
  - git: `...\research\foveated_rendering\foveated-community-shaders-research\tools\portable-git\cmd\git.exe`
  - gh: `...\tools\github-cli\bin\gh.exe`, with
    `GH_CONFIG_DIR=...\tools\gh-config`. **gh needs portable-git on PATH.**
- CI builds **only on a pull request**; a branch push alone runs nothing. A build
  is ~38 min. Failures naming `codeload`, `429`, `502`, `503` are GitHub
  infrastructure - re-run, nothing to fix. Real errors look like `error C####:`.
- Streamline headers are NOT checked out in the fork. A copy exists at
  `...\research\foveated_rendering\foveated-community-shaders-research\source\particletroned-skyrim-community-shaders\extern\Streamline-DX12\include\`.

## Current install state

- `CSX Hot-Envelope PoC` is a **separate MO2 mod** holding only our patched DLL.
  Unticking it fully reverts to stock. MGO's CSX mod was never modified.
- `SettingsUser.json` restored to `renderScaleMode: 0`, `qualityMode: 4`
  (backup at `SettingsUser.json.prepoc-backup`).
- Governor untouched throughout: DLL 2026-08-15, `AdaptiveMode = 1`,
  `Order = 4,3,2,1`, `TransitionHoldFrames = 3`.

## Working rules (non-negotiable)

- **Rule 13**: never write API details from memory - read field names,
  signatures and enums from the headers or the repo, every time. Ask Rik when
  there is genuinely no source. A build costs 38 min; a session costs his
  evening.
- **Never poll CI.** Push and stop. Rik reports the result.
- **Always hash-verify** installed files against the artifact.
- Install **both** the .dll and the .ini when shipping the governor.
- Do **not** ask Rik to visit specific scenes or stand still and read numbers.
- Never display or commit tokens. Access notes:
  `C:\Data\game info\SkyrimVR\development\custom_plugins\context.txt`
- All rules live in `docs/MEASUREMENT_METHOD.md`. `docs/GOVERNOR_DESIGN.md` is
  the contract - changing a decision requires argument and evidence recorded
  there BEFORE code.

## The honest lesson from this session

I repeatedly called the work "a few small changes" and kept saying it after the
evidence stopped supporting it. Each fix was locally correct and globally
unverified, and we converged by attrition - six sessions, six symptoms - rather
than by design. If the next approach starts to feel like patching one consumer
at a time, that is the signal to stop and map, not to build again.
