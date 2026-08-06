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
