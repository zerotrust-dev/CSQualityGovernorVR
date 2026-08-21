# Plan: one pure geometry planner, typed consumers, bound resources

> **ON HOLD from 2026-08-21, after phase 1a.** Superseded as the active strategy
> by `PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md`, which merges this plan with
> insights from the CS community meetup. Phases 0, 0A and 1a are **done and
> kept** — the new protocol starts from their output, at commit `2eaf90dd` with
> its green plugin build, and explicitly reorders what follows: objective
> first-divergence localization comes *before* any phase 2 semantic sizing
> correction, so existing behaviour stays characterized rather than repaired.
>
> Phases 1b, 2, 3G, 3P and 4 below are **not cancelled but deferred**. Their
> content reappears in the new protocol as phase 1 (the pure contract model,
> which is `BindResources()` generalized to layouts and affine transforms),
> phase 8 (one-variable causal A/B in place of a semantic pass), phase 9
> (the minimal fix and non-regression) and phase 10 (measure the prize).
>
> This document remains the reference for how the three geometries were derived
> and why the six earlier attempts failed. Read it for that; execute the other.

**Revision 5** — 2026-08-20. **Implementation-ready.**
**History:** r1–r2 type split; r3 withdrew the allocation-discriminator claim;
r4 added Rik's derived baseline; **r5 promotes that derivation from a test
oracle to the production source of truth**, per Codex's r4 review.

---

## 0. What changed in revision 5

| # | change | source |
|---|---|---|
| 1 | **`DeriveGeometry()` becomes production code**, not a parallel model. One pure entry point, one immutable decision per frame | Codex C1 |
| 2 | **7 x 7 boot x active matrix** plus recovery phase, metamorphic invariants, and an HMD-dimension sweep. The r4 table was only the diagonal | Codex C2 |
| 3 | **Performance is below the vendor accepted *minimum*, not merely off optimal.** Elevated to the top phase 2 candidate, with a pre-registered experiment | Codex C3 |
| 4 | Two float32 labels corrected; two prose claims narrowed | Codex C4, verified independently |
| 5 | **`BindResources()` as a separate pure stage.** Eye origin belongs there, never in the logical planner | Codex C5 |
| 6 | **Phase 0A inserted before the type migration** — extract the calculator first, so 124 sites are retyped around one authoritative producer | Codex |

## 1. The problem

CSX has three logical geometries described by four `float2` fields
(`trueHMDDisplaySize`, `engineAllocationSize`, `engineRenderSize`,
`finalOutputSize`). In every shipped configuration two of the three coincide:

| configuration | allocation | render | output |
|---|---|---|---|
| Render Scale **off** | = display | ratio x display | display |
| Render Scale **on** | quality-sized | **= allocation** | display |
| **Hot-Envelope** | boot-quality-sized | **< allocation** | display |

Hot-Envelope is the first configuration where all three differ at once.
Codex confirmed `RefreshRuntimeResolutionPlan()` assigns
`finalOutputSize = trueHMDDisplaySize` under Render Scale Mode, so both are
output-space extents.

## 2. Evidence, and what it does and does not support

**Withdrawn in r3:** "allocation is the discriminator". It came from reading a
`sort -u` listing as if it were representative. The archived log counts 30 clean
windows against 8 fragmented at allocation 4656; the fragmentation is
**intermittent at a fixed allocation and render extent**, which points at
timing or lifecycle rather than static geometry. The `EMPTY` windows are all at
UltraPerformance, a preset that never ran at the comparison allocation, so that
category is preset-confounded.

**Still standing:** five patches have failed, each locally correct; we cannot
state what fraction of the geometry surface any session covered; and a rigorous
negative result is worth the same effort as a fix — now the more likely outcome.

**Provenance.** Run 2026-08-20 13:34–13:41, build `fbae22c36` (reverted by
`6fd49535d`), archived at
`captures/geometry-evidence-2026-08-20/CommunityShaders_run-fbae22c36.log`,
SHA-256 `0380f23c…4bff495`, confirmed by Codex. **Gap:** the preceding run on
`3b3041753` was overwritten before archiving, so its "eye image is whole"
finding is not reproducible from artifacts we hold.

## 2A. The derived baseline

Every visual observation is contaminated — the governor moved presets
mid-session, quality was changed by hand, and §2 records a case where I misread
my own instrument. Derivation removes all of that.

### 2A.1 The two rules, from source

**RS-off**, `ConfigureUpscaling`, on the **combined** width:

