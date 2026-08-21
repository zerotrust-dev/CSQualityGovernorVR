# Note: crossed eyes on menu open — the eye-path split

**Written:** 2026-08-19. Parked, not fixed.
**Read this if:** stereo suddenly goes crossed when a menu opens and recovers
when it closes, under VR Render Scale Mode.

---

## Symptom

- Gameplay: eyes registered (geometry may still be wrong for other reasons).
- A menu opens: **image goes crossed.**
- The menu closes: registration returns.
- Perfectly reproducible while the menu is open — no intermittency.

## Mechanism

The two eyes are submitted through **different presentation contracts in the
same compositor cycle**:

- eye 0 completes CSX's vendor path and hands the compositor CSX's replacement
  texture;
- eye 1 returns early from `SubmitVRUpscaledFrame` and the outer hook therefore
  gives the compositor **Skyrim's original submission**.

The compositor receives one upscaled eye and one native eye. That is the
crossing.

Measured by StereoTrace v4: 1,671 consecutive frames of `L1/R0`, beginning at
the frame the menu opened and ending when it closed. Zero intermittency, which
is the signature of a control-flow invariant rather than a race.

## The gate

`SubmitVRUpscaledFrame`, near the top:

```cpp
if (vrRenderScaleMode &&
    vrMenuFrameTransaction.frame == currentFrame &&
    vrMenuFrameTransaction.presentationDecisionLatched) {
    if (vrMenuFrameTransaction.poisoned)
        return false;
}
```

**`presentationDecisionLatched` can only be true for the second eye**, because
the first eye is what latches it. So the predicate does not mean "the
transaction is poisoned" — it means *"the transaction is poisoned **and** I am
eye 1"*. Eye asymmetry is built into the condition.

The guard also protects nothing: eye 0 published its output in the same frame
under the same poison. It only guarantees the eyes disagree.

## What poisoned it is still unknown

In the captured run the transaction owned **no menu work** — `captured=0`,
committed `=0`, `menuPresentationAttempt=false` on the first-eye decision. So
the poison describes a fault in work that was never built.

Three of the obvious reasons are excluded because they sit behind
`OwnsPresentationWork()`, which was false — including
`required-menu-layer-missing-before-submit`, which explicitly tests
`capturedOperations == 0`. The two that can fire without ownership are:

- `menu-composite-preflight-failed`
- `menu-transport-unavailable`

Both set `menuLayerRequired = true` while `capturedOperations` stays 0, which is
exactly the observed state.

## How to identify the reason without a build

Stock CSX already logs it, at debug level:

```
[VRMenuComposite] Poisoned menu frame transaction. frame={} reason={} recognized={} captured={} ...
```

Set `"Log Level": 1` in `SettingsUser.json` (it defaults to `2` = info) and
reproduce. `SetLogLevel` keeps the menu presentation trace diagnostics enabled
at debug or below, so the surrounding context comes with it.

## Ownership: unconfirmed

Do not record this as "ours" or "his" without the test below.

**For it being stock CSX's:** the gate, the poison flag and the whole menu
transaction machinery are all in the pristine `CSX3.18` base. The Hot-Envelope
branch never touched menu presentation. The gate is conditioned on
`vrRenderScaleMode`, **not** on the envelope. And the most plausible
envelope-specific trigger is ruled out —
`PrewarmVRMenuFinalCompositeResources` gates on
`finalWidth > renderWidth`, which is 6988 > 3492 under the envelope and
6988 > 4656 in stock; both pass comfortably.

**For it being ours:** it has only ever been observed in envelope sessions.

**But that observation is weak.** Those were also the only sessions in which the
**Skyrim game menu** was opened while Render Scale Mode was active at a
sub-envelope quality. Rik's own envelope sessions used the **Community Shaders
menu** (END key), which is a different context predicate
(`IsCommunityShadersMenuOpen` versus `IsKnownGameMenuContextActive`). So the
correlation may be with *which menu was opened* rather than with the envelope.

**The deciding test, no build required:** stock CSX (untick the PoC), Render
Scale on, boot Quality, open and close the Skyrim game menu. If the eyes cross,
it is a live defect in shipped CSX 3.18-VR and worth reporting upstream on its
own.

## Fix direction, if we ever take it

The presentation policy must be **compositor-cycle atomic across the eye pair**,
and chosen *before* either eye is submitted rather than discovered between them.
One policy for both eyes:

1. ordinary vendor output for both, or
2. final menu composite for both, or
3. fail-open to the original submission for both.

For the captured state — a transaction owning no menu work — option 1 is the
right answer: menu presence alone should not turn a non-owning poison into an
eye-asymmetric fallback. A fault discovered after eye 0 has gone out applies to
the **next** cycle. Device loss stays the exception, since neither path can
continue safely.

Land the failure-reason recording **before** the policy change. If the policy
lands first the crossing disappears and we never learn why a transaction owning
nothing was poisoned — which is probably a real defect in its own right.

## Evidence

- `research/stereofusion/docs/STEREOTRACE_V4_RUN_RESULT.md` (Codex)
- `docs/CLAUDE_REPLY_STEREOTRACE_V4_RUN.md` (analysis and the exclusions above)
- Trace: `stereotrace-p8440-f13238-16603.json` / `.sttrace`
