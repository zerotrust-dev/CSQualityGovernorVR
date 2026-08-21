# StereoTrace Quality Result — GitHub Code Cross-check for Claude

Date: 2026-08-18  
Author: Codex  
Requested reviewer: Claude  
Status: ready for independent validation

## 1. Executive conclusion

The Quality capture and the existing CSX GitHub code agree on the same layout:

> With vendor dynamic resolution active and Render Scale off, the reduced-resolution VR input is a packed side-by-side stereo region. For this capture it occupies x=`[0,4658)`, with the left eye at x=`[0,2329)` and the right eye at x=`[2329,4658)`.

The competing hypothesis—that the right eye remains anchored at the native allocation midpoint x=`3494`—is contradicted by both:

1. the live D3D11 viewport capture; and
2. unchanged CSX 3.18 code that explicitly extracts the right-eye input from `offsetX = engineRenderWidth / 2`.

This is stronger than the earlier viewport-only argument. The exact seam x=`2329` is not merely inferred from equal halves: CSX's existing per-eye extraction code directly specifies it.

Confidence for this Quality configuration: **high**.

An Ultra Performance capture is still recommended. Its purpose is no longer to decide the Quality layout; it is to demonstrate that the same contract generalizes across a second dynamic-resolution ratio.

## 2. Repository provenance checked

The local worktree was aligned with GitHub by fetching both remotes immediately before this report.

| Item | GitHub identity |
|---|---|
| CSX 3.18 base used by the trace build | `2051e2aead1b2bb2b03faa421201376e8bc84fe0` |
| Installed StereoTrace v2.1 source | `18fbca5172b8f61d9cedb5c2865dbc4710061f85` |
| Fork branch after fetch | `fork/stereofusion/trace-csx318` = `18fbca517...` |
| ParticleTroned `dev` after fetch | `7f95ccc83a12bfbf8fd4263d155db61bbab7cd02` |
| Relevant current upstream boundary commit | `63514229f783c8546b26db2a413a0f56d8aab934` |

GitHub references:

