# Codex final review: geometry type-split plan, revision 2

**Reviewed:** 2026-08-20  
**Document:** `PLAN_GEOMETRY_TYPE_SPLIT.md`, Revision 2  
**Verdict:** **Foundation approved; make four targeted corrections before freezing the plan.**

Revision 2 is substantially stronger than Revision 1. It correctly brings output geometry into the mechanical pass, separates resource coordinates from engine allocation, makes the evidence claim appropriately conditional, adds reproducible provenance, and keeps geometry correctness conceptually separate from eye-pair atomicity. Rik's baseline oracle is a useful addition because it protects the two working configurations during a large refactor.

I would proceed after correcting the four major points below. None requires abandoning or redesigning the project.

## What I independently confirmed

The following claims check out against the current local CSX trace source and Git history:

- Commit `fbae22c367e9dcba71469f507a758eeab75b8af2` is the encode-origin change and `6fd49535d94ea7e7d94cac7007512f77617b9ec5` is its revert.
- At commit `6fd49535d`, line-based `git grep` reproduces the plan's counts exactly: 107 `engineRenderSize`, 17 `engineAllocationSize`, 103 `finalOutputSize`, and 10 `trueHMDDisplaySize` sites.
- `BUILD_CONTROLLER_TESTS` and the four named policy tests exist in the inspected tree, so a dependency-free table test follows the repository's established pattern.
- The current `RefreshRuntimeResolutionPlan()` initializes `trueHMDDisplaySize` and `finalOutputSize` from the same display size and, in VR Render Scale Mode, explicitly assigns `finalOutputSize = trueHMDDisplaySize`. There is no current assignment that makes them differ. Classifying both as `OutputExtent` is therefore supported by the inspected implementation. A separate name is only needed if a future contract deliberately separates recommended HMD size from actual presentation output.
- The VR encode C++ and HLSL genuinely combine local dispatch coordinates with source-resource and destination-resource offsets. The fourth-space treatment is necessary.
- The v4 compute dispatch remains a valid audit target, but its runtime shader identity still needs to be mapped before its source-level contract is asserted.

## Major correction 1: the illustrated types are still unwrap-friendly

The plan calls these types “non-unwrappable”:

```cpp
template <class Space, class Scalar>
struct Extent2D { Scalar width{}, height{}; };
```

They prevent assigning an entire `RenderExtent` to an `AllocationExtent`, but `render.width` and `allocation.width` are both immediately raw `float`. A component can therefore still be passed to the wrong consumer, compared, halved, or stored under another semantic name without a compiler error. This is the exact level at which many current call sites operate.

Choose one of two honest contracts:

1. **Inventory types:** keep public scalars for a low-risk mechanical phase, but describe the guarantee as “whole-extent tagging plus exhaustive field inventory,” not “geometry conflation is un-typeable.” Then harden high-risk component flows in Phase 1b.
2. **Enforcement types:** make components opaque or tagged too. Width access returns something like `Dimension<RenderSpace, XAxis, float>`, not `float`. Named policy operations produce another typed value, and raw extraction exists only at D3D/vendor/logging boundaries.

For example, the intended API can be expressed as:

```cpp
RenderExtent render = ...;
AllocationExtent allocation = ...;
DynamicResolutionRatio ratio = ComputeRatio(render, allocation);
PerEyeRenderExtent eyeInput = SplitStereo(render);
MainSourceRegion eyeRegion = ResolveEyeRegion(sourceLayout, Eye::Right);

context->RSSetViewports(1, ToD3DViewport(renderRegion));
```

The key property is that `SplitStereo(render)` returns a **per-eye render-space value**, while the right-eye source origin comes from the **actual source layout**, not from an untyped width calculation.

This correction is required if “un-typeable” remains a project claim. Otherwise the current type model is still useful, but its guarantee must be narrowed.

## Major correction 2: narrow the baseline oracle's proof claim

The oracle correctly establishes a regression net for the two working modes. It also ensures that each pair of logical extents differs in at least one baseline row. That means it can catch a **direct interchange of allocation, render, and output in every covered oracle calculation**.

It does not catch every substitution error in the codebase:

- a consumer not represented in the policy test can still be wrong;
- a conditional formula can agree in both baseline states and diverge under the envelope;
- `min`, clamping, rounding, half-width, resource-view, or stereo-layout logic can mask a wrong input;
- infinitely many formulas reproduce two baseline points but disagree at a third;
- a source-resource origin cannot be inferred from logical extents without knowing how that concrete resource is laid out.

The plan acknowledges part of this later, but the sentence “Together they catch every pairwise mis-routing” still overstates the result. Replace it with:

> Together the baselines catch every direct pairwise substitution among allocation, render, and output for the quantities represented in the oracle. They preserve known-good behavior but do not prove the envelope formula or cover resource-layout decisions.