```cpp
renderWidth  = static_cast<int>(screenWidth  * resolutionScaleBase);
renderHeight = static_cast<int>(screenHeight * resolutionScaleBase);
```

**RS-on and envelope**, `ScaleVRRenderDimension`, **per eye**, then doubled:

```cpp
scaled  = static_cast<float>(a_dimension) * std::clamp(a_scale, 0.1f, 1.0f);
bounded = std::clamp<uint32_t>(static_cast<uint32_t>(std::floor(scaled)), 2u, a_dimension);
return bounded & ~1u;                                    // force even, both axes
```

Different operand (combined vs per eye) and different rounding (truncate vs
floor-then-force-even) for the same logical quantity.

### 2A.2 All three geometries, all three flows

The table the derivation was for. Envelope boot-latched at Quality, which is what
we run. Every cell is a combined-stereo extent.

| preset | RS-off alloc / render | RS-on alloc / render | Envelope alloc / render |
|---|---|---|---|
| NativeAA q0 | 6988x3558 / **6988x3558** | 6988x3558 / **6988x3558** | 4656x2372 / *6988x3558* |
| Hoshipa q1 | 6988x3558 / **5939x3024** | 5936x3024 / **5936x3024** | 4656x2372 / *5936x3024* |
| UltraQuality q2 | 6988x3558 / **5375x2736** | 5372x2736 / **5372x2736** | 4656x2372 / *5372x2736* |
| **Quality q3** | 6988x3558 / **4658x2372** | 4656x2372 / **4656x2372** | 4656x2372 / **4656x2372** |
| Balanced q4 | 6988x3558 / **4110x2092** | 4108x2092 / **4108x2092** | 4656x2372 / **4108x2092** |
| Performance q5 | 6988x3558 / **3494x1779** | 3492x1778 / **3492x1778** | 4656x2372 / **3492x1778** |
| UltraPerf q6 | 6988x3558 / **2329x1186** | 2328x1186 / **2328x1186** | 4656x2372 / **2328x1186** |

Output is **6988x3558 in every cell** - it is the display contract and never
moves, which is why it is omitted from the columns.

*Italic* render values exceed their allocation. The planner returns
`RelatchRequired`; production substitutes the allocation.

**This is the whole problem in one table:**

- **RS-off** — allocation is *always* the display. Allocation and output are the
  same number in every row, so those two can be confused freely and nothing
  breaks.
- **RS-on** — allocation and render are *always* equal. Those two can be
  confused freely and nothing breaks.
- **Envelope** — allocation is pinned by the boot quality while render moves
  independently. **All three differ at once.** It is the only column in which
  any confusion is visible, and therefore the only one in which the defect can
  appear.

Note also that Quality q3 is the single envelope row where allocation equals
render - and it is the only preset that has ever looked correct. Every other
envelope row has a gap, and the gap tracks the reported severity.

### 2A.3 The diagonal cases

Display 3494 x 3558 per eye. Scales are the **float32** values the source
expressions produce, verified with IEEE-754 single-precision division:

| q | preset | float32 scale | RS-off combined | per eye | RS-on / envelope | per eye |
|---|---|---|---|---|---|---|
| 0 | NativeAA | 1.000000000 | 6988 x 3558 | 3494 | 6988 x 3558 | 3494 |
| 1 | Hoshipa | 0.850000024 | **5939** x 3024 | 2969 | 5936 x 3024 | 2968 |
| 2 | UltraQuality | **0.769230783** | **5375** x 2736 | 2687 | 5372 x 2736 | 2686 |
| 3 | Quality | 0.666666687 | 4658 x 2372 | 2329 | 4656 x 2372 | 2328 |
| 4 | Balanced | **0.588235259** | 4110 x 2092 | 2055 | 4108 x 2092 | 2054 |
| 5 | Performance | 0.500000000 | 3494 x 1779 | 1747 | 3492 x 1778 | 1746 |
| 6 | UltraPerformance | 0.333333343 | **2329** x 1186 | 1164 | 2328 x 1186 | 1164 |

Bold scales are r4 corrections: `1.0f/1.3f` is `0.76923078298…` and
`1.0f/1.7f` is `0.58823525905…`. Revision 4 printed `…797` and `…319`. The
integers are unaffected. Independently recomputed rather than taken on trust.

**Validation — four independent paths, all agreeing:**

1. The **C++ `static_assert`s compiled in CI**. This is the authoritative check:
   the real compiler, the real `float32` expressions, the real truncation. A
   wrong integer would have been a build error.
