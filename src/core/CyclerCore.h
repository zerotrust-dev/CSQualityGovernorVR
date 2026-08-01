#pragma once

#include "Presets.h"
#include "Stats.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace csgov {

// Abstract view of the Community Shaders plugin API, so the state machine can
// be driven by a fake in tests. See docs/CS_PLUGIN_API.md for the real thing.
struct ICSApi
{
	virtual ~ICSApi() = default;

	virtual bool Available() const = 0;
	virtual unsigned int BuildNumber() = 0;
	virtual Preset GetPreset() = 0;
	virtual void SetPreset(Preset a_preset) = 0;
	virtual std::uint32_t BlockReasons() = 0;
	virtual bool ApplyAllowed() = 0;
};

enum class CyclerState {
	Idle,        // not started
	Starting,    // waiting out the start delay
	Applying,    // apply requested, or blocked and retrying
	Settling,    // applied, waiting for frametime to stabilise
	Dwelling,    // settled, collecting steady-state samples
	Done,        // sweeps complete
	Aborted,     // gave up (no API, or terminal block)
};

[[nodiscard]] std::string_view CyclerStateName(CyclerState a_state) noexcept;

// One completed preset visit. This is the row the CSV is built from - the
// "cold numbers" the cycler exists to produce.
struct TransitionRecord
{
	int sweep = 0;
	int index = 0;  // position within the sweep

	Preset from = Preset::NativeAA;
	Preset to = Preset::NativeAA;
	float toScale = 1.0f;
	float toPixelFraction = 1.0f;

	double requestedAt = 0.0;   // seconds, plugin clock
	double appliedAt = 0.0;     // when the API call actually went through
	double settledAt = 0.0;     // when frametime stabilised, 0 if never
	double blockedForSeconds = 0.0;

	int applyAttempts = 0;
	std::uint32_t blockReasonsAtRequest = 0;
	std::uint32_t blockReasonsWorst = 0;

	bool readbackMatched = false;  // GetPreset() == requested after apply
	bool settled = false;
	bool settleTimedOut = false;

	// Steady-state statistics, sampled after settling.
	FrameStats steady;
	// Everything from request to end of dwell, including the disturbance.
	FrameStats whole;

	[[nodiscard]] double ApplyLatencySeconds() const noexcept { return appliedAt - requestedAt; }
	[[nodiscard]] double SettleLatencySeconds() const noexcept
	{
		return settled ? settledAt - appliedAt : 0.0;
	}
};

struct CyclerConfig
{
	// Seconds to wait after arming before the first change, so the player can
	// get in-world and stop moving.
	double startDelaySeconds = 20.0;
	// Steady-state sampling time per preset, after settling.
	double dwellSeconds = 12.0;
	// How long to wait for frametime to stabilise before giving up on settling.
	double settleTimeoutSeconds = 8.0;
	// Retry cadence while an apply is blocked.
	double retryIntervalSeconds = 0.5;
	// Give up on a preset entirely if it stays blocked this long.
	double blockGiveUpSeconds = 30.0;

	int sweeps = 3;
	double frameBudgetMs = 1000.0 / 72.0;

	SettleDetector::Config settle{};

	// Visited in this order each sweep. Defaults to the full ladder,
	// cheapest first.
	std::vector<Preset> order{};

	[[nodiscard]] static CyclerConfig Default();
};

// Pure state machine. No SKSE, no D3D, no clock of its own - the caller
// supplies time and frametime, which is what makes it testable.
class CyclerCore
{
public:
	using RecordSink = std::function<void(const TransitionRecord&)>;
	using LogSink = std::function<void(std::string_view)>;

	CyclerCore(ICSApi& a_api, CyclerConfig a_config);

	void SetRecordSink(RecordSink a_sink) { _recordSink = std::move(a_sink); }
	void SetLogSink(LogSink a_sink) { _logSink = std::move(a_sink); }

	// Arm the sweep. nowSeconds is the caller's monotonic clock.
	void Start(double a_nowSeconds);
	void Abort(std::string_view a_reason);

	// Call once per rendered frame. frameTimeMs may be 0 if unavailable, in
	// which case timing still advances but no statistics are collected.
	void Tick(double a_nowSeconds, double a_frameTimeMs);

	[[nodiscard]] CyclerState State() const noexcept { return _state; }
	[[nodiscard]] int CompletedSweeps() const noexcept { return _sweep; }
	[[nodiscard]] const std::vector<TransitionRecord>& Records() const noexcept { return _records; }
	[[nodiscard]] Preset CurrentTarget() const noexcept { return _current.to; }

private:
	void BeginNext(double a_nowSeconds);
	void TryApply(double a_nowSeconds);
	void FinishCurrent(double a_nowSeconds);
	void Log(std::string_view a_message) const;

	ICSApi& _api;
	CyclerConfig _config;

	CyclerState _state = CyclerState::Idle;
	double _stateEnteredAt = 0.0;
	double _lastRetryAt = 0.0;

	int _sweep = 0;
	std::size_t _orderIndex = 0;

	TransitionRecord _current{};
	SettleDetector _settle;
	FrameWindow _steadyWindow{ 4096 };
	FrameWindow _wholeWindow{ 8192 };

	std::vector<TransitionRecord> _records;
	RecordSink _recordSink;
	LogSink _logSink;
};

}
