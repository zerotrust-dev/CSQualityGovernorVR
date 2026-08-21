# CDO-001 execution log

**Experiment:** `CDO-001` — compositional differential oracle for Hot-Envelope.
**Protocol:** `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md`
(SHA-256 `683aa8f393ccdbeaa23fb90fa80b863ffe04d31ddd003f977679b4c71f003eb2`).
**Opened:** 2026-08-21.

This is the running record of everything done under the new strategy: what each
phase produced, what it cost, what it found, and what was decided. It is
appended to, never rewritten. A correction is a new dated entry that says what
it corrects, not an edit to the entry that was wrong.

The protocol says what we intend to do. **This says what we actually did.**

---

## Parked items

Raised, deliberately not acted on yet, with the trigger that brings each back.
Nothing here is a blocker today; all of it is a blocker later. Check this list
at the start of every phase named in the "raise at" column.

### P-1 — the RS-off control fails its own first edge

**Raise at:** Phase 0 review, and **hard gate at the `edge-contracts.json`
step** (protocol §8.4), which is before Phase 5 capture.

**The conflict.** §8.4 defines the `B-1 -> B0` predicate as *"exact integer
equality with `VRGeometryPolicy::Derive`; zero tolerance."* But `OFF-Q4` is one
of the five localization controls, and under Render Scale **off** production does
not take its render extent from the planner. It uses
`resolveVendorDynamicRenderSize()`, which reads `resolutionScale` — and
`ConfigureUpscaling` writes `resolutionScale` later in the same frame. For
exactly one frame after a quality change the plan therefore carries the previous
quality's render size (`FINDINGS-FOR-CS.md` §2.4; found in phase 0A by running
the planner beside production).

So a control run will FAIL its very first scored edge, for a known benign
reason, unless something is decided first.

**Why it is not merely cosmetic.** A control that fails an edge cannot calibrate
the tolerance for that edge, and §6.2 forbids declaring a later edge "first"
while an ancestor is FAIL or INCONCLUSIVE. Left alone, this stalls the ancestor
walk on every RS-off run.

**The two ways out**, both legitimate, to be chosen deliberately:

