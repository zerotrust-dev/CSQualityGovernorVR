# Step 2: the differential map

**Written:** 2026-08-18. Follows `CSX_STEP1_EYE_PACKING.md` (whose section 1 this
document corrects).
**Sources:** `CSX3.18..csx318-hot-envelope-full`, 10 commits, 435 insertions
across 5 files; `CommunityShaders.log` 2026-08-18 09:14.

---

## 1. The correction that reshaped this step

Step 1 concluded that stock's packed eye origin is correct under an envelope and
the branch's override is the defect. Reading the branch history disproves it.

`deef8e9f` **already built the packed convention** - colour resolved against
`sourceStereoLayout.width`, depth already packed - and that build was reported
cross-eyed at every quality except the envelope. The
`hotEnvelopeActive ? ApplyDynamicResolutionState : ApplyLockedFullResolution`
switch is present from the first commit `cff819e3` onward, so the sub-rect was
reaching the engine in that run as much as in the current one.

| build | colour origin | depth origin | reported |
|---|---|---|---|
| pre-`deef8e9f` | allocation half, full half width | packed | relatch loop, flickering terrain |
| `deef8e9f` | **packed** | **packed** | **cross-eyed** |
| `4c308aee` | allocation half, clamped extent | packed | mixed |
| `46d88e69` (current) | **allocation half** | **allocation half** | **cardboard depth** |

**Both self-consistent conventions have been built, and both failed.** The eye
origin is therefore not the defect, or not the only one. Three of the four rows
above are origin adjustments, each producing a new stereo symptom - which is
precisely the pattern the handoff named as the signal to stop and map.

I recorded the deletion conclusion too confidently in step 1. The evidence that
falsified it was in the branch's own commit messages.

## 2. What the branch actually contains

Ten commits, classified by what they do rather than when they landed.

| class | commits / hunks | status |
|---|---|---|
| **Core mechanism** | settings flag; `RefreshRuntimeResolutionPlan` allocation/render split; `ConfigureUpscaling` envelope ratio; `ApplyDynamicResolutionState` guard | necessary, and the part that works |
| **Relatch guards** | `cff819e3`, `8369df3d`, `3912b783`, `2eab1a6b`, `PerfModeRestartState.h` | **proven**: 1 latch/session against 171 |
| **Geometry overrides** | `deef8e9f`, `4c308aee`, `46d88e69` | **the unresolved area**; both settings fail |
| **Instrumentation** | `b18a1a78`, `79f94e06`, `49b3d6cb`, vendor dirty/ready logging | keep; this is what made step 1 and 2 possible at all |

The split is cleaner than the audit implied. The relatch work and the geometry
work are independent, and only the second is in doubt.

## 3. The map: what is actually unproven

This is the subtraction the step was for. Three configurations exist, and two of
them are known good:

| config | allocation | render extent | status |
|---|---|---|---|
| RS **off** | `state->screenSize` (full display) | `screenSize x qualityScale` | **known good** - CS's VR default |
| RS **on**, stock | `screenSize` (reduced to boot quality) | `== allocation` | **known good** - freezes, but renders correctly |
| RS **on** + envelope | `screenSize` (reduced to boot quality) | `< allocation` | broken |

Read as a pair of independent properties:

- *"the allocation is smaller than the display"* - exercised by **RS on**.
- *"the render extent is smaller than the allocation"* - exercised by **RS off**.

Hot-Envelope is the composition of two individually proven properties. Any site
that depends on only one of them is already covered by a working configuration
and needs no analysis. **The unproven surface is exactly the set of sites that
depend on both at once** - in practice, code gated on `IsVRRenderScaleModeLatched()`
that also reads the render extent.

That is a short list, and it is the right target for the remaining work. It also
explains why the audit's reference counts (103 `finalOutputSize`, 101
`engineRenderSize`, ...) were so discouraging and so misleading: almost every one
of them is exercised by RS-off or RS-on already.

## 4. The sharpened question

Not *"where does each eye go"* - that has been guessed twice and measured never.
It is:

> **Why does the repack hold under RS-off but not under RS mode?**

RS-off and RS-on take structurally different paths. `Upscale()` early-returns
under RS mode (`vrRenderScaleSubmitStageOwnsOutput`), so the main-pass encode -
the code that states `seamCenterX = renderSize.x * 0.5f` and reads each eye at
`renderSize/2` - **does not run at all under RS mode.** The submit-stage path
runs instead. The two paths have never had to agree, because until the envelope
existed no configuration exercised a sub-rect under RS mode.

That is a concrete, bounded thing to map, and it is where step 3's work should
start rather than in the relatch lifecycle.

## 5. What to do next, and what it costs

No further origin adjustment. The next action must **measure what the engine
writes**, not infer it.

Cheapest sufficient test: the existing `SetScissorRect` hook already receives the
engine's rect for every target. One log line recording the post-scale rect when
the target is `kMAIN`, under an active envelope, states the engine's eye
placement directly. That is a diagnostic-only build - ~38 min CI, one short
session, no gameplay judgement required of Rik beyond running the route.

If that shows repacking, `deef8e9f` was reading the right region and the
cross-eye came from somewhere else - most likely the projection or the
per-eye output placement, both of which the submit-stage path owns. If it shows
allocation halves, stock's own submit arithmetic is wrong under an envelope and
the change becomes larger than the "small enough to merge" constraint allows,
which is itself a reportable result.

Either answer closes a door. That is the point of running it.
