# Codex review: `PLAN_GEOMETRY_TYPE_SPLIT.md`

**Reviewed:** 2026-08-20  
**Plan reviewed:** `PLAN_GEOMETRY_TYPE_SPLIT.md`  
**Code inspected:** the current local `skyrim-community-shaders-rs-trace` source, including `RuntimeResolutionPlan`, the VR encode paths, `EncodeTexturesCS.hlsl`, and the StereoTrace v4 findings.

## Verdict

**Approve the direction, but revise the type boundary and verification plan before Phase 1.**

Replacing isolated geometry patches with a compiler-assisted, exhaustive inventory is the right move. The present proposal, however, would not yet make geometry conflation “un-typeable.” It types only two of the relevant extents, exposes their raw `float2` immediately, and leaves output and resource-coordinate geometry as untyped escape paths. That would still be valuable as an inventory exercise, but it would be weaker than the plan claims and could let the same class of defect survive Phase 2.

The strongest version of this plan is:

1. Type all three logical plan extents: render, allocation, and output.
2. Treat a concrete texture's coordinate space as a separate concept, not as an alias for engine allocation.
3. Type or explicitly classify regions and offsets as well as extents.
4. Audit the v4 compute writer explicitly.
5. Validate with a controlled allocation A/B and geometry invariants, not temporal activity alone.
6. Keep the menu pair-atomic submission defect as an independent workstream and regression test.

With those corrections, this becomes a strong foundation rather than another hypothesis-driven patch.

## Direct answers to Claude's questions

### 1. Is the Phase 1 boundary right?

**Not quite. Include `finalOutputSize` in the mechanical type pass.**

The plan's own thesis is that CSX has three geometries. Typing only two means the compiler cannot prove that a render or allocation consumer did not accidentally receive the still-raw output `float2`. It also leaves no protection against the inverse error: using render geometry where output geometry is required.

There is another related field in the current plan structure:

```cpp
float2 trueHMDDisplaySize;
float2 engineAllocationSize;
float2 engineRenderSize;
float2 finalOutputSize;
```

`trueHMDDisplaySize` and `finalOutputSize` may be equal in the tested configuration, but their semantic relationship should be stated. If both are output-space extents, both should use `OutputExtent`. If they can differ, the plan actually contains another logical extent that must be named.

If the extra mechanical churn is considered too risky for one commit, split it into Phase 1a and 1b, but do not defer it until after semantic review:

- **Phase 1a:** introduce the types and retype render/allocation fields.
- **Phase 1b:** retype `finalOutputSize` and classify `trueHMDDisplaySize` before any semantic correction.

Only after both compile should the plan claim complete compiler enumeration of the three logical plan geometries.

The site counts also need to be regenerated from the exact implementation branch immediately before Phase 1. A simple identifier recount on the local trace tree did not reproduce the document's 107/17/103 values exactly. This may just reflect branch drift or a different counting rule, but the site table should record its command, commit, and counting definition so its completeness is reproducible.

### 2. Is there a fourth concept?

**Yes. It must not be folded into `AllocationExtent`.**

The fourth concept is the coordinate space of the **specific D3D11 resource being accessed**. Engine main-target allocation is only one possible resource space. A source may instead be:

- the combined stereo main target;
- a per-eye intermediate;
- a texture array slice;
- a presentation/output texture;
- a foveated intermediate;
- a vendor input or output resource.

The current encode path demonstrates this directly. In `Upscaling.cpp`, it derives a local `renderSize` from `inputStereoLayout`, sets `trueSamplingDim` from it, and sets:

```cpp
sourceOffset = sourceEyeRegion.minX + inputMinX;
```

The HLSL then performs:

```hlsl
uint2 localPos = dispatchID.xy;
uint2 sourcePos = localPos + uint2(SourceOffset + 0.5);
```

Those values do not all inhabit one space:

- `DispatchDim` is a local work-region extent.
- `SourceOffset` is an absolute texel offset in the bound source resource.
- `sourcePos` is a source-resource texel coordinate.
- `OutputOffset` is an offset in the bound output resource.
- `TrueSamplingDim` is being used as a source-coordinate bounds/normalization domain.

Calling all of this “allocation” would hide exactly the composition error that the type split is intended to expose. The concrete resource and view descriptions (`D3D11_TEXTURE2D_DESC` plus the selected mip/slice/view) are the authoritative bounds for source and destination regions, while `engineAllocationSize` describes a particular engine allocation policy.

The design should distinguish at least:

- logical extents: `RenderExtent`, `AllocationExtent`, `OutputExtent`;
- concrete resource extent: `TextureExtent<ResourceTag>`;
- coordinates: `TexelOffset<ResourceTag>` and `TextureRegion<ResourceTag>`;
- local dispatch extent: `DispatchExtent` or a typed region extent;
- normalized UVs: a separate normalized type or clearly named values, never an extent type.

