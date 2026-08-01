#include "Presets.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace csgov {

std::optional<PresetInfo> FindPreset(Preset a_preset) noexcept
{
	const auto it = std::find_if(kPresets.begin(), kPresets.end(),
		[a_preset](const PresetInfo& info) { return info.preset == a_preset; });
	if (it == kPresets.end()) {
		return std::nullopt;
	}
	return *it;
}

std::optional<PresetInfo> FindPresetByPublicValue(std::uint32_t a_value) noexcept
{
	const auto it = std::find_if(kPresets.begin(), kPresets.end(),
		[a_value](const PresetInfo& info) { return info.publicValue == a_value; });
	if (it == kPresets.end()) {
		return std::nullopt;
	}
	return *it;
}

std::string_view PresetName(Preset a_preset) noexcept
{
	if (const auto info = FindPreset(a_preset)) {
		return info->name;
	}
	return "Unknown";
}

float PresetScale(Preset a_preset) noexcept
{
	if (const auto info = FindPreset(a_preset)) {
		return info->scale;
	}
	return 1.0f;
}

float PresetPixelFraction(Preset a_preset) noexcept
{
	const float scale = PresetScale(a_preset);
	return scale * scale;
}

std::string DescribeBlockReasons(std::uint32_t a_mask)
{
	if (a_mask == 0) {
		return "None";
	}

	struct Entry
	{
		std::uint32_t bit;
		const char* name;
	};

	static constexpr Entry kEntries[]{
		{ static_cast<std::uint32_t>(BlockReason::RaceSexMenu), "RaceSexMenu" },
		{ static_cast<std::uint32_t>(BlockReason::RaceSexStartupTail), "RaceSexStartupTail" },
		{ static_cast<std::uint32_t>(BlockReason::LoadingMenu), "LoadingMenu" },
		{ static_cast<std::uint32_t>(BlockReason::RelatchPending), "RelatchPending" },
		{ static_cast<std::uint32_t>(BlockReason::TransitionPending), "TransitionPending" },
		{ static_cast<std::uint32_t>(BlockReason::OpenCompositeUpscaling), "OpenCompositeUpscaling" },
	};

	std::string out;
	std::uint32_t known = 0;
	for (const auto& entry : kEntries) {
		if ((a_mask & entry.bit) != 0) {
			if (!out.empty()) {
				out += '|';
			}
			out += entry.name;
			known |= entry.bit;
		}
	}

	if (const std::uint32_t leftover = a_mask & ~known; leftover != 0) {
		char buf[32]{};
		std::snprintf(buf, sizeof(buf), "Unknown(0x%X)", leftover);
		if (!out.empty()) {
			out += '|';
		}
		out += buf;
	}

	return out;
}

bool IsTerminalBlock(std::uint32_t a_mask) noexcept
{
	return (a_mask & static_cast<std::uint32_t>(BlockReason::OpenCompositeUpscaling)) != 0;
}

}
