# Codex final review: Revision 4 and the pure three-flow calculator

**Reviewed:** 2026-08-20  
**Document:** `PLAN_GEOMETRY_TYPE_SPLIT.md`, Revision 4  
**Verdict:** **The new idea is the right architecture. Promote it from a test oracle into the single production geometry planner, with the corrections below.**

Revision 4 is the strongest version of the plan. Withdrawing the allocation-discriminator claim was correct, the evidence ledger is now reproducible, and the derived table has already exposed a more concrete discrepancy than the earlier tracing hypotheses.

The pure-calculation idea should become the centre of the implementation:

```text
one immutable input snapshot
        ↓
pure logical geometry planner (RS-off / RS-on / envelope)
        ↓
typed logical plan
        ↓
resource-layout binding using actual D3D descriptions
        ↓
validated regions consumed by raster, compute, vendor and submit paths
```

This complements rather than replaces the type split. The pure planner makes geometry production deterministic; the types protect its consumers; runtime resource assertions cover the engine/D3D boundary that calculation alone cannot know.

## What I independently confirmed

- The two sizing expressions quoted in §2A.1 match the inspected source.
- `ScaleVRRenderDimension()` applies `floor` and then clears the low bit for **both width and height**.
- All integer dimensions in the derived table reproduce those expressions.
- The archived log exists at the stated relative path and its SHA-256 is exactly `0380f23cbf4bfd50608e2730a9b956599b7d0c8fe4552bc0aa115ff414bff495`.
- The Streamline measurements in `CSX_HOT_ENVELOPE_POC.md` report a minimum of `1747×1779` for ranged modes and a fixed `1165×1186` input for Ultra Performance.
- Streamline maps qualities 1–3 to `eMaxQuality`, quality 4 to `eBalanced`, quality 5 to `eMaxPerformance`, and quality 6 to `eUltraPerformance`.

## Required correction 1: make the calculator production code, not a parallel model

The plan currently says the oracle will be a dependency-free policy header, but it does not explicitly require every production flow to call that same policy. If the tests reproduce expressions while runtime code keeps separate calculations in `ConfigureUpscaling`, relatch planning, request profiles, and resolution-plan refresh, the oracle can become a second implementation that drifts while remaining green.

Define one immutable input and one pure entry point used by production and tests:

```cpp
enum class GeometryFlow
{
    RenderScaleOff,
    RenderScaleOn,
    HotEnvelope
};

enum class GeometryPhase
{
    Stable,
    PhysicalRecovery
};

struct GeometryInputs
{
    GeometryFlow flow{};
    GeometryPhase phase{};
    PerEyeOutputExtent display{};
    QualityMode bootQuality{};
    QualityMode activeQuality{};
    VendorInputConstraints vendor{};
};

struct GeometryDecision
{
    LogicalGeometryPlan plan{};
    GeometryAction action{}; // Use, RelatchRequired, or Invalid
    GeometryReason reason{};
};

GeometryDecision DeriveGeometry(const GeometryInputs& inputs);
```

All stateful work happens before this call. Display dimensions, boot and active quality, flow, recovery phase, and vendor constraints are captured once. `DeriveGeometry()` must not read globals, atomics, settings, `perfMode`, frame state, or D3D resources.

Production then publishes one immutable decision for the relevant frame/transition generation. Consumers must not independently recompute dimensions from quality or `screenSize`. Log a stable hash of the canonical inputs and outputs so one frame can be proven to have used one plan.

This is particularly important because §2 now points toward timing/lifecycle state. A pure planner cannot remove vendor lifecycle bugs, but it can eliminate mixed-snapshot geometry as a confound.

## Required correction 2: the envelope is a boot-quality × active-quality matrix

The seven-row derived table is correct for the two numerical sizing rules, but it is not yet “all presets through three flows.” An envelope has two quality inputs:

- boot quality determines physical allocation;
- active quality determines the render region.

The exhaustive state space is therefore:

- 7 RS-off cases;
- 7 RS-on cases;
- 49 envelope boot×active cases;
- recovery-phase variants where render temporarily collapses to allocation.

Add matrix/metamorphic tests:

```text
RSOn(q).render == RSOn(q).allocation
Envelope(boot=q, active=q) == RSOn(q) for logical geometry
Envelope(boot, active).allocation == RSOn(boot).allocation
Envelope(boot, active).render == RSOn(active).render
Envelope(...).output == display
stable envelope requires render <= allocation
render > allocation returns RelatchRequired; it is never silently clamped
physical recovery explicitly returns render == allocation
```

The last two rules are essential for upward quality changes. For example, a low-resolution boot allocation cannot contain a higher-resolution active render. That is a state decision, not a dimension to repair implicitly.

The current Pimax `3494×3558` fixture should remain as a golden case, but the policy tests must also sweep many even and odd HMD dimensions. That is what makes the solution genuinely agnostic to headset resolution, refresh mode, Pimax upscaling, and Image Quality changes.

Useful generated/property invariants are:

- determinism for identical inputs;
- positive nonzero dimensions;
- output equals the captured display contract;
- render dimensions are monotonic down the quality ladder;
- stable render fits allocation;
- combined stereo width exactly equals the sum of its eye regions;
- all derived vendor inputs satisfy the registered vendor range;
- no source or destination region exceeds its concrete resource after binding.

During the behavior-null extraction, distinguish **characterization checks** from **semantic gates**. The vendor-range check must initially record the current Performance and Ultra Performance violations rather than make Phase 0A impossible to pass. It becomes a required invariant only after Phase 2 deliberately reconciles the constraints. Apply the same rule to any newly discovered resource-layout violation: preserve and expose current behavior first, then change it in the semantic phase.