2. An independent recomputation in IEEE-754 single precision with explicit
   truncation.
3. Every RS-on/envelope value reproduces an **observed boot latch** from the run
   logs.
4. RS-off q3 reproduces **Codex's captured** 4658 x 2372 with eyes at 0–2329 and
   2329–4658.

**A second arithmetic trap, recorded because it nearly put wrong values in this
table.** A verification pass produced 5940 / 5375x2737 / 4659 / 4111x2093 — each
one pixel high — and briefly looked like it had caught the derivation. It had
not. The cause was not precision at all: PowerShell's `[int]` cast **rounds**,
while C++'s `static_cast<int>` **truncates**. The "more careful" tool was the
wrong one.

So the rule from 2A.3 needs widening. It is not only *use float32 rather than
decimals* — it is **reproduce the production expression's rounding semantics as
well as its precision**, and treat any recomputation that disagrees with the
compiled `static_assert`s as wrong until proven otherwise.

### 2A.4 Finding 1 — RS-off orphans a column at three presets

At q1, q2 and q6 the combined width is odd, so
`eyeWidthIn = (uint32_t)(renderSize.x / 2)` truncates and `2 x eyeWidthIn` is
one less than the combined width. At Hoshipa: 5939 combined, 2969 per eye,
**column 5938 never covered**. The envelope cannot do this. Probably harmless;
real, and invisible until now.

### 2A.5 Finding 2 — the envelope is below the vendor's accepted minimum

Measured `slDLSSGetOptimalSettings` at this output, with Codex's confirmation
that qualities 1–3 map to `eMaxQuality`, 4 to `eBalanced`, 5 to
`eMaxPerformance`, 6 to `eUltraPerformance`:

| mode | optimal | accepted minimum |
|---|---|---|
| `eMaxQuality` | 2329 x 2372 | 1747 x 1779 |
| `eBalanced` | 2027 x 2064 | 1747 x 1779 |
| `eMaxPerformance` | 1747 x 1779 | 1747 x 1779 |
| `eUltraPerformance` | 1165 x 1186 | fixed, no range |

| preset | RS-off | envelope | verdict |
|---|---|---|---|
| Quality | 2329 x 2372 | 2328 x 2372 | RS-off exactly optimal; envelope 1 below optimal, still in range |
| Performance | 1747 x 1779 | **1746 x 1778** | RS-off exactly the minimum; **envelope one below the minimum in both axes** |
| UltraPerformance | 1164 x 1186 | 1164 x 1186 | both one below a fixed 1165; identical |

**This is a vendor-contract violation, not a rounding preference.** At
Performance the envelope feeds DLSS an extent smaller than the range Streamline
reports as accepted. That does not prove it causes the visual failure — drivers
may accept undocumented values, and UltraPerformance runs at 1164 against a
required 1165 in both flows — but it is the highest-priority phase 2 candidate.

Note also that every archived `EMPTY` probe window occurred at
UltraPerformance, the other preset below its stated contract.

**The even-forcing rule is questionable on its face** (Codex's argument, which
I accept):

- per-eye width need not be even for combined width to be even — doubling any
  integer is already even;
- height has no left/right split, yet it is forced even too;
- if a half-resolution resource needs alignment, rounding **down past a vendor
  minimum** is not a safe way to get it;
- UltraPerformance's fixed **odd** 1165 contradicts any universal
  "per-eye width must be even" rule.

**Narrowed claims.** RS-off lands exactly on the vendor optimal at Quality and
Performance only — not at Balanced (2055 x 2092 against an optimal 2027 x 2064)
and not at UltraPerformance. Even-forcing changes the result at q1–q5 for this
display; q0 and q6 are identical in both arithmetic paths.

**Pre-registered experiment**, cheap and falsifiable, justified before the full
audit: feed the stable envelope vendor-valid per-eye extents — at least
1747 x 1779 at Performance — change nothing else, and log the tagged extent and
resource descriptions. Visual improvement prioritises this path; no improvement
falsifies it cleanly.

### 2A.6 The boundary derivation cannot cross

It covers **CS arithmetic** exhaustively and exactly, and stops at the **engine
and D3D boundary**: where pixels actually live in real resources is not
derivable and must be measured or asserted. §4.2 keeps that boundary explicit.

## 3. Architecture

```text
one immutable input snapshot
        v
DeriveGeometry()        pure: RS-off / RS-on / envelope -> typed logical plan
        v
BindResources()         pure: logical plan + real D3D descriptions -> typed regions
        v
raster, compute, vendor and submit consumers
```

