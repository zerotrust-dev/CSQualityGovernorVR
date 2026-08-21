# Hot-Envelope: where we are, and what the design actually requires

**Written:** 2026-08-17, after six test sessions.
**Purpose:** stop patching by symptom. Establish what is proven, what is broken,
and what a clean implementation has to look like.
**Branch:** `csx318-hot-envelope-full`.

---

## 1. The honest status

**We do not have a working Hot-Envelope build.** Every session with
`vrHotEnvelope = 1` has been visually broken, in a different place each time.

One clarification that matters, because it shaped our expectations. The session
that felt best - long stretches at Quality and UltraQuality, "nice visuals" -
was the **diagnostic build with the feature off**. Its own log says so on every
line:

```
[plan] render 4656x2372 into allocation 4656x2372 | scale 1.000x1.000 | subRect=no
```

That was stock CSX with Render Scale Mode on: good quality because render scale
supplies the headroom, freezes because it is stock. Hot-Envelope contributed
nothing to it. We were not close and then further away; we have not yet been
close.

---

## 2. What *is* proven

These are measured, reproduced, and independent of whether the rendering is
correct.

| finding | evidence |
|---|---|
| The boot latch can hold across quality changes | **1 boot latch per session** against 171, two consecutive runs |
| Five distinct relatch triggers exist, four never consult `restartRequired` | Code, and each guarded in turn |
| Vendor-resource readiness is reachable **only** through the relatch | `MarkVendorRuntimeResourcesReady` called only inside the relatch routine |
| A guard must never abandon an in-flight relatch | `[inflight]` line fired exactly once, on `postMutationSerialization` |
| The submit-bounds watchdog was the amplifier | mismatches 140+ -> **0**, `matches=true` at every quality |
| DLSS accepts input down to half the output per axis, except UltraPerformance | measured via `slDLSSGetOptimalSettings` |
| The engine renders each eye into **its own half of the allocation** | two runs: origin 2328 aligned, origin 2054 cross-eyed |

The relatch problem is solved. That was the objective, and it holds.

---

## 3. What is broken, and why

### D1 - colour and depth read different origins (cause of the "cardboard depth")

| quality | colour origin | depth origin | gap |
|---|---|---|---|
| Quality (envelope) | 2328 | 2328 | 0 - correct |
| Balanced | 2328 | 2054 | 274 px |
| Performance | 2328 | 1746 | 582 px |
| UltraPerformance | 2328 | 1164 | 1164 px |

Depth no longer corresponds to the pixel it belongs to, so stereo reconstruction
flattens the scene into planes. Correct at exactly one preset - the envelope,
where both origins coincide.

**This was known before the build shipped** and recorded in the commit message
as "not changed". That was a process failure, not a discovery.

### D2 - the render extent violates DLSS's accepted range at UltraPerformance

```
eUltraPerformance output 3494x3558: accepts 1165x1186 .. 1165x1186
```

A zero-width range. `ScaleVRRenderDimension(3494, 1/3)` yields **1164**. We feed
an out-of-range extent at the one mode with no tolerance. Consistent with
UltraPerformance refusing to step back up, and a candidate for the hang.

### D3 - unexplained hang at UltraPerformance

The log simply stops. No error, no deferral, no relatch. Possibly D2; not
established.

### D4 - validators that assert render size equals allocation

`Upscaling.cpp:6467` requires `engineRenderSize.x == boot.renderEyeWidth * 2`.
True by construction in stock, false by construction under an envelope. Not yet
observed failing, but it is a latent defect of the same family as the submit
watchdog - and that one cost three sessions.

### D5 - relaxed convergence predicate

`IsVRRenderScalePhysicalContractConverged` now answers "converged" for a fitting
quality. Flagged as an open risk when written; still unexamined.

---

## 4. The actual problem

Every defect above is the same defect. CS holds one invariant everywhere:

> **render size == allocation size**

