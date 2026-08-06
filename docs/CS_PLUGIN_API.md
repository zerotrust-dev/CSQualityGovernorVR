# Community Shaders Plugin API — Reference For The Governor

Author: Claude
Date: 2026-07-31
Source: `include/VRAPI/CSinterface001.h` in
`github.com/ParticleTroned/skyrim-community-shaders` (branch `cs-1.7-PL-VR`,
GPL-3.0). This is the fork installed by MGO 4.0 as
*Community Shaders - Particle Lights Fork*.

**This is the file the governor is built against.** For everything the governor
*actuates*, no fork of CS is needed and no upstream contribution is required —
CS publishes a versioned, ABI-stable inter-plugin API for exactly this.

**One exception, added 2026-08-06: revision 4.** The API has no way to report GPU
time, and nothing outside CS can measure it honestly (design doc D-11, D-13), so
revision 4 — `GetLastFrameGpuTimeUs()` and `GetLastFrameGpuTimeFrameIndex()` —
is *our own* extension, carried in
`zerotrust-dev/skyrim-community-shaders`, branch `feat/expose-gpu-time-pl3.15`,
and destined for upstream. It exists only in builds from that fork.

Consequences for anyone reading this code:

- Revision 4 methods **must** be gated on both the acquired revision and
  `getBuildNumber() >= 9`, via `GpuTimingAvailable()`. A stock provider's vtable
  ends before those slots; calling one is not a graceful failure.
- The governor must work with no GPU time at all (D-10). The frametime tier is
  not a stub — it is what runs against stock Community Shaders.

## Acquiring The Interface

```cpp
namespace CSPluginAPI {
    constexpr const auto CSPluginName = "CommunityShaders";
    inline constexpr uint32_t CSInterfaceMessageType = 0x43534150;  // "CSAP"
    inline constexpr unsigned int CSInterfaceRevision = 4;          // 4 = our fork; 3 = stock

    struct CSMessage {
        enum : uint32_t { kMessage_GetInterface = CSInterfaceMessageType };
        void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
    };
}
extern CSPluginAPI::ICSInterface001* g_CSInterface;
```

> "Returns an ICSInterface001 object compatible with the API shown below. This
> should only be called after SKSE sends `kMessage_PostLoad` to your plugin."

ABI contract: *"keep virtual methods append-only. Inserting new virtuals before
existing entries changes vtable slots for already-compiled consumers."* Revisions
1, 2 and 3 exist upstream, and revision 4 exists in our fork; request the newest
and negotiate downwards, which is what `main.cpp` does — asking only for the
newest and giving up would report "CS unavailable" on a provider that would have
answered revision 3 perfectly well.

## THE ENUM TABLE — Read This Before Writing Any Value

**There are two different numberings in the codebase and they do not match.**
Confusing them silently selects the wrong preset.

| Preset | Scale | **Public API** `UpscalePreset` | Internal `qualityMode` |
|---|---:|---:|---:|
| Native AA / DLAA | 1.000 | **0** | 0 |
| Quality | 0.667 | **1** | 3 |
| Balanced | 0.588 | **2** | 4 |
| Performance | 0.500 | **3** | 5 |
| Ultra Performance | 0.333 | **4** | 6 |
| Hoshipa | 0.850 | **5** | 1 |
| Ultra Quality | 0.769 | **6** | 2 |

The public enum looks scrambled because of an explicit compatibility promise —
*"Values 0-4 are kept stable for existing compiled API users"* — so Hoshipa and
Ultra Quality were appended at 5 and 6 rather than inserted in scale order.

**Use the public numbering when calling the API. Use the internal numbering only
when reading CS source or devbench documentation.**

Scales come from `Upscaling.h`:

```cpp
static constexpr float GetQualityModeResolutionScale(uint32_t a_qualityMode)
{   // internal numbering
    case 1: return 0.85f;         case 2: return 1.0f / 1.3f;
    case 3: return 1.0f / 1.5f;   case 4: return 1.0f / 1.7f;
    case 5: return 0.5f;          case 6: return 1.0f / 3.0f;
    default: return 1.0f;
}
```

### Correction to an earlier note

`VR_FPS_STABILIZER_RESEARCH.md` flagged the prototype INI comment
`CS>DLSSMode = 5   #Use DLSS Mode 0.85` as possibly stale, because internal
`qualityMode 5` is Performance.

**The comment was correct.** `CS>` lines use the **public** enum, where
`5 = kHoshipa = 0.85`. Likewise `CS>DLSSProfile = 1` is `kK` and `= 3` is `kM`.
No discrepancy — two enums, and that line used the right one.

### DLSS profiles

```cpp
enum class DLSSProfile : uint32_t { kJ = 0, kK = 1, kL = 2, kM = 3, kF = 4 };
```

Note the public enum omits `E`, which exists internally as index 5.

## The Governor's Control Surface

