# Note 3 to ParticleTroned — correction to the vendor-lifecycle finding

Draft, 2026-08-18. To follow the two notes already sent.

---

One correction, and it is to the part of our last note we told you was the most
useful thing we had.

We wrote that vendor-resource readiness is reachable **only** through the
relatch — that `MarkVendorRuntimeResourcesReady` is called nowhere else, and
that this was the real reason the boot latch cannot move. That is wrong. It is
called from four places: the relatch, `RecreateVendorRuntimeResources`,
`ApplyPendingVendorRuntimeReset` and `CheckResources`. It also clears
`pendingDLSSReset` unconditionally, with no generation matching, so any of them
lifts the block. We looked at the region around the relatch, found the calls
there, and did not check the rest of the file.

What we actually ran into is narrower, and we think still worth having.
`ApplyPendingVendorRuntimeReset` begins with:

```cpp
if (pendingPerfModeRenderTargetRecreate.load(std::memory_order_acquire) ||
    perfModeRenderTargetRecreateInProgress.load(std::memory_order_acquire)) {
    return true;
}
```

The vendor reset does not *require* the relatch; it **defers** to one. That is
invisible in the shipped build, because a quality change always sets that flag
and the relatch always completes, so the two are never both absent. It only
becomes visible if something can set the flag while the recreate cannot finish
— then the vendor lifecycle stalls indefinitely and refuses both the plugin API
and the menu's own clicks, and the reason it reports is `RelatchPending` rather
than anything naming the vendor state, which is what misled us for two sessions.

**We are not claiming this is a bug in unmodified CSX.** We reached it by
guarding the relatch ourselves, and the flag was set by
`ServiceSubmitStageBoundsFallbackWatchdog` reacting to a bounds mismatch our own
change had caused. In stock, `matchesExpectedSize` is true by construction and
we would not expect the watchdog to fire at all.

We are flagging it because you are actively working on Render Scale Mode, and
this is the kind of coupling that is entirely safe until the moment someone
changes when the relatch runs — which is exactly what that work involves. The
silent `return true` is the part worth knowing about.

For completeness: with the bounds mismatch fixed, the stall is gone. Our current
build holds one boot latch for a whole session across sixteen quality changes,
with no stranded reset flags. The relatch side of this is behaving; what remains
broken for us is the stereo geometry, which is ours and not yours.