The eye-origin conclusion needs the same narrowing. The working RS-off baseline proves that the shipped RS-off path used an origin numerically equal to `render.x / 2`. It does **not** prove that `render.x / 2` is the universal source-origin rule. Under the envelope, the bound source may contain packed active pixels, allocation-separated eyes, a per-eye intermediate, or another layout.

The general rule should instead be:

> Obtain the eye region from a typed description of the bound source resource and its active stereo layout. Verify that RS-off resolves to 2329 and RS-on resolves to 2328; do not pre-select 1746 or 2328 for the envelope until the source layout contract identifies it.

This preserves the useful 1746-versus-2328 contradiction as a Phase 2 question instead of accidentally encoding one answer into the oracle.

## Major correction 3: specify how the A/B varies only allocation

Phase 3 says to fix quality, vendor options, render extent, and other state while varying **only** boot allocation between 4656 and 5936. The current plan does not state a mechanism capable of doing that.

If allocation is selected by boot quality, changing boot allocation also changes boot-latched quality/vendor initialization and potentially feature generation or history. Logging those variables proves the covariance; it does not remove it.

Add one of these designs:

- **Preferred:** a diagnostic allocation-cap override that changes the physical envelope allocation while leaving active quality, vendor preset, options, and transition policy identical. Reject the run unless logs confirm all registered state except resource dimensions is identical.
- **Fallback:** explicitly call the test a boot-allocation A/B, not an allocation-only A/B, and retain the conclusion “allocation or boot-latched state.” An A-B-A order controls drift and warm-up, but it cannot remove a structural boot-quality confound.

Pre-register the equality gate. A sample is valid only if requested and active quality, vendor options, feature generation policy, foveation, menu state, transition/recovery state, output, render extent, scene/camera, and instrumentation configuration match. Resource identity may differ because resource recreation is the manipulated consequence; its descriptions must be recorded.

Without an isolation mechanism, Phase 3 can verify reproducibility but cannot promote allocation from discriminator to cause.

## Major correction 4: separate the menu release gate from geometry completion

Phase 3 currently says “All six required,” while criterion 6 demands that opening the menu not split the eyes. Section 7 correctly says that the observed menu split is an independent pair-atomicity defect.

Those statements conflict if the pair-atomicity defect already exists before the geometry refactor. A correct geometry branch could satisfy criteria 1–5 and still fail criterion 6 for the known independent reason. That would prevent the geometry plan from reaching a truthful conclusion.

Split verification into:

- **Phase 3G — geometry exit:** resource containment, dispatch coverage, controlled allocation test, temporal probe, and stereo scale/depth.
- **Phase 3P — pair-atomicity non-regression:** compare menu behavior with the branch baseline and confirm the geometry work did not worsen it.
- **Integration/release gate:** require zero menu eye split after the separate pair-atomic submission fix is integrated.

If this project is intentionally expanded to fix pair atomicity too, say so and add that implementation to scope. Otherwise “zero split” belongs to the combined release gate, not the geometry refactor's causal exit test.

## Minor precision corrections

### Compare canonical values, not raw C++ object bytes

“Byte-identical before and after” is safe only if it refers to a canonical serialized scalar snapshot or fixed policy output buffer. Do not `memcmp` wrapper objects: padding, object representation, or a deliberate type-layout change can differ without changing behavior. Prefer exact comparison of canonical integer dimensions and stable serialized fields. Exclude timestamps, pointers, resource identities, and nondeterministic log metadata.

### Test exact arithmetic, not the rounded table labels

The table's displayed values are rounded:

- `4658 / 6988 = 0.666571...`
- `6988 / 4658 = 1.500214...`
- `6988 / 4656 = 1.500859...`
- `6988 / 3492 = 2.001145...`

Use the exact integer inputs and the production calculation order in tests. Treat `0.667`, `1.500`, and similar values as documentation only, not golden floating-point constants.

### Clarify the resource-creation rubric

“Allocation of that concrete resource” is correct in a generic sense but can be mistaken for `engineAllocationSize`. Use this wording:

> Resource creation and view selection use `TextureExtent<ResourceTag>` derived from that resource's ownership contract and verified against its actual D3D resource/view description. It may be render-sized, engine-allocation-sized, output-sized, or per-eye-sized.

### Finish provenance before Phase 1

The commit and counts are now reproducible. The outstanding run ledger should archive or link the exact log, record its hash and test manifest, and identify the source of every Section 2 interval. A filename and wall-clock range alone are fragile once later runs create another `CommunityShaders.log`.

## Final recommendation

After these four major corrections, I consider the plan ready to execute.

The project should retain three distinct claims:

1. **Compiler inventory:** every typed plan-geometry consumer is enumerated.
2. **Baseline preservation:** all oracle-covered calculations remain correct in the two shipped modes.
3. **Envelope validation:** runtime resource/layout invariants and a properly controlled test determine whether the three-way-divergent mode is correct.

Keeping those claims separate prevents the compiler, baseline oracle, or temporal probe from being asked to prove more than it can. That makes the plan both rigorous and capable of producing a trustworthy negative result if ambiguous geometry is not the root cause.
