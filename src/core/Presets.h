#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace csgov {

// Public CS plugin-API numbering. See docs/CS_PLUGIN_API.md.
//
// WARNING: this is NOT the same numbering as CS's internal qualityMode. The
// public values 0-4 are frozen for already-compiled consumers, so Hoshipa and
// Ultra Quality were appended at 5 and 6 rather than inserted in scale order.
// Confusing the two silently selects the wrong preset.
enum class Preset : std::uint32_t {
	NativeAA = 0,  // DLAA / Native AA
	Quality = 1,
	Balanced = 2,
	Performance = 3,
	UltraPerformance = 4,
	Hoshipa = 5,
	UltraQuality = 6,
};

struct PresetInfo
{
	Preset preset;
	std::uint32_t publicValue;    // value passed to SetUpscalePreset
	std::uint32_t internalValue;  // CS-internal qualityMode, for cross-referencing logs
	float scale;                  // linear render-resolution ratio
	std::string_view name;
};

// Scales from Upscaling.h::GetQualityModeResolutionScale (internal numbering),
// mapped here onto the public enum.
inline constexpr std::array<PresetInfo, 7> kPresets{ {
	{ Preset::UltraPerformance, 4, 6, 1.0f / 3.0f, "UltraPerformance" },
	{ Preset::Performance, 3, 5, 0.5f, "Performance" },
	{ Preset::Balanced, 2, 4, 1.0f / 1.7f, "Balanced" },
	{ Preset::Quality, 1, 3, 1.0f / 1.5f, "Quality" },
	{ Preset::UltraQuality, 6, 2, 1.0f / 1.3f, "UltraQuality" },
	{ Preset::Hoshipa, 5, 1, 0.85f, "Hoshipa" },
	{ Preset::NativeAA, 0, 0, 1.0f, "NativeAA" },
} };

// kPresets is ordered cheapest -> most expensive, which is the order a sweep
// should walk. Do not reorder without updating the sweep tests.

[[nodiscard]] std::optional<PresetInfo> FindPreset(Preset a_preset) noexcept;
[[nodiscard]] std::optional<PresetInfo> FindPresetByPublicValue(std::uint32_t a_value) noexcept;
[[nodiscard]] std::string_view PresetName(Preset a_preset) noexcept;
[[nodiscard]] float PresetScale(Preset a_preset) noexcept;

// Fraction of native pixels rendered: scale^2.
[[nodiscard]] float PresetPixelFraction(Preset a_preset) noexcept;

// Block reasons, mirroring CSPluginAPI::VRUpscalingApplyBlockReason.
enum class BlockReason : std::uint32_t {
	None = 0,
	RaceSexMenu = 1u << 0,
	RaceSexStartupTail = 1u << 1,
	LoadingMenu = 1u << 2,
	RelatchPending = 1u << 3,
	TransitionPending = 1u << 4,
	OpenCompositeUpscaling = 1u << 5,
};

// Human-readable decomposition, e.g. "LoadingMenu|RelatchPending".
// Returns "None" for 0 and appends "Unknown(0x..)" for unrecognised bits.
[[nodiscard]] std::string DescribeBlockReasons(std::uint32_t a_mask);

// kOpenCompositeUpscaling is terminal for the session: CS upscaling is blocked
// entirely while OpenComposite upscaling is active, so retrying is pointless.
[[nodiscard]] bool IsTerminalBlock(std::uint32_t a_mask) noexcept;

}