The pure planner makes geometry production deterministic; the types protect its
consumers; runtime assertions cover the boundary calculation cannot know.

### 3.1 `DeriveGeometry()` — the single source of truth

```cpp
enum class GeometryFlow  { RenderScaleOff, RenderScaleOn, HotEnvelope };
enum class GeometryPhase { Stable, PhysicalRecovery };
enum class GeometryAction{ Use, RelatchRequired, Invalid };

struct GeometryInputs {
    GeometryFlow flow{}; GeometryPhase phase{};
    PerEyeOutputExtent display{};
    QualityMode bootQuality{}; QualityMode activeQuality{};
    VendorInputConstraints vendor{};
};

struct GeometryDecision { LogicalGeometryPlan plan{}; GeometryAction action{}; GeometryReason reason{}; };

GeometryDecision DeriveGeometry(const GeometryInputs& inputs);
```

**It must not read globals, atomics, settings, `perfMode`, frame state or D3D
resources.** All stateful capture happens before the call. Production publishes
**one immutable decision per frame/transition generation**; consumers must never
recompute dimensions from quality or `screenSize`. Log a stable hash of
canonical inputs and outputs so one frame can be proven to have used one plan.

This matters because §2 points at timing and lifecycle: a pure planner cannot
fix a lifecycle bug, but it removes mixed-snapshot geometry as a confound.

### 3.2 `BindResources()` — the second pure stage

```cpp
BoundGeometry BindResources(const LogicalGeometryPlan& logical,
                            const ResourceLayoutInputs& resources);
```

`ResourceLayoutInputs` carries texture extent, mip, array slice/view, active
subregion, combined-vs-per-eye layout and eye ordering. It produces typed
source/destination regions and validates containment, without touching a live
D3D context.

**Eye origin lives here, never in `DeriveGeometry()`.** RS-off proves the
shipped path used an origin numerically equal to `render.x / 2` *for that
layout*; it does not establish a universal rule. The 1746-vs-2328 question is
resolved by the bound layout, not chosen.

### 3.3 Types

Tagged extents, offsets and regions, with resource tags for concrete spaces:

```cpp
template <class Space, class Scalar> struct Extent2D { Scalar width{}, height{}; };
template <class Space, class Scalar> struct Offset2D { Scalar x{}, y{}; };
template <class Space, class Scalar> struct Region2D { Offset2D<Space,Scalar> origin{}; Extent2D<Space,Scalar> extent{}; };
```

**Narrowed guarantee:** these stop a whole `RenderExtent` reaching an
`AllocationExtent` consumer and force every field access to be rewritten, so the
inventory is exhaustive. They do **not** stop `render.width` reaching an
allocation consumer, since both are raw `float`. Component hardening
(`Dimension<RenderSpace, XAxis, float>`, named operations) applies in 1b to the
flows the inventory shows to be high-risk. Extents must record **combined stereo
vs per eye**. Conversions are explicit and named (`ToD3DExtent()`) so every loss
of type information is greppable.

## 4. Verification

### 4.1 The state space is a matrix, not a list

The envelope has **two** quality inputs — boot sets allocation, active sets
render. The full space is 7 RS-off + 7 RS-on + **49 envelope boot x active** +
recovery-phase variants.

Metamorphic invariants:

```text
RSOn(q).render == RSOn(q).allocation
Envelope(boot=q, active=q) == RSOn(q)
Envelope(boot, active).allocation == RSOn(boot).allocation
Envelope(boot, active).render     == RSOn(active).render
Envelope(...).output == display
stable envelope requires render <= allocation
render > allocation  =>  RelatchRequired, never silently clamped
PhysicalRecovery     =>  render == allocation
```

The last two matter for upward quality changes: a small boot allocation cannot
contain a larger active render, and that is a **state decision**, not a
dimension to repair implicitly.

Property invariants: determinism; positive dimensions; output equals the
captured display contract; render monotonic down the ladder; combined width
exactly equals the sum of its eye regions; derived vendor inputs satisfy the
registered range; no bound region exceeds its resource.

**Sweep many HMD dimensions, even and odd**, not only the Pimax 3494 x 3558
fixture — that is what makes this agnostic to headset, refresh mode and Image
Quality changes. Keep 3494 x 3558 as a golden case.

### 4.2 Characterization checks vs semantic gates

