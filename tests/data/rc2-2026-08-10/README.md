# RC2 baseline — archived 2026-08-10, immediately before installing RC3

Everything here was produced on **MGO 4.0beta RC2 with Community Shaders
Particle Lights fork PL3.15** (Nexus 166950, archive stamped
`2026-07-09T21-13Z`, CS build 9), on a Pimax Crystal Super at 72 Hz with an
RTX 5090.

It is archived rather than left in place because captures land in
`Documents/My Games/Skyrim VR/SKSE/CSQualityGovernorVR/`, which is **not**
per-profile — `LocalSettings` and `LocalSaves` redirect INIs and saves into each
MO2 profile, but not this. RC3 writes into the same folder, so without this copy
the two lists' data would differ only by timestamp.

These captures predate the provenance lines added in Phase 0, which is exactly
the gap that change closes: from the next build onward a capture records its own
CS build, API revision and preset ladder.

## Sessions

| stamp | what it is |
|---|---|
| `20260809_195900` | evening session, 4 sweeps |
| `20260810_085403` | aborted after the player mistook the 4-sweep calibration for controller oscillation; useful for its settle numbers (0.73–0.90 s) and for showing NativeAA at 99.2% miss / 7.9% dropped in that scene |
| `20260810_091513` | **the valuable one.** `Sweeps = 1`, then the governor held the lever for 141 s: 8 applied changes, no circuit-breaker trip. This is E-33 |

RC3 is being installed alongside, into
`C:\Data\Games\SkyrimVR MGO 4.0 beta rc3`, sharing the same downloads folder —
so the RC2 instance these captures came from remains installed and re-runnable
rather than being upgraded away.

## Environment

`ENVIRONMENT.txt` records the CS version and instance layout.
`modlist-rc2.txt` is the enabled mod list (1705 enabled of 1773 entries), kept so
RC3's additions can be diffed rather than guessed at — the cost model and step
ratios are properties of the mod list, and knowing *what* changed is the
difference between explaining a shift and merely observing one.

## What these are good for after RC3

- as the reference the RC3 numbers are compared against
- `20260810_091513_timeline.csv` is the evidence behind E-33 and the
  pre-registered prediction in D-20 (climbs 2 and 3 refused, four of eight
  changes removed)

They are **not** tuning data for RC3 or anything later. A cost model fitted
across two mod lists is meaningless; see `docs/RC_PORTING_PLAN.md`.
