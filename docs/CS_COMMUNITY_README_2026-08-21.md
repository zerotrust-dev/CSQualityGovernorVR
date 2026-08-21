# CS Quality Governor VR, and the Hot-Envelope experiment

**For the Community Shaders community.** Prepared 2026-08-21.

Two things are described here. One works and is in daily use. The other does not
work yet, and the honest account of *why* is probably the more useful half.

Nothing here is a request that anything be adopted.

---

## In short

**What we built and it works.** An external SKSE plugin that changes Community
Shaders' DLSS quality preset *while you play*, so SkyrimVR holds a locked 72 Hz
instead of you picking one preset and hoping it survives the worst scene in the
game. It drives CSX only through ParticleTroned's plugin API.

**What we want next, and have not got.** Render Scale Mode is worth **1.6–2.5 ms
of GPU time** — measured, real image quality. But with it on, every quality
change costs a **96–130 ms** stall, because changing quality changes texture
dimensions. So today you choose: the headroom, or smooth changes. We want both.

**The idea.** A lower quality's render dimensions already *fit inside* the
targets allocated for a higher one. So make the boot quality an **upper bound**
rather than a fixed point: everything at or below it becomes free to select
during play, and only going above it needs a real reallocation.

**What we proved.** It can be done — the boot latch holds across quality
changes, **1 relatch per session against 171**, and the GPU saving survives:
P95 GPU of 12.50 / 11.41 / 10.11 / 6.26 ms down the ladder.

**What is wrong.** The *image* is wrong below the envelope quality. Six attempts
to fix it have failed. The submitted texture is whole and the OpenVR bounds are
correct. **Corrected 2026-08-21:** we had described the symptom as "too close and
flattened" without stating that this is the symptom of **one experimental eye-origin
arm**; the other arm produces cross-eyes instead. See §3.

**What we are doing about it now.** We think CSX has three geometric concepts
and names that distinguish only two, because in every shipped configuration two
of them coincide — ours is the first thing that makes all three differ at once.
So: one pure geometry planner as the single source of truth, then distinct
*types* so the compiler enumerates every consumer including code no play session
executes, then a semantic pass over that exhaustive list.

**And what we already have for you regardless.** Two CSX defects, the DLSS
accepted-input ranges CSX never queries, the five relatch triggers, and a
one-pixel sizing asymmetry between the two VR paths — all in
`FINDINGS-FOR-CS.md`, none of it dependent on our feature being a good idea.

---

## Start here

| file | what it is |
|---|---|
| **`FINDINGS-FOR-CS.md`** | **Read this one.** Things we learned about CSX VR that hold whatever happens to our experiment — two defects, measured DLSS ranges, the five relatch triggers, a sizing asymmetry between the two VR paths |
| **`STATUS.md`** | where we are in the plan right now, what the current phase produced, and what is deliberately unfinished |
| **`REPOS.md`** | the three repositories, which branch is which, how to clone and build, and the preset-numbering trap |
| **`docs/EXPLAINER_THREE_FLOWS_AND_RESEARCH_STRATEGY.md`** | **Non-specialist introduction.** VR upscaling, the three flows, the image defect, the objective research strategy, and why the work matters |
| `docs/CSX_HOT_ENVELOPE_POC.md` | the original proposal to ParticleTroned, its measurements, and its results including the failures |
| `docs/PLAN_GEOMETRY_TYPE_SPLIT.md` | **on hold since 2026-08-21.** How the three geometries were derived, and why the previous six attempts failed. Its finished phases feed the protocol above |
| `docs/PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md` | objective three-flow protocol: constrain and justify the Hot-Envelope contracts, localize the first divergent dependency frontier, then prove causality by controlled intervention |
| **`docs/FINDING_DYNAMIC_RESOLUTION_PASS_REPLACEMENT.md`** | **Newest.** A stock CSX copy box that assumes one stereo layout - invisible in every shipped configuration because the two layouts coincide there. Source-derived, test built, not yet confirmed |
| `docs/CDO_EXECUTION_LOG.md` | the running record of the current strategy: what each phase did, what it cost, what it found, and two parked items |
| `docs/PHASE_1A_SITE_TABLE.md` | every consumer of the render extent and the allocation, enumerated by the compiler and classified: 10 suspects, 43 sites proven off the envelope path |
| `docs/NOTE_MENU_EYE_PATH_SPLIT.md` | the menu crossed-eye defect, in full |
| `docs/CS_PLUGIN_API.md` | how an external plugin drives CSX's upscaler |
| `docs/MEASUREMENT_METHOD.md` | how we measure, and the rules we adopted after getting it wrong |
| `code/` | a patch against `CSX3.18`, an offline source snapshot, and the two new source files |
| `evidence/` | a hashed run log backing the numbers |

