# The eye origin, settled by measurement

**Written:** 2026-08-18, from the PR #5 sweep. Log session 14:28:06–14:30:38.
**Supersedes:** `CSX_STEP1_EYE_PACKING.md` §1 and `CSX_STEP2_DIFFERENTIAL_MAP.md` §1.
**Build:** `csx318-hot-envelope-diag` @ `d27e3b48`, DLL
`fc521aba420d5f8a59de73a6a9db926d9358e71325c3481f9c94ffd5b9db2433`.

---

## 1. The result

Measured at quality 4 (envelope quality 3, ratio 0.882), eye 1:

| mode | eye-1 colour box | reported |
|---|---|---|
| 0 packed | `[2054, 4108]` | double vision |
| 1 allocation half | `[2328, 4382]` | **good** |
| 2 manual, px 0 | `[0, 2054]` | double vision |

> **Under RS mode with an active envelope, the engine renders each eye into its
> own half of the allocation, shrunken within that half. It does not repack.**

`4c308aee`'s inference was right and my step-1 reading was wrong. Stock's packed
convention — `ResolveVRSideBySideStereoLayout` fed the render eye width —
describes CS's *intended* layout, not where the engine actually puts pixels
under RS mode.

## 2. Why both earlier builds failed

Neither pure convention is correct. The working configuration is **mixed**, and
it had never been isolated as a deliberate choice:

| build | colour | depth | result |
|---|---|---|---|
| `deef8e9f` | packed | packed | cross-eyed |
| `4c308aee` | **allocation** | **packed** | **correct** |
| `46d88e69` | allocation | allocation | cardboard depth |
| current (`50d5fdde` + mode 1) | **allocation** | **packed** | **correct** |

Colour and depth genuinely want different origins, because they are different
consumers: `region.depth*` drives `DispatchHMDMaskClear` — the hidden-area mask
— and the downstream `exactDepthLayout` check wants
`depthOffsetX == eyeIndex * renderEyeWidth`, the packed value.

`4c308aee` reached the correct state and its own commit message called the split
"not changed… left alone rather than changed on a hunch". That caution was
right. `46d88e69` then unified them and broke it.

## 3. The envelope itself held

Independently of the geometry, this session is the cleanest evidence yet:

- **1 boot latch**, at startup
- **1 deferral**, at 14:28:06, transient
- ~20 quality changes and 3 origin-mode changes across 4.5 minutes
- `matches=true` on every submit line, at every quality, in every mode

## 4. Two session artefacts explained

**"The slider does nothing anymore."** Not a stall. The last submit lines are
`expect=2328` — the governor had parked at quality 3, the envelope quality,
where the allocation half and the packed origin are the same pixel and all three
modes produce `[2328, 4656]`. The diagnostic is inert there by construction.
This is the same trap that made three earlier builds uninformative, and it
should have been designed out by refusing to display the slider at the envelope
quality.

**"DLAA or Hoshipa makes the governor step down to UltraPerf."** Both sit
*above* the envelope, so the fits-check correctly refuses them and the governor
falls back. Expected behaviour, but it makes the ladder unusable during a sweep;
the governor's `Order` should be restricted to qualities at or below the
envelope for envelope sessions.

## 5. What is now open

The step-2 question inverts. It is no longer "why does the repack hold under RS
mode" but:

> **RS-off reads each eye at `renderSize/2` (packed) and works. RS-on + envelope
> needs the allocation half. Why do the two paths differ?**

Both run `ApplyDynamicResolutionState` with a sub-1.0 ratio. The difference must
lie in what `state->screenSize` means to the engine in each case — the display
size under RS-off, the reduced allocation under RS mode. That is a bounded
question and it is the last one before a merge proposal.

**It does not block progress.** The correct origins are known empirically for
both configurations, and the mixed rule is expressible without knowing why.
