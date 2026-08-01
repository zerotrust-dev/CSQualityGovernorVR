#include "core/Presets.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace csgov;
using Catch::Approx;

TEST_CASE("public preset values match CSinterface001.h", "[presets]")
{
	// These come straight from the published header. If CS ever renumbers them
	// this test is the tripwire - a wrong value here silently selects the
	// wrong preset in game.
	CHECK(FindPreset(Preset::NativeAA)->publicValue == 0);
	CHECK(FindPreset(Preset::Quality)->publicValue == 1);
	CHECK(FindPreset(Preset::Balanced)->publicValue == 2);
	CHECK(FindPreset(Preset::Performance)->publicValue == 3);
	CHECK(FindPreset(Preset::UltraPerformance)->publicValue == 4);
	CHECK(FindPreset(Preset::Hoshipa)->publicValue == 5);
	CHECK(FindPreset(Preset::UltraQuality)->publicValue == 6);
}

TEST_CASE("public and internal numbering genuinely differ", "[presets]")
{
	// The whole reason this table exists. Quality is 1 publicly but 3
	// internally; Hoshipa is 5 publicly but 1 internally.
	const auto quality = FindPreset(Preset::Quality).value();
	const auto hoshipa = FindPreset(Preset::Hoshipa).value();

	CHECK(quality.publicValue != quality.internalValue);
	CHECK(hoshipa.publicValue != hoshipa.internalValue);
	CHECK(quality.internalValue == 3);
	CHECK(hoshipa.internalValue == 1);
}

TEST_CASE("scales match GetQualityModeResolutionScale", "[presets]")
{
	CHECK(PresetScale(Preset::NativeAA) == Approx(1.0f));
	CHECK(PresetScale(Preset::Hoshipa) == Approx(0.85f));
	CHECK(PresetScale(Preset::UltraQuality) == Approx(1.0f / 1.3f));
	CHECK(PresetScale(Preset::Quality) == Approx(1.0f / 1.5f));
	CHECK(PresetScale(Preset::Balanced) == Approx(1.0f / 1.7f));
	CHECK(PresetScale(Preset::Performance) == Approx(0.5f));
	CHECK(PresetScale(Preset::UltraPerformance) == Approx(1.0f / 3.0f));
}

TEST_CASE("pixel fraction is the square of the scale", "[presets]")
{
	CHECK(PresetPixelFraction(Preset::Performance) == Approx(0.25f));
	CHECK(PresetPixelFraction(Preset::NativeAA) == Approx(1.0f));
	// Balanced -> Ultra Quality is the jump that broke 72 fps before foveation.
	CHECK(PresetPixelFraction(Preset::UltraQuality) /
			  PresetPixelFraction(Preset::Balanced) ==
		  Approx(1.7096).margin(0.01));
}

TEST_CASE("table is complete and unambiguous", "[presets]")
{
	std::set<std::uint32_t> publicValues;
	std::set<std::uint32_t> internalValues;
	for (const auto& info : kPresets) {
		CHECK(publicValues.insert(info.publicValue).second);
		CHECK(internalValues.insert(info.internalValue).second);
		CHECK(info.scale > 0.0f);
		CHECK(info.scale <= 1.0f);
		CHECK_FALSE(info.name.empty());
	}
	CHECK(publicValues.size() == 7);
}

TEST_CASE("sweep order runs cheapest to most expensive", "[presets]")
{
	// CyclerConfig::Default() walks kPresets in order; a sweep that jumped
	// around would confound the measurements.
	for (std::size_t i = 1; i < kPresets.size(); ++i) {
		CHECK(kPresets[i].scale > kPresets[i - 1].scale);
	}
}

TEST_CASE("lookup by public value round-trips", "[presets]")
{
	for (const auto& info : kPresets) {
		const auto found = FindPresetByPublicValue(info.publicValue);
		REQUIRE(found.has_value());
		CHECK(found->preset == info.preset);
	}
	CHECK_FALSE(FindPresetByPublicValue(99).has_value());
}

TEST_CASE("block reasons decode to readable text", "[presets]")
{
	CHECK(DescribeBlockReasons(0) == "None");
	CHECK(DescribeBlockReasons(static_cast<std::uint32_t>(BlockReason::LoadingMenu)) ==
		  "LoadingMenu");

	const auto combined = static_cast<std::uint32_t>(BlockReason::LoadingMenu) |
	                      static_cast<std::uint32_t>(BlockReason::RelatchPending);
	CHECK(DescribeBlockReasons(combined) == "LoadingMenu|RelatchPending");

	// Unrecognised bits must still surface rather than vanish.
	CHECK(DescribeBlockReasons(0x80u).find("Unknown") != std::string::npos);
}

TEST_CASE("only OpenComposite blocking is terminal", "[presets]")
{
	CHECK(IsTerminalBlock(static_cast<std::uint32_t>(BlockReason::OpenCompositeUpscaling)));
	CHECK_FALSE(IsTerminalBlock(static_cast<std::uint32_t>(BlockReason::LoadingMenu)));
	CHECK_FALSE(IsTerminalBlock(0));
}
