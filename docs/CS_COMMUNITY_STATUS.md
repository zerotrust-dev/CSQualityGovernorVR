# Where we are, 2026-08-21 (evening)

The active plan is now `docs/PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md`.
`docs/PLAN_GEOMETRY_TYPE_SPLIT.md` (revision 5) is **on hold** — its finished
phases are kept and the new protocol starts from their output.

---

## What changed

After the CS community meetup on 2026-08-21, the type-split plan was merged with
the insights from that discussion into one objective execution protocol. The
substantive change is an ordering one:

> Localize the **first divergence** objectively before correcting anything.

The old plan's next step was a semantic pass over the site table — that is, decide
which consumers look wrong and change them. The new protocol treats that as a
seventh guess. Instead it records the coordinate transform at every pipeline
boundary, finds the earliest edge whose measured mapping contradicts a
pre-registered, independently sourced contract, and only then changes one
variable in a counterbalanced A/B to test causality.

Nothing that was finished is discarded.

## Phases

### Done and kept

| phase | what it produced | status |
|---|---|---|
| **0** | evidence ledger; reproducible site counts; corrected float labels | **complete** |
| **0A** | `VRGeometryPolicy.h`, the pure planner, and its compile-time tests; production takes both extents from one `Derive()` call | **complete** |
| **1a** | `engineRenderSize` / `engineAllocationSize` retyped to mutually non-convertible types; **116 sites across 32 consumers** enumerated by the compiler and classified | **complete**, CI green on the plugin build |

Phase 1a's deliverable is `docs/PHASE_1A_SITE_TABLE.md`. Its result in one line:
ten suspect consumers, 43 sites proven off the envelope path, and one falsifiable
hypothesis connecting the menu eye-path split to the same conflation.

### The active protocol

| phase | what it is | headset |
|---|---|---|
| 0 | freeze the protocol and the experiment manifest | no |
| 1 | the pure pipeline contract model: tagged layouts, half-open extents, affine transforms, the Raw and Expanded candidate paths | no |
| 1A | the offline analyzer, with synthetic and golden fixtures, built and tested in CI | no |
| 2 | one immutable frame contract — a plan hash every boundary must carry | no |
| 3 / 3A | passive boundary telemetry B-1..B7, no readback; then close the decoder gaps it finds | yes |
| 4 | prove the telemetry is behaviour-null against a minimal common recorder | yes |
| 5 | bounded passive resource capture | yes |
| 6 | score the passive candidate frontier | no |
| 7 | add the absolute screen-space fiducial and confirm the frontier | yes |
| 8 | one-variable causal A/B at the localized frontier | yes |
| 9 | the minimal fix, plus non-regression on both shipped flows | yes |
| 10 | qualitative safety veto, then measure the prize | yes |

Where the deferred phases went: old 1b (`BindResources()`) becomes new phase 1,
generalized from regions to layouts and transforms; old 2 (semantic correction)
becomes new phase 8, with a causal test in front of it; old 3G/3P become new
phase 9; old 4 becomes new phase 10.

## The honest position

The relatch problem is solved and the GPU saving is real and measured. The image
below the envelope quality is still wrong, six attempts to fix it have failed, and
the previous strategy would have made a seventh attempt better-informed rather
than making it unnecessary.

The new protocol's promise is deliberately narrower and harder: not "we will fix
it", but **"we will identify the first boundary at which the measured coordinate
mapping contradicts a contract we wrote down beforehand — or we will name which
assumption failed."** Outcomes include `INCONCLUSIVE`, `DIAGNOSTIC_INTERFERENCE`
and `OPAQUE_TEMPORAL_STATE`, and those are reportable results rather than
failures to be reworded.

The leading hypothesis is a duplicate spatial transform: if an earlier Skyrim
pass already expanded the complete image from the render extent to the
allocation, and the submit path then crops a render-extent region and expands
that again, the predicted zoom is `A_eye / R_eye` — about 1.13x at Balanced,
1.33x at Performance, 2.00x at UltraPerformance, and exactly 1.00x on the
envelope diagonal where the image is known to be correct. Consistent with
everything observed; not proven, and its falsifier is written down.
