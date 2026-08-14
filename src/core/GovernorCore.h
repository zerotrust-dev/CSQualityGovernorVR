#pragma once

#include "Presets.h"
#include "Stats.h"

#include <array>
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
	// Mean over the window. Only simple mode decides on this; everything else
	// uses the tail, because a locked framerate is lost at the tail.
	double meanFrameMs = 0.0;
	double dropRate = 0.0;
	// D-25: fraction of the window's frames later than budget x missTolerance.
	// Distinct from dropRate, which counts only outright missed intervals at
	// 1.5x budget - far too coarse to steer on. This is the signal the adaptive
	// controller descends on.
	double missRate = 0.0;
	// Longest run of consecutive late frames ending at this evaluation. A burst
	// is felt where the same frames spread thinly are not.
	std::size_t consecutiveMisses = 0;
	// What the adaptive controller required to climb, and what it was made of.
	// Logged so a hold explains itself without anyone reading the code (D-12).
	double climbThresholdFrac = 0.0;
	double rungCostFrac = 0.0;
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

	// --- what a rung costs (D-18) ---
	// How fast a step's measured ratio follows a new observation.
	double stepRatioAlpha = 0.3;
	// A ratio outside this range is not a measurement of a step, it is a scene
	// that changed during the transition. Rejected rather than believed.
	// A rung that costs nothing is not a measurement of a rung. 1.0 exactly is
	// what a harness feeding constant GPU time produces, and the controller
	// believed it until a test caught it.
	double stepRatioMin = 1.01;
	double stepRatioMax = 2.0;
	// Keeps the predicted landing off the descend edge, so a model error does
	// not immediately trigger the descent the climb just caused.
	double landingMarginMs = 1.0;
	// Bounds the damage from a bad k to a known number of rungs.
	int maxClimbRungs = 3;

	// --- D-20: do not re-try a rung that just failed at this headroom ---
	//
	// A climb reversed within this long is treated as having failed rather than
	// as the scene changing. E-33 saw three climbs to the same rung inside 85 s,
	// reversed after 15, 9 and 29 s; E-48 saw nine of fourteen changes follow
	// that shape, each costing about 1.5 s over budget in the headset.
	double climbReversalWindowSeconds = 30.0;
	// A failed rung is unlocked by HEADROOM, not by a clock. Remembering the
	// headroom it failed at and requiring more before trying again means it
	// re-opens when the scene has actually improved - and the observed
	// reversals spanned 9 to 29 s, so no single timeout was even descriptive.
	double climbRetryMarginMs = 0.5;
	// Beyond this the memory is about somewhere else. Mirrors D-9's T_reset.
	double climbFailForgetSeconds = 120.0;

	// --- simple mode: an instrument for building intuition, not a controller ---
	//
	// "Whenever I see 15-20% overhead I think we are ok to go up one preset" is a
	// hypothesis that no amount of replay can answer, because what it is really
	// asking is what the result LOOKS like. So this does exactly that and nothing
	// else, and the person wearing the headset judges.
	//
	// Reads the mean rather than P95 on purpose: the mean is what the readout
	// prints, so it is what the intuition was formed from. Acting on a statistic
	// the observer cannot see would answer a different question. That same choice
	// is why this must not ship as the controller - the mean hides the tail.
	bool simpleMode = false;
	// Climb one rung when mean-GPU overhead reaches this fraction of budget.
	double simpleClimbHeadroomFrac = 0.20;
	// Descend one rung when windowed mean fps falls below this.
	double simpleDescendFps = 70.0;

	// --- D-25 + D-26: the adaptive threshold controller ---
	//
	// Supersedes the landing check for climbing. D-18 and D-24 both put a
	// PREDICTOR between the measurement and the decision, and both failed inside
	// it. This does not have one: it climbs when spare capacity exceeds what a
	// rung is known to cost, and lowers what it demands when experience says it
	// was asking too much.
	//
	// Climb:   headroomP95 / budget  >=  threshold[from]     and clean for a while
	// Descend: missRate over the window is too high, or too many in a row
	//
	// The descend rule is the whole reason this exists. E-49: on 92% of missed
	// frames the GPU timer read UNDER budget, so GPU time cannot see most of
	// what makes frames late. Frame delivery can. GPU time proposes; delivery
	// disposes.
	bool adaptiveMode = false;

	// D-26. threshold[R] = cost of the rung above R, as a fraction of budget,
	// PLUS the margin. Only the margin is a preference; the cost is measured.
	//
	// The player named "15-20%" from feel and the measured cost of leaving
	// UltraPerformance is 15.1% of budget - the number being felt was the price
	// of a rung (E-57). 20% held; 14%, below the price, hunted.
	double climbMarginFrac = 0.05;
	// Used only for a rung whose cost has never been measured. Deliberately
	// pessimistic: an unmeasured rung should under-reach, and the decay below
	// walks it down until experience says otherwise.
	double unknownRungThresholdFrac = 0.30;
	// A threshold relaxes while nothing goes wrong, which is what stops a
	// pessimistic value being self-confirming - the rung can only be learned by
	// climbing it, and the threshold is what refuses the climb (E-54).
	double thresholdDecayFrac = 0.01;
	double thresholdDecaySeconds = 8.0;
	// Raised on a reversal to just above what was attempted, so the same bid is
	// not made twice. Generalises D-20.
	double thresholdRaiseFrac = 0.02;
	// Decay floors at cost + margin for a measured rung, so clean running can
	// only undo a raise from a past failure - never the margin itself. Eroding
	// the margin would walk the demand down to the bare price of the rung, which
	// is the bid that made 14% hunt, and would discard the one number the player
	// actually sets. An unmeasured rung has no resting value and keeps relaxing
	// until a climb teaches it one.

	// Descend when the window's late-frame rate exceeds this...
	double descendMissRate = 0.05;
	// ...or when this many consecutive frames are late, which catches a cliff
	// faster than a windowed rate can.
	std::size_t descendConsecutiveMisses = 3;
	// A frame is late beyond budget x this. 1.05 lands between points on the
	// 1/6 ms measurement grid, so the operative threshold is the next one up
	// (E-56); do not tune this more finely than that.
	double missToleranceFrac = 1.05;
	// Clean running required before a climb is considered at all.
	double climbCleanSeconds = 8.0;

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
	// What the controller currently believes a one-rung climb from this preset
	// costs, as a multiplier on P95 GPU time (D-18). Exposed so a session's log
	// shows the belief, not just the decision it produced.
	[[nodiscard]] double StepRatio(Preset a_from) const noexcept;
	// Whether that belief came from a measurement on this machine or is still
	// the shipped seed. A seed that happens to be wrong and a measurement that
	// is wrong call for opposite responses, and the number alone cannot tell
	// them apart.
	[[nodiscard]] bool StepMeasured(Preset a_from) const noexcept;
	// How many observations that belief rests on. A ratio smoothed from six
	// transitions and one taken from a single transition are different claims,
	// and the number cannot show which it is.
	[[nodiscard]] std::size_t StepObservations(Preset a_from) const noexcept;
	// D-20: 0 when this rung may be climbed to, otherwise the headroom it now
	// requires because a climb to it recently failed.
	[[nodiscard]] double ClimbBlockedByFailure(Preset a_target, double a_headroomMs,
		double a_nowSeconds) const noexcept;

	// D-26: what one step up from each preset actually costs on this machine, in
	// milliseconds of P95 GPU time. Supplied by the caller after the calibration
	// sweep, which already visits all seven presets and until now threw the
	// result away.
	//
	// Measured by direct subtraction of adjacent P95s - no scale-squared, no
	// fitted curve, nothing D-24 was rejected for. A zero entry means that rung
	// was never measured and falls back to unknownRungThresholdFrac.
	//
	// Index by the preset being LEFT, same convention as the step table.
	void SetRungCosts(const std::array<double, kPresets.size()>& a_costMs);

	// The headroom fraction currently demanded to climb from this preset, and
	// the measured rung cost inside it. Diagnostics: a threshold nobody can
	// decompose is a threshold nobody can argue with.
	[[nodiscard]] double ClimbThreshold(Preset a_from) const noexcept;
	[[nodiscard]] double RungCostFraction(Preset a_from) const noexcept;

