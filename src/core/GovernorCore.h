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
	// Re-swept 2026-08-08 across a light and a marginal capture, with D-16's
	// landing check active (E-25).
	//
	// margin_up is no longer the climb criterion - the landing check is. Below
	// 2.0 this parameter is inert: every value from 2.0 down to -1.0 produces
	// an identical replay, because the prediction binds first. What remains is
	// its role as a conservatism limiter, and 2.5 is where that limit still
	// keeps both captures inside the 2% over-budget constraint. At 2.0 the
	// light capture breaches it at 2.4%.
	//
	// 2.5 over 3.0 buys about 3.5% more pixels for about 30% more preset
	// changes. That trade was taken deliberately, by the person who sees the
	// transitions, because replay has no settle latency and cannot price them.
	double marginUpMs = 2.5;    // climb when p95 GPU < budget - this
	double marginDownMs = 0.0;  // descend when p95 GPU > budget - this

	// --- multi-rung climbing on the headroom tier (D-15) ---
	// Starting value for the resolution sensitivity used to predict where a
	// climb lands. It is MEASURED from every applied change after that (D-17),
	// because assuming it produced E-26: a scene whose real k was 5.6 against
	// an assumed 1.3, and a controller that hunted for a whole session.
	//
	// Starts pessimistic. A high k over-states what a rung costs, so the first
	// climbs of a session under-reach rather than overshoot, which is the safe
	// direction while nothing has been learnt yet.
	double costK = 3.0;
	// How fast the estimate follows a new observation.
	double costKAlpha = 0.25;
	double costKMin = 0.2;
	double costKMax = 12.0;
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
	// The learnt resolution sensitivity (D-17). Exposed so a session's log can
	// show what the controller believed, not just what it decided.
	[[nodiscard]] double CostK() const noexcept { return _costK; }

private:
	struct Entry
	{
		double t = 0.0;
		double frameMs = 0.0;
		double gpuMs = 0.0;  // 0 when untimed
	};

	void Trim(double a_nowSeconds);
	[[nodiscard]] std::optional<double> SolveK(double a_p95Before, double a_fBefore,
		double a_p95After, double a_fAfter) const;
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

	// D-17: the two-point calibration a change gives us for free. The P95 and
	// pixel fraction from just before the change, held until the window has
	// refilled at the new preset.
	double _costK = 3.0;
	double _p95BeforeChange = 0.0;
	double _fBeforeChange = 0.0;
	bool _awaitingCalibration = false;
	// The last evaluation's readings, which become the "before" half of the
	// calibration when a change is applied.
	double _lastDecisionP95Gpu = 0.0;
	double _lastDecisionF = 0.0;
};

}
