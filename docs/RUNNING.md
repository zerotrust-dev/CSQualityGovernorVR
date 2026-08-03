# Running The Cycler

The point is that **one game session produces a complete dataset**. Arm it, walk
to your test spot, stand still, and read the CSV afterwards. No watching the
screen, no menu, no repeated launches.

## Install

From the CI artifact `CSQualityGovernorVR-plugin`:

```
SKSE/Plugins/CSQualityGovernorVR.dll
SKSE/Plugins/CSQualityGovernorVR.ini
```

Install as a mod in MO2 alongside the rest of the list, not loose in the game
folder.

## Preconditions — check these first

| Requirement | Why |
|---|---|
| **OCU DLSS off** | It blocks CS upscaling entirely. Every apply will be refused with `OpenCompositeUpscaling` and the run aborts. |
| **CS Upscaling on**, method DLSS | The preset is the lever being measured. |
| **CS menu closed** | The overlay costs 1.5–2 ms while open and corrupts every number. This is the whole reason the cycler exists. |
| **Nothing else changing quality** | PrimaShock VRS off, Pimax Center Rendering fixed. One owner per lever. |

Set `TargetHz` in the INI to your actual refresh rate. Get it wrong and every
miss-rate figure is meaningless.

## First run: a two-minute smoke test

Before committing to a full sweep, prove the plumbing. In the INI:

```ini
Sweeps = 1
DwellSeconds = 4
StartDelaySeconds = 10
SettleTimeoutSeconds = 4
```

Load a save, stand still, wait. Then check the SKSE log for:

```
acquired Community Shaders interface, build NNNN
armed: 1 sweep(s) x 7 preset(s), ...
```

**If the interface line is missing**, nothing else will work — the plugin could
not reach Community Shaders, or the installed build does not expose API
revision 3.

**Then check `readback=` in the log lines.** `readback=MISMATCH` means
`SetUpscalePreset` was accepted but the value did not stick, which is the single
most important thing a first run can tell you.

## Full run

Restore the defaults (`Sweeps = 3`, `DwellSeconds = 12`) and expect roughly
**seven to eight minutes** of unattended sweeping after the start delay.

Pick the spot deliberately. Everything measured so far came from a quiet
hillside, and a locked framerate is bound by its worst moment — a busy town,
heavy combat, rain. **The scene you choose is the result you get.**

## Output

```
Documents\My Games\Skyrim VR\SKSE\CSQualityGovernorVR\
    YYYYMMDD_HHMMSS_transitions.csv
    YYYYMMDD_HHMMSS_summary.txt
    YYYYMMDD_HHMMSS_frames.csv      (only if WritePerFrameCsv = 1)
```

`_transitions.csv` is flushed as each preset completes, so a crash costs only
the visit in progress.

### Reading the summary

```
preset            scale  visits  mean_ms   p95_ms  worst_p95   miss%  settle_s ...
```

- **p95 and worst_p95 matter more than mean.** A locked framerate is lost at the
  tail, not on average.
- **miss%** is the share of frames over budget — the number that predicts
  stutter.
- **settle_s** is API call to stable frametime. This is the figure that decides
  whether a governor is viable at all: if a change takes seconds to settle, a
  controller cannot chase the scene.
- **rbfail** non-zero means the API is not doing what it claims.
- **timeouts** non-zero means frametime never stabilised at that preset — itself
  a result.

### Multiple visits are the point

Each preset is visited once per sweep. Three sweeps means three independent
measurements of the same setting, which is what separates a real difference from
run-to-run noise. A monotonic ordering across presets is the cheapest validation
available — noise does not produce clean orderings.

## Troubleshooting

**Run aborts immediately with a terminal block.** OCU DLSS is on. That gate
cannot be retried past.

**Frames counted look far too low.** The log reports
`frame source: N frames from M polls` at the end. The frame source samples the
game's frame delta and treats each change as a frame, which can undercount if
consecutive deltas come back bit-identical. If N is implausible relative to run
time, the frametime figures are suspect and the source needs replacing with a
swapchain hook. Nothing else depends on where frametimes come from.

**Applies are refused repeatedly.** Check `block_worst` in the CSV.
`LoadingMenu` and `RelatchPending` during normal play are expected and handled;
persistent blocking of everything is not.

**Nothing happens at all.** `AutoStart = 0` in the INI waits for the hotkey
(default F12, `Hotkey = 0x58`).
