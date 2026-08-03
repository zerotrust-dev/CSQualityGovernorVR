#include "ApiProbe.h"

#include "core/Presets.h"

#include "VRAPI/CSinterface001.h"

#include <algorithm>
#include <format>

namespace csgov {

namespace {

// Every getter is wrapped: a partial or mismatched build could return
// nonsense or fault, and a diagnostic tool must report that rather than take
// the process down.
template <class Fn>
ProbeResult Probe(std::string a_name, Fn&& a_fn)
{
	ProbeResult result;
	result.name = std::move(a_name);
	try {
		auto [text, plausible, note] = a_fn();
		result.value = std::move(text);
		result.plausible = plausible;
		result.note = std::move(note);
		result.ok = true;
	} catch (const std::exception& e) {
		result.value = "<threw>";
		result.note = e.what();
		result.plausible = false;
	} catch (...) {
		result.value = "<threw>";
		result.note = "unknown exception";
		result.plausible = false;
	}
	return result;
}

struct Reading
{
	std::string value;
	bool plausible = true;
	std::string note;
};

}

bool ApiProbe::AllOk() const
{
	return revisionAcquired != 0 &&
	       std::all_of(results.begin(), results.end(),
			   [](const ProbeResult& r) { return r.ok && r.plausible; });
}

ApiProbe ApiProbe::Run(CSPluginAPI::ICSInterface001* a_interface, unsigned int a_requested,
	unsigned int a_acquired)
{
	ApiProbe probe;
	probe.revisionRequested = a_requested;
	probe.revisionAcquired = a_acquired;

	if (!a_interface) {
		probe.results.push_back({ "interface", "<null>", false, false,
			"Community Shaders did not return an interface" });
		return probe;
	}

	auto* api = a_interface;

	probe.results.push_back(Probe("getBuildNumber", [&]() -> Reading {
		const auto build = api->getBuildNumber();
		return { std::to_string(build), build != 0, build == 0 ? "build number is zero" : "" };
	}));
	probe.buildNumber = api->getBuildNumber();

	probe.results.push_back(Probe("GetUpscalePreset", [&]() -> Reading {
		const auto raw = static_cast<std::uint32_t>(api->GetUpscalePreset());
		const auto info = FindPresetByPublicValue(raw);
		return { std::format("{} ({})", raw, info ? info->name : "UNKNOWN"), info.has_value(),
			info ? "" : "value is outside the documented 0-6 range" };
	}));

	probe.results.push_back(Probe("GetUpscaleMethod", [&]() -> Reading {
		const auto raw = static_cast<std::uint32_t>(api->GetUpscaleMethod());
		static constexpr const char* kNames[]{ "None", "TAA", "FSR", "DLSS" };
		const bool known = raw < 4;
		return { std::format("{} ({})", raw, known ? kNames[raw] : "UNKNOWN"), known,
			raw == 0 ? "upscaling appears to be off - the preset lever will do nothing" : "" };
	}));

	probe.results.push_back(Probe("GetDLSSProfile", [&]() -> Reading {
		const auto raw = static_cast<std::uint32_t>(api->GetDLSSProfile());
		static constexpr const char* kNames[]{ "J", "K", "L", "M", "F" };
		const bool known = raw < 5;
		return { std::format("{} ({})", raw, known ? kNames[raw] : "UNKNOWN"), known, "" };
	}));

	probe.results.push_back(Probe("GetRenderAtUpscaleResEnabled", [&]() -> Reading {
		return { api->GetRenderAtUpscaleResEnabled() ? "true" : "false", true, "" };
	}));

	probe.results.push_back(Probe("GetRenderAtUpscaleResActive", [&]() -> Reading {
		return { api->GetRenderAtUpscaleResActive() ? "true" : "false", true, "" };
	}));

	probe.results.push_back(Probe("GetVRUpscalingApplyBlockReasons", [&]() -> Reading {
		const auto mask = api->GetVRUpscalingApplyBlockReasons();
		const bool terminal = IsTerminalBlock(mask);
		return { std::format("0x{:X} ({})", mask, DescribeBlockReasons(mask)), !terminal,
			terminal ? "OpenComposite upscaling is active - CS upscaling is blocked for the "
			           "whole session and no preset change can succeed"
					 : "" };
	}));

	probe.results.push_back(Probe("IsVRUpscalingProfileApplyAllowed", [&]() -> Reading {
		const bool allowed = api->IsVRUpscalingProfileApplyAllowed();
		return { allowed ? "true" : "false", true,
			allowed ? "" : "not allowed right now - may simply be a loading screen" };
	}));

	probe.results.push_back(Probe("GetSSSEnabled", [&]() -> Reading {
		return { api->GetSSSEnabled() ? "true" : "false", true, "" };
	}));
	probe.results.push_back(Probe("GetSSGIEnabled", [&]() -> Reading {
		return { api->GetSSGIEnabled() ? "true" : "false", true, "" };
	}));
	probe.results.push_back(Probe("GetVolumetricLightingExteriorEnabled", [&]() -> Reading {
		return { api->GetVolumetricLightingExteriorEnabled() ? "true" : "false", true, "" };
	}));
	probe.results.push_back(Probe("GetLightLimitFixContactShadowsEnabled", [&]() -> Reading {
		return { api->GetLightLimitFixContactShadowsEnabled() ? "true" : "false", true, "" };
	}));

	return probe;
}

std::string ApiProbe::Format() const
{
	std::string out;
	out += "===== Community Shaders API probe =====\n";
	out += std::format("  revision requested : {}\n", revisionRequested);
	out += std::format("  revision acquired  : {}{}\n", revisionAcquired,
		revisionAcquired == 0 ? "   <-- NOTHING ACQUIRED"
							  : (revisionAcquired < revisionRequested ? "   <-- fell back" : ""));
	out += std::format("  build number       : {}\n", buildNumber);
	out += "  ---------------------------------------------------------------\n";
	for (const auto& r : results) {
		out += std::format("  {:<38} {}{}\n", r.name, r.value,
			r.plausible && r.ok ? "" : "   <-- CHECK");
		if (!r.note.empty()) {
			out += std::format("  {:<38}   note: {}\n", "", r.note);
		}
	}
	out += std::format("  verdict: {}\n", AllOk() ? "all getters responded plausibly"
												  : "SOMETHING IS WRONG - see CHECK lines above");
	out += "=======================================\n";
	return out;
}

ApiSnapshot ApiSnapshot::Capture(CSPluginAPI::ICSInterface001* a_interface)
{
	ApiSnapshot snap;
	if (!a_interface) {
		return snap;
	}
	try {
		snap.upscalePreset = static_cast<std::uint32_t>(a_interface->GetUpscalePreset());
		snap.upscaleMethod = static_cast<std::uint32_t>(a_interface->GetUpscaleMethod());
		snap.dlssProfile = static_cast<std::uint32_t>(a_interface->GetDLSSProfile());
		snap.renderScaleEnabled = a_interface->GetRenderAtUpscaleResEnabled();
		snap.renderScaleActive = a_interface->GetRenderAtUpscaleResActive();
		snap.blockReasons = a_interface->GetVRUpscalingApplyBlockReasons();
		snap.applyAllowed = a_interface->IsVRUpscalingProfileApplyAllowed();
	} catch (...) {
	}
	return snap;
}

std::string ApiSnapshot::CsvHeaderSuffix(std::string_view a_prefix) const
{
	return std::format("{0}_preset,{0}_method,{0}_dlss_profile,{0}_rs_enabled,"
					   "{0}_rs_active,{0}_apply_allowed,{0}_block",
		a_prefix);
}

std::string ApiSnapshot::CsvValues() const
{
	return std::format("{},{},{},{},{},{},0x{:X}", upscalePreset, upscaleMethod, dlssProfile,
		renderScaleEnabled ? 1 : 0, renderScaleActive ? 1 : 0, applyAllowed ? 1 : 0,
		blockReasons);
}

}