---

## 1. The Governor: what it is, and that it works

An external SKSE plugin that changes CSX's DLSS quality preset **during play**,
to hold a locked framerate at the best image quality each scene allows.

The problem it solves: a single preset has to be chosen for the worst scene you
will meet, so most of the time you are running far below what the GPU could
deliver. A governor spends that headroom instead of leaving it on the floor.

It reads GPU time, compares it against the frame budget, and moves up or down the
preset ladder. It drives CSX entirely through the plugin API ParticleTroned
added — `GetVRUpscalingTransitionProfileDecision`, then
`SetVRUpscalingTransitionProfileForMethod`. The preflight in particular saved a
great deal of guesswork.

What that is worth, measured over a sweep with Render Scale off: a
**time-weighted pixel fraction of 0.216** while holding the frame budget, with
frames beyond two display periods at **0.03%**. With Render Scale on the pixel
fraction rises to **0.320** — which is the prize section 2 is about, and the
reason we are not content with the working half.

This part is finished and in daily use on a Pimax Crystal Super at 72 Hz. Its
controller logic builds and tests in about a minute on Linux or Windows with no
Skyrim SDK at all (`REPOS.md` §3).

## 2. Hot-Envelope: what we want, and why

With Render Scale Mode **on**, CSX replaces the runtime's recommended
render-target size with `HMD size x quality scale`, so Skyrim allocates physical
targets at the boot quality's resolution. That is worth **1.6–2.5 ms** of GPU
headroom (measured, `FINDINGS-FOR-CS.md` §2.1) — real image quality, not noise.

But because a quality change then changes texture *dimensions*, every change
costs a **96–130 ms** relatch: `RecreateRenderTargetsForVRRenderScale`,
`globals::ReInit()`, and a rebuild of every render-target-dependent feature.

For a governor changing quality several times a minute, the two configurations
are each half of what we want:

| | Render Scale on | Render Scale off |
|---|---|---|
| time-weighted pixel fraction | **0.320** | 0.216 |
| frames beyond two display periods | 1.35% | **0.03%** |
| worst frame within 0.5 s of a change | 59 ms | **34 ms** |

85% of those severe frames sit within 0.7 s of a preset change.

**The observation we put to ParticleTroned** is narrow:

> When the new quality is **lower** than the boot quality, its render dimensions
> **fit inside the targets that are already allocated.** Nothing needs
> reallocating; only the logical extent rendered into them needs to change.

That would make the boot quality an **upper bound** rather than a fixed point.
Everything at or below it becomes selectable during play; anything above it still
relatches and can wait for a loading screen.

## 3. Where it actually stands: not working

**The relatch part works.** With the five triggers guarded, the boot latch holds
— 1 latch per session against 171 — and the GPU saving is real: P95 GPU of
12.50 / 11.41 / 10.11 / 6.26 ms across the ladder, at least as good as the
boot-latched equivalent.

**The image is wrong**, and on 2026-08-21 we found that we had been describing it
imprecisely. Corrected here rather than quietly edited, because the imprecision
was in the direction of making the problem sound simpler than it is.

