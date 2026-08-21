# Code gems: facts found by reading, and the measurements that will test them

**Purpose.** A register of things established by *reading* Community Shaders,
kept separate so they do not get buried in narrative documents. Each entry holds
its claim, its source evidence, why no shipped configuration can see it, the test
that checks it, and a slot for that test's result.

**Started** 2026-08-21, during `CDO-001`. Expected to grow.

---

## What qualifies as a gem

A discipline, so the register does not dilute into a list of observations:

1. **It is checkable at a line number.** A quotable statement in source, not an
   impression of how the code feels.
2. **It is invisible in every shipped configuration** — normally because two
   quantities coincide there and stop coinciding in ours. That is what makes it
   a gem rather than a bug someone would already have hit.
3. **It has a stated test**, and that test can fail. An entry with no falsifier
   is a note, not a gem.
4. **Its attribution is verified** — stock CSX or ours. Extracted from the
   `CSX3.18` baseline and compared, not assumed. This has already changed one
   entry's meaning materially.
5. **The result slot stays honest.** `PENDING` until measured, and a measurement
   that contradicts the reading is recorded as such rather than reworded.

Line numbers are the **`CSX3.18` baseline** (`2051e2ae`) wherever the code is
stock, because our branch's numbering shifts with every instrument. Function
names are given always.

---

## The register at a glance

| # | claim | kind | whose code | test | result |
|---|---|---|---|---|---|
| **1** | the replacement **crops, does not expand** — but steps aside during gameplay for in-place/UI passes, and then vanilla *does* expand | fact | stock | `[DynResPass] reason=` | **PENDING** |
| **2** | the engine's own constants say the eyes are **packed**; a measurement in our repo says allocation-separated | **contradiction** | stock vs ours | `[DynResPass] vp0=` | **PENDING** |
| **3** | vendor **inputs** are accepted at `>=` while **outputs** require `==`; only reachable when the contract generation is held stable | fact | stock | `[VRIntermediate] oversizedInput=` | **PENDING** |
| **4** | one normalized bound is resolved against **two different extents** in the same function — colour against the resource, depth against the field | fact | stock | `[DynResPass]` + a follow-up | **PENDING** |
| **4b** | our headline symptom is **one experimental arm's** symptom | correction | ours | already established | **DONE** |

Three facts, one contradiction, one correction. None of them says what the pixels
are — see "What the register cannot do" at the end.

---

## Gem 1 — the pass replacement crops, but steps aside during gameplay

**Claim.** Under Render Scale Mode, CS intercepts Skyrim's dynamic-resolution
passes and replaces them with a **1:1 crop**, no scale factor anywhere. But it
declines for in-place and UI-target passes whenever a game menu is *not* open —
i.e. during ordinary play — and then vanilla runs, and vanilla expands.

**Attribution.** Stock CSX. Verified: the function extracted from the baseline
and from our branch differs in 2 lines of 239, both our `.x` → `.width` rename.

**Evidence.**

```
FrameAnnotations.cpp:51-59   three passes routed through one CS function
FrameAnnotations.cpp:111,132 `if (TryReplace...) return;`  -> true SKIPS vanilla
Upscaling.cpp:45794          TryReplaceVanillaDynamicResolutionUpsample
Upscaling.cpp:46014-46015    D3D11_BOX{0,0,0,inputWidth,inputHeight,1}, 1:1 to (0,0)
Upscaling.cpp:45968-45972    the in-place / UI bail, gated on !IsKnownGameMenuContextActive()
```

**Why no shipped flow sees it.** With Render Scale on there is no sub-rect at
all, so what the replacement copies and what vanilla would expand are the same
region. The bail is invisible because both routes give the same picture.

**Test.** `vrDynResPassTrace`. Each `return` carries a distinct `reason=`, so the
log names which of ~13 conditions decided it, per pass and per stage.

**What would falsify the interesting half.** No pass reporting
`vanillaRuns=YES` under Hot-Envelope, or reporting it identically under
Render Scale on. Then the bail is not envelope-specific and gem 1 reduces to
"the replacement crops", which is true but uninteresting.

### Result

```
STATUS: PENDING
sessions: A (RS-on), B (RS-off), C (envelope Q3->Q4)
build:    c88b8e490
```

---

## Gem 2 — packed or allocation-separated? Two sources disagree

**Claim.** Skyrim's own dynamic-resolution constants place the two eyes
**packed** — adjacent from zero. A measurement recorded in our repository says
each eye sits in its own half of the allocation. Both cannot be right.

**Attribution.** The shader side is stock. The contradicting measurement is ours.

**Evidence — packed:**

```hlsl
// package/Shaders/Common/FrameBuffer.hlsli:70-85  (stock)
bool isRight = screenPosition.x >= 0.5;
minValue.x = 0.5 * (DynamicResolutionParams2.z * minFactor);   // minFactor 0 or 1
maxValue.x = 0.5 * (DynamicResolutionParams2.z * maxFactor);   // maxFactor 1 or 2
```

