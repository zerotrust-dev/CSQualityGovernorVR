#pragma once

#include "core/CyclerCore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace csgov {

struct PluginConfig
{
	CyclerConfig cycler = CyclerConfig::Default();

	// Automatically arm the sweep once the player is in-world. With this off
	// the sweep only starts on the hotkey.
	bool autoStart = true;
	// DirectInput scan code. 0x58 = F12. Toggles arm/abort.
	std::uint32_t hotkey = 0x58;

	bool writeCsv = true;
	bool writePerFrameCsv = false;  // large; only for deep analysis
	bool logToFile = true;

	// Keep recording frames after the sweep finishes, for the rest of the play
	// session. Without this the capture covers only the sweep - 4m38s of a much
	// longer session on 2026-08-06 - and anything seen while simply playing has
	// no counterpart in the data.
	bool monitorAfterSweep = true;

	// Run the controller and record every decision. Phase 4: the first live run
	// must not also be the first test.
	bool shadowGovernor = true;
	bool writeTimelineCsv = true;

	// Let the controller actually change the preset. Phase 5.
	//
	// Off by default, and deliberately separate from shadowGovernor: turning
	// the observer on is free, turning the actuator on is not, and the two
	// should never be the same switch.
	//
	// It only ever acts once the sweep has finished. Two owners on one lever
	// has bitten this project repeatedly, and the cycler owns it until then.
	bool applyGovernor = false;

	// Circuit breaker: disable the governor for the session if it applies more
	// changes than this in any rolling minute.
	//
	// The replay predicts 2-3 a minute and D-8 wants few correct changes, so a
	// sustained double-digit rate means something is wrong - a bad threshold, a
	// scene the cost model mispredicts, or an oscillation. In a headset the
	// alternative to stopping by itself is quitting the game, since editing an
	// ini is not an option mid-session.
	int maxChangesPerMinute = 12;

	// Public preset value to select once monitoring begins, or -1 to leave
	// whatever the sweep ended on.
	//
	// The sweep ends on the cheapest rung, so free play afterwards runs with
	// large headroom and never exercises the region where the thresholds
	// decide - 71% of the 2026-08-08 session sat above 30% headroom for exactly
	// this reason. Setting a demanding preset here produces marginal load
	// during ordinary play, which is the alternative to asking someone to go
	// and stand somewhere expensive.
	int monitorPreset = -1;

	// Refresh rate the frame budget is derived from.
	double targetHz = 72.0;

	[[nodiscard]] static PluginConfig Load(const std::filesystem::path& a_path);
	void ApplyDerived();
};

// Minimal INI reader. Intentionally dependency-free and forgiving: an
// unreadable or absent file yields defaults rather than a failure, because a
// diagnostic tool that refuses to run is worse than one running on defaults.
class IniFile
{
public:
	bool Load(const std::filesystem::path& a_path);

	[[nodiscard]] bool Has(std::string_view a_key) const;
	[[nodiscard]] std::string GetString(std::string_view a_key, std::string a_default) const;
	[[nodiscard]] double GetDouble(std::string_view a_key, double a_default) const;
	[[nodiscard]] int GetInt(std::string_view a_key, int a_default) const;
	[[nodiscard]] bool GetBool(std::string_view a_key, bool a_default) const;
	[[nodiscard]] std::uint32_t GetHex(std::string_view a_key, std::uint32_t a_default) const;

private:
	std::map<std::string, std::string, std::less<>> _values;
};

}
