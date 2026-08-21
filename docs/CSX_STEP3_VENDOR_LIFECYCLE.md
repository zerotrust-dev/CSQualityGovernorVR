# Step 3: the vendor-resource lifecycle

**Written:** 2026-08-18. Follows `CSX_STEP2_DIFFERENTIAL_MAP.md`.
**Sources:** pristine base `CSX3.18` = `2051e2ae`; `CommunityShaders.log`
2026-08-18 09:14 (build `46d88e69`).

---

## 1. The answer

**Step 3 does not block.** The vendor-resource strand is already resolved, and
the claim that made it look structural is false.

The audit recorded, as proven and as the single most valuable finding to send
upstream:

> Vendor-resource readiness is reachable **only** through the relatch.
> `MarkVendorRuntimeResourcesReady` called only inside the relatch routine.

It is called from **four** functions in stock:

| function | line | context |
|---|---|---|
| `ApplyPendingPerfModeRenderTargetRecreate` | 25923, 25989, 25996 | the relatch |
| `RecreateVendorRuntimeResources` | 33400 | |
| `ApplyPendingVendorRuntimeReset` | 33616, 33683 | |
| `CheckResources` | 34155, 34328 | |

`MarkVendorRuntimeResourcesReady` clears `pendingDLSSReset` **unconditionally** -
there is no generation-matching requirement, so any of the four clears the
block. The three non-relatch sites were probably missed because the grep that
found them also finds the far more numerous `Dirty` calls in the same region.

## 2. What the real coupling is

`ApplyPendingVendorRuntimeReset` has an explicit early-out:

```cpp
if (pendingPerfModeRenderTargetRecreate.load(std::memory_order_acquire) ||
    perfModeRenderTargetRecreateInProgress.load(std::memory_order_acquire)) {
    return true;   // do nothing; the relatch owns this
}
```

The vendor reset **defers to** a pending relatch rather than being unreachable
without one. In stock that is invisible: a quality change always sets
`pendingPerfModeRenderTargetRecreate`, so the reset always defers and the
relatch always does the work. The two are never both absent.

It matters that this path is reachable under RS mode at all. `Upscale()`
early-returns under `vrRenderScaleSubmitStageOwnsOutput`, so the main-pass call
at 50096 never runs there - but the **submit-stage** path calls it at 44947
(`"submit-stage "`), which does.

## 3. What actually happened in run 2

The deferral gate turns into a deadlock only if a relatch is pending *and*
cannot complete. That is exactly the state run 2 was in, and the trigger was the
submit-bounds mismatch, not the envelope:

1. The colour box spanned the allocation half while `expectedEye*` was the
   active quality's size, so `matchesExpectedSize` went false.
2. `ServiceSubmitStageBoundsFallbackWatchdog` queued a **recovery relatch**,
   setting `pendingPerfModeRenderTargetRecreate`.
3. `ApplyPendingVendorRuntimeReset` hit the gate above and did nothing.
4. The envelope guards refused the relatch, so the flag never cleared.

The block reason the governor logged was literally `RelatchPending` - which is
that flag, not the vendor flag. The evidence was pointing at the gate the whole
time.

**So the strand was a consequence of the geometry defect, one layer removed.**
Not a design coupling in need of decoupling.

## 4. The measurement

Current build, one session, 16 quality changes:

| | run 2 (`3912b783`) | current (`46d88e69`) |
|---|---|---|
| boot latches | 11 (startup only) | **1** |
| `fits` decisions avoiding a relatch | - | 16 |
| vendor DIRTY | 172 | **1** (startup, generation 0) |
| vendor READY | 0 | 0 |
| dirty flags stranded | all | **0** - cleared without rebuild |
| requests blocked `RelatchPending` | continuous | **1**, transient, 1.5 s into startup |

The submit-bounds fix that removed the mismatch (`matches=true` at every
quality) removed the strand with it.

## 5. What this does *not* prove

Stated plainly, because the temptation is to read this as more than it is.

**READY was never exercised.** The one dirty flag was cleared by
`ClearVendorRuntimeResourcesDirty`, not by a `Ready`. So this is evidence that
nothing strands, not evidence that the readiness path works under an envelope.
If a future change makes a quality transition dirty the vendor resources, that
path is still untested.

**No quality change dirtied anything.** Under the envelope the contract
generation does not move, and the `Dirty` calls are keyed to it, so the 16
changes marked nothing. DLSS is instead reconfigured for each new input extent
through the viewport slots keyed by `(qualityMode, dlssPreset)` - which is what
the PoC hoped for, and is the mechanism the reset lifecycle was previously
standing in front of.

**The slot cache is the residual risk.** `kVRDLSSViewportSlotCount = 2` against
four or five presets in rotation evicts on most changes. That was flagged in the
PoC as the likely next hitch once the relatch was gone, and it is now the next
hitch by elimination rather than by measurement.

## 6. Consequence for the plan

Two of the three obstacles are down and measured:

| obstacle | status |
|---|---|
| the relatch / boot latch | **solved** - 1 latch per session against 171 |
| vendor-resource strand | **solved** - a consequence of the geometry defect |
| stereo geometry | **open** - PR #5 sweeps it |

The remaining work is the eye geometry and nothing else. That is a much smaller
project than the audit described, and it means the "small enough for the author
to merge" constraint is still plausibly satisfiable.

## 7. Correction to send upstream

The PoC document (section 8, *"What we take from this"*) tells ParticleTroned
that vendor readiness is reachable only through the relatch and that this is the
thing we would most want him to have. **That is wrong and needs retracting
before it is relied on.** The accurate finding is more useful anyway:

> `ApplyPendingVendorRuntimeReset` defers to a pending render-target recreate.
> If anything can set that flag without the recreate being able to complete, the
> vendor lifecycle stalls indefinitely and blocks both the external API and the
> CS menu. The submit-stage bounds watchdog can set it.

That is a genuine latent deadlock in stock, independent of Hot-Envelope, and
worth reporting on its own.
