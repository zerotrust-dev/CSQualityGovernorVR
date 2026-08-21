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
| CSQualityGovernorVR | *(see below)* | protocol, explainer, execution log, manifest, evidence skeleton |

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
