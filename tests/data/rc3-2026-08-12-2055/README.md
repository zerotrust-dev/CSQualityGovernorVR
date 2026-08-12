# RC3 session 2026-08-12 20:55 — the capability test (E-53) and the climb deadlock (E-54)

Plugin built from `9c976f3`, hash-verified against the CI artifact. MGO 4.0 beta
RC3, Community Shaders build 11, Pimax Crystal Super at Pimax Play image quality
0.75. First capture with `ft_*` delivery columns.

**Deliberately not a normal session.** The player stood still throughout, let both
calibration sweeps complete, then drove the preset by hand from the Community
Shaders menu — first to NativeAA, letting the governor walk down; then to
UltraPerformance, waiting to see whether it would climb back. It did not.

## E-53 — the compositor's delivery counters are not populated

    calls 18663 | returned true 18663 (100.0%) | frame index advancing YES (100.0%)
    fields ever varied: presents NO | dropped NO | misPresented NO
                      | reprojection NO | clientInterval yes

Against **5478 late frames (31%)**, 121 over two display periods, individual
frames at 35 ms. Reproduce:

    awk -F, '!/^#/&&NR>1&&$3+0>0{
        n++; if($3>14.5) late++; if($11+0>0) drop++; if($10+0>1) extra++
    } END{printf "frames %d | late %d (%.1f%%) | ft_dropped>0 %d | ft_presents>1 %d\n",
        n, late, 100*late/n, drop, extra}' 20260812_205541_frames.csv

Columns are `ft_fresh` 9, `ft_presents` 10, `ft_dropped` 11, `ft_mispresented` 12,
`ft_reproj` 13.

The entry point is real — every call answers, the frame index is live, and two
fields the runtime can compute for itself do vary. The four delivery counters are
inert. **The cause is structural:** core OpenXR exposes no portable result for
which image was scanned out, so OpenComposite has nothing to fill them from. Do
not re-test this hoping for a different answer on the same stack; re-test only if
the runtime, OpenComposite build, or a vendor extension changes.

## E-54 — UltraPerformance is a rung with no exit

Two gates in series, alternating in the log:

    hold: p95 GPU 11.39 ms inside the band [11.39, 13.89]
    hold: 2.52 ms spare but one rung would land past 12.89 ms

P95 GPU sits at 11.20–11.57 ms against a climb threshold of 11.389. Above it, the
band holds; below it, the landing check computes `11.2 × 1.243 = 13.93` against a
12.89 ms limit and refuses. Escape needs P95 GPU ≤ 10.37 ms, which this scene
never reaches.

This capture is the clean demonstration of the self-confirming seed: the rung can
only be learned by climbing it, and the seed forbids the climb. Standing still
does not help — it is not a matter of conditions.

## A measurement note that affects thresholds

Frame times are quantised to 1/6 ms steps: the modes are 14.0000 (53%) and
13.8333 (47%), straddling a true 72 Hz period, so the display is ~72 Hz and the
14.00 readings are our measurement grid rather than a slow headset.

Consequence: any miss threshold has an effective resolution of **0.1667 ms**.
D-25's `budget × 1.05` = 14.58 falls between grid points, so the operative
threshold is 14.6667 and tuning below that granularity changes nothing.

`m_flClientFrameIntervalMs` — the runtime's own period measurement, and a better
basis for deriving refresh than our quantised timing — was logged only in the
periodic summary, not per-frame. Add it to the CSV next time.
