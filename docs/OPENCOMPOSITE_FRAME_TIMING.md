# What `IVRCompositor::GetFrameTiming` actually returns under OpenComposite

**Source of truth for every `ft_*` column in our captures.** Read this before
interpreting one, and re-verify it whenever the mod list ships a new
OpenComposite — the answers below are specific to one build and have already
changed once.

Evidence: **E-53** (counters inert), **E-55** (what the live fields contain).

---

## 1. Which build this describes

| | |
|---|---|
| Mod list | MGO 4.0 beta RC3 |
| Mod | OpenComposite Unleashed for Skyrim VR, Nexus **171182** |
| Version | **4.2.3**, archive dated **2026-08-07T22-24Z** |
| Source | `github.com/Wondernuttz/Open-Composite-Unleashed-for-Skyrim-VR` |
| Branch | **`Unstable`** — tip 2026-08-07, matching the archive date |
| File | `OpenCompositeSkyrimVR/DrvOpenXR/XrBackend.cpp` |

### Do not read `master`

`master` was last committed **2026-02-22**, six months stale, and uses a
different directory name (`OpenCompositeUnleashedSkyrimVR/`). Its
`GetFrameTiming` hardcodes **every** field:

```cpp
// master - NOT what ships
pTiming->m_flPreSubmitGpuMs = 8.0f;
pTiming->m_flCompositorRenderGpuMs = 1.5f;   // "very conservative guesses"
pTiming->m_flClientFrameIntervalMs = 11.1f;
```

Reasoning from `master` produces two wrong conclusions at once: that these
fields are constants, and that the ones which vary must therefore be something
else. Both were briefly believed here. **Match the branch tip date against the
installed archive date before trusting any of it.**

---

## 2. Field by field, as shipped

Ordered by what the field is worth to us, not by struct order.

| Field | What it really is | Usable? |
|---|---|---|
| `m_nFrameIndex` | real, monotonic | **yes** — freshness/dedup |
| `m_flPreSubmitGpuMs` | OpenComposite's **own D3D11 timestamp queries** of app GPU time. Fallback `displayPeriod × 0.7` | **yes** — independent cross-check on D-21 |
| `m_flClientFrameIntervalMs` | measured frame interval; fallback OpenXR `predictedDisplayPeriod` | **yes** — true display period, unquantised |
| `m_flCompositorRenderGpuMs` | **a residual, not a measurement** — see below | **no** — not a cost |
| `m_nNumDroppedFrames` | real only when `ovrPerf.available` (**Oculus** API) → always 0 on Pimax/OpenXR | **no** |
| `m_nNumFramePresents` | literal `1` | **no** |
| `m_nNumMisPresented` | literal `0` | **no** |
| `m_nReprojectionFlags` | ASW flag, but only when `ovrPerf.available` → 0 here | **no** |

### The trap: `m_flCompositorRenderGpuMs`

The name says compositor GPU cost. The code says otherwise:

```cpp
// Compositor overhead: residual = frame_interval - app_gpu - app_cpu - xrEndFrame_cpu
float residual = frameInterval - measuredCpuFrameMs - appGpu - measuredEndFrameMs;
compositorOverheadMs = clamp(residual, 0, frameInterval);
...
pTiming->m_flCompositorRenderGpuMs =
    compositorOverheadMs > 0.0f ? compositorOverheadMs : displayPeriod * 0.1f;
```

That is **leftover frame time with idle included**. It is *large when the
application was fast*, which is the opposite of a cost, and it explains the
readings that first looked promising:

- `10.51 ms` against a `12.74 ms` interval — a fast frame, lots of residual
- `1.39 ms` against a `26.99 ms` interval — that is the other branch,
  `displayPeriod × 0.1` = **1.389 ms** at 72 Hz

This killed the E-49 hypothesis that hidden compositor GPU work explained late
frames. It does not. **E-49 remains open.**

### Why the delivery counters are structurally dead

`m_nNumDroppedFrames` and `m_nReprojectionFlags` are populated from
`GetOVRPerfData()` — the **Oculus** performance hook. On a Pimax headset through
OpenXR that returns unavailable, so those fields are zero by construction, not by
oversight. Core OpenXR exposes no portable equivalent, so no downstream patch can
supply them. This is E-53's mechanism, and it is why our delivery signal is the
application's own frame interval.

---

## 3. Consequences for this plugin

- **`ft_compositor_gpu_us` is recorded to confirm the residual shape, not as a
  cost.** Expect it high on fast frames. If it ever correlates positively with
  late frames, something changed and this document is stale.
- **`ft_presubmit_gpu_us` is a second opinion on D-21.** Two independent D3D11
  timers watching one GPU; disagreement between them is more informative than
  either alone.
- **`ft_interval_us` is the best available refresh source.** Our own frame timing
  is quantised to 1/6 ms and cannot separate 72 Hz from 71.4 Hz; this can. It is
  the input `PROPOSED_SAFE_LADDER_GOVERNOR.md` §4.2 asks for.
- **Never call a late application interval a headset miss.** Nothing here
  reports what the headset scanned out.

## 4. Re-verifying on a new RC

Belongs in the RC checklist (`RC_PORTING_PLAN.md`), because a new mod list can
ship a different OpenComposite without anything in our logs changing:

1. Read the installed version from MO2:
   `mods/<OC mod>/meta.ini` → `version` and `installationFile` (the archive name
   carries the release date).
2. Find the branch whose tip date matches that archive date. Check **branches**,
   not just `master` — the shipping branch here was `Unstable`.
3. Re-read `XrBackend::GetFrameTiming` and update section 2.
4. If `ovrPerf.available` ever becomes true on this hardware, the delivery
   counters become real and the whole delivery-telemetry design in
   `PROPOSED_SAFE_LADDER_GOVERNOR.md` §5.2 comes back into play.

## 5. Community Shaders has no prior art

Checked at the same time, recorded so nobody repeats it: CS contains **no
performance-driven quality logic at all**. It *disables* Skyrim's built-in
dynamic resolution and reuses that plumbing to hand DLSS a fixed per-preset
scale. Its `Upscaling::GetRefreshRate` uses the Windows CCD API
(`QueryDisplayConfig`) for an exact rational refresh rate — a good technique, but
it reads the **desktop monitor**, not the headset, so it cannot serve a VR frame
budget.
