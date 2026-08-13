#pragma once

#include "core/CyclerCore.h"
#include "core/GovernorCore.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace csgov {

// One frame's compositor delivery counters, for the D-25 capability test.
//
// Deliberately a plain struct here rather than the reader's own type: Reporter
// belongs to the core-facing side and should not gain a dependency on the
// plugin's OpenVR declarations just to write five integers.
struct FrameDelivery
{
	bool fresh = false;
	std::uint32_t presents = 0;
	std::uint32_t dropped = 0;
	std::uint32_t misPresented = 0;
	std::uint32_t reprojectionFlags = 0;
	// The two fields OpenComposite DOES populate (E-53). Microseconds, so the
	// column is an integer like every other timing column here.
	//
	// compositorGpuUs is the lead worth following: it reads 4-7 ms, which is
	// GPU work outside our WaitGetPoses->Submit bracket, and E-49 is still
	// looking for exactly that. intervalUs is the runtime's own measurement of
	// the frame period - a better basis for deriving refresh than our own
	// timing, which is quantised to 1/6 ms.
	std::uint32_t intervalUs = 0;
	std::uint32_t compositorGpuUs = 0;
};

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

	// Lines written as "# key=value" above the capture's header row.
	//
	// A capture that does not say what produced it cannot be compared with one
	// from a different mod list, and the two committed captures are already
	// implicitly "MGO 4.0beta RC2 + CS PL3.15" with nothing in the files saying
	// so. With release candidates arriving that is a fact worth losing once.
	//
	// The preset ladder goes in here too. The scales are hardcoded and the CS
	// API cannot report them, so recording what was assumed at capture time is
	// the only way a later replay can tell whether they still hold.
	void SetProvenance(std::vector<std::string> a_lines) { _provenance = std::move(a_lines); }

	bool OpenTransitions();
	bool OpenFrames();
	bool OpenApiState(std::string_view a_header);

	// One row per controller evaluation, including the ones that decided to do
	// nothing. Phase T of the design doc: a session has to be reconstructible
	// from the artifacts alone - what the scene cost, what the controller saw,
	// what it did, and why.
	bool OpenTimeline();
	// a_outcome records what became of the decision - shadow, applied,
	// deferred, refused - because "it decided to climb" and "the climb
	// happened" are different facts and a log that conflates them cannot be
	// used to explain a session.
	void WriteTimeline(std::uint64_t a_wallMs, const GovernorDecision& a_decision,
		std::uint32_t a_presetPublicValue, std::uint32_t a_targetPublicValue,
		std::string_view a_outcome);

	// Free-form artifact, e.g. the startup API probe.
	void WriteText(std::string_view a_suffix, std::string_view a_content);
	void WriteApiState(std::string_view a_line);

	void WriteTransition(const TransitionRecord& a_record);
	// a_wallMs is Unix epoch milliseconds (UTC), carried per row so this
	// capture can be joined against another tool's wall-clock log.
	//
	// a_gpuTimeUs is 0 when the provider has no GPU timer, or when it had no
	// measurement for this frame; a_gpuFrameIndex tells those two apart, and
	// tells a repeated reading from a fresh one.
	void WriteFrame(std::uint64_t a_wallMs, double a_time, double a_frameTimeMs,
		std::uint32_t a_presetPublicValue, std::string_view a_state, std::uint64_t a_gpuTimeUs,
		std::uint64_t a_gpuFrameIndex, std::uint64_t a_postSubmitUs,
		const FrameDelivery& a_delivery);

	// Aggregates all records into a comparison table and closes the transitions
	// stream. The per-frame and API streams stay open for monitor mode.
	void Finish(const std::vector<TransitionRecord>& a_records, double a_budgetMs);

	// Bounds how much a hard exit can lose. Called once a second while
	// monitoring, because a plugin's static destructors are not guaranteed to
	// run when the game process goes away.
	void Flush();
	void CloseStreams();

	[[nodiscard]] const std::filesystem::path& Directory() const noexcept { return _directory; }

private:
	std::filesystem::path Path(std::string_view a_suffix) const;

	std::filesystem::path _directory;
	std::string _stamp;
	std::string _sessionInfo;
	std::vector<std::string> _provenance;
	std::ofstream _transitions;
	std::ofstream _frames;
	std::ofstream _apiState;
	std::ofstream _timeline;
};

}
