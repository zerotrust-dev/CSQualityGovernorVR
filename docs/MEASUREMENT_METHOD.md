# Measurement Method

Carried over from `custom_OpenXR_Toolkit/docs/MEASUREMENT_TRAPS.md`. Most of
2026-07-31 was lost to bad measurement rather than bad ideas, and several
confident conclusions had to be reversed. These traps will recur.

## The Rules

**1. Never read numbers with the CS menu open.**
The overlay renders every frame while open and costs roughly 1.5–2 ms. Menu
open, a configuration read 15.12 ms / 66 fps; menu closed, the same
configuration ran 72–73 fps steady with 15% headroom and no stutter.

*This is the single strongest argument for the Phase 1 cycler plugin* — it lets
transitions be observed and logged without the observer changing the result.

**2. The GPU field in CS Performance Tuning is broken.**
It read `9.72 ms` at `0.50x`, `0.77x` and `0.85x` — three configurations
differing by nearly 3× in rendered pixels. It does not measure GPU work. `Game`,
`CPU` and `FPS` in the same header respond correctly; only GPU is suspect.

A whole "you are CPU-bound, foveation cannot help" conclusion was built on that
number and was wrong.

**3. Point samples are anecdotes.**
Header values jitter — `CPU` was observed at 3.03 and 8.21 ms in otherwise
identical conditions. Use Avg / P95 / P99 over a window, or sustained in-game
observation. Never compare two single readings.

**4. Measure the worst scene, not the convenient one.**
Every ladder so far was captured on a quiet hillside. A locked framerate is
bound by its worst moment — a busy town, heavy combat, rain. Tuning to a calm
scene guarantees re-tuning later.

Also note SSGI is configured interiors-only in MGO4, so outdoor profiling
systematically understates GPU load. **The bottleneck is a property of the
scene, not of the configuration.**

**5. Perceptual A/B across a restart is unreliable.**
Repeatedly, "I can't tell if anything changed" was the outcome. Two things fix
it:

- **A visualiser** — CS's FOV Mask Visualization shows the mask directly.
- **A position, not a quality** — "where does the edge sit" survives a restart;
  "does it look slightly better" does not.

Where neither is available, capture the mirror window and compare afterwards
rather than trusting memory.

**6. One owner per lever.**
At one point three systems were pacing frames simultaneously — PrimaShock turbo
and throttle, CS/Reflex FPS limit at 70, and the 72 Hz compositor. A 70 fps cap
against a 72 Hz display cannot hit every vsync, and it presented as turning
stutter.

The same rule ended the PrimaShock-VRS-versus-CS-foveation question, and it
applies to the governor: it must be the **only** thing changing the upscale
preset while it is running.

**7. A per-preset average taken while the player is moving is not a measurement
of the scene. It is a measurement of nothing.**

This one cost a wrong conclusion on 2026-08-06, stated confidently, in writing,
from good data.

The cycler visited each preset four times over 4m38s while the player walked
around normally. Averaging every visit to a preset produced this, which looks
authoritative:

| preset | GPU (ms) | headroom P95 |
|---|---:|---:|
| UltraPerformance | 11.73 | +7.0% |
| Performance | 12.47 | +1.4% |
| Balanced | 13.19 | −1.0% |
| … | … | … |

The conclusion drawn was "the scene was heavy — only the cheapest preset has any
headroom". **Both halves of that sentence are wrong.**

There was no "the scene". The session moved through light and heavy areas, and
each preset's average is a blend of wherever the player happened to be during
its four visits. The player's own account — "lots of 70–74 fps, up to 34%
overhead, also some 56 and 60 fps" — was accurate, and the same file confirms
it: 61.5% of frames ran at ≥70 fps, 7.6% below 60, and the per-frame **minimum**
GPU time was 9.63 ms, i.e. 30.7% headroom. The averages hid every bit of that.

**Why it survives the ranking but destroys the levels.** Interleaving presets
rapidly and reversing alternate sweeps is designed to cancel scene drift, and it
works: the *ordering* was monotonic in pixel count, within single sweeps as well
as in aggregate (E-17). What that machinery cannot do is make an average
meaningful as a statement about a place. Ordering survives; levels do not.

So:

- A per-preset **ranking** from a moving session is evidence.
- A per-preset **level** — "this scene costs 13.19 ms", "there is 1.4% headroom
  here" — requires a stationary scene and a named hard save (Phase 1), or it
  requires not being said at all.
- When a headroom or cost number is quoted, the scene it belongs to must be
  quoted with it. If that cannot be named, the number is not about a scene.

**The tell that was ignored.** The player reported 34% headroom; the aggregate
said the best case was 7%. That contradiction was the data disagreeing with the
method, and the method was wrong. A large disagreement with a direct observation
is never resolved by trusting the summary statistic — go back to the per-frame
distribution first. It was in the same file, and it agreed with the player.

**8. Deduplicate by measurement identity before computing statistics.**

The per-frame CSV records the most recently *published* GPU time, so a frame
whose query had not completed carries the previous frame's value. In the
2026-08-06 capture that was 153 of 21,019 rows — 0.73%. Small, but it weights a
measurement by how long it stayed published rather than by how often it
occurred.

Group by `gpu_frame` before averaging. The same applies to any signal that is
published asynchronously and sampled synchronously, which is most of them.

**9. Another tool's log is not a matching window.**

The reference overlay accumulates over ~1 s, writes the row when the window
closes, and formats the timestamp to whole seconds. Bucketing our per-frame rows
by floored wall-clock second therefore compares *overlapping but not identical*
windows. On a stable plateau this does not matter — a shifted window over a flat
signal gives the same answer, and correlation there reaches 0.995. During a
transition it matters a great deal, and sub-millisecond claims taken across
those seconds are not supported by the data.