with `DynamicResolutionParams2.z = fDynamicResolutionWidthRatio - fDRClampOffset`
(same file, `packoffset(c86)` comment). Each eye is clamped to **half of the
ratio**, not half of the target. Those constants are filled by the engine.

**Evidence — separated:** `Upscaling.h`, the comment justifying
`vrHotEnvelopeEyeOrigin = 1`: *"MEASURED 2026-08-18 ... eye 1 reads correctly
from 2328 and is double-visioned from the packed 2054."*

**Why Render Scale off decides it.** There the two conventions are 1439 pixels
apart — packed puts eye 1 at `0.5 * ratio * A` = 2055, the allocation half at
`A/2` = 3494. RS-off works, so its shaders use the engine's convention, and that
is packed. Under Render Scale **on** the two coincide exactly, which is why
nothing ever had to choose.

**Caveat on the contradicting evidence.** Its stated basis — *"reads correctly"*
versus *"double-visioned"* — is a **visual judgement**, which the protocol rules
out as an oracle for exactly this class of question. It was nonetheless recorded
as settled: *"the default is now the answer rather than a candidate."*

**Test.** `[DynResPass] vp0=[x… …]` — the second eye's viewport x-origin.

| observed | means |
|---|---|
| `2054` | packed; the August measurement was confounded |
| `2328` | separated; the shader clamp does not describe this configuration |
| a full-target viewport | **inconclusive at this boundary** — needs a scene-time observation |

The third outcome is a real answer, not a failure.

### Result

```
STATUS: PENDING
```

---

## Gem 3 — the vendor input may legitimately be larger than the picture inside it

**Claim.** Per-eye vendor **input** textures are reused when they are merely
*large enough*; **output** textures must match exactly. Under Hot-Envelope that
slack becomes reachable, so a boot-quality input is kept for a smaller active
field and never resized.

**Attribution.** Stock CSX.

**Evidence.**

```cpp
// Upscaling.cpp:37380  AreActiveVRIntermediateTexturesCompatible
if (!stableFSRInputBounds && vrIntermediateTextureGeneration != a_contractGeneration)
    return false;
...
// :37428   inputs
a_texture->desc.Width >= allocationWidth
// :37435   outputs
a_texture->desc.Width == a_outputWidth
```

**Why no shipped flow sees it.** A quality change bumps the contract generation,
the guard fires, everything is recreated, and `>=` never has to be `>`. It is
dead slack. Hot-Envelope holds that generation stable **on purpose** - that is
the feature - so the guard passes and the slack becomes live.

The result is a texture whose description says `2328` holding a picture `2054`
wide, with the remaining columns carrying whatever the previous quality left
there. In the phase 1 contract model:
`resourceExtent 2328x2372, coverage { field 2054x2092, covered 2054x2092 }`.

**Two consumers already handle it**, which is what makes this interesting rather
than alarming - the state is modelled, not overlooked:

```cpp
// Upscaling.cpp:38412  StretchSubmitStageEyeOutput passes BOTH numbers
stretchData.inputSize         = { inputWidth, inputHeight };     // the field
stretchData.sourceTextureSize = { desc.Width, desc.Height };     // the resource

// Streamline.cpp:1949  DLSS is told the field, not the resource
sl::Extent extentIn{ 0, 0, eyeWidthIn, eyeHeightIn };
```

There are **109 further references** to those per-eye arrays across the upscaling
sources. Those two are correct; the rest are unaudited.

**Note.** The slack covers depth and motion vectors too, where a misread would
misregister reprojection rather than crop - a different symptom class, worth
keeping separate.

**Test.** `[VRIntermediate] ... oversizedInput={}`.

| observed | means |
|---|---|
| RS-on: descriptions **equal** the field | no slack ever taken |
| envelope after a downward change: descriptions **exceed** it | mechanism live; 109 consumers now matter |
| envelope: descriptions **equal** the field | slack not reached in practice; gem 3 is theoretical |

**Honest limit.** This does **not** explain a zoom. An unhandled consumer would
more likely make the world look *smaller* with stale edge columns.

### Result

```
STATUS: PENDING
```

---

## Gem 4 — one normalized bound, two different extents, same function

**Claim.** `ResolveVRSubmitSourceRegion` resolves the *same* OpenVR bound against
**the physical resource width for colour** and **the logical stereo-layout width
for depth**. Those agree only when the rendered field fills the resource.

**Attribution.** **Stock CSX** - and this was corrected while writing. An earlier
draft framed gem 4 around our own two-arm eye-origin sweep. That sweep is ours;
the colour-versus-depth extent split underneath it is upstream, and it is the
more interesting half.

**Evidence.**

