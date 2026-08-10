# Following MGO 4.0 through its release candidates

Written 2026-08-10, before RC3 was installed.

MGO 4.0beta will arrive as a series of release candidates, each adding mods and
changing load, ending in a final 4.0 release. The governor has to stay operative
across all of them and be fully working on the final one. This is the plan for
doing that without re-deriving everything each time.

## What is version-dependent, and what is not

The distinction is the whole plan. Most of the work does not need redoing.

**Survives every RC unchanged:**

- the controller itself (`GovernorCore`) — tiers, thresholds, landing check,
  step-ratio learning
- the replay tool and its invariants — the constrained optimum being a true
  bound, the divergence identity, grid alignment
- the committed captures **as regression fixtures**. They are what caught the
  optimum being broken three separate ways, and that value does not depend on
  which mod list produced them
- the measurement rules and this design's decisions

**Must be re-measured on each RC:**

- the cost model (`t_fixed`, `t_scaled`) — a session average of that mod list
- settle latency (E-2, ~1.0 s) — and everything set from it: `cooldownSeconds`
  at ≥2× settle, the landing check, probe pacing
- the step-ratio seeds (D-18), though the controller relearns these at runtime
- threshold tuning, if and only if the numbers move materially

**Forces code work rather than re-tuning:**

- a quality mode added, removed, or **rescaled** → `Presets.h` and every pixel
  fraction
- the CS interface revision moving → our fork patch and the negotiation floor
- preset-apply semantics changing → PL3.17 already reworked "live preset
  changes" and VR FPS Stabilizer integration
- frametime ceasing to be censored → D-4's blind-probe tier becomes unnecessary

## Phase 0 — harden before the release candidates land

Done 2026-08-10. Three changes, all aimed at the same failure: an RC changing
something under us **quietly**.

1. **Pin the CS build the ladder was verified against** (`VerifiedCsBuild`, and
   `RequireVerifiedCsBuild` to decide what a mismatch does). `Presets.h`
   hardcodes each quality mode's resolution scale, copied by hand from CS's
   `Upscaling.h`, and the VR API **cannot report them** — it sets and gets the
   preset, not its scale. So this cannot verify the scales; nothing available
   can. It detects that CS changed, and refuses to apply rather than producing
   numbers that are wrong without looking wrong. PL3.17's "major VR Render Scale
   upgrade" makes this a live risk, not a theoretical one.
2. **Provenance in every capture**, as `# key=value` lines above the header: CS
   build, API revision acquired, whether GPU timing was available, the frame
   budget, and **the preset ladder that was assumed**. The last one matters
   most: since the scales are hardcoded and unqueryable, recording them at
   capture time is the only way a later replay can tell whether they still hold.
   `ParseTrace` skips these lines, with a test.
3. **Make the downgrade loud.** The plugin negotiates the CS interface revision
   *downwards*, so a CS without our GPU-timer patch does not stop it — the
   controller silently falls back to the frametime tier, which is censored at
   the cap (E-1), making every climb a blind probe. That now logs at error level
   and says what to do about it.

**Still open from Phase 0:** the real fix for the ladder is a scale getter in the
CS API. We already ship a forked CS for the GPU timer, so adding
`GetQualityModeResolutionScale` at revision 5 is nearly free and would turn the
build-number pin into an actual verification. It is also a good candidate for
the upstream PR alongside the timer.

## Phase 1 — the per-RC routine

Repeat unchanged for each candidate.

0. **Read the CS source before touching anything.** Not the release notes — the
   source, on the branch that RC ships. Three files answer almost everything:
   the VR API header (what the interface now offers), `Upscaling.h`'s
   `GetQualityModeResolutionScale` (whether our hardcoded ladder still holds),
   and anything named Adaptive / Auto / Stabilizer (whether CS has grown a
   policy engine that makes part of the governor redundant).

   This step is here because skipping it nearly cost us twice. Reading
   `csx-3-VR` established in minutes that the ladder was unchanged, that CSX
   3.18's revision 4 is a *different* revision 4 from ours, and — the part we
   would have missed entirely — that CS had added
   `GetVRUpscalingTransitionProfileDecision`, a preflight built **for external
   controllers like ours** that supersedes hand-rolled apply plumbing. CS is
   actively growing support for what we are doing; the cost of not looking is
   reimplementing what they already give us, or missing it for a whole release.

1. Install into a **new** MO2 instance, never over the previous one — the old RC
   is the only environment in which the previous numbers mean anything. See
   `knowledge/MGO_INSTALL_LAYOUT.md` for the shared-game-folder constraints.
2. Read the CS mod's `meta.ini` → record the version. RC2 was Nexus 166950
   `PL3.15`, archive stamped `2026-07-09T21-13Z`.
3. If the CS version moved: **rebase our timer patch**, and re-verify the
   bracket still sits where it must — open at the first draw after
   `WaitGetPoses` returns, close at the compositor `Submit`. A render-scale
   rework is exactly the change that moves that code. Then re-verify the
   interface revision and update `VerifiedCsBuild`.
4. **Re-measure settle.** The sweep already records settle latency per
   transition. This is the parameter most likely to have moved: RC3 ships
   precompiled shaders for all CS presets, and PL3.17 claims smoother preset
   changes.
5. Capture one sweep plus ~6 minutes of mixed free play.
6. Run the replay. Read, in order: cost model residual, believed-vs-observed
   step ratios, the divergence breakdown, and the score against the constrained
   optimum.
7. Re-tune only if the numbers moved materially. Commit the capture as
   `tests/data/rc<N>-*.csv`; old captures stay.
8. Record an `E-nn` line naming the RC.

### Capture hygiene

Captures land in `Documents/My Games/Skyrim VR/SKSE/CSQualityGovernorVR/`, which
is **not** per-profile — `LocalSettings` and `LocalSaves` redirect INIs and saves
into each MO2 profile, but not this. Two instances therefore write into one
folder, distinguishable only by timestamp. Copy and label captures before
installing the next RC. Phase 0's provenance lines make this recoverable rather
than merely careful.

## Phase 2 — definition of done for final MGO 4.0

Stated as measurements so it is not a judgement call:

- GPU timing validated against an independent instrument on the final list
- ≥85% of the constrained optimum on a fresh capture from that list
- ≤2% of frames over budget
- settle re-measured, and `cooldownSeconds` consistent with it
- a long session with `ApplyGovernor = 1` and no crash, no circuit-breaker trip
- a recorded decision on whether we ship with the actuator enabled. The ini
  still says "ship as 0", written before it worked, and that has not been
  revisited.