Restrict comparisons to stable stretches, or accept that the residual you are
measuring includes the alignment error.

## Phase 1 Logging Contract

The cycler should record per transition, without opening any menu:

```
QPC timestamp at request
requested preset        (public enum value + name)
GetUpscalePreset()      readback, immediately and after settle
GetVRUpscalingApplyBlockReasons()   at request time
frametime samples       for N seconds either side of the change
time to settle          request -> frametime stable within band
```

That yields latency and acceptance behaviour objectively, leaving only "how
does it look" to human judgement — which is the one thing human judgement is
actually good at.

## What Good Evidence Looked Like

Two results from 2026-07-31 that were trustworthy, and why:

**The foveation ladder.** Five settings, measured menu-closed, monotonic in the
predicted direction. Noise does not produce a clean ordering across five points.

**The Pimax foveation levels.** Off / Quality / Performance gave headroom floors
of 4 / 5 / 7 with spreads of 4 / 2 / 1. Again monotonic across three settings,
and the mechanism (peripheral cost is the variable part of the frame) explained
the shape.

**Monotonic ordering across three or more settings is the cheapest form of
validation available.** Prefer it to any single A/B.

**10. The serpentine sweep visits its endpoints twice in a row.**

The cycler traverses the ladder out and back, so the preset at each turn is
dwelled on twice *consecutively*. Segmenting a capture by contiguous runs of
`preset_public` therefore merges those two dwells into one block of double
length and attributes it entirely to the first pass.

On 2026-08-12 that produced a "pass 1 versus pass 2" comparison which was
silently a seven-point fit against a six-point one, in support of a proposal that
was subsequently rejected (see `REVIEW_2026-08-12_D24.md`).

Segment by transition records, or by dwell index, or split a double-length block
at its midpoint. Never by contiguous preset alone. And check the elapsed time
between passes before describing them as independent samples of different scenes:
in that capture they were 90–120 s apart, not the ten minutes claimed.

**11. Do not score a proposal on the analysis that produced it.**

Rules 8 and 10 were both already knowable when they were broken — one is written
above, the other is a documented property of our own cycler. Knowing them was not
enough, because the analysis was built by the person who wanted its conclusion.

Every error in the D-24 analysis pushed the same direction: fitted quantities
were compared against raw ones, stability was scored on the data used to fit, and
the currency switched to whichever supported the claim. Individually each looks
like an oversight; collectively they are a pattern, and the pattern is authorship.

So, for anything that would change a decision:

- Pre-register the comparison — statistic, deduplication, segmentation, held-out
  set, and the bar — *before* computing it. `REVIEW_2026-08-12_D24.md` §6 is the
  template.
- Score on data not used to fit. Held-out error is the only honest test of a
  predictor.
- Compare like with like. A fitted quantity against a raw one flatters the fit by
  construction.
- Get a second opinion from something that did not write the hypothesis, and
  hand it the suspected weaknesses rather than the case for the conclusion.

The rejection this rule comes from was worth more than the proposal would have
been if it had been accepted.

**12. Verify the configuration you are asserting, every session.**

E-61 was published on the sentence "with Render Scale Mode off there is no
relatch possible, so this 57 ms spike must be something else". Render Scale Mode
was on. It had been on since an A/B two days earlier whose second arm turned it
on and whose instructions never said to turn it back off, and three sessions were
then interpreted against a configuration that did not exist.

The setting was readable the whole time, in one line:

    grep -o '"renderScaleMode":[^,}]*' \
      "<mod>/SKSE/Plugins/CommunityShaders/SettingsUser.json"

So the rule is not "be careful". It is: **when a conclusion depends on a
configuration, read the configuration in the same breath as the measurement, and
record it with the capture.** A premise that lives only in someone's memory of
what they set two days ago is not a premise, and an experiment that flips a
setting must include flipping it back as an explicit step.

This is the same failure as Rule 11 wearing different clothes: the analysis was
built by someone who already believed the answer, and the belief was load-bearing
rather than checked. The capture provenance lines exist for exactly this, and
`renderScaleMode` is not among them - it should be.

**13. Never write an API detail from memory. Read it from the source.**

Field names, function signatures, enum values, struct layouts, units, argument
order: look them up in the vendored headers, the repository, or the project's own
documents, every time. Not "check when unsure" - the failure mode is *feeling*
sure and being wrong, which by definition does not announce itself.

This is not a general caution about carefulness. It is an economic rule. A CI
build here costs 38 minutes and a game test costs a session of Rik's evening, so
a single wrong field name spends an hour to learn something a two-second `grep`
would have said. That trade is never worth taking, however confident the guess
feels.

It nearly happened on 2026-08-17. `sl::DLSSOptimalSettings` was written from
memory - seven field names, a call signature and a result enum, none of them
verified, because the Streamline submodule is not checked out in the working
tree. Rik asked for a review before the push. The header existed on disk in
another checkout:

    find /c/Data -name "sl_dlss.h"

All seven names happened to be right. The result enum was *not* what the header's
own doc comment claimed (`sl::ResultCode::eOk` in the comment,
`sl::Result::eOk` in the code), and `UINT32_MAX` would have needed an include
that `PerfMode.cpp` does not have. Two compile failures avoided by looking, in a
file where confidence had been high.

**When the answer genuinely cannot be found** - no header, no documentation, no
prior art in the repository - that is not licence to guess. Say so plainly and
ask Rik. An unanswered question costs one message; a guessed answer costs a build,
a session, and the credibility of every number that follows it.

The same applies to our own history. We document every step precisely so that
nothing has to be recalled: if a decision, a measurement or a configuration is
being asserted, it is in these documents and it can be read.