1. **Characterize.** Write the one-frame lag into the RS-off `B-1 -> B0` edge
   contract as expected behaviour, with the frame-relative window stated. This
   is what protocol Phase 1 item 1 already asks for ("characterize, rather than
   silently replace"). Keeps the baseline untouched.
2. **Make the planner authoritative for RS-off** before capture. Cleaner
   predicate, but it changes shipped-flow behaviour by removing the lag — which
   is exactly the kind of silent repair the protocol exists to prevent. If
   chosen, it is its own commit with its own before/after evidence, not a
   side effect of writing a contract.

**Recommendation:** option 1. The lag is real behaviour of the flow we are using
as a control; a control should be described, not corrected. Option 2 also
mutates a baseline mid-protocol.

**Related:** the planner derives `finalOutputSize` as `CombineStereo(displayPerEye)`
while production assigns it directly. These differ only when the display width is
odd. This headset is 3494 per eye, so 2 x 3494 = 6988 = the display width and the
two agree exactly. **Not an issue on this hardware**, but the edge contract
should say so rather than leave it to luck on someone else's headset.

### P-2 — allow Phase 3 an explicit early read

**Raise at:** the end of **Phase 3**, before committing to the Phase 1A / 3A /
5 / 7 build-out.

**The observation.** Phase 3 is metadata-only: no readback, no analyzer, no
fiducial. But item 2 of that phase records *whether each Skyrim
dynamic-resolution pass executed, was replaced, or fell back*, and item 3
records every bound source and destination region.

H1 — the duplicate spatial transform — predicts that an `R -> A` expansion runs
and the submit path then crops `R` and expands it again. **If that is what
happens, both halves of it are visible in Phase 3 metadata alone:** a pass that
executed with an `R`-sourced, `A`-sized destination box, followed by a submit
path sourcing an `R`-sized box. No pixel is read to see that.

**What this would and would not buy.** It would tell us which branch of the §12
decision table we are on after phases 0–3 instead of 0–7, and let the analyzer
decoders (1A), the capture-format work (3A) and the fiducial (7) be scoped to
what is actually needed instead of built for every branch.

It would **not** prove H1, and must not be allowed to. The protocol is right
that a recorded box does not establish the coordinate state of the content
inside it (§2, §4 falsifier: "seeing an `R_eye` field at one boundary alone is
not enough"). A box says what a pass was *asked* to do; only the image says what
the pixels *are*.

**So the proposal is narrow:** at the end of Phase 3, write down which branch
the metadata indicates and use it to *scope* later phases. Record it as a
prediction with the date, so it is on the record before the image evidence
exists and cannot be retrofitted. It does not localize, does not close an edge,
and does not skip a phase.

**Decision:** open. Recommend deciding it at the Phase 3 exit, not now — the
value depends on what the pass graph actually looks like.

---

## Phase log

### Phase 0 — freeze the protocol · 2026-08-21 · COMPLETE

**Protocol requirement (phase 0):** commit the protocol document before any
diagnostic implementation; create an experiment ID and evidence directory;
record the branch commit and upstream baseline; copy the test matrix,
thresholds, settle period and rejection rules into the manifest; do not edit the
manifest after the first Hot-Envelope capture.
**Exit condition:** the protocol and manifest exist before any scored capture.

| step | done |
|---|---|
| 1. protocol committed before implementation | yes — see "commits" below |
| 2. experiment ID | `CDO-001` |
| 3. evidence directory | `evidence/compositional-differential-oracle/CDO-001/` |
| 4. branch commit and upstream baseline recorded | yes |
| 5. matrix, thresholds, settle period, rejection rules in the manifest | yes |
| 6. immutability rule stated with an append-only amendment list | yes |

**Baseline verified, not assumed.** The protocol names commit `2eaf90dd` "and
the first successful CI run made after this protocol is committed". That commit
is `2eaf90ddc4a8c2e8f0ab2f62f183c4a152570504`, and its CI run
[32457806725](https://github.com/zerotrust-dev/skyrim-community-shaders/actions/runs/32457806725)
was checked rather than presumed:

```
success  Build (PR) / Build plugin and addons (vs2026)      <- required
success  Build (PR) / Validate shader compilation (VR)
success  Build (PR) / Run Shader Unit Tests
failure  Build (PR) / Validate shader compilation (Flatrim)  <- known, documented
```

The Flatrim failure is recorded in the manifest with its cause and the condition
under which it stays acceptable: byte-for-byte the same failure, and any new
failure blocks the phase.

**What the manifest deliberately leaves null.** The DLL hash, the analyzer
identity, the accepted candidate transforms and the frozen thresholds are not
knowable at phase 0. Each is present as `null` with a `dueAtPhase`, so a missing
value is visibly outstanding rather than quietly absent. `edge-contracts.json`
is listed as absent with `dueBeforePhase: 5` and P-1 named as its blocker.

**One thing carried in from the previous strategy.** The `stateAtBaseline` list
records what the branch does and does not yet do, including that the planner is
**not** authoritative for the RS-off render extent. That is the fact behind P-1,
and it is in the manifest so the conflict cannot be discovered mid-capture.

**Not done, and deliberately so:** no code was written, no instrument was
designed, no capture was taken. Phase 0 is a freeze, not a start.

#### Commits

| repo | commit | content |
|---|---|---|
| CSQualityGovernorVR | `388ff28` | protocol, explainer, execution log, manifest, evidence skeleton, LF pinning |

#### Cost

About one working session, all of it reading and recording. No CI, no build,
no headset.

---

## Standing rules for this log

1. **Append, never rewrite.** A correction is a new dated entry naming what it
   corrects. The wrong entry stays.
2. **Record cost, not only outcome.** What a phase took is part of whether the
   next one is worth it.
3. **Record what was not done** and why. A phase that skipped something is not
   the same as a phase that had nothing to skip.
4. **A non-success outcome is an entry, not a gap.** `INCONCLUSIVE`,
   `DIAGNOSTIC_INTERFERENCE`, `OPAQUE_TEMPORAL_STATE`, `MISSING_BOUNDARY` are
   results and get the same treatment as a pass.
5. **Check the parked items** at the start of every phase named in their
   "raise at" line.
6. **CI is judged by `Build plugin and addons (vs2026)`.** The Flatrim shader
   job fails on stock CSX code and is accepted only while byte-identical to the
   documented failure.
7. **No locally compiled DLL is evidence.** Hash what was installed, from the
   exact CI artifact.

## Where the previous strategy went

`PLAN_GEOMETRY_TYPE_SPLIT.md` is on hold, not discarded. Its phases 0, 0A and 1a
are done, and this protocol starts from their output. Its remaining phases
reappear here:

| old | new |
|---|---|
| 1b `BindResources()` | phase 1, generalized from regions to layouts and affine transforms |
| 2 semantic correction | phase 8, with a counterbalanced causal test in front of it |
| 3G / 3P verification | phase 9 |
| 4 measure the prize | phase 10 |

The phase 1a site table is not superseded. Its ten suspect consumers are prior
candidates for the frontier, mapped onto this protocol's boundaries:

| suspects from `PHASE_1A_SITE_TABLE.md` | boundary |
|---|---|
| `TryReplaceVanillaDynamicResolutionUpsample` | **B3** — the same boundary H1 predicts |
| `PreparePerEyeInputs`, `FinalizePerEyeOutputs`, `Upscale()` | B2 to B4 |
| foveated region block, `RefreshSubmitStageUnderwaterMask` | B5 |
| `CheckResources`, `ApplyPendingVendorRuntimeReset`, `BuildFSRResourceLifecycleRequestKey` | B6 |
| `SubmitVRUpscaledFrame` | B7 |

They are **priors, not findings**. The protocol scores every edge on its own
evidence; a suspect that turns out to pass is a real result and gets recorded as
one. What the table buys is that no boundary starts with an empty hypothesis
set, and that 43 sites are already known to be off the envelope path.

---

### Verification pass · 2026-08-21 · phase 0 · PASS with two corrections

New standing rule from Rik, applied from here on: **re-check what we created
before every push, and prove each phase met its expected result before starting
the next.** This is the first application.

**Method.** Every number in the manifest was checked against the compiled
`static_assert`s in `tests/vr_geometry_policy_test.cpp`, not against my own
arithmetic. That ordering is deliberate: this project has twice produced
confident numbers that were wrong — float32 scale labels written as `1.0f/1.3f`
instead of `0.769230783`, and a PowerShell verification that produced five
values one pixel high because `[int]` rounds where C++ truncates. The compiled
assertion outranks any recomputation.

| manifest value | compiled source | result |
|---|---|---|
| `ON-Q3` A 4656x2372 | `On(3).plan.allocationCombined` | match |
| `ON-Q4` A/R 4108x2092 | `On(4).plan.allocationCombined` | match |
| `OFF-Q4` R 4110x2092 | `Off(4).plan.renderCombined` | match |
| `HE-Q3-Q4` A 4656x2372, R 4108x2092 | `On(3)` allocation, `On(4)` render | match |
| Performance `R_eye` 1746x1778 | `On(5).plan.renderPerEye` | match |
| UltraPerformance `R_eye` 1164x1186 | `On(6).plan.renderPerEye` | match |
| Quality `A_eye` 2328x2372 | `On(3).plan.renderPerEye` | match |
| the 2-pixel `OFF-Q4` vs `HE-Q3-Q4` width gap | 4110 vs 4108 | match |
| display period 13.889 ms | 1000 / 72 = 13.8889 | match |

**Correction 1 — the H1 prediction was recorded on one axis only.**

The manifest carried `zoomX` alone. H1 predicts `A_eye / R_eye` **per axis**,
and those axes do not agree:

| preset | zoom X | zoom Y |
|---|---:|---:|
| Balanced | 1.1334 | 1.1338 |
| Performance | 1.3333 | 1.3341 |
| UltraPerformance | 2.0000 | 2.0000 |
| Quality (diagonal) | 1.0000 | 1.0000 |

The split comes from the even-forcing in `ScaleVRRenderDimension`, which lands
differently on width and height. Recording only the horizontal would have
thrown away a discriminator: **an observed zoom that is isotropic where the
prediction is anisotropic is evidence against a pure duplicate expansion.**
Both axes are now recorded and both will be scored.

**Correction 2 — an ambiguous baseline reference.** `governorPlugin.commit`
read `b7f365b`, which is the commit *before* the freeze, while the manifest
itself lives in `388ff28`. Renamed to `commitBeforePhase0Freeze` with a note,
so the record cannot be misread as claiming the manifest describes its own
parent.

Both corrections are new commits, not edits to the phase 0 entry above.

### Phase 0 exit condition · answered

> **Exit:** the protocol and manifest exist before any scored capture.

**Met.** `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md` and `manifest.json` are
committed at `388ff28`; no capture, no instrument and no rendering change
exists. The stronger reading — that the manifest is *complete enough to be
frozen* — is also met: every value that can exist at phase 0 is present and
verified above, and every value that cannot is `null` with a `dueAtPhase`.
Nothing is silently missing.

---

### Phase 1 — the pure pipeline contract model · 2026-08-21 · awaiting CI

**Protocol requirement (phase 1):** keep `VRGeometryPolicy::Derive`
authoritative and characterize rather than replace the RS-off producer; add a
dependency-free pipeline contract model for tagged origins, half-open extents,
resource layouts and affine transforms; encode the Raw and Expanded paths from
section 7.1; add tests for all seven qualities, the 7x7 matrix, packed and
allocation-separated stereo, per-eye and array-slice resources, transform
composition, the predicted duplicate-scale matrices, and invalid containment
and generation combinations; ensure no production consumer reads it yet.
**Exit condition:** the finite candidate transforms compile and their golden
values are fixed before runtime data is seen.

**Delivered.** `src/Features/Upscaling/VRPipelineContract.h` (520 lines) and
`tests/vr_pipeline_contract_test.cpp` (460 lines, 70 `static_assert`s), plus
one CMake registration. **No production file was touched** — `git diff` against
`Upscaling.cpp`, `Upscaling.h`, `Streamline.cpp` and `FidelityFX.cpp` is empty,
and nothing outside the test includes the new header.

#### The design decision that matters

The model **takes per-eye extents as inputs and derives no size of its own.**
Every extent in the test comes from `VRGeometryPolicy::Derive`. That is not a
layering preference: duplicating the quality-scale table is precisely how this
project previously produced float labels wrong in the fourth decimal, and a
contract model that could disagree with production about a size would be worse
than none.

Coverage is carried as **exact integers** and compared as **exact rationals by
cross-multiplication**. Nothing is divided, so there is no decimal to get wrong.
Floats appear only in the analyzer-facing affine form, and that form is derived
from the integer model rather than maintained beside it.

#### What the model adds beyond size

A boundary is described by three things, never by a size alone:

```
resourceExtent   how big          - a capture confirms this directly
activeRegion     where            - a capture confirms this directly
coverage         which part of the complete logical eye field those pixels are
```

The third is the one a resource description cannot answer, and the one six
failed attempts kept guessing. `FieldCoverage{ fieldExtent, covered }` states it
exactly: a raw field in an A-sized resource is `field == R, covered == R`; a
field already resampled to A is `field == A, covered == A`; **an R-sized crop of
a resampled field is `field == A, covered == R`** — and that third state is
entirely legal in every resource description while holding `R/A` of the picture.

#### Two errors caught during the write, before CI

1. **Active region conflated with field coverage.** At the vendor output the
   whole output resource is active while only part of the field is present.
   Passing one extent for both made `Expect(B6/B7, DuplicateExpansion, ...)`
   report an R-sized active region in an O-sized resource — wrong, and wrong in
   a direction that would have made the defect *easier* to spot than it is.
   Split into separate parameters.
2. **Separated stereo strided by the engine allocation at every boundary.** At
   B7 that meant striding an output-space resource by 2328, which is
   meaningless — and, worse, still produced a contained, legal-looking region.
   Changed to stride by the per-eye half of *that* resource. A regression test
   now pins `SubmitOrigin(AllocationSeparatedStereo, eye 1) == 3494`.

#### Verification before push

Per the standing rule. Extents were not recomputed — they come from
`VRGeometryPolicy` and were already verified against its compiled assertions in
the phase 0 pass. What needed independent checking was the model's own
arithmetic:

| claim | check | result |
|---|---|---|
| duplicate zoom X at Balanced | `Ratio{2328, 2054}`, cross-multiplied | exact, no division |
| duplicate zoom Y at Balanced | `Ratio{2372, 2092}` | exact |
| duplicate zoom at UltraPerformance | `2328/1164 = 2`, `2372/1186 = 2` | exactly 2 in both axes |
| diagonal zoom | `2328/2328`, `2372/2372` | unity — the defect vanishes |
| affine `sx` for duplicate@UltraPerf | `3494 x 2328 = 8134032 < 2^24`, `/1164 = 6988` | exactly representable in float32 |
| affine `sy` for duplicate@UltraPerf | `3558 x 2372 = 8439576 < 2^24`, `/1186 = 7116` | exactly representable |
| affine for the correct paths | `3494 x 1164 / 1164`, `3558 x 1186 / 1186` | exact |
| packed eye-1 origin at B2 | `1 x 2054`, right edge `4108 <= 4656` | contained |
| separated eye-1 origin at B2 | `1 x 2328`, right edge `4382 <= 4656` | contained |
| packed eye-1 origin at B7 | `1 x 3494`, right edge `6988 <= 6988` | contained, exactly |

Every float asserted in the test is an exactly representable float32 value: each
product stays under 2^24 and each division is by a factor of its numerator. That
was chosen deliberately — **a candidate transform that needed a tolerance to
assert would not be a candidate the analyzer could be scored against.** Balanced
and Performance are therefore asserted as rationals only, never as floats.

Also confirmed by hand: `Derive` does **not** clamp a non-fitting render extent,
it returns it and sets `RelatchRequired`. Without that, the invalid-containment
test would have found nothing to reject and would have passed vacuously. The
test guards against exactly that by requiring it saw at least one non-fitting
pair.

#### Exit condition · answered, pending CI

> **Exit:** the finite candidate transforms compile and their golden values are
> fixed before runtime data is seen.

- **Golden values fixed before data:** yes. Every assertion is a `static_assert`,
  so the values are frozen in the binary; no capture exists.
- **Compile:** **not yet confirmed.** No MSVC is available on this machine, so
  this cannot be claimed until CI reports. The phase is not complete until it
  does.

One foreseeable failure mode worth naming now rather than diagnosing later:
`EveryStateIsContained` evaluates roughly 2,940 `Expect` calls in a single
constant expression. That is well inside MSVC's default limit of 1,048,576
constexpr steps on my estimate, but if CI reports a step-limit error the fix is
to split the loop per layout, not to weaken the assertion.

#### Deliberately not done

- **P-1 was not acted on.** Its trigger is the `edge-contracts.json` step, not
  this phase. The RS-off producer is untouched and uncharacterized; phase 1's
  instruction was to characterize *rather than replace*, and characterizing it
  belongs with the edge contract that will consume the characterization.
- **`BindResources()` was not implemented** as a production stage. The model is
  the pure half of it; binding to live D3D resources is not a phase 1 exit.
- **B0, B1 and B5 have no coverage model.** Plan and camera state are not
  colour-field coverage questions, and the auxiliaries need correspondence
  contracts rather than coverage. Named here so their absence is not later read
  as an oversight.

#### Commit

| repo | commit | content |
|---|---|---|
| skyrim-community-shaders (`csx318-hot-envelope-diag`) | `db9687f77` | `VRPipelineContract.h`, `vr_pipeline_contract_test.cpp`, CMake registration |

Pushed 2026-08-21. CI result pending — Rik reports it. **Phase 1 stays open
until the plugin build and `controller_tests` are green.** Phase 1A does not
start before then, because an analyzer built against a candidate set that does
not compile would be built against nothing.

#### Cost

One session. No CI time spent by me, no headset, no game launch.

---

### Correction · 2026-08-21 · I corrupted `D:\CS\README.md` and caught it

Recorded rather than quietly fixed, per the project's own rule.

**What happened.** Editing the community package README, I used PowerShell
`Get-Content -Raw` to read it and `[System.IO.File]::WriteAllText` to write it
back. In Windows PowerShell 5.1 `Get-Content` decodes a UTF-8 file *without a
BOM* as ANSI, while `WriteAllText` encodes as UTF-8. The round trip therefore
re-encoded every non-ASCII character. **All 21 em-dashes became mojibake** —
27 corrupted byte sequences, zero clean ones — in a document written for people
outside this project to read.

**How it was caught.** The corruption was visible in the console output of my
own verification command. The value of the standing rule is not that it prevents
mistakes; it is that the check happened at all.

**How it was fixed.** Rebuilt from the pristine copy taken at the start of the
session, re-applying the edits with UTF-8-safe tools, then verified at the byte
level: `0` occurrences of `C3 83`, `20` of `E2 80 94`. Every `.md` file in
`D:\CS` was then scanned for the same signature; none was affected.

**The general rule this adds.** *The language's default encoding is part of the
tool's semantics, and defaults differ between reading and writing in the same
shell.* This is the same class of error as the earlier PowerShell `[int]`
rounding where C++ truncates, which produced five values one pixel high and
briefly looked like a discovery. Both times the "more convenient" tool silently
changed the data.

**Practice going forward:** edit UTF-8 documents with byte-preserving tools, and
verify at the byte level rather than by eye — a corrupted em-dash renders as
plausible-looking noise and is easy to skim past.
