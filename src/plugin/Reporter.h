#pragma once

#include "core/CyclerCore.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace csgov {

// Writes the cold numbers. The point of the cycler is that one game session
// produces a complete dataset, so everything worth knowing has to land on
// disk without anyone watching the screen.
//
// Three artifacts:
//   *_transitions.csv  one row per preset visit - the primary result
//   *_frames.csv       optional per-frame trace for deep analysis
//   *_summary.txt      human-readable, including a per-preset comparison table
class Reporter
{
public:
	Reporter(std::filesystem::path a_directory, std::string a_sessionStamp);

	void SetSessionInfo(std::string a_info) { _sessionInfo = std::move(a_info); }

	bool OpenTransitions();
	bool OpenFrames();
	bool OpenApiState(std::string_view a_header);

	// Free-form artifact, e.g. the startup API probe.
	void WriteText(std::string_view a_suffix, std::string_view a_content);
	void WriteApiState(std::string_view a_line);

	void WriteTransition(const TransitionRecord& a_record);
	// a_gpuTimeUs is 0 when the provider has no GPU timer, or when it had no
	// measurement for this frame; a_gpuFrameIndex tells those two apart, and
	// tells a repeated reading from a fresh one.
	void WriteFrame(double a_time, double a_frameTimeMs, std::uint32_t a_presetPublicValue,
		std::string_view a_state, std::uint64_t a_gpuTimeUs, std::uint64_t a_gpuFrameIndex);

	// Aggregates all records into a comparison table, then closes everything.
	void Finish(const std::vector<TransitionRecord>& a_records, double a_budgetMs);

	[[nodiscard]] const std::filesystem::path& Directory() const noexcept { return _directory; }

private:
	std::filesystem::path Path(std::string_view a_suffix) const;

	std::filesystem::path _directory;
	std::string _stamp;
	std::string _sessionInfo;
	std::ofstream _transitions;
	std::ofstream _frames;
	std::ofstream _apiState;
};

}
