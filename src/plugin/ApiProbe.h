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
		unsigned int a_acquired);
};

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

	static ApiSnapshot Capture(CSPluginAPI::ICSInterface001* a_interface);

	[[nodiscard]] bool operator==(const ApiSnapshot&) const = default;
	[[nodiscard]] std::string CsvHeaderSuffix(std::string_view a_prefix) const;
	[[nodiscard]] std::string CsvValues() const;
};

}
