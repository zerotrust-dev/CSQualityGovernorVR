#pragma once

#include "Presets.h"
#include "Stats.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace csgov {

// One frame, as the controller sees it.
struct GovernorSample
{
	double nowSeconds = 0.0;
	double frameTimeMs = 0.0;
	// 0 means "no measurement" - not "the GPU was idle". A stock Community
	// Shaders never supplies this, and even a forked one reports 0 for a frame
	// it could not time (D-10).
	std::uint64_t gpuTimeUs = 0;
	// Increments once per timed frame. A repeated index with a changed value
	// cannot happen; an unchanged index means the timer produced nothing new.
	std::uint64_t gpuFrameIndex = 0;
};

// Which measurement the decision rested on. Recorded per decision because a
// session can move between tiers - the GPU timer goes quiet during loading -
// and a decision is not interpretable without knowing what it saw.
enum class GovernorTier {
	Headroom,   // GPU time available: climb and descend are both informed
	Frametime,  // no GPU time: censored at the cap, so climbing is a blind probe
};

enum class GovernorAction {
	Hold,
	Climb,    // one rung up, always
	Descend,  // may jump several rungs
};

[[nodiscard]] std::string_view GovernorTierName(GovernorTier a_tier) noexcept;
[[nodiscard]] std::string_view GovernorActionName(GovernorAction a_action) noexcept;

// What the controller decided, and why. The reason is not decoration: D-12
// requires that a session be reconstructible from the logs alone, including
// decisions to do nothing.
struct GovernorDecision
{
	GovernorAction action = GovernorAction::Hold;
	Preset target = Preset::NativeAA;
	GovernorTier tier = GovernorTier::Frametime;
	double atSeconds = 0.0;
	std::string reason;

	// Window statistics the decision was taken on.
	std::size_t samples = 0;
	double p95GpuMs = 0.0;   // 0 when no GPU time
	double meanGpuMs = 0.0;  // logged for comparison against external tools
	double p95FrameMs = 0.0;
	double dropRate = 0.0;
	double headroomMs = 0.0;  // budget - p95Gpu; negative means over budget
	bool censored = false;
};

struct GovernorConfig
{
	double frameBudgetMs = 1000.0 / 72.0;

	// D-10c: absolute milliseconds, not percentages. E-13's 10%/5% were read
	// off an external overlay whose GPU time runs about 1 ms below ours, and
	// 1 ms is 7.2 points of the budget - more than the gap between the two
	// thresholds. Percentages could not survive the translation.
	//
	// Fitted 2026-08-08 by replaying a captured session (E-23), replacing
	// placeholders translated from an external overlay's thresholds.
	//
	// 3.0 is not the sweep's highest-scoring row: 2.5 scored f=0.511 against
	// 0.489, but changed preset 5.15 times a minute against 3.13. Replay has no
	// settle latency at a synthesised preset, so it flatters parameter sets
	// that change often - 4% more pixels for 65% more transitions is the wrong
	// side of D-8.
	//
	// Still thin where it matters: the session behind the fit spent 109 s near
	// the budget, and 71% of its frames had over 30% headroom.
	double marginUpMs = 3.0;    // climb when p95 GPU < budget - this
	double marginDownMs = 0.0;  // descend when p95 GPU > budget - this

	// Consecutive evaluations over the descend line before descending (D-16).
	// One is too few: 8 of 14 changes in the first live session fired at
	// 0.01-0.27 ms over, which is noise, and each cost about six dropped frames
	// plus a rung of quality for the next half-minute. Two costs half a second
	// of response to a genuine overload, which drops nothing.
	int descendConfirmations = 2;

	// --- multi-rung climbing on the headroom tier (D-15) ---
	// Resolution sensitivity used to predict where a climb lands. Defaults to
	// the HIGHEST value measured across sessions (0.61, 0.81, 0.95, 1.29), so
	// the prediction over-states the cost of resolution and the jump
	// under-reaches rather than overshooting.
	double costK = 1.3;
	// Keeps the predicted landing off the descend edge, so a model error does
	// not immediately trigger the descent the climb just caused.
	double landingMarginMs = 1.0;
	// Bounds the damage from a bad k to a known number of rungs.
	int maxClimbRungs = 3;

