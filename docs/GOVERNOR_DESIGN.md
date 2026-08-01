# Governor Design

Status: **specification, not implemented.** Numbers below are starting points to
be replaced by Phase 1 measurements.

## What This Controls

One lever: the Community Shaders **upscale preset**, via
`ICSInterface001::SetUpscalePreset`.

Deliberately **one** lever. The CS API also exposes SSGI, screen-space shadows,
volumetric lighting and contact-shadow toggles, and those are tempting as a
second axis. They should stay out of the first version — two levers square the
state space and multiply the oscillation modes. Get one stable first.

## What The Governor Must Not Do

- **Not duplicate CS's anti-oscillation.** CS already has a rapid-transition
  guard, memory-relief arming, submit-stage resume cooldowns and stable-frame
  counting. A second layer of hysteresis on the same lever is the "two owners,
  one lever" failure that has bitten this project repeatedly. The governor must
  pace *slower* than CS's guards, not fight them.
- **Not toggle `renderScaleModeEnabled`.** That forces a render-target relatch.
  Static configuration only.
- **Not act while blocked.** Check `IsVRUpscalingProfileApplyAllowed()` first.
- **Not degrade when degrading will not help.** See CPU-bound guard below.

## Signal

Frametime, smoothed over roughly one second, evaluated at a **high percentile**
(P95). Never the mean — a locked framerate is lost at the tail, and an average
will hide exactly the misses that matter.

## Policy

Starting thresholds at 72 Hz (13.89 ms budget):

```
step DOWN (lower quality)  when  P95 frametime > 13.0 ms
step UP   (raise quality)  when  P95 frametime < 11.0 ms, sustained
```

The gap between the two is the deadband that stops hunting.

### Asymmetric timing — the most important rule

```
step down   fast    ~0.5 s of evidence
step up     slow    10–20 s of sustained headroom
cooldown    >= 3 s after any change, longer after a step up
```

These are not symmetric problems. Dropping quality quickly prevents judder;
raising it quickly causes visible pumping. Do not give them the same constants.

### Prefer a narrow band

CS keeps compatible inactive DLSS/FSR resources resident when relatching at the
same dimensions, so oscillating between two **adjacent** rungs is far cheaper
than roaming the whole ladder. Policy should favour small moves and resist large
jumps except on a clear cliff.

### Remember what failed

If rung *n* has forced a step-down twice within the last minute, become reluctant
to return to it — require a longer sustained-headroom period, or cap the ceiling
below it temporarily. This kills the sawtooth where the governor keeps
rediscovering the same failure.

### CPU-bound guard

If a step down does **not** improve frametime, step back up and stop descending.

In CPU-bound moments — dense towns, script storms — reducing render resolution
buys nothing, and a naive governor will strip image quality for no gain in
exactly the situations where it is powerless. A few lines, and it prevents the
worst failure mode.

## Applying A Change

```
1. policy decides a target preset
2. if GetVRUpscalingApplyBlockReasons() != 0:
       buffer the target as "latest desired", do not retry-spam, return
3. SetUpscalePreset(target)          // PUBLIC enum, see CS_PLUGIN_API.md
4. start cooldown
5. on the next tick, reconcile: if GetUpscalePreset() != target and no longer
   blocked, re-apply
```

Step 2 is prescribed by the API itself: *"Non-zero block reasons mean the caller
should buffer its latest desired profile and try again later."*

Block reasons to expect: `kLoadingMenu`, `kRelatchPending`,
`kTransitionPending`, `kRaceSexMenu`, `kRaceSexStartupTail`,
`kOpenCompositeUpscaling`.

`kOpenCompositeUpscaling` is terminal for the session — if OCU DLSS is enabled,
CS upscaling is blocked entirely and the governor should disable itself with a
clear log line rather than retrying forever.

## Event-Driven Pre-emption — Later

A reactive governor always eats the first second of stutter on entering a heavy
area. Being an SKSE plugin, this one can see cell transitions, interior/exterior
changes, and weather changes, and could step down *before* the frames drop.

Genuinely valuable — most of the felt benefit is at transitions, not in steady
state — but it is a Phase 3 refinement. Reactive control has to work first.

## MCM Surface

| Setting | Purpose |
|---|---|
| Enabled | master switch |
| Target frametime / refresh | 72 Hz default; must match the headset |
| Min preset / Max preset | clamp the ladder range |
| Step-down threshold | ms |
| Step-up threshold | ms |
| Step-up delay | seconds |
| Cooldown | seconds |
| Show current preset | read-only status |
| CPU-bound guard | on/off |

Defaults should be conservative. A governor that is invisible and slightly
suboptimal is far better than one that pumps.

## Open Questions For Phase 1

1. **Transition latency** — call to visible effect. Everything above assumes it
   is well under a second. If it is not, the design changes.
2. **Visual disruption** — is a preset change perceptible when it happens? DLSS
   history resets on a resolution change, so expect a brief softening.
3. **Block frequency** — how often is an apply actually refused in normal play?
4. **Which CS build ships with MGO4**, and whether it exposes API revision 3.
   `getBuildNumber()` answers this; it should be the first call made.
5. **Is `SetUpscalePreset` sufficient on the VR path**, or is
   `SetVRUpscalingTransitionProfile` required?