## Required correction 3: elevate the Performance vendor-minimum violation

The new arithmetic exposes a stronger fact than Revision 4 currently states.

At Performance:

- working RS-off produces `1747×1779` per eye;
- Streamline reports `1747×1779` as the accepted minimum;
- current RS-on/envelope sizing produces `1746×1778` per eye.

That is not merely “one pixel below optimal.” It is **one pixel below the reported accepted minimum in both axes**. This does not prove it causes the visual failure—drivers may accept undocumented values—but it is an explicit vendor-contract violation and should be the highest-priority Phase 2 candidate.

Ultra Performance similarly produces `1164×1186` against a reported fixed `1165×1186`. The fact that an RS-off path may appear to work at 1164 shows tolerance or another width interpretation, not that the advertised contract can be ignored. It is especially relevant because every archived `EMPTY` probe window occurred at Ultra Performance.

The current even-forcing rule is questionable on its face:

- per-eye width need not be even to make combined stereo width even; doubling any integer already does that;
- height has no left/right split, yet it is forced even too;
- if a downstream half-resolution resource requires alignment, rounding **down below a vendor minimum** is not a safe resolution;
- Ultra Performance's fixed odd width demonstrates that a universal “per-eye width must be even” rule conflicts with the vendor-reported contract.

Phase 2 should classify `ScaleVRRenderDimension()` against explicit constraints rather than just compare it with RS-off. The pure policy should first calculate the requested size, then reconcile independently named constraints:

```text
requested quality size
    ∩ vendor accepted range/fixed extent
    ∩ engine/resource alignment requirements
    ∩ physical allocation containment
```

If the intersection is empty, return `RelatchRequired` or `Invalid`; never round silently outside the vendor range.

A minimal pre-registered experiment is justified before the full semantic audit: feed the stable envelope vendor-valid per-eye extents—at least `1747×1779` for Performance—and change nothing else. Log the actual tagged extent and resource descriptions. A visual improvement would prioritize this path; no improvement would falsify it cleanly.

## Required correction 4: fix two float32 labels and narrow two prose claims

The integer table is correct, but two displayed float32 values are not the values returned by the C++ `constexpr float` expressions:

| preset | Revision 4 | IEEE-754 result of source expression |
|---|---:|---:|
| UltraQuality, `1.0f / 1.3f` | `0.769230797` | **`0.769230783`** |
| Balanced, `1.0f / 1.7f` | `0.588235319` | **`0.588235259`** |

The products still truncate to the listed integer dimensions, so no downstream row changes. Correct the labels because the section explicitly claims exact float32 reproduction.

Also narrow these statements:

- “RS-off reproduces DLSS's optimal sizes exactly” is true for the displayed Quality and Performance cases, not Ultra Performance.
- Even-forcing does not change every preset. It changes q1–q5 for this display fixture; q0 and q6 produce the same per-eye integer dimensions in both arithmetic paths.

## Required correction 5: keep logical derivation and resource binding as two pure stages

The plan correctly says source-eye origin cannot be selected from render or allocation alone. Preserve that boundary in the architecture.

Use a second pure function whose inputs include the actual resource/view descriptions and an explicit stereo-layout declaration:

```cpp
BoundGeometry BindResources(
    const LogicalGeometryPlan& logical,
    const ResourceLayoutInputs& resources);
```

`ResourceLayoutInputs` should identify texture extent, mip, array slice/view, active subregion, combined-versus-per-eye layout, and eye ordering. The function produces typed source/destination regions and validates containment. It does not inspect a live D3D context; the stateful layer captures descriptions before calling it.

This yields a clean proof boundary:

- `DeriveGeometry()` answers how large allocation, render, and output should be.
- `BindResources()` answers where those pixels live in the actual resources.
- runtime tracing checks that the engine and vendor consumed the published result.

Do not put eye origin into the first calculator unless the resource layout is also an explicit input.

## Recommended phase ordering

The pure planner should be established before the large type migration:

1. **Phase 0:** finish the evidence ledger and correct the Revision 4 numeric labels.
2. **Phase 0A:** extract current behavior into `DeriveGeometry()` with golden and 7×7 matrix tests. Production calls it. This phase is behavior-null.
3. **Phase 1a:** retype the published logical plan and produce the exhaustive consumer inventory.
4. **Phase 1b:** implement `BindResources()`, resource-tag high-risk regions, and harden component flows.
5. **Phase 2:** reconcile vendor/alignment constraints and correct semantically wrong consumers. Test Performance's below-minimum input first.
6. **Phase 3G/3P:** retain the already-correct split between geometry exit and pair-atomicity non-regression.

Extracting the calculator first gives the type migration one authoritative producer and prevents 124+ sites from being rewritten around calculations that are still duplicated.

## Final assessment

Rik's idea materially improves the project. It converts the work from “rename ambiguous values and inspect the fallout” into a small deterministic core with typed and observable boundaries. That is more elegant, easier to test, and much more resistant to changes in HMD resolution or Pimax configuration.

Revision 4 is ready in principle, but I would not freeze it until it:

1. makes the pure planner the production source of truth;
2. covers the full boot×active envelope matrix and recovery phase;
3. treats the Performance below-minimum extent as a priority contract violation;
4. corrects the two float labels and overbroad prose;
5. preserves a separate resource-binding stage.

After those edits, I consider the plan implementation-ready.