During behaviour-null extraction the vendor-range check must **record** the
current Performance and UltraPerformance violations, not fail the build. It
becomes a required gate only after phase 2 deliberately reconciles the
constraints. Same rule for any newly found resource-layout violation: preserve
and expose current behaviour first, change it in the semantic phase.

### 4.3 Constraint reconciliation, phase 2

The planner should compute the requested size, then intersect independently
named constraints:

```text
requested quality size
    n vendor accepted range / fixed extent
    n engine or resource alignment requirements
    n physical allocation containment
```

Empty intersection returns `RelatchRequired` or `Invalid`. **Never round
silently outside the vendor range.**

## 5. Phases

| phase | content | exit | headset |
|---|---|---|---|
| **0** | evidence ledger; r4 label corrections | counts reproduced at the start commit; labels fixed | no |
| **0A** | extract current behaviour into `DeriveGeometry()`; production calls it; golden + 7x7 + recovery tests | behaviour-null: canonical values identical, characterization checks record existing violations | no |
| **1a** | retype the published logical plan; exhaustive consumer inventory | CI green; static_asserts; oracle identical; **site table delivered** | no |
| **1b** | implement `BindResources()`; resource-tag and component-harden high-risk flows | table classified; oracle identical | no |
| **2** | reconcile vendor/alignment constraints; correct semantically wrong consumers. **Performance below-minimum first** | every change carries contract, invariant and falsifier | no |
| **3G** | geometry exit: region containment, dispatch coverage, controlled allocation test, probe, stereo scale and depth | all five | yes |
| **3P** | pair-atomicity non-regression vs branch baseline | no worse than baseline | yes |
| **4** | measure the prize: P95 GPU and delivery vs the boot-latched table | — | yes |

**Phase 0 status:** counts re-confirmed at `6fd49535d` on 2026-08-20; log
archived and hashed; labels corrected in this revision. **Complete.**

**Phase 0A status:** five commits ending `0bda6f9af`; the planner is the only
producer of both extents; compile-time tests cover both shipped flows, the 7x7
envelope matrix, physical recovery and seven display sizes. **Complete.**

**Phase 1a status:** `2eaf90ddc`. `engineRenderSize` and `engineAllocationSize`
retyped to mutually non-convertible `VRGeometry::RenderExtent` and
`VRGeometry::AllocationExtent`; 116 sites across 32 consumers enumerated by the
compiler and classified. Deliverable: `PHASE_1A_SITE_TABLE.md`. Scope note: the
census is exhaustive over `RuntimeResolutionPlan` only - the same two geometries
are also carried per eye on five other structures (~190 further sites), which
1b must either retype or explicitly decline. **Complete, CI pending.**

Extracting the calculator before the type migration is deliberate: it gives 124
retyped sites one authoritative producer instead of rewriting them around
calculations that are still duplicated.

### 3G's unresolved item

Boot quality currently selects allocation, so varying allocation varies boot
state. Either add a **diagnostic allocation-cap override** that changes physical
allocation while quality, vendor preset, options and transition policy stay
identical — rejecting any run whose logs show another registered variable moved
— or call it a **boot-allocation A/B** and weaken the conclusion to "allocation
or boot-latched state". This mechanism does not exist yet and must be designed
in phase 3.

## 6. Three claims, kept separate

1. **Compiler inventory** — every typed plan-geometry consumer is enumerated.
2. **Baseline preservation** — oracle-covered calculations unchanged in the two
   shipped modes. They separate the three concepts pairwise (RS-off separates
   allocation from render, RS-on separates allocation from output), but only for
   quantities the oracle represents; they do not prove the envelope formula or
   cover resource layout.
3. **Envelope validation** — runtime invariants and a controlled test decide
   whether the three-way-divergent mode is correct.

Keeping them separate stops any one instrument being asked to prove more than it
can.

## 7. Risks

| risk | mitigation |
|---|---|
| The oracle drifts into a second implementation | 0A makes production call the same pure planner; hash inputs and outputs per frame |
| Phase 0A/1a not behaviour-null | canonical-value comparison, never `memcmp` of wrappers; static_asserts; characterization not gating |
| MSVC stops at 100 errors per TU | grep enumerates, compiler verifies |
| Phase 2 judgement wrong | rubric, invariant and falsifier per site; oracle as regression net |
| **Defect is not geometry at all** | §2 makes this the more likely outcome; phase 2 finding nothing is a result |
| Scope creep into a rewrite | 0A and 1a are behaviour-null; hardening confined to high-risk flows |