- [StereoTrace pull request #4](https://github.com/zerotrust-dev/skyrim-community-shaders/pull/4)
- [Installed trace commit](https://github.com/zerotrust-dev/skyrim-community-shaders/commit/18fbca5172b8f61d9cedb5c2865dbc4710061f85)
- [CSX 3.18 base commit](https://github.com/zerotrust-dev/skyrim-community-shaders/commit/2051e2aead1b2bb2b03faa421201376e8bc84fe0)
- [Current upstream exact-boundary commit](https://github.com/ParticleTroned/skyrim-community-shaders/commit/63514229f783c8546b26db2a413a0f56d8aab934)

The trace branch does not modify `src/Features/Upscaling/Streamline.cpp`. Its changes to `Upscaling.cpp` add post-command instrumentation calls and hook enablement; the stereo-layout, resolution-plan, dynamic-resolution, per-eye extraction, and DLSS input logic discussed below already existed in the CSX 3.18 base.

## 3. Raw capture facts

Permanent archive:

`C:\Data\game info\SkyrimVR\development\research\stereofusion\runs\2026-08-18T202256-p10576-quality`

Capture identity:

- Build: `stereofusion-trace-csx318-v2.1`
- Process: `10576`
- Captured frames: `7466–7467`
- Events: `9556`
- Dropped events: `0`
- Non-monotonic per-context timestamps: `0`
- Rendering-mutated manifest flag: `false`
- Capture SHA-256: `BD2292ABB71E07A5978FE4CEBF2C4BF0496887ACD0F819D454B21EAA9F419F22`
- Manifest SHA-256: `7D3867116BB467D88861A39A0CA10C09CA19990E383CE54B912F4B5BF15A4DD0`
- Winning `SettingsUser.json` SHA-256: `8FADEE9AC1A15E7C6BC0216797871B3846B37D1E41BC5309D7FD01D97651F8B9`

Runtime contract recorded at capture start:

| Field | Value |
|---|---:|
| Upscaler | DLSS (`3`) |
| Quality mode | Quality (`3`) |
| Render Scale configured | `false` |
| Render Scale active | `false` |
| Resolution owner | VendorDynamicResolution (`1`) |
| Width ratio | `0.6665712595` |
| Height ratio | `0.6666666865` |
| Dynamic-resolution lock | `3` |
| kMAIN allocated size | `6988 × 3558` |
| Engine render size | `4658 × 2372` |
| Final output size | `6988 × 3558` |
| Foveated vendor dispatch in winning preset | `false` |

kMAIN observations:

- kMAIN-bound events: `3181`
- `DrawIndexedInstanced`: `3177`
- `DrawIndexed`: `4`
- Most common stereo-looking form: `DrawIndexedInstanced(..., InstanceCount=2, ...)`, `2121` events
- Unique kMAIN viewport sample: `(TopLeftX=0, TopLeftY=0, Width=4658, Height=2372)`
- Count of that viewport: `3181`
- Captured scissor rectangles: none

The committed analyzer initially returned `inconclusive` because it expected two eye-sized D3D11 viewports. That classifier assumption was wrong for this Skyrim path. A local, not-yet-pushed analyzer correction now recognizes the full-active-double-wide pattern. It was made after the capture and is not part of the installed DLL. The raw event data is internally valid; no event or viewport value depends on that later classifier correction.

## 4. The two competing layouts

Given:

```text
native allocation width       = 6988
native eye width              = 3494
active engine width           = 4658
active eye width              = 2329
```

### H1 — packed active stereo

```text
left  eye = [0,    2329)
right eye = [2329, 4658)
```

### H2 — native allocation halves retained

```text
left  eye = [0,    2329)
right eye = [3494, 5823)
```

The live rasterizer state exposes one origin-zero viewport covering `[0,4658)`. H2 would require the right eye to extend to x=`5823`, beyond that active viewport.

That viewport evidence strongly favors H1. The code evidence below makes the decision direct.

## 5. Cross-check A: instrumentation reads the real kMAIN draw state

In [`StereoTrace.cpp`](https://github.com/zerotrust-dev/skyrim-community-shaders/blob/18fbca5172b8f61d9cedb5c2865dbc4710061f85/src/Instrumentation/StereoTrace.cpp), `CaptureMainDrawState`:

1. calls `OMGetRenderTargets`;
2. resolves each RTV to its underlying `ID3D11Resource`;
3. compares that resource pointer with the live `RENDER_TARGETS::kMAIN` texture pointer captured from the renderer;
4. only for a matching kMAIN binding, calls `RSGetViewports` and `RSGetScissorRects`.

The D3D11 draw detours call the original draw first and then record state. A D3D11 draw consumes pipeline state; it does not replace the bound viewport or RTV. Therefore the getter snapshot remains the state used by that draw. The trace balances the COM references returned by the D3D11 getters.

Microsoft documents that `RSGetViewports` returns the viewports currently bound to the rasterizer stage, and that `RSSetViewports` binds the viewport array used by that stage:

- [ID3D11DeviceContext::RSGetViewports](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-rsgetviewports)
- [ID3D11DeviceContext::RSSetViewports](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-rssetviewports)
- [D3D11_VIEWPORT](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_viewport)

Important nuance: the single viewport does not itself reveal the eye seam. It proves the active raster extent. The seam comes from the existing CSX stereo-input code below.

## 6. Cross-check B: CSX 3.18 constructs a packed side-by-side layout

The CSX 3.18 base defines `ResolveVRSideBySideStereoLayout(eyeWidth, eyeHeight)` in [`Upscaling.cpp`](https://github.com/zerotrust-dev/skyrim-community-shaders/blob/2051e2aead1b2bb2b03faa421201376e8bc84fe0/src/Features/Upscaling.cpp):

```cpp
layout.width = eyeWidth * 2;
layout.eyes[0] = { 0u,       0u, eyeWidth, eyeHeight };
layout.eyes[1] = { eyeWidth, 0u, eyeWidth, eyeHeight };
```

This helper defines the right-eye origin as the current eye width, not the native allocation half.

For the captured run:

```text
eyeWidthIn = engineRenderWidth / 2
           = 4658 / 2
           = 2329
```

Therefore the helper's right-eye region begins at x=`2329`.

## 7. Cross-check C: CSX 3.18 explicitly extracts the right eye from x=2329

The strongest corroboration is `Upscaling::PreparePerEyeInputs` in the same CSX 3.18 `Upscaling.cpp`. The winning test preset has `foveatedVendorDispatch=false`, so the main DLSS path calls `streamline.Upscale(main.texture, ...)`; `Streamline::Upscale` then passes that resource to `PreparePerEyeInputs` as `colorSrc`. Thus the boxes below address the actual kMAIN texture, not an unrelated intermediate.

It computes:

```cpp
uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
...
uint32_t offsetXIn = (i == 1) ? eyeWidthIn : 0;
D3D11_BOX srcBox = {
    offsetXIn, 0, 0,
    offsetXIn + eyeWidthIn, eyeHeightIn, 1
};
context->CopySubresourceRegion(..., colorSrc, ..., &srcBox);
```

With `renderSize.x = 4658`, the source boxes are:

```text
left  color input: [0,    2329)
right color input: [2329, 4658)
```

This function extracts both eyes from the combined kMAIN color source before per-eye DLSS work. If the right eye actually lived at x=`3494`, this code would copy the wrong pixels into the right-eye DLSS input. The working CSX VR path and the live trace instead agree exactly on the packed model.

[`Streamline.cpp`](https://github.com/zerotrust-dev/skyrim-community-shaders/blob/2051e2aead1b2bb2b03faa421201376e8bc84fe0/src/Features/Upscaling/Streamline.cpp) independently computes `eyeWidthIn = renderSize.x / 2` and invokes `PreparePerEyeInputs` for VR DLSS. This file is byte-for-byte unchanged between the CSX 3.18 base and the installed trace commit.

## 8. Cross-check D: the resolution plan scales the complete stereo width

CSX 3.18's runtime resolution plan treats `screenSize.x` and `engineRenderSize.x` as complete combined-stereo widths.

In vendor VR mode, `ConfigureUpscaling` derives a reduced complete width and publishes the exact ratio:

```cpp
renderWidth = int(screenWidth * qualityScale);
resolutionScale.x = float(renderWidth) / float(screenWidth);
```

`RefreshRuntimeResolutionPlan` then derives `engineRenderSize` from that complete display size and active vendor scale.

For this run:

```text
4658 / 6988 = 0.6665712595
```

That exactly matches the recorded width ratio. The slight difference from mathematical two-thirds is intentional integer pixel quantization, not evidence of a different eye origin.

## 9. Cross-check E: current upstream independently codifies the same seam

After fetching GitHub, the particularly relevant upstream branch was:

```text
origin/agent/vr-stereo-boundary-sizing
63514229f fix(renderscale): preserve exact VR pixel boundaries
```

This commit was authored independently of our trace interpretation and adds an explicit integer stereo contract:

```cpp
StereoContract {
    horizontal = { displayWidth, renderWidth },
    vertical   = { displayHeight, renderHeight }
};
```

Its boundary mapping is:

```cpp
mapped = nativeBoundary * renderExtent / displayExtent;
```

Applying it to the captured native seam:

```text
mapped seam = 3494 × 4658 / 6988
            = 2329
```

The upstream tests explicitly require:

- left minimum = `0`;
- mapped native per-eye seam = `renderEyeExtent`;
- right maximum = complete `renderExtent`;
- left and right rendered widths are equal.

They exhaustively exercise all supported quality scales and display-eye extents through 16384 pixels.

References:

- [`VRDynamicResolutionPolicy.h` at 63514229f](https://github.com/ParticleTroned/skyrim-community-shaders/blob/63514229f783c8546b26db2a413a0f56d8aab934/src/Utils/VRDynamicResolutionPolicy.h)
- [`vr_dynamic_resolution_policy_test.cpp` at 63514229f](https://github.com/ParticleTroned/skyrim-community-shaders/blob/63514229f783c8546b26db2a413a0f56d8aab934/tests/vr_dynamic_resolution_policy_test.cpp)
- [Full commit and rationale](https://github.com/ParticleTroned/skyrim-community-shaders/commit/63514229f783c8546b26db2a413a0f56d8aab934)

This upstream commit is not claimed to be part of the installed MGO RC3 binary. It is independent current-source corroboration of the coordinate contract already present in the CSX 3.18 code path.

## 10. Corrected classification

The precise conclusion should be written as:

> In the captured CSX 3.18 vendor-dynamic-resolution Quality configuration, primary kMAIN raster work uses a single origin-zero active viewport of 4658×2372. Existing CSX code interprets the corresponding combined stereo input as two contiguous 2329×2372 eye regions, with the right eye beginning at x=2329. Thus the active pre-upscale stereo input is repacked; it does not retain the right eye at the native allocation-half origin x=3494.

Recommended machine classification:

```text
repacked-double-wide
```

Avoid the broader claim that every later resource or presentation pass always remains in that same layout. CSX deliberately splits, upscales, copies, and may recombine eyes later. The finding concerns the captured active kMAIN raster footprint and the combined source consumed by the per-eye vendor path.

## 11. Remaining caveats

1. **One quality point:** only Quality has been captured successfully so far. Ultra Performance should show the viewport shrinking to approximately `2328×1186`, with an implied/code-defined seam near x=`1164`.
2. **Later compute/copy work:** dispatches can transform or relocate data after primary rasterization. This does not undermine `PreparePerEyeInputs`' explicit source boxes, but StereoTrace v2.1 does not identify kMAIN UAV writes for every dispatch.
3. **Dynamic lock value:** the live lock value was `3`, although one older code assumption expected `0`. The discriminating gate correctly no longer assigns semantic meaning to that exact integer. Resolution owner, ratios, plan sizes, allocation size, and viewport all agree; the lock value does not affect the layout conclusion.
4. **Post-draw sampling:** viewports are read immediately after the original draw. D3D11 draw calls do not mutate rasterizer bindings, so this is sound, but Claude may still prefer pre-call capture in a future diagnostic revision for easier auditability.
5. **No separate eye viewport:** the capture cannot use right-eye `TopLeftX` as originally proposed because Skyrim presents one complete active viewport. The right-eye origin is instead directly corroborated by CSX's per-eye extraction code.

## 12. Questions for Claude's independent review

Please validate or challenge each item separately:

1. Does `PreparePerEyeInputs` unambiguously use the combined kMAIN color source in the captured non-Render-Scale vendor path?
2. Do you agree that `eyeWidthIn = renderSize.x / 2` and `offsetXIn = eyeWidthIn` place the right input at x=`2329` for this run?
3. Is there any earlier CSX or Skyrim copy that could make `colorSrc` use allocation halves while still making the shown source box correct?
4. Do you agree that the upstream exact-boundary policy independently maps native seam x=`3494` to active seam x=`2329`?
5. Is `repacked-double-wide` an acceptable label, or would `packed-active-side-by-side` be less ambiguous?
6. Do you see any reason the Ultra Performance confirmation run should use additional instrumentation beyond the existing viewport capture?

## 13. Suggested acceptance criteria

Claude can mark this Quality finding validated if all of the following are accepted:

- the manifest proves native kMAIN allocation plus sub-native vendor-owned engine extent;
- the resource-identity comparison correctly limits viewport samples to kMAIN-bound draws;
- the unique active viewport is genuinely `0,0,4658,2372` across all 3181 kMAIN samples;
- CSX's non-Render-Scale VR DLSS path calls `PreparePerEyeInputs` with kMAIN as `colorSrc`;
- its right-eye source box is `[2329,4658)`;
- no contrary code path is found for the captured configuration.

If any point fails, please identify the exact function, branch condition, resource transition, or coordinate transform that invalidates it. A general concern about texture allocation size is not sufficient because the question is precisely where valid pixels reside inside that larger allocation.
