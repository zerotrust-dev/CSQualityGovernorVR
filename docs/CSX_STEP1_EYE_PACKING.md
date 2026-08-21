# Step 1 result: the eye-packing contradiction is resolved

> ## CORRECTION, same day, during step 2
>
> **Section 1's conclusion is wrong and section 6's "the fix is a deletion" does
> not follow.** Step 2 read the branch history and found the packed convention
> was already built and tested, at `deef8e9f`, with **colour and depth both at
> the packed origin** - and it was reported cross-eyed at every quality except
> the envelope. The `hotEnvelopeActive ? ApplyDynamicResolutionState` switch was
> present in every build from `cff819e3` onward, so the sub-rect was reaching
> the engine in that run too. There is no "the ratio wasn't engaged" escape.
>
> So the record is: **both conventions have been built, and both failed.**
>
> | build | colour origin | depth origin | reported |
> |---|---|---|---|
> | `deef8e9f` | packed (2054) | packed (2054) | cross-eyed |
> | `46d88e69` | allocation (2328) | allocation (2328) | cardboard depth / world too close |
>
> If neither origin works, the origin is not the defect - or not the only one.
> What survives from this document is section 2 (stock's convention is packed,
> four code sites), section 3 (the engine repacks **under RS-off**, proven by the
> default path working), and section 4 (the current build disagrees with stock's
> own layout). What does **not** survive is the inference that packed is
> therefore correct under RS-mode-plus-envelope.
>
> The live question is now sharper and better posed: **why does the repack hold
> under RS-off but not under RS mode?** See `CSX_STEP2_DIFFERENTIAL_MAP.md`.

**Written:** 2026-08-18. Follows `CSX_DYNRES_GATING_ANSWER.md` section 4.
**Sources:** pristine base `CSX3.18` = `2051e2ae`; branch
`csx318-hot-envelope-full` @ `46d88e69`; `CommunityShaders.log` session
2026-08-18 09:14.

---

## 1. The answer

Both observations were right. They were measuring different states.

> **When the sub-rect is engaged, the engine repacks the eyes contiguously at
> the render size. When it is not, the eyes sit in their allocation halves.**

The audit's "proven" row - *each eye in its own half, shrunken within that half*
- was measured on a build where the sub-rect was **not** reaching the engine
(`path=ApplyLockedFullResolution`, `scale 1.000x1.000`, `subRect=no`). Under
that state the eyes genuinely do sit at the allocation halves and origin 2328
genuinely is correct. It was a true observation of the wrong configuration.

Stock CS has exactly one convention, and it is repacked.

## 2. Stock's convention, four independent sites

All base, all VR:

| site | line | eye-1 origin |
|---|---|---|
| `ResolveVRSideBySideStereoLayout` | 1323 | `eyes[1].minX = eyeWidth` - contiguous at whatever width it is handed |
| main-pass encode | 49957 | handed `renderSize.x / 2`; also sets `seamCenterX = renderSize.x * 0.5f` |
| `PreparePerEyeInputs` | 37302 | `offsetXIn = renderSize.x / 2` |
| submit stage | 44430-44470 | RS-off: `ConvertToDynamic(screenSize).x / 2`; RS-on: `engineRenderSize.x / 2` |

`seamCenterX` is the plainest of them. It is a physical statement about where in
`kMAIN` the two eyes meet, and stock says **half the render width**, not half
the allocation.

The submit-stage line matters most, because it is the path RS mode uses:

```cpp
if (vrRenderScaleMode) {
    eyeWidthIn = ClampPositiveDimension(resolutionPlan.engineRenderSize.x) / 2u;
```

`engineRenderSize` is precisely the variable Hot-Envelope sets to the sub-rect
size. **Stock CSX, unmodified, already computes the correct repacked origin
under a hot envelope.** It needed no help.

## 3. The proof that the engine follows it

Render Scale Mode **off** is CS's default VR path. It runs
`ApplyDynamicResolutionState` at ratio 0.588, and CS reads each eye at
`renderSize/2`. If the engine placed the right eye at the allocation half
instead, the default configuration - the one Rik plays - would be grossly
cross-eyed for every user of CS in VR.

It is not. The engine repacks.

## 4. What the current build actually does

From the 2026-08-18 09:14 log. `eyeOriginX` is stock's value
(`sourceStereoLayout.eyes[1].minX`); `box` is what the branch overrode it to:

| quality | scale | subRect | stock `eyeOriginX` | branch `box.left` | gap |
|---|---|---|---|---|---|
| 3 (envelope) | 1.000 | no | 2328 | 2328 | **0** |
| 4 | 0.882 | yes | 2054 | 2328 | 274 |
| 5 | 0.750 | yes | 1746 | 2328 | 582 |
| 6 | 0.500 | yes | 1164 | 2328 | 1164 |

The log has been printing stock's correct answer next to the branch's wrong one
at every preset, all along.

They coincide at exactly one row - the envelope - **which is the one preset
where the image was correct.** That correlation is the finding.

## 5. Why the symptom was "cardboard depth" and not a black eye

This was the objection that had to be answered before accepting the above, and
the arithmetic answers it.

At quality 4 the engine renders repacked: left eye `[0, 2054]`, right eye
`[2054, 4108]`. The build reads the right eye from `[2328, 4382]`. That overlaps
the true right eye across most of its width, **offset by 274 px**, with the last
274 px falling outside the rendered region.

A horizontal offset applied to one eye only *is* a disparity error. It does not
look like corruption; it looks like the world sitting at the wrong distance.
And the offset grows with the gap - 274, 582, 1164 px - so the effect worsens as
quality drops, which matches "cardboard layers, or the world far too close".

Only at quality 6 does the read region fall entirely outside the render
(`[2328, 3492]` against a rendered `[0, 2328]`), leaving stale content from an
earlier full-envelope frame. **That is a candidate explanation for the
unexplained UltraPerformance stickiness and hang** (audit item 4), though it is
not yet established and should not be recorded as established.

## 6. Consequences

**The audit's D1 was fixed, and the symptom survived.** Commit `46d88e69`
unified depth with colour; the log confirms it - colour box and depth offset are
both 2328 at every preset. The cardboard depth was therefore never caused by
colour/depth divergence. It was caused by both of them being wrong together.

**Commit `4c308aee`** - *"clamp the submit box, do not repack the eye origin"* -
is the defect. It overrode correct stock behaviour with an inference drawn from
a run in which the sub-rect was not engaged.

**The fix is a deletion, not an addition.** Stock's own arithmetic is right;
what is needed is to stop overriding it.

## 7. Standing back

Three of the five `if (hotEnvelope)` conditionals exist to impose the
allocation-half origin. On this reading they are not merely unnecessary - they
are the bug. That is consistent with the handoff's own warning sign: *"if the
next approach starts to feel like patching one consumer at a time, that is the
signal to stop and map."* The consumers did not need patching. They were right.

**Confidence.** Sections 2, 3, 4 and 6 are established from code and logs.
Section 5 is arithmetic consistent with the reported symptom but not directly
observed; section 5's last paragraph is explicitly a hypothesis. The cheapest
confirmation is one diagnostic line in the existing `SetScissorRect` hook
recording the post-scale rect for `kMAIN`, which would show the engine's eye
placement directly rather than by inference.
