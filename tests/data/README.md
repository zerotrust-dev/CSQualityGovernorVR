# Captured sessions

Traces committed so the parameter fit is reproducible. CI replays them on every
build and publishes the report as an artifact, which is what stops a threshold
from becoming a number somebody quoted once.

## `session-20260808.csv`

Skyrim VR, 2026-08-08 12:19, Pimax Crystal Super at 72 Hz, RTX 5090, MGO 4.0
beta. Community Shaders fork `33d552e0` — the first build with the D-13b start
boundary, so its GPU times agree with an external reference to within 0.3 ms at
every load (E-22).

Contents: the cycler sweep (four passes over all seven presets) followed by free
play. 22 000 rows, **deduplicated by `gpu_frame`** — a published GPU time
repeats until the next query completes, and keeping the repeats would weight a
measurement by how long it stayed published (Rule 8 in
`docs/MEASUREMENT_METHOD.md`). `wall_ms` is zeroed; the join against external
logs was already done and the absolute clock is not needed for replay.

**What it can support:** the cost-model fit, the fixed-preset baselines, the
perfect-foresight ceiling, and a ranking of parameter sets by the objective.

**What it cannot:** a threshold tuned for scenes near the cap. 71% of its frames
have at least 30% headroom, and the replay report prints a warning about exactly
this. A session with sustained marginal load is still needed before the climb
and descend thresholds mean much.

## Adding a session

Deduplicate by `gpu_frame` and keep the columns as-is; the parser reads by
header name, so column order does not matter. Note in this file what the session
covers and, more usefully, what it does not.