It should also state whether an extent is **combined stereo** or **per eye**. “Render extent” alone does not encode that distinction.

### 3. Should the v4 clear and compute dispatch be explicitly in Phase 2?

**Yes, especially the compute dispatch.**

The v4 census found 2,923 kMAIN scene draws with viewport `(0,0,3492,1778)`. The only non-draw kMAIN writers per frame were:

- one full RTV clear before the final draw;
- one compute dispatch before the final draw, with groups `437 x 223 x 1`.

The group counts equal `ceil(3492 / 8)` by `ceil(1778 / 8)`. The inspected `EncodeTexturesCS.hlsl` declares `[numthreads(8,8,1)]`, so this is a strong numerical clue for a render-domain work extent. The v4 runtime shader identity has not yet been tied to that source shader, however. Phase 2 must confirm the shader identity, bound resources, thread-group declaration, and index mapping rather than infer solely from the numbers.

The clear does not choose a raster/dispatch extent, but its target view and underlying resource creation are allocation-space decisions. Therefore:

- classify and audit the compute dispatch as a geometry consumer;
- classify the clear's resource/view ownership, not merely the clear call;
- retain the v4 result that no second compute, copy, resolve, or update writes kMAIN in that observed path.

### 4. Can Phase 2 be made less judgement-dependent?

**Yes. Classify by boundary contract and require a falsifiable invariant per site.**

Do not decide from the old variable name. For every site, record:

| field | meaning |
|---|---|
| producer | where the value originates |
| consumer | API, shader parameter, resource, or calculation receiving it |
| semantic space | render, allocation, output, concrete source, concrete destination, local dispatch, or normalized UV |
| stereo shape | combined stereo, per eye, or texture-array slice |
| resource identity | the actual bound/created resource where applicable |
| invariant | the condition that must hold if the classification is correct |
| runtime falsifier | a log/assert/probe observation that would disprove it |

Use this decision rubric:

| consumer purpose | expected geometry |
|---|---|
| engine/main resource creation and D3D `Width`/`Height` | allocation of that concrete resource |
| viewport, scissor, raster work region | render extent/region |
| compute dispatch coverage | local work/render region, plus explicit source and destination resource regions |
| vendor input sampling dimensions and dynamic ratio | render extent unless the vendor API contract explicitly says otherwise |
| final compositor or HMD-facing output | output extent |
| copy source box | region in the concrete source resource |
| copy destination offset/box | region in the concrete destination resource |
| stereo eye origin | offset in the parent combined resource, never inferred solely from an unrelated extent |
| shader UV math | normalized coordinate space, with an explicitly named normalization domain |

Useful debug-only invariants include:

```cpp
assert(renderExtent.width > 0 && renderExtent.height > 0);
assert(renderExtent.width <= engineAllocation.width);
assert(renderExtent.height <= engineAllocation.height);
assert(sourceRegion.FitsWithin(sourceTextureExtent));
assert(destinationRegion.FitsWithin(destinationTextureExtent));
assert(dispatch.Covers(localWorkExtent, threadGroupExtent));
```

For copies and compute passes, log the actual resource description and the typed region in the same record. This makes a mismatch mechanically falsifiable rather than dependent on a reviewer's interpretation.

### 5. Does “allocation is the discriminator” survive?

**It survives as the leading discriminator, but not yet as causal proof of this exact defect.**

The reported observation is important: the same render extent and output behaved differently at allocations 4656 and 5936. That strongly implicates allocation-dependent state or resource geometry. It also weakens the previous encode-origin theory.

However, the session changed presets manually while the governor was active. Allocation may have covaried with:

- boot/requested/runtime quality state;
- vendor preset/options;
- DLSS feature generation or recreation;
- history reset and temporal cache contents;
- source resource identity or stereo layout;
- foveation state;
- transition/recovery generation;
- a menu or fail-open state.

The probe also measures temporal activity. A clean changing span is useful, but an unchanged span can be influenced by content, camera motion, history, or the sampled row. It does not by itself prove that a coordinate is unreachable.

Therefore the conclusion should be worded as:

> Allocation, or state that covaries with allocation, is the strongest current discriminator. The type audit is justified because allocation-dependent resource geometry is now the leading defect family; a controlled A/B is still required for causality.

Pre-register a counterbalanced A/B:

1. Disable the governor.
2. Fix scene, camera, output resolution, requested quality, render extent, menu state, foveation, and vendor options.
3. Vary only boot allocation, using 4656 and 5936.
4. Record exact build/manifest, settings, requested and active quality, transition epoch, vendor feature generation, history-reset state, and source/destination texture descriptions.
5. Run A-B-A if practical to distinguish allocation from warm-up/order effects.

The plan's evidence section should link the exact run report/log and record its build commit. At present, the numeric evidence in Section 2 is not independently traceable from the plan itself.

## Required strengthening of the type design

The proposed wrappers are too easy to unwrap:

```cpp
struct RenderExtent { float2 value; float Width() const; };
```

If `.value`, `.Width()`, and `.Height()` immediately return raw values accepted everywhere, the compiler enumerates direct field accesses but does not prevent a render width from being passed to an allocation consumer. Prefer a tagged, non-implicitly-convertible model:

```cpp
struct RenderSpace {};
struct AllocationSpace {};
struct OutputSpace {};

template <class Space, class Scalar>
struct Extent2D
{
    Scalar width{};
    Scalar height{};
};

template <class Space, class Scalar>
struct Offset2D
{
    Scalar x{};
    Scalar y{};
};

template <class Space, class Scalar>
struct Region2D
{
    Offset2D<Space, Scalar> origin{};
    Extent2D<Space, Scalar> extent{};
};

using RenderExtent = Extent2D<RenderSpace, float>;
using AllocationExtent = Extent2D<AllocationSpace, float>;
using OutputExtent = Extent2D<OutputSpace, float>;
```

Resource-specific tags can then prevent accidental source/destination mixing:

```cpp
struct MainSourceSpace {};
struct EyeIntermediateSpace {};

using MainSourceRegion = Region2D<MainSourceSpace, uint32_t>;
using EyeOutputRegion = Region2D<EyeIntermediateSpace, uint32_t>;
```

Conversions should be explicit, named, and placed at policy or API boundaries. Avoid exposing a public `.value` throughout the implementation. A named adapter such as `ToD3DExtent()` at the D3D call site is preferable because it makes every loss of type information searchable and reviewable.

Phase 1 must also preserve:

- floating-point versus integer semantics;
- rounding and truncation order;
- division-before-rounding versus rounding-before-division;
- standard-layout/trivial-copy requirements for any structure copied or logged as bytes;
- equality, hashing, serialization, and formatting behavior.

CI proving that the code compiles is not, by itself, proof that Phase 1 is behavior-null. Add `static_assert` checks for the required wrapper properties and compare resolution-plan logs for identical input settings before and after the mechanical refactor.

## Revised phases

### Phase 0 — evidence ledger

Before editing, record the exact source commit, test artifact, config manifest, run logs, and commands used for the site counts. Separate established observations from hypotheses.

### Phase 1a — logical extent types, behavior-null

Type render, allocation, output, and the classified HMD display field. Provide explicit raw adapters only at external boundaries. Preserve all calculations and rounding exactly.

### Phase 1b — region and resource-coordinate inventory

Inventory every offset, box, region, eye origin, sampling dimension, dispatch extent, and normalization domain that composes with the three logical plan extents. At minimum include both VR encode paths, submit/presentation paths, foveated passes, menu composite, Streamline, and FidelityFX.

This phase need not immediately introduce a unique C++ type for every resource, but the site table must classify them. High-risk source/destination compositions should receive resource-tagged regions before semantic correction.

### Phase 2 — semantic correction

Review the exhaustive site table using the rubric above. Each changed site must have a contract, invariant, and runtime falsifier. Explicitly include the v4 compute writer and resource/view creation associated with the clear.

### Phase 3 — controlled verification

The current pass condition is necessary but insufficient. `eyeOut` can show clean temporal activity while the image is a clean but incorrect crop, stretch, stereo origin, or projection.

Require all of the following:

1. Every logged source and destination region fits its concrete resource description.
2. Dispatch coverage matches the intended local work extent.
3. The controlled allocation A/B no longer changes correctness at a fixed render/output contract.
4. The temporal probe is clean for the registered rows/regions.
5. Visual stereo scale and depth are correct at Balanced and Performance.
6. Opening and closing the Skyrim menu does not split the eyes.

The last criterion is a regression test, not evidence that the geometry refactor fixes the menu defect.

## Keep the menu defect separate

The v4 report found 1,671 frames in which the left eye took the CSX success path while the right eye independently fell open to a different path. That pair-atomicity failure explains the menu-triggered crossed-eye result without requiring an allocation/render conflation.

The geometry type split may improve shared inputs, but it must not be treated as the established fix for menu divergence. Track these as two independent correctness properties:

- **geometry correctness:** both eyes sample and present the intended regions;
- **pair atomicity:** both eyes use one presentation policy for a frame.

Phase 3 should test both, and failure of one must not be reported as falsifying or confirming the other.

## Recommendation to Claude

Proceed with the compiler-assisted refactor after revising the plan as above. The most important changes are to type output geometry in the first mechanical pass, model resource-coordinate regions separately from engine allocation, and strengthen Phase 3 beyond temporal activity. The allocation evidence is good enough to justify this audit, but not yet strong enough to pre-declare the root cause.

With those revisions, the work has a useful outcome even if no wrong consumer is found: it will either expose a concrete contract violation or establish, with much stronger coverage, that the remaining defect lies in resource lifecycle, vendor state, or stereo submission rather than ambiguous geometry names.