private:
	struct Entry
	{
		double t = 0.0;
		double frameMs = 0.0;
		double gpuMs = 0.0;  // 0 when untimed
	};

	void Trim(double a_nowSeconds);
	// Records what an adjacent step actually cost. Ignores anything that is not
	// a single upward rung, since a multi-rung move measures a product rather
	// than a step.
	void LearnStepRatio(Preset a_from, double a_p95Before, Preset a_to, double a_p95After);
	// D-20: records whether the climb in flight held or was reversed.
	void NoteClimbOutcome(Preset a_newPreset, double a_nowSeconds);
	[[nodiscard]] GovernorDecision Evaluate(double a_nowSeconds, Preset a_current);
	// The rule as a person would state it: up on overhead, down on fps. See the
	// definition for why it reads the mean.
	[[nodiscard]] GovernorDecision EvaluateSimple(double a_nowSeconds, Preset a_current,
		GovernorDecision a_base);
	// D-25 + D-26.
	[[nodiscard]] GovernorDecision EvaluateAdaptive(double a_nowSeconds, Preset a_current,
		GovernorDecision a_base);
	// Relaxes a rung's threshold after a stretch with nothing going wrong, never
	// below the rung's measured price.
	void DecayThresholds(double a_nowSeconds);
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

	// D-18: what each upward step has actually cost, as a multiplier on P95 GPU
	// time. Index by the preset being left; seeded from measurement and updated
	// by every adjacent change. Six numbers, no model.
	std::array<double, kPresets.size()> _stepRatio{};
	// Whether a step has been measured on THIS machine yet. Until it has, the
	// entry is a seed from someone else's hardware, and the first real
	// observation replaces it outright.
	std::array<bool, kPresets.size()> _stepMeasured{};
	// Diagnostic only - nothing reads this to decide anything. It exists because
	// a belief cannot be argued with until you know how much evidence is behind
	// it.
	std::array<std::size_t, kPresets.size()> _stepObservations{};

	// D-20: what each rung is known to have failed at.
	//
	// Indexed by the rung being climbed TO. Holds the headroom the failed climb
	// was taken on, so a later attempt needs materially more before it is
	// allowed - the rung re-opens when the scene improves rather than when a
	// timer expires.
	std::array<double, kPresets.size()> _climbFailedHeadroom{};
	std::array<double, kPresets.size()> _climbFailedAt{};
	// The climb in flight, so a descent below it can be recognised as that
	// climb having failed rather than as an unrelated decision.
	Preset _lastClimbTarget = Preset::NativeAA;
	double _lastClimbAt = -1.0e9;
	double _lastClimbHeadroom = 0.0;
	bool _climbInFlight = false;

	// --- D-25/D-26 state ---
	//
	// What each rung costs as a fraction of budget, and what is currently
	// demanded to climb it. Indexed by the preset being LEFT.
	std::array<double, kPresets.size()> _rungCostFrac{};
	std::array<double, kPresets.size()> _climbThreshold{};
	bool _rungCostsKnown = false;
	double _lastDecayAt = -1.0e9;
	// When the window last showed trouble. A climb wants a clean stretch behind
	// it, not merely a clean instant.
	double _lastMissAt = -1.0e9;
	// Consecutive late frames, carried across pushes so a burst spanning an
	// evaluation boundary is still seen as a burst.
	std::size_t _consecutiveMisses = 0;

	// The two-point measurement a change gives us for free: the P95 from just
	// before it, held until the window has refilled at the new preset.
	double _p95BeforeChange = 0.0;
	Preset _presetBeforeChange = Preset::NativeAA;
	bool _awaitingCalibration = false;
	// The last evaluation's readings, which become the "before" half.
	double _lastDecisionP95Gpu = 0.0;
	Preset _lastDecisionPreset = Preset::NativeAA;
};

}