	// Decision window and cadence.
	double judgeWindowSeconds = 2.0;
	double evalIntervalSeconds = 0.5;
	// No change within this long of the last one. Settle is ~1.0 s (E-2), and
	// pacing slower than the actuator is what keeps us from measuring our own
	// transients (D-8).
	double cooldownSeconds = 3.0;

	// Minimum samples in the window before any decision is taken. A window that
	// has just been cleared says nothing.
	std::size_t minSamples = 30;

	// --- frametime tier only (no GPU time available) ---
	// D-7b: p95 within this fraction of budget, with no drops, means the
	// compositor is holding us and the sample is censored.
	double capTolerance = 0.05;
	double dropFloor = 0.005;
	double dropMax = 0.02;
	// D-4: climbing is a blind probe when censored, so it is paced far slower
	// than descending, and backs off on failure (D-9).
	double probeIntervalSeconds = 20.0;
	double probeIntervalMaxSeconds = 300.0;
	// Clean running after a probe before the backoff is forgotten (D-9's
	// T_reset). Without this a single bad patch of scenery would keep the
	// controller timid for the rest of the session.
	double probeResetSeconds = 120.0;

	[[nodiscard]] static GovernorConfig Default();
};

// Decides, and nothing else.
//
// It owns no clock, no API and no lever: the caller supplies samples and
// applies the result. That is what lets a recorded session be replayed through
// it in CI, which is where Phase 3 chooses the parameters above - deliberately
// not in-game, and deliberately not by feel.
class GovernorCore
{
public:
	explicit GovernorCore(GovernorConfig a_config);

	// Feed one rendered frame. Returns a decision only on evaluation ticks and
	// only when one is warranted; otherwise nullopt, which is the common case.
	std::optional<GovernorDecision> Push(const GovernorSample& a_sample, Preset a_current);

	// The caller tells us what actually happened. A refused or deferred apply
	// must NOT be reported, or the cooldown starts against a change that never
	// took place.
	void NotifyApplied(Preset a_preset, double a_nowSeconds);

	// Clears the window without touching the cooldown or the probe backoff.
	// For a scene cut - a load or a cell change - where old samples describe a
	// place we are no longer in.
	void Reset(double a_nowSeconds);

	[[nodiscard]] GovernorTier Tier() const noexcept { return _tier; }
	[[nodiscard]] double ProbeIntervalSeconds() const noexcept { return _probeInterval; }
	[[nodiscard]] std::size_t WindowSize() const noexcept { return _window.size(); }

private:
	struct Entry
	{
		double t = 0.0;
		double frameMs = 0.0;
		double gpuMs = 0.0;  // 0 when untimed
	};

	void Trim(double a_nowSeconds);
	[[nodiscard]] GovernorDecision Evaluate(double a_nowSeconds, Preset a_current);
	[[nodiscard]] GovernorDecision EvaluateHeadroom(double a_nowSeconds, Preset a_current,
		GovernorDecision a_base);
	[[nodiscard]] GovernorDecision EvaluateFrametime(double a_nowSeconds, Preset a_current,
		GovernorDecision a_base);

	GovernorConfig _config;
	std::deque<Entry> _window;

	GovernorTier _tier = GovernorTier::Frametime;
	std::uint64_t _lastGpuFrameIndex = 0;
	std::size_t _timedSamples = 0;

	double _lastEvalAt = -1.0e9;
	double _lastChangeAt = -1.0e9;
	double _lastProbeAt = -1.0e9;
	double _probeInterval = 20.0;
	// A blind climb is in flight: its outcome decides whether the backoff
	// doubles (D-9). Only ever set on the frametime tier - the headroom tier
	// climbs on evidence, so there is nothing to back off from.
	bool _probeActive = false;

	// What the last returned decision asked for. The caller confirms it through
	// NotifyApplied, and only then does it count: a refused or deferred apply
	// must not start a cooldown or a probe against a change that never
	// happened.
	GovernorAction _pendingAction = GovernorAction::Hold;
	GovernorTier _pendingTier = GovernorTier::Frametime;

	// Consecutive evaluations currently over the descend line. Reset by
	// anything that is not over it, so a trend has to be continuous.
	int _overBudgetEvals = 0;
};

}
