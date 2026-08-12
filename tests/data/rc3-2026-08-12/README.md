# RC3 session 2026-08-12 12:14 — first session with D-20 live

Kept because it is the evidence behind **E-49** and **E-50**, and the first
capture of D-20 actually blocking a climb.

## What it is

MGO 4.0 beta RC3, Community Shaders build 11, Pimax Crystal Super at Pimax Play
image quality **0.75** (recorded here because the RC2 captures did not record it
and the question could no longer be answered — see below). Plugin built from
`ce701ac`, hash-verified against the CI artifact before the run.

Submitted texture, from `session.log`: `3494x3558` per eye, `6988x3558` once
Community Shaders latches its render scale. That pair of numbers is the durable
proxy for the whole resolution chain — headset, runtime and render scale
together — and is the reason to keep the log next to the CSVs.

## What it shows

**D-20 works.** Sixteen blocks, all on the same rung:

    hold: 3.19 ms spare but Performance failed at 3.52 ms spare and needs 4.02

Against the immediately preceding session on the pre-D-20 build
(`20260812_113152`, same evening, same mod list):

| | pre-D-20 | with D-20 |
|---|---|---|
| governed | 333 s | 261 s |
| applied changes | 19 | 11 |
| changes/min | 3.42 | **2.53** |
| failed climb -> next attempt | ~20 s | **136 s** |

Different route and different scenery, so this is suggestive rather than a
controlled comparison. The 136 s gap is the informative number: it is just past
`climbFailForgetSeconds` (120 s), meaning the rung stayed shut until the memory
expired rather than until the scene improved. Worth watching — if that is the
usual shape, the forget timer is doing the work the headroom margin was supposed
to do.

**And it may be treating a symptom.** See E-49: on 92% of missed frames the GPU
timer read *under* budget, and the miss rate scales with preset while
GPU-over-budget does not. Reproduce with:

    awk -F, '!/^#/&&$5=="Monitoring"&&$6>0{
        if($4!=p){p=$4;chg=$2}
        if($2-chg>3.0&&$3>14.5){n[$4]++; if($6<13889) under[$4]++}
    } END{for(k in n) if(n[k]>40)
        printf "preset %s: %d missed | GPU under budget on %.1f%%\n",
        k, n[k], 100*under[k]/n[k]}' 20260812_121451_frames.csv

`preset_public` 4 is UltraPerformance and 3 is Performance — confirmed by
correlating the trace against the preset names in `session.log`, not assumed.
Community Shaders' public enum is not our ladder order (its probe reports 6 for
UltraQuality), so do not index it as one.

## Why the log is archived too

E-50: hold decisions are written to `timeline.csv` and not to the log, so the
sixteen blocks above appear nowhere in `session.log`. Until that is fixed, the
CSV is the only record of what the controller decided *not* to do, and the log
is only useful here for the texture dimensions and the API probe.
