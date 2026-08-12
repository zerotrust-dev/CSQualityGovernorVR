# RC3 session 2026-08-12 17:10 — the capture behind E-49's refutation, E-51 and D-24

Plugin built from `192a950`, hash-verified against the CI artifact. MGO 4.0 beta
RC3, Community Shaders build 11, Pimax Crystal Super at Pimax Play image quality
**0.75**. Submitted texture `3494x3558` per eye.

First capture with a `post_submit_us` column, and the first with hold reasons in
the log.

## E-49 is refuted by this capture

Post-submit GPU time is **0.033 ms at UltraPerformance and 0.034 ms at
Performance** — negligible, and flat across presets. The compositor's own submit
is not where the missing milliseconds go, so the OpenComposite-copy hypothesis is
dead.

The gap it was meant to explain is still here: 11.0% of steady frames missed at
Performance against 0.3% at UltraPerformance, with GPU under budget on 73% of the
misses. The remaining candidate is work outside this process entirely — the
runtime composites in its own — which no D3D11 query here can reach.

## E-51 / D-24: the ladder, measured properly

Per-preset mean GPU time from the sweep dwells, in ladder order:

    UltraPerformance  9.70    Quality       13.50    Hoshipa   16.75
    Performance      11.32    UltraQuality  15.64    NativeAA  20.81
    Balanced         12.59

Monotonic in scale, and fits `t = 8.22 + 12.34·f` (f = scale²) to within
**0.39 ms** across all seven.

Note the **summary file's table is frame time**, which is censored at the cap and
comes out non-monotonic — UltraPerformance reads *worse* than Performance there.
Use the per-frame CSV and `gpu_us`. This trap is the reason the summary is a poor
place to read cost from.

Split by sweep, the same data shows why D-18's per-step learning is the noisier
choice, and why E-27's conclusion was about its estimator rather than its model:

| | sweep 1 | sweep 2 |
|---|---|---|
| pairwise slope, per rung | 5.49 … 18.74 | 5.15 … 18.06 |
| global fit (fixed, scaled) | 7.12, 13.13 | 9.23, 11.97 |
| fixed/scaled | 0.54 | 0.77 |

Reproduce the split with:

    awk -F, '!/^#/&&$5=="Dwelling"&&$6>0{
        if($4!=cur){ if(n>50) printf "%s %d %.3f\n", cur, ++seen[cur], s/n/1000;
                     cur=$4; s=0; n=0 }
        s+=$6; n++
    } END{ if(n>50) printf "%s %d %.3f\n", cur, ++seen[cur], s/n/1000 }' \
      20260812_171012_frames.csv | sort -k1n -k2n

`preset_public` here is `0` NativeAA, `1` Quality, `2` Balanced, `3` Performance,
`4` UltraPerformance, `5` Hoshipa, `6` UltraQuality — confirmed against the
timeline's named transitions (`0 -> 5` is NativeAA→Hoshipa, `3 -> 2` is
Performance→Balanced), not assumed from the enum's order.

## Also in here: why the governor would not climb

The player stood still at a reported 23% headroom and nothing happened. The log
now says why, which it could not before E-50:

    hold UltraPerformance | hold: 2.64 ms spare but one rung would land past
    12.89 ms | p95gpu=11.25ms headroom=2.64ms

Two things to know when reading this capture:

- The readout's `headroom %` is computed from **mean** GPU time; the governor
  decides on **P95**. Standing still flatters the mean, so the two disagree by
  more than a rung's worth. Our own readout advertises a number the controller
  does not use.
- No step ratio was ever learned this session — the log only ever shows `1.139x`
  and `1.243x`, which are the shipped seeds. The `UltraPerformance` seed of 1.243
  against a measured 1.167 is what refused the climb, and it is self-confirming:
  the rung can only be learned by climbing it.

## CORRECTION (2026-08-12): the E-51 analysis above is wrong in three ways

Independent review found method errors in the sweep-split analysis on this
capture. All three reproduce. Do not reuse the `awk` recipe above as written.

1. **It does not deduplicate.** 350 of 7028 dwell rows repeat a `gpu_frame`.
   A repeated index means the timer produced nothing new — that is the documented
   `GovernorSample` contract — so 5.0% of the input was stale readings counted as
   fresh measurements. Filter on `$7 != prev` before anything else.

2. **The serpentine turnaround gets pooled.** The sweep runs out and back, so the
   endpoint preset is visited twice *consecutively*. Segmenting by contiguous runs
   of `preset_public` merges both NativeAA dwells into one 17-second block
   (t=128–145 s, against an 8 s dwell) and attributes it entirely to pass 1. The
   headline comparison was therefore seven points against six.

3. **The two passes are ~90–120 s apart, not "ten minutes".** Pass 1 is
   t=70–125 s, pass 2 is t=146–198 s. They are much less independent than claimed.

Corrected numbers, deduplicated and on P95 — the currency the controller actually
decides in:

    P95 fit: t = 9.26 + 15.11·f      worst residual 0.90 ms  (landing margin 1.00 ms)
    fixed/scaled mix across passes:  0.300 -> 0.646  (115% change, not 43%)
    UltraPerformance -> Performance: 1.290 pooled P95  (shipped seed 1.243)

So the seed is mildly **optimistic**, not pessimistic — the opposite of what the
original analysis concluded. The 1.167 figure quoted above is an undeduplicated
mean: the wrong statistic, computed wrongly.

D-24 was rejected on this basis. The per-preset cost table and the `t = A + B·f`
shape remain useful for understanding the ladder; they were not adequate as a
replacement for the controller's per-step table.