```cpp
struct ICSInterface001
{
    virtual unsigned int getBuildNumber() = 0;

    // Feature toggles
    virtual bool GetSSSEnabled() = 0;                    // Screen Space Shadows
    virtual void SetSSSEnabled(bool) = 0;
    virtual bool GetSSGIEnabled() = 0;
    virtual void SetSSGIEnabled(bool) = 0;
    virtual bool GetVolumetricLightingExteriorEnabled() = 0;
    virtual void SetVolumetricLightingExteriorEnabled(bool) = 0;
    virtual bool GetLightLimitFixContactShadowsEnabled() = 0;
    virtual void SetLightLimitFixContactShadowsEnabled(bool) = 0;

    // The primary lever
    virtual UpscalePreset GetUpscalePreset() = 0;
    virtual void          SetUpscalePreset(UpscalePreset) = 0;

    virtual DLSSProfile GetDLSSProfile() = 0;
    virtual void        SetDLSSProfile(DLSSProfile) = 0;

    // VR render-scale mode (legacy names)
    virtual bool GetRenderAtUpscaleResEnabled() = 0;
    virtual void SetRenderAtUpscaleResEnabled(bool) = 0;
    virtual bool GetRenderAtUpscaleResActive() = 0;

    virtual void SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled,
                                                 UpscalePreset, DLSSProfile) = 0;

    // Revision 2 — explicit method control
    virtual UpscaleMethod GetUpscaleMethod() = 0;
    virtual void          SetUpscaleMethod(UpscaleMethod) = 0;
    virtual void SetVRUpscalingTransitionProfileForMethod(
        UpscaleMethod, bool renderScaleModeEnabled, UpscalePreset, DLSSProfile) = 0;

    // Revision 3 — written for external controllers
    virtual uint32_t GetVRUpscalingApplyBlockReasons() = 0;
    virtual bool     IsVRUpscalingProfileApplyAllowed() = 0;

    // Revision 4 — OUR extension, forked builds only (build number >= 9)
    virtual uint64_t GetLastFrameGpuTimeUs() = 0;
    virtual uint64_t GetLastFrameGpuTimeFrameIndex() = 0;
};
```

Note that the feature toggles give a governor **more than one lever**: it could
drop SSGI or volumetric lighting before dropping resolution, or in combination.

## Revision 3 Exists For Us

> "External transition controllers should query this before applying VR
> upscaling profiles. Non-zero block reasons mean the caller should **buffer its
> latest desired profile and try again later**. A zero result is a
> runtime-safety check, not permission to synthesize a door transition while the
> player remains in the same cell type."

That prescribes latest-wins buffering on the caller side. Block reasons:

```cpp
enum class VRUpscalingApplyBlockReason : uint32_t {
    kNone                   = 0,
    kRaceSexMenu            = 1u << 0,
    kRaceSexStartupTail     = 1u << 1,
    kLoadingMenu            = 1u << 2,
    kRelatchPending         = 1u << 3,
    kTransitionPending      = 1u << 4,
    kOpenCompositeUpscaling = 1u << 5
};
```

`kOpenCompositeUpscaling` is the same gate that blocks CS upscaling when OCU DLSS
is on — a governor will simply never be permitted to act in that configuration.

## Revision 4 Is What We Add Back

Everything above is CS answering questions about *its own settings*. None of it
answers the only question a governor actually needs: how much of the frame budget
is left. Frametime cannot answer it, because the compositor pins it at the
refresh rate (E-1) — four presets spanning 6× in pixels all read 13.7–13.9 ms.

`GetLastFrameGpuTimeUs()` returns GPU **work** for the last completed frame,
bracketed by D3D11 timestamp queries that open at the frame's first draw and
close immediately before `Present`, so the compositor wait is outside the
bracket. Headroom is then `1 − gpuUs / budgetUs`, and D-10a's thresholds (climb
above 10%, descend below 5%) apply to it directly.

`GetLastFrameGpuTimeFrameIndex()` increments once per timed frame. A steady GPU
time with a rising index is a genuinely steady scene; a steady GPU time with a
frozen index is a stopped timer, and those must not be confused.

Both return 0 when there is no measurement. **0 means "unknown", not "idle".**

## CORRECTION: The Fade Constants Are Advisory

An earlier note warned that a six-second black hold might make a governor
unusable. **That was wrong.** The header is explicit:

```cpp
// Guidance for VR transition controllers that hide render-scale relatches
// behind a game fade. These constants are advisory only and do not change
// the ABI; Community Shaders does not drive Game.FadeOutGame itself.
inline constexpr float CSVRRenderScaleTransitionFadeOutSeconds = 1.0f;
inline constexpr float CSVRRenderScaleTransitionBlackHoldAfterProfileSeconds = 6.0f;
inline constexpr float CSVRRenderScaleTransitionFadeInSeconds = 1.0f;
```

**CS does not fade anything.** These are a *suggested* fade budget for a caller
that chooses to hide a render-scale **relatch** behind `Game.FadeOutGame`. They
are irrelevant to preset-only changes — which matches the instant, unfaded
switching observed in-headset on 2026-07-31.

Practical reading: **preset changes are cheap; toggling `renderScaleModeEnabled`
is not.** A governor should move presets freely and treat render-scale mode as a
static configuration choice, not a lever.

## Implementation Shape

A standalone SKSE plugin:

1. On `kMessage_PostLoad`, request the CS interface at revision 3.
2. Sample frametime.
3. Apply policy — hysteresis, asymmetric up/down timing, cooldowns.
4. Before applying, check `IsVRUpscalingProfileApplyAllowed()`; if blocked,
   buffer the latest desired profile and retry.
5. `SetUpscalePreset(...)` using the **public** enum.
6. Expose thresholds through MCM.

Everything below that — relatch, resource lifecycle, epoch ownership,
anti-oscillation guards — stays inside CS, where it already works and where the
author spent twenty-plus documented iteration steps stabilising it.

## Still Unverified

- Which CS build number ships with MGO4, and whether it exposes revision 3.
- Whether `SetUpscalePreset` alone is enough, or whether
  `SetVRUpscalingTransitionProfile` is required for VR paths.
- Real transition latency from call to visible effect — the reason to build the
  cycler tool first.
