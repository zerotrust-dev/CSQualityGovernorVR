#pragma once

#include <cstdint>

namespace csgov {

// GPU frame timing measured from this plugin, with no forked Community Shaders
// (D-21).
//
// The brackets go in the same two places the fork put them, because that
// placement is what E-18 to E-22 established: open when IVRCompositor's
// WaitGetPoses returns - the point at which the runtime releases the
// application to render - and close at the compositor Submit, before the
// runtime blocks for pacing with the GPU idle. A bracket that encloses that
// wait re-measures frametime and reproduces the censoring we exist to escape.
//
// Nothing here needs the OpenVR SDK. The compositor comes from
// openvr_api.dll's exported VR_GetGenericInterface, and only the two vtable
// slots are touched, so there is no build dependency to keep in step with
// whatever the modlist ships.
namespace CompositorTimer {

// Hooks the compositor and starts timing. Safe to call more than once; only
// the first call does anything. Returns false when the compositor could not be
// found, in which case the caller keeps whatever timing source it had.
bool Install();

void Uninstall();

[[nodiscard]] bool Active() noexcept;

// Microseconds of GPU work in the last completed frame, and the index of that
// frame. The index increments once per timed frame, so a repeated index means
// the timer produced nothing new rather than that the GPU was idle - the same
// contract GovernorSample expects.
[[nodiscard]] std::uint64_t LastFrameGpuTimeUs() noexcept;
[[nodiscard]] std::uint64_t LastFrameGpuTimeFrameIndex() noexcept;

// Frames whose bracket opened before WaitGetPoses returned. In the fork this
// was the diagnostic that caught the real error - 80.5% of frames opening
// early, worth +1.18 ms of phantom GPU time (E-22).
//
// Here it reads 0 by construction, because we open the bracket at WaitGetPoses
// return rather than at a draw. Exposed anyway, as the assertion that the open
// point is where we think it is: a non-zero value would mean something else is
// opening brackets and the whole measurement is suspect. What it cannot do on
// this path is validate the open point - only the reference comparison in D-21
// does that.
[[nodiscard]] std::uint64_t FramesOpenedBeforeSync() noexcept;

}

}