Hot-Envelope breaks it deliberately - that *is* the idea. But the invariant is
assumed in a great many places, and every one of them was written when the two
were interchangeable:

| symbol | references |
|---|---|
| `finalOutputSize` | 103 |
| `engineRenderSize` | 101 |
| `trueHMDEyeWidth` | 41 |
| `boot.renderEyeWidth` | 24 |
| `sourceStereoLayout` | 13 |

**We have been finding the ones that matter by breaking them, one session at a
time.** Six sessions, six symptoms, each fixed locally and correctly, each
revealing the next. That converges by attrition, not by design.

---

## 5. What a clean implementation looks like

Three geometries exist. CS conflates two of them because they were always equal.
Name all three, and make every consumer state which it wants:

| name | meaning | who wants it |
|---|---|---|
| **allocation** | physical target dimensions; fixed while the envelope holds | resource creation, texture descs, submit-box origin, validators about memory |
| **render extent** | what is actually rendered this frame | viewports, scissors, dispatch sizes, jitter, DLSS input extent, submit-box size |
| **output** | display size per eye | DLSS output extent, projection, compositor |

Under that model the current defects stop being expressible rather than being
fixed:

- **D1** - colour and depth both ask for the same named origin, so they cannot
  disagree.
- **D2** - the render extent comes from DLSS's `optimalRenderWidth`, not from our
  own scale arithmetic, so 1164/1165 cannot arise.
- **D4** - a validator asks whether the *allocation* still matches the boot
  contract, which stays true.

And the five `if (hotEnvelope)` conditionals we have accumulated all disappear,
because no site has to decide anything.

---

## 6. The consumer classification

Done by clustering every reference by its enclosing function. The result is much
better than the raw counts suggested: **most consumers are already correct**, and
the real risk list is short.

### Class A - resource sizing. Wants allocation. **Already safe, no change.**

`CreateUpscalingTextureResources` sizes every texture from
`renderTargets[kMAIN]`'s own `D3D11_TEXTURE2D_DESC`, not from the plan at all:

```cpp
main.texture->GetDesc(&texDesc);   // then reused for every vendor texture
```

Because `kMAIN` *is* the allocated target, resource sizing follows the allocation
automatically and cannot shrink with the render extent. The `engineRenderSize`
references inside `CheckResources`, `RecreateVendorRuntimeResources`,
`ApplyPendingVendorRuntimeReset` and `PrewarmVRMenuFinalCompositeResources` are
**diagnostic logging only**.

This was the biggest suspected risk and it is not one.

### Class B - upscale ratio. Wants render extent. **Already correct.**

These read `engineRenderSize` and `finalOutputSize` as a *pair* describing "we
upscale from X to Y". They want what was actually rendered, which is exactly
what `engineRenderSize` now holds:

`ConfigureUpscaling`, `Upscale`, `PreparePerEyeInputs`, `FinalizePerEyeOutputs`,
`UpdateHistoryResetState`, `ResolveRuntimeMipBias`, `SubmitVRUpscaledFrame`,
`GetRuntimeFoveatedRegionDimensions`, `RefreshSubmitStageUnderwaterMask`,
`TryReplaceVanillaDynamicResolutionUpsample`.

`UpdateHistoryResetState` deserves a note: it resets temporal history when those
sizes change, so under an envelope it resets on every quality change. That is
correct and wanted, not a bug.

### Class C - validators asserting render == allocation. **Broken by design.**

| site | assertion | under an envelope |
|---|---|---|
| `IsVRFixedVendorResolutionPlanOwnerExact` :6467 | `engineRenderSize.x == boot.renderEyeWidth * 2` | **false** |
| `IsVRFixedVendorMaskRepairContractExact` :6830-6842 | same family | **false** |
| `IsVRFixedVendorResolutionPlanOwnerExact` :6773 | `engineRenderSize == finalOutputSize` (a "no upscale" test) | fine |
| `QueueVRMenuPresentationTraceD3DHookBank` :12018 | `finalOutputSize > engineRenderSize` | fine |