```cpp
// Upscaling.cpp:1458  ResolveVRSubmitSourceRegion   (stock, 82 lines)

// :1495  COLOUR - resolved against the physical resource
const uint32_t left  = Util::NormalizedCoordinates::ResolvePixelBoundary(minU, sourceDesc.Width);
const uint32_t right = Util::NormalizedCoordinates::ResolvePixelBoundary(maxU, sourceDesc.Width);

// :1509  DEPTH - resolved against the logical stereo layout
const uint32_t depthLeft  = Util::NormalizedCoordinates::ResolvePixelBoundary(minU, sourceStereoLayout.width);
const uint32_t depthRight = Util::NormalizedCoordinates::ResolvePixelBoundary(maxU, sourceStereoLayout.width);
```

`ResolvePixelBoundary` is `round(clamp(u,0,1) * extent)`
(`Utils/NormalizedCoordinates.h:15`). `minU` comes from the OpenVR bounds, which
are `u[0,0.5]` and `u[0.5,1]` - a **logical** split the game submits whatever the
active quality. `sourceStereoLayout` is built from `sourceEyeWidthIn`, so its
`width` is the **field**; `sourceDesc.Width` is the **resource**.

> A normalized coordinate is only meaningful against the extent it normalizes.

**Why no shipped flow sees it.** In both, the field fills the source - Render
Scale on because `A == R`, Render Scale off because the submit source is at full
output size. The two extents are the same number, so nothing distinguishes them.
Under Hot-Envelope they differ by `A_eye - R_eye`: **2328 against 2054**, so
colour and depth resolve the same bound 274 pixels apart.

**What that does NOT mean.** CS's own comment states this depth box drives
`DispatchHMDMaskClear`, **not** DLSS's depth input. So the consequence is about
the hidden-area mask, not vendor reconstruction - a different symptom class
(nose-region artefacts), and not evidence for the zoom. Recorded because the
*mechanism* is a two-extent split, which is the defect class this project exists
to find.

**Test.** Partly the same telemetry - `[DynResPass]` gives the resource width and
the field extent at that point, so the two candidate origins can be computed and
compared. Fully settling it needs the mask-clear box logged as well, which is
**not yet instrumented**.

### Result

```
STATUS: PENDING (partial coverage only)
follow-up: log the DispatchHMDMaskClear box alongside the colour box
```

---

## Gem 4b — our headline symptom is one experimental arm's symptom

**Claim.** `vrHotEnvelopeEyeOrigin` has **no neutral value** - only packed,
allocation half, or a manual pixel. It defaults to the allocation half. The two
arms fail *differently*, and what we reported as "the" defect is the default
arm's.

**Attribution.** Ours. A correction to our own record, not a finding about CSX.

**Evidence.** The comment above the eye-origin switch in `Upscaling.cpp`:

> both broke stereo: the packed origin ... was **cross-eyed**, the allocation
> half was **flat/cardboard**, and each was correct only at the envelope quality -
> which is the one case where they are the same pixel, so neither run
> distinguishes anything.

against `README.md` section 3 as it stood: *"the world looks too close and
flattened"*.

**Consequence.** "Both were built and both failed" is true but compressed away
the fact that they failed in two different ways - which is evidence. And the
imprecise description had gone to the Community Shaders community three days
earlier.

### Result

```
STATUS: DONE - README.md section 3 corrected 2026-08-21, correction left visible
```

---

## What the register cannot do

None of these says **what the pixels are**. Every entry is about extents,
descriptions, boxes and control flow - what a pass was *asked* to do.

A texture's dimensions never describe the coordinate state of the image inside
it, which is the whole reason `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md` exists.
So this register **scopes** that protocol - it says which branches of the
decision table are live and which are already excluded - and does not replace it.
No entry here may be reported as confirming `H1`.

What the register *does* give is compounding. Four entries, four different
places, one underlying shape: **a number that meant one thing in every shipped
configuration now means two.** In a copy box, in a shader constant, in a
resource-compatibility test, and in a normalized coordinate. That consistency is
evidence about the defect **class** even while the instance stays open - and it
is the reason to keep collecting rather than stopping at the first plausible
cause.

## Adding a new gem

```markdown
## Gem N - one-line claim

**Claim.**
**Attribution.** stock CSX / ours - verified how?
**Evidence.** file:line at the CSX3.18 baseline, quoted
**Why no shipped flow sees it.**
**Test.** which log line, and what each outcome would mean
**What would falsify it.**

### Result
STATUS: PENDING
```

Then add a row to the register table.

Say plainly when one turns out to be nothing. **A gem that dies under measurement
is worth as much as one that survives**, and this register is the place that has
to show it - otherwise it becomes a list of things we believe rather than a list
of things we checked.

## Related

| document | what it adds |
|---|---|
| `FINDING_DYNAMIC_RESOLUTION_PASS_REPLACEMENT.md` | gems 1, 2, 4 in narrative form, with the reasoning that found them |
| `CDO_EXECUTION_LOG.md` | the running record of the protocol, and the parked items |
| `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md` | the strategy these scope but do not replace |
| `PHASE_1A_SITE_TABLE.md` | the 116 consumers of the two extents, classified |
