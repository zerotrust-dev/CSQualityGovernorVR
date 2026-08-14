# VR Render Scale Mode: could the relatch check ask "does it fit?" instead of "same quality?"

**Build:** CSX 3.18-VR (tag `CSX3.18`, `2051e2ae`), API build 11, on MGO 4.0 beta
RC3. Pimax Crystal Super, 72 Hz, DLSS, OpenComposite.

## What I'm doing

I've written an SKSE plugin that adjusts the VR upscale preset during gameplay to
hold a locked framerate — it measures GPU time per frame and steps the quality
ladder up and down as the scene load changes. It applies changes through the
documented path, `GetVRUpscalingTransitionProfileDecision` then
`SetVRUpscalingTransitionProfileForMethod`, i.e. preflight plus the door fade
rather than setting the preset underneath the renderer.

Thank you for adding that API — it's exactly what an external controller needs,
and the preflight in particular saved me a lot of guesswork.

## What I ran into

With **VR Render Scale Mode enabled**, every quality change triggers a full
render-target recreation. Measured on my system, the worst frame within 0.5 s of
a change:

| | worst frame after a change |
|---|---|
| VR Render Scale Mode **on** | 59 ms |
| VR Render Scale Mode **off** | 34 ms |

At 72 Hz that's the difference between a clearly visible hitch and something I can
mostly hide. Since my plugin changes quality a few times a minute, it adds up.

Reading `Features/Upscaling/PerfMode.cpp`, the cause looks like
`UpdateRestartRequiredState`:

```cpp
VRPerfModeRestartState::Refresh(
    restartRequired,
    ActiveBootContractInputs{
        .bootActive = boot.active,
        .requestedNow = requestedNow,
        .displaySizeChanged = displaySizeChanged,
        .eligibleNow = eligibleNow,
        .methodMatches = boot.method == a_method,
        .qualityModeMatches = boot.qualityMode == qualityMode,
    });
```

`qualityModeMatches` means a relatch happens whenever the quality differs from
the one latched at boot — including when the new quality is **lower**, and its
render dimensions would fit inside the already-allocated targets with room to
spare.

## The question

Would it be reasonable for that condition to be based on whether the new render
size **fits** the latched allocation, rather than on whether the quality mode
matches? Something like: a quality whose `renderEyeWidth/Height` are ≤
`boot.renderEyeWidth/Height` keeps the existing targets and changes only the
logical extent, via `ApplyDynamicResolutionState` with a ratio of
`activeScale / bootScale` instead of
`ApplyLockedFullResolutionDynamicResolutionState`.

That would make the boot quality an upper bound rather than a fixed point:
anything at or below it becomes free to select during gameplay, anything above it
still needs the relatch and could be deferred to a loading screen.

## What I am not sure about, and why I'm asking rather than proposing a patch

I can see three ways this could be wrong, and you'd know immediately where I
can't:

1. **Passes that don't consult the dynamic-resolution ratio** would keep working
   at the allocated size. Correctness-wise that's fine, but it costs some of the
   feature's benefit and I don't know how many such passes there are.
2. **Features that cache extent-derived state** — dispatch sizes, history
   validity, jitter — would need invalidating on a logical-extent change even
   though no texture was recreated.
3. **DLSS's dynamic-resolution range.** The ladder spans scale 0.333 to 1.0, a 3×
   linear span. Streamline defines a valid input range per output resolution via
   `slDLSSGetOptimalSettings`; I couldn't find the returned min/max being used
   anywhere, so I don't know whether one context can cover that span or whether
   the envelope would have to be narrower.

If any of those makes it impractical, that's a completely satisfying answer and
I'll stop asking.

## What I'm doing meanwhile

Running with VR Render Scale Mode **disabled**. Quality changes then never set
`restartRequired`, because `IsEligible` returns false and no boot latch is
created — so the governor works well and the transition is short.

For what it's worth, on my hardware I could not measure a benefit from the
feature being on. A controlled comparison standing still in one spot, both sweeps
completed, deduplicated P95 GPU time per preset, gave +0.12 ms at Performance and
−0.01 ms at UltraPerformance — against a 0.88 ms noise floor established at
NativeAA, where the feature cannot do anything by construction. So I'm not
reporting a lost win on my own setup; on a lower-end GPU or a higher-resolution
headset I'd expect that to look different, which is why I'd rather the two
features were compatible than pick one.

Happy to test a branch, run controlled measurements, or provide captures — I have
per-frame GPU traces and tooling for this and can turn a comparison around in an
evening.

---

## Sent

**Channel:** Nexus Mods private message to `ParticleTroned`, sent from the Nexus
account `DemosDrax` — a reply will arrive there, not on GitHub
(<https://www.nexusmods.com/profile/ParticleTroned>), 2026-08-13.

**Why not a GitHub issue:** `ParticleTroned/skyrim-community-shaders` has
`has_issues=false` and `has_discussions=false`, and the README states no contact
route. Nexus is the only channel the author offers. The mod page also has a Bugs
tab, which is public and trackable — kept in reserve as a follow-up if the direct
message gets no reply.

**What was sent:** a short covering message with the measurement, the source
reference, the question, and a link to this document. The document was **not**
attached — the link renders properly in a browser and can be updated in place if
the conversation continues.

**Subject line used:**
`CSX VR: quality changes force a render-target relatch when VR Render Scale Mode is on`

**Deliberately left out of the covering message**, and kept here instead: that
Render Scale Mode showed no measurable benefit on this hardware. Leading with it
in a direct message reads as "your feature is pointless", which is not the point
being made — the ask is that the two features be compatible, and the null result
belongs with the method that produced it.

**Nothing on our side is blocked on a reply.** The governor runs with VR Render
Scale Mode disabled at a ~42 ms transition. `RC_PORTING_PLAN.md` carries the
per-RC re-check that would reopen this if a future CS build makes the feature
worth having.

**If a reply comes**, record it here — particularly a "by design" answer with a
reason, which would close the question properly and is worth as much as a fix.
