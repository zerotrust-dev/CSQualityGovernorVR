#pragma once

#include "GovernorCore.h"
#include "Presets.h"

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace csgov {

// One recorded frame, as read from a *_frames.csv capture.
struct TraceFrame
{
	std::uint64_t wallMs = 0;
	double timeSeconds = 0.0;
	double frameTimeMs = 0.0;
	std::uint32_t presetPublicValue = 0;
	std::string state;
	std::uint64_t gpuUs = 0;
	std::uint64_t gpuFrameIndex = 0;
};

// The session's cost curve, fitted across the presets the sweep visited.
//
// Deliberately not a per-scene fit: a capture from ordinary play moves through
// many scenes, and Rule 7 in MEASUREMENT_METHOD.md is about exactly that. This
// is a session average and is used only to scale a counterfactual, never to
// state what a scene costs.
struct CostModel
{
	double tFixedMs = 0.0;
	double tScaledMs = 0.0;
	std::size_t presetsFitted = 0;
	double worstResidualMs = 0.0;

	[[nodiscard]] bool Valid() const noexcept { return presetsFitted >= 3 && tFixedMs > 0.0; }
	[[nodiscard]] double K() const noexcept { return tFixedMs > 0.0 ? tScaledMs / tFixedMs : 0.0; }
	[[nodiscard]] double PredictMs(double a_pixelFraction) const noexcept
	{
		return tFixedMs + tScaledMs * a_pixelFraction;
	}
};

// How a frame's cost is projected onto a preset it was not rendered at (D-14).
// Both exist because neither is obviously right, and a conclusion that depends
// on the choice is an artefact rather than a result.
enum class Counterfactual {
	Scaled,    // fixed and scaled costs move together with scene load
	Additive,  // only the fixed cost moves; per-pixel cost stays put
};

// What a replayed session produced. These are the terms of the objective in
// section 1 of the design doc, so a parameter sweep can be ranked by them
// rather than by eye.
struct ReplayResult
{
	std::size_t frames = 0;
	std::size_t decisions = 0;
	std::size_t changes = 0;
	double durationSeconds = 0.0;

	// The thing being maximised: mean rendered pixel fraction, weighted by how
	// long each preset was held.
	double timeWeightedPixelFraction = 0.0;
	// The constraint. Frames whose synthesised GPU time exceeded the budget.
	double overBudgetRate = 0.0;
	// Sustained oscillation shows up here: changes that reversed direction.
	std::size_t reversals = 0;
	double changesPerMinute = 0.0;

	double meanGpuMs = 0.0;
	double p95GpuMs = 0.0;
};

// Parses a *_frames.csv capture. Unknown or malformed rows are skipped rather
// than throwing: a capture truncated by a crash is still worth replaying, and
// that is exactly when one gets truncated.
[[nodiscard]] std::vector<TraceFrame> ParseTrace(std::istream& a_stream);

// Fits the cost model across every preset the trace dwelt at, deduplicating by
// GPU frame index first - an asynchronously published value would otherwise be
// weighted by how long it stayed published (Rule 8).
[[nodiscard]] CostModel FitCostModel(const std::vector<TraceFrame>& a_trace);

// Replays a trace through a controller, synthesising the cost of presets the
// controller chooses and the capture did not.
[[nodiscard]] ReplayResult Replay(const std::vector<TraceFrame>& a_trace,
	const CostModel& a_model, const GovernorConfig& a_config, Counterfactual a_mode);

}