There is a setting, `vrHotEnvelopeEyeOrigin`, that chooses where eye 1 begins
inside the allocation. It has **no neutral value** — only "packed" (directly after
eye 0's rendered region), "allocation half" (the physical half boundary), or a
manual pixel. It defaults to the allocation half, so every session has one of the
two conventions active. The two fail **differently**:

| eye-origin arm | symptom below the envelope quality |
|---|---|
| allocation half (**the default**, and what we have been reporting) | the world looks too close and flattened into layers |
| packed | cross-eyed |

So the accurate statement is *"under the allocation-half origin, the world looks
too close and flattened"* — not *"Hot-Envelope looks too close and flattened."*
Our earlier wording collapsed those, and collapsed "both arms failed" into a
single outcome when in fact they failed in two different ways. That difference is
evidence, and we lost it for three days.

Six attempts to fix it have failed, each locally correct and globally wrong.

What we have established, mostly by ruling things out:

- the submitted eye texture is **whole**, not cropped — active across its full
  width, and the OpenVR output bounds are correct;
- the defect is **monocular** — with either eye closed the image is equally
  wrong, which rules out every stereo-relationship explanation we had;
- it is correct at exactly one preset: the envelope quality, where allocation and
  render extent are the same number — which is also the one preset where the two
  eye-origin arms resolve to the same pixel, so no run so far distinguishes them;
- the eye origin is **not settled**. We previously wrote that it was "not the
  cause" because both conventions were built and both failed. That is weaker than
  it sounded: they failed differently, neither was tested against anything but a
  visual judgement, and the code that computes them resolves the same normalized
  OpenVR bound against two different extents — a physical resource width in one
  case, a logical field width in the other. Those agree only when the rendered
  field fills the resource, which is true in both shipped flows and false here.

The last point is documented in full in
`docs/FINDING_DYNAMIC_RESOLUTION_PASS_REPLACEMENT.md` §12.

## 4. Why we think it is a naming problem

CSX has three geometric concepts and, in the resolution plan, names that only
distinguish two — because in every shipped configuration two of the three
coincide:

| configuration | allocation | render extent | output |
|---|---|---|---|
| Render Scale **off** | = display | ratio x display | display |
| Render Scale **on** | quality-sized | **= allocation** | display |
| **Hot-Envelope** | boot-quality-sized | **< allocation** | display |

In both shipped modes you can say "the render size" and be unambiguous.
Hot-Envelope is the first configuration in which all three differ at once, so
every consumer that asks the old question gets an answer that used to be right.

That is not a criticism of CSX. The conflation is invisible and harmless in
every configuration that ships. Our feature is the first thing to make it
matter — which is also why this may turn out to be a change only the author can
sensibly make, and we would regard that as a legitimate outcome.

**What we are doing about it** (`docs/PLAN_COMPOSITIONAL_DIFFERENTIAL_ORACLE.md`,
which replaced `PLAN_GEOMETRY_TYPE_SPLIT.md` as the active strategy on
2026-08-21 after a meeting with members of this community):

1. one pure, dependency-free geometry planner, derived from source and verified
   by compile-time tests across all three flows, the full 7x7 boot x active
   matrix, and seven display sizes. **Done.**
2. distinct types for the three geometries, so the *compiler* enumerates every
   consumer, including code no play session ever executes. **Done** - 116 sites
   across 32 consumers, classified in `docs/PHASE_1A_SITE_TABLE.md`.
3. then, instead of a semantic pass over that list, **record the coordinate
   transform at every pipeline boundary and find the earliest edge whose
   measured mapping contradicts a contract written down beforehand** - and only
   then change one variable, in a counterbalanced A/B, to test causality.

Step 3 is the change of direction. A semantic pass over a list of suspects would
have been a seventh guess, better informed than the previous six but still a
guess. The protocol replaces that with a finite pipeline bisect whose failure
modes are named outcomes: `INCONCLUSIVE`, `DIAGNOSTIC_INTERFERENCE`,
`MISSING_BOUNDARY`, `OPAQUE_TEMPORAL_STATE`. "We do not know" is a reportable
result rather than a prompt to try something else.

If it finds no wrong consumer, we will have established with much better
coverage than any play session that the defect lies elsewhere — in resource
lifecycle, vendor state, or stereo submission. We would consider that worth the
same effort as a fix, and we would say so.

## 5. How we try to work

Because it is relevant to how much weight to give any of this:

- **Corrections are recorded, not quietly fixed.** Several are in these
  documents, including a headline finding we had to withdraw and a measurement
  we got backwards.
- **Pre-registration.** Test criteria are written before the run, so a result
  cannot be chosen afterwards.
- **Independent review.** A second agent reviews findings and plans and has
  caught real errors in every revision.
- **We do not report what we have not measured.** Where something is inferred
  rather than observed, it says so.

## 6. The code

**Public branch** — the authoritative copy, with full history and CI:

```
https://github.com/zerotrust-dev/skyrim-community-shaders
branch csx318-hot-envelope-diag
```

`code/hot-envelope-vs-CSX3.18.patch` — every change as a readable diff, roughly
1,900 lines across 8 files, against tag `CSX3.18` (`2051e2ae`, CSX 3.18-VR,
build 11). Apply to a clean checkout of that tag.

`code/hot-envelope-source-snapshot.zip` — the whole source tree at the branch
tip, for reading offline. No git history: our working clone is shallow, so a
self-contained bundle was not possible. Use the GitHub branch if you want the
commit-by-commit story; `code/commits.txt` lists it.

`code/VRGeometryPolicy.h` and `code/vr_geometry_policy_test.cpp` — the pure
planner and its compile-time test, the most self-contained and probably most
interesting part to read.

**Please treat the branch as evidence, not as a patch.** It contains
diagnostics, characterization tests that assert current behaviour including known
contract violations, and one experimental setting we would not ship. It is
public so the findings are checkable, not because it is ready.

## 7. Environment

Pimax Crystal Super at 3494 x 3558 per eye, 72.000 Hz, RTX 5090, DLSS,
OpenComposite Unleashed 4.2.3, MGO 4.0 beta RC3, CSX 3.18-VR build 11.

One machine, one headset. The size of the prize will differ elsewhere, and on a
GPU where fixed costs dominate it may differ a lot.
