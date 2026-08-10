#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CSPluginAPI {
struct ICSInterface001;
}

namespace csgov {

// A single getter call and what came back.
struct ProbeResult
{
	std::string name;
	std::string value;
	bool ok = false;      // the call returned without throwing
	bool plausible = true;  // the value is inside its documented range
	std::string note;
};

// Calls every getter on the CS interface once, before any cycling, and reports
// what responded.
//
// The point is that a broken or partial API is *stated* rather than inferred
// from a sweep that quietly produced nothing. If this section of the log looks
// wrong, nothing after it is worth reading.
struct ApiProbe
{
	unsigned int buildNumber = 0;
	unsigned int revisionRequested = 0;
	unsigned int revisionAcquired = 0;
	std::vector<ProbeResult> results;

	[[nodiscard]] bool AllOk() const;
	[[nodiscard]] std::string Format() const;

	static ApiProbe Run(CSPluginAPI::ICSInterface001* a_interface, unsigned int a_requested,
		unsigned int a_acquired, unsigned int a_verifiedBuild);
};

// True when the provider is one of our forked builds, i.e. when the revision-4
// GPU timing slots actually exist on its vtable.
//
// This has to be checked before every revision-4 call. The revision-4 methods
// are our own extension, so a stock Community Shaders returns an interface whose
// vtable simply ends earlier; calling one of the new slots on it does not fail
// cleanly, it jumps to whatever follows in memory.
//
// It requires the build number to MATCH the verified one, not merely to be at
// least it. Build numbers are not namespaced across forks: CSX 3.18 reports
// revision 4 build 11 and has no timing methods at all - its revision 4 adds
// GetVRUpscalingTransitionProfileDecision where ours adds GetLastFrameGpuTimeUs.
// A `>=` test passed on it and the next call jumped off the end of their vtable,
// crashing the game at startup (E-34). Two forks can always agree on a number
// by accident; only an exact match against a build somebody has actually
// checked is evidence of anything.
[[nodiscard]] bool GpuTimingAvailable(CSPluginAPI::ICSInterface001* a_interface,
	unsigned int a_acquiredRevision, unsigned int a_verifiedBuild);

// Snapshot of every readable piece of API state, sampled around each
// transition so drift in anything other than the preset is visible.
struct ApiSnapshot
{
	std::uint32_t upscalePreset = 0;
	std::uint32_t upscaleMethod = 0;
	std::uint32_t dlssProfile = 0;
	bool renderScaleEnabled = false;
	bool renderScaleActive = false;
	bool applyAllowed = false;
	std::uint32_t blockReasons = 0;
	// 0 when the provider has no GPU timer, which is also how a forked provider
	// reports "no measurement for this frame". The frame index disambiguates.
	std::uint64_t gpuTimeUs = 0;
	std::uint64_t gpuFrameIndex = 0;

	static ApiSnapshot Capture(CSPluginAPI::ICSInterface001* a_interface, bool a_withGpuTime);

	[[nodiscard]] bool operator==(const ApiSnapshot&) const = default;
	[[nodiscard]] std::string CsvHeaderSuffix(std::string_view a_prefix) const;
	[[nodiscard]] std::string CsvValues() const;
};

}
