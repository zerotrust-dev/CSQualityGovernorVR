# Phase 1a — the consumer site table

**Commit:** `2eaf90ddc` on `csx318-hot-envelope-diag` (parent `0bda6f9af`, the phase 0A tip).
**Date:** 2026-08-21. **Headset:** not needed for anything in this document.

---

## What phase 1a was for

Six attempts to fix the image below the envelope quality failed. Every one of
them was a guess about *which* consumer asks the wrong question, and reading the
code cannot settle that, because a consumer that wants the allocation and asks
for the render extent looks exactly like one that is right — and in both shipped
configurations it *is* right.

So phase 1a stops reading and makes the compiler produce the list.
`engineRenderSize` and `engineAllocationSize` now have two different types that
do not convert into one another, with `width`/`height` instead of `x`/`y`, so
every existing access had to be rewritten. The rewrite is the census.

**The deliverable is this table, not the commit.**

## How to reproduce the counts

Counting rule: **occurrences of `.engineRenderSize` / `.engineAllocationSize` as
a member access, in the three implementation files, excluding whole-line
comments.** Two occurrences on one line count twice.

```bash
git checkout 2eaf90ddc
```

| | render | allocation | total |
|---|---|---|---|
| sites | 99 | 17 | **116** |
| distinct consumers | | | **32** |

Earlier revisions of the plan quoted 107/17 from a different commit and a
looser rule (bare identifier occurrences, including declarations and comments).
The number moved because phase 0A added sites, not because either count was
wrong; the rule above is the one to use from here.

## What the types do and do not guarantee

**Do:** a whole `RenderExtent` cannot reach a consumer expecting an
`AllocationExtent`, in either direction, by any implicit route — asserted in
`VRGeometrySpaces.h` and again in `tests/vr_geometry_spaces_test.cpp`. Every
field access had to be rewritten, so the list below is exhaustive over the plan,
including code no play session executes.

**Do not:** `.width` is a bare `float` on both, so a *component* can still cross.
Component hardening is phase 1b, aimed only at the flows this table marks risky.

**Behaviour-null, and why that is more than a claim.** Every change is either a
field rename or a named adapter wrapped around the identical expression. No
arithmetic, no rounding order, no float semantics. The header asserts standard
layout, trivial copyability, and the same size and offsets as the `float2` it
replaced; the plan is not byte-copied anywhere, which was checked rather than
assumed.

## The one thing this table does not cover

The plan is **not** the only carrier of these two geometries. Five other
structures carry them per eye as `renderEyeWidth` / `displayEyeWidth`:

```
Upscaling.h:532  VRRenderScaleRelatchSignature
Upscaling.h:564  VRRenderScaleResourceKey
Upscaling.h:1262 VRRenderScaleProfileSnapshot
Upscaling.h:1325 BootSnapshot
Upscaling.h:1909 VRExistingVendorProviderSnapshot
```

That is roughly 190 further sites, untouched here. Several consumers below read
*both* representations in the same expression. Stated so the census is not read
as complete: it is exhaustive over `RuntimeResolutionPlan`, not over CSX.

---

## The table

Legend for the buckets below. The 32 consumers partition into five;
`RefreshRuntimeResolutionPlan` appears twice because it both writes the plan and
reads it for the foveated block, so the bucket sizes sum to 116 sites over 32
distinct consumers.

| | meaning |
|---|---|
| **P** | producer — writes the field |
| **D** | diagnostic or log only; cannot change behaviour |
| **N** | not reachable on the envelope path (gated to Native, RS-off or fixed-vendor) |
| **E** | already envelope-aware — distinguishes the two geometries deliberately |
| **L** | live on the envelope path, and the render extent is genuinely what it wants |
| **S** | **suspect** — live on the envelope path *and* composes with a physical resource |

`*` marks a site reading the allocation.

### S — suspects (10 consumers, 20 sites)

These are live when the envelope holds, and each one takes a *render* extent and
uses it against something sized by the *allocation*. That is the shape of the
defect. None of them is yet a finding.

| consumer | lines | what it does with the render extent |
|---|---|---|
| `Upscaling::PreparePerEyeInputs` | 38013 | `eyeWidthIn = renderSize.x / 2` — an eye stride into resources allocated at the allocation extent |
| `Upscaling::FinalizePerEyeOutputs` | 38347 | same stride, output side |
| `Upscaling::Upscale` | 51055 | encode dispatch extent over allocation-sized targets |
| `Upscaling::RefreshSubmitStageUnderwaterMask` | 51578–51579 | per-eye input width for a copy into `kMAIN` / `kUNDERWATER_MASK`, both allocation-sized |
| `Upscaling::TryReplaceVanillaDynamicResolutionUpsample` | 46886–46887 | `inputWidth` for a blit whose source is the allocation-sized intermediate |
| `Upscaling::GetRuntimeFoveatedRegionDimensions` | 35642 | per-eye foveated input extent |
| `Upscaling::RefreshRuntimeResolutionPlan` (foveated block) | 19798–19799 | foveated region plan input extent, same question one layer up |
| `Upscaling::CheckResources` | 34842–34843, 34849–34850 | `canPreserveFSRResourcesForCurrentVRPlan` compares FSR resources against the **render** extent per eye |
| `Upscaling::ApplyPendingVendorRuntimeReset` | 34300–34301, 34307–34308 | `areCurrentFSRResourcesCompatible`, the same comparison |
| `BuildFSRResourceLifecycleRequestKey` | 5595–5596 | hashes the render extent into the key identifying FSR **resources** |

