#include "Config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace csgov {

namespace {

std::string Trim(std::string_view a_text)
{
	const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
	auto begin = std::find_if(a_text.begin(), a_text.end(), notSpace);
	auto end = std::find_if(a_text.rbegin(), a_text.rend(), notSpace).base();
	return begin < end ? std::string{ begin, end } : std::string{};
}

std::string ToLower(std::string a_text)
{
	std::transform(a_text.begin(), a_text.end(), a_text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return a_text;
}

}

bool IniFile::Load(const std::filesystem::path& a_path)
{
	std::ifstream file(a_path);
	if (!file) {
		return false;
	}

	std::string line;
	while (std::getline(file, line)) {
		// Strip comments; both ';' and '#' are common in Skyrim INI files.
		if (const auto hash = line.find_first_of(";#"); hash != std::string::npos) {
			line.erase(hash);
		}
		const auto eq = line.find('=');
		if (eq == std::string::npos) {
			continue;  // section headers and blanks
		}
		auto key = ToLower(Trim(std::string_view{ line }.substr(0, eq)));
		auto value = Trim(std::string_view{ line }.substr(eq + 1));
		if (!key.empty()) {
			_values[std::move(key)] = std::move(value);
		}
	}
	return true;
}

bool IniFile::Has(std::string_view a_key) const
{
	return _values.find(ToLower(std::string{ a_key })) != _values.end();
}

std::string IniFile::GetString(std::string_view a_key, std::string a_default) const
{
	const auto it = _values.find(ToLower(std::string{ a_key }));
	return it == _values.end() ? std::move(a_default) : it->second;
}

double IniFile::GetDouble(std::string_view a_key, double a_default) const
{
	const auto text = GetString(a_key, {});
	if (text.empty()) {
		return a_default;
	}
	try {
		return std::stod(text);
	} catch (...) {
		return a_default;
	}
}

int IniFile::GetInt(std::string_view a_key, int a_default) const
{
	const auto text = GetString(a_key, {});
	if (text.empty()) {
		return a_default;
	}
	try {
		return std::stoi(text, nullptr, 0);
	} catch (...) {
		return a_default;
	}
}

bool IniFile::GetBool(std::string_view a_key, bool a_default) const
{
	const auto text = ToLower(GetString(a_key, {}));
	if (text.empty()) {
		return a_default;
	}
	return text == "1" || text == "true" || text == "yes" || text == "on";
}

std::uint32_t IniFile::GetHex(std::string_view a_key, std::uint32_t a_default) const
{
	const auto text = GetString(a_key, {});
	if (text.empty()) {
		return a_default;
	}
	try {
		return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
	} catch (...) {
		return a_default;
	}
}

void PluginConfig::ApplyDerived()
{
	if (targetHz > 0.0) {
		cycler.frameBudgetMs = 1000.0 / targetHz;
	}
	if (cycler.order.empty()) {
		cycler.order = CyclerConfig::Default().order;
	}
}

PluginConfig PluginConfig::Load(const std::filesystem::path& a_path)
{
	PluginConfig config;

	IniFile ini;
	if (!ini.Load(a_path)) {
		config.ApplyDerived();
		return config;  // defaults are a valid configuration
	}

	config.autoStart = ini.GetBool("AutoStart", config.autoStart);
	config.hotkey = ini.GetHex("Hotkey", config.hotkey);
	config.writeCsv = ini.GetBool("WriteCsv", config.writeCsv);
	config.writePerFrameCsv = ini.GetBool("WritePerFrameCsv", config.writePerFrameCsv);
	config.logToFile = ini.GetBool("LogToFile", config.logToFile);
	config.monitorAfterSweep = ini.GetBool("MonitorAfterSweep", config.monitorAfterSweep);
	config.shadowGovernor = ini.GetBool("ShadowGovernor", config.shadowGovernor);
	config.writeTimelineCsv = ini.GetBool("WriteTimelineCsv", config.writeTimelineCsv);
	config.monitorPreset = ini.GetInt("MonitorPreset", config.monitorPreset);
	config.applyGovernor = ini.GetBool("ApplyGovernor", config.applyGovernor);
	config.maxChangesPerMinute = ini.GetInt("MaxChangesPerMinute", config.maxChangesPerMinute);
	config.targetHz = ini.GetDouble("TargetHz", config.targetHz);

	config.cycler.startDelaySeconds =
		ini.GetDouble("StartDelaySeconds", config.cycler.startDelaySeconds);
	config.cycler.startMaxWaitSeconds =
		ini.GetDouble("StartMaxWaitSeconds", config.cycler.startMaxWaitSeconds);
	config.cycler.dwellSeconds = ini.GetDouble("DwellSeconds", config.cycler.dwellSeconds);
	config.cycler.settleTimeoutSeconds =
		ini.GetDouble("SettleTimeoutSeconds", config.cycler.settleTimeoutSeconds);
	config.cycler.retryIntervalSeconds =
		ini.GetDouble("RetryIntervalSeconds", config.cycler.retryIntervalSeconds);
	config.cycler.blockGiveUpSeconds =
		ini.GetDouble("BlockGiveUpSeconds", config.cycler.blockGiveUpSeconds);
	config.cycler.sweeps = ini.GetInt("Sweeps", config.cycler.sweeps);

	config.cycler.settle.minSamples = static_cast<std::size_t>(
		ini.GetInt("SettleMinSamples", static_cast<int>(config.cycler.settle.minSamples)));
	config.cycler.settle.warmupSamples = static_cast<std::size_t>(
		ini.GetInt("SettleWarmupSamples", static_cast<int>(config.cycler.settle.warmupSamples)));
	config.cycler.settle.toleranceMs =
		ini.GetDouble("SettleToleranceMs", config.cycler.settle.toleranceMs);

	// Order = comma-separated public preset values, e.g. "3,2,1,6,5"
	if (const auto order = ini.GetString("Order", {}); !order.empty()) {
		std::vector<Preset> parsed;
		std::stringstream stream(order);
		std::string token;
		while (std::getline(stream, token, ',')) {
			token = Trim(token);
			if (token.empty()) {
				continue;
			}
			try {
				const auto value = static_cast<std::uint32_t>(std::stoul(token));
				if (const auto info = FindPresetByPublicValue(value)) {
					parsed.push_back(info->preset);
				}
			} catch (...) {
			}
		}
		if (!parsed.empty()) {
			config.cycler.order = std::move(parsed);
		}
	}

	config.ApplyDerived();
	return config;
}

}