The first two are latent defects of exactly the family that cost three sessions
via the submit watchdog. They should compare the **allocation** against the boot
contract, which stays true.

### Class D - submit geometry. **Colour fixed, depth still wrong.**

`ResolveVRSubmitSourceRegion`: colour now takes its origin from the allocation
and its extent from the active quality. Depth still resolves against
`sourceStereoLayout`, giving packed origins. That divergence is D1.

### Class E - extent derivation. **Wrong source.**

`ScaleVRRenderDimension` computes the render extent from our own scale
arithmetic. It should come from DLSS's `optimalRenderWidth/Height`, which is
authoritative and would not produce 1164 where the runtime demands 1165.

### Class F - post-load presentation recovery. **Safe.**

`ShouldSuppressVRPostLoadCompositorSubmit` (17 references) and
`TryRepairVRPostLoadFixedCompositorCandidate` (8) were the last unclassified
clusters. Both only check that the render sizes are **whole numbers**:

```cpp
a_plan.engineRenderSize.x == static_cast<float>(planRenderWidth)   // integrality
```

Their real size comparisons are `finalOutputSize` and `sourceDesc` against
`state->screenSize` - the display. Neither cross-checks the render extent
against the allocation, so neither is affected by an envelope.

### Two corrections to this classification

Made while implementing, and recorded because the errors are instructive.

**`IsVRFixedVendorMaskRepairContractExact` is safe.** It was listed in Class C.
It derives `renderWidth` *from* `plan.engineRenderSize` and compares them - a
self-consistency check, not a cross-check against the allocation. Only
`IsVRRenderScaleResolutionPlanExact` genuinely compares against `boot`.

**Class E was not a defect and has been dropped.** The claim was that the render
extent 1164 violates `eUltraPerformance`'s zero-width accepted range of
1165x1186, and that this explained the UltraPerformance stickiness.
`ScaleVRRenderDimension` floors and then forces even (`bounded & ~1u`), so it
can never produce an odd 1165 - **but stock CSX computes the same 1164 and works
at UltraPerformance.** DLSS evidently tolerates it. Changing the rounding would
have altered allocation sizes everywhere to chase a defect that was never
established. The UltraPerformance stickiness remains **unexplained** rather than
falsely attributed.

---

## 7. What is still to be done

**Done**

1. ~~Read the two unclassified clusters~~ - Class F above, both safe.
2. ~~**D1** - depth takes the same region as colour~~ - depth is now *derived
   from* the colour box rather than resolved separately, so the two cannot
   disagree.
3. ~~**Class C** - validator compares the allocation~~ -
   `IsVRRenderScaleResolutionPlanExact` now reads `engineAllocationSize`, with a
   fallback to `engineRenderSize` for plans that never set it.

**Remaining**

4. **Explain the UltraPerformance stickiness and the hang.** Both are still
   unexplained. Not to be attributed to anything without evidence.
5. **Introduce the three names** in `RuntimeResolutionPlan` so Class B and Class
   C cannot be confused again. `engineAllocationSize` is the first of them.
6. **Remove the five `if (hotEnvelope)` conditionals** as their sites become
   unambiguous.

Items 1-3 were what stood between us and a build worth testing, and each had a
named cause rather than a symptom - which is the difference between this list
and the previous six sessions. Item 4 is the honest remaining unknown.

---

## 7. What this is worth to the upstream author

Independent of whether we ever ship working code, this is the useful output:

- the five relatch triggers, and that four never consult `restartRequired`
- that vendor-resource readiness is reachable only through the relatch, which is
  the real reason the boot latch cannot currently move
- that the boot latch *can* hold, measured
- the DLSS accepted-range table, including the zero-width UltraPerformance range
- **the list of places that assume render size equals allocation size** - the
  thing nobody can see from outside the renderer

`PR #2` should be described as a demonstration of these findings, **not** as a
candidate patch. It provably does not work on its own, and the note already sent
says so.