**The last three are one story.** Under the envelope the FSR resources are
allocation-sized and do not move, but all three of these track the render
extent, so every quality change makes them report a changed resource identity.
That is a vendor-lifecycle churn mechanism that costs nothing to test and would
not show up as a visual defect at all — it would show up as rebuilds.

**A separate hypothesis, flagged not claimed.**
`TryCaptureAndSuppressVRMenuBridgeDraw` (14574–14575, classed **L** below)
accepts a destination only if it matches *either* the render extent or the final
output:

```cpp
const bool destinationSizeValid =
    (destination.width == renderWidth && destination.height == renderHeight) ||
    (destination.width == finalWidth && destination.height == finalHeight);
```

Under Hot-Envelope an allocation-sized destination matches **neither**, so the
bridge draw is not captured and the transaction ends with
`capturedOperations == 0` — which is exactly what the menu eye-path split
capture showed. That would make the menu defect a consequence of this same
conflation rather than a stock CSX bug.

*Falsifier, cheap:* log `destination.width/height`, `renderWidth` and the
allocation at that decision point, open a menu under the envelope, and look for
`destination-size-mismatch`. If the destination is allocation-sized, the
hypothesis stands; if it matches `renderWidth`, it is dead. This does **not**
change the honest caveat in `FINDINGS-FOR-CS.md` 1.1 — it is a reason the split
may be ours, not evidence that it is.

### E — already envelope-aware (2 consumers, 12 sites)

| consumer | lines | note |
|---|---|---|
| `IsVRRenderScaleResolutionPlanExact` | 7042–7047 (4 allocation) | uses `boot.renderEyeWidth` under an active render-scale contract and the plan otherwise |
| `Upscaling::ConfigureUpscaling` | 39970–39971, 39981–39985* | computes the sub-rect ratio from render over allocation; the one consumer written for this feature |

### L — live, and the render extent is the right question (8 consumers, 16 sites)

| consumer | lines |
|---|---|
| `Upscaling::SubmitVRUpscaledFrame` | 45321–45322, 46061–46064* (the allocation use is the trace record) |
| `Upscaling::IsVRMenuTransportContractPresent` | 12599–12600 |
| `Upscaling::PrewarmVRMenuFinalCompositeResources` | 14238–14239 |
| `Upscaling::TryCaptureAndSuppressVRMenuBridgeDraw` | 14574–14575 (see the hypothesis above) |
| `Upscaling::ResolveRuntimeMipBias` | 19595 |
| `Upscaling::UpdateHistoryResetState` | 50643 |
| `Streamline::Upscale` / `GetRuntimeUpscaleSizes` (FidelityFX) | Streamline.cpp:1846, FidelityFX.cpp:178 |

The vendor upscaler genuinely wants the extent that was rendered. These are the
sites where a "fix" would be the actual mistake.

### N — not on the envelope path (5 consumers, 43 sites)

All gated to `ResolutionOwner::Native`, the fixed-vendor post-load path, or
`!IsVRRenderScaleCurrentOrTargetRelevant`. 37% of the census, and none of it can
explain the defect. Worth stating: without the compiler this bulk would have
consumed the audit.

| consumer | lines |
|---|---|
| `Upscaling::ShouldSuppressVRPostLoadCompositorSubmit` | 42783–43138 (16) |
| `IsVRFixedVendorResolutionPlanOwnerExact` | 7226–7356 (9) |
| `Upscaling::TryRepairVRPostLoadFixedCompositorCandidate` | 40838–40865 (8) |
| `IsVRFixedVendorMaskRepairContractExact` | 7411–7423 (6) |
| `Upscaling::PrepareVRNativeRestorePresentationObservation` | 29764–29776 (4) |

### D — diagnostics (7 consumers, 21 sites)

| consumer | lines |
|---|---|
| `Upscaling::DrawSettings` | 15689–15716 (4 allocation) |
| `LogGeometryPolicyShadowMismatch` | 6727–6731 (2 allocation) |
| `MakeRuntimeResolutionPlanLogKey` | 11213–11214 |
| `RecordVRMenuPresentationTraceLayerStage` | 11055–11056 |
| `LogVRMenuPresentationTraceBridgeDecision` | 14944–14945 |
| `RecreateVendorRuntimeResources` | 34186–34187 |
| `LogDLSSDispatchDiagnostics` (Streamline) | 388–389 |

### P — producer (4 sites, in a consumer already listed under S)

`Upscaling::RefreshRuntimeResolutionPlan` — 19652, 19745*, 19747, 19753. Since
phase 0A this is the only writer, and it takes both extents from one
`VRGeometryPolicy::Derive()` call.

---

## What the census changes about the plan

1. **The suspect set is ten consumers, not "somewhere in 20,000 lines".** Phase
   2 has a finite list with a rubric, and the three FSR-lifecycle ones can be
   tested without a headset.
2. **43 of 116 sites are provably irrelevant.** That is the specific thing a
   play session could never have established.
3. **One new hypothesis with a cheap falsifier**, connecting the menu eye-path
   split to the same conflation. It was found by the compiler, not by looking
   for it.
4. **A stated boundary.** ~190 further sites carry the same two geometries per
   eye on five other structures. Phase 1b should decide whether to retype those
   or say why not.

## Open question for review

The **L** bucket is the one worth arguing about. It rests on a judgement —
"the vendor upscaler wants the extent that was actually rendered" — which is
true for the vendor call itself but not obviously true for the menu transport
sites that share the bucket. If any of those seven belong in **S**, that changes
what phase 2 does first.
