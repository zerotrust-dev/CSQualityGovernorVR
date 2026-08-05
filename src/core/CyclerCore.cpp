#include "CyclerCore.h"

#include <cstdio>

namespace csgov {

std::string_view CyclerStateName(CyclerState a_state) noexcept
{
	switch (a_state) {
	case CyclerState::Idle:     return "Idle";
	case CyclerState::Starting: return "Starting";
	case CyclerState::Applying: return "Applying";
	case CyclerState::Settling: return "Settling";
	case CyclerState::Dwelling: return "Dwelling";
	case CyclerState::Done:     return "Done";
	case CyclerState::Aborted:  return "Aborted";
	}
	return "Unknown";
}

CyclerConfig CyclerConfig::Default()
{
	CyclerConfig config;
	config.order.reserve(kPresets.size());
	for (const auto& info : kPresets) {
		config.order.push_back(info.preset);
	}
	return config;
}

CyclerCore::CyclerCore(ICSApi& a_api, CyclerConfig a_config) :
	_api(a_api),
	_config(std::move(a_config)),
	_settle(_config.settle)
{
	if (_config.order.empty()) {
		_config.order = CyclerConfig::Default().order;
	}
}

void CyclerCore::Log(std::string_view a_message) const
{
	if (_logSink) {
		_logSink(a_message);
	}
}

void CyclerCore::Start(double a_nowSeconds)
{
	if (_state != CyclerState::Idle) {
		return;
	}

	if (!_api.Available()) {
		Abort("Community Shaders interface unavailable");
		return;
	}

	_sweep = 0;
	_orderIndex = 0;
	_records.clear();
	_loggedStartWait = false;
	_state = CyclerState::Starting;
	_stateEnteredAt = a_nowSeconds;

	char buf[192]{};
	std::snprintf(buf, sizeof(buf),
		"armed: %d sweep(s) x %zu preset(s), start delay %.1fs, dwell %.1fs, CS build %u",
		_config.sweeps, _config.order.size(), _config.startDelaySeconds, _config.dwellSeconds,
		_api.BuildNumber());
	Log(buf);
}

void CyclerCore::Abort(std::string_view a_reason)
{
	if (_state == CyclerState::Done || _state == CyclerState::Aborted) {
		return;
	}
	_state = CyclerState::Aborted;
	std::string message = "aborted: ";
	message.append(a_reason);
	Log(message);
}

void CyclerCore::BeginNext(double a_nowSeconds)
{
	if (_orderIndex >= _config.order.size()) {
		_orderIndex = 0;
		++_sweep;
		if (_sweep >= _config.sweeps) {
			_state = CyclerState::Done;
			char buf[96]{};
			std::snprintf(buf, sizeof(buf), "complete: %zu transition record(s)", _records.size());
			Log(buf);
			return;
		}
	}

	// Serpentine traversal: odd sweeps run the ladder backwards.
	//
	// Always visiting cheapest-first makes a preset's position in the sweep a
	// proxy for elapsed time, so anything that drifts during the session - the
	// scene changing around a standing player, GPU thermals, time of day -
	// aliases into the preset ranking. Repeating the same direction three times
	// repeats that bias instead of averaging it away.
	//
	// Reversing alternate sweeps cancels a linear drift in the per-preset mean,
	// and the ascending-vs-descending difference on the same preset measures
	// how much drift there was. Sweep parity in the CSV recovers the direction.
	const bool reversed = _config.serpentine && (_sweep % 2) == 1;
	const std::size_t position =
		reversed ? _config.order.size() - 1 - _orderIndex : _orderIndex;
	const Preset target = _config.order[position];

	_current = TransitionRecord{};
	_current.sweep = _sweep;
	_current.index = static_cast<int>(position);
	_current.from = _api.GetPreset();
	_current.to = target;
	_current.toScale = PresetScale(target);
	_current.toPixelFraction = PresetPixelFraction(target);
	_current.requestedAt = a_nowSeconds;

	_settle.Reset();
	_steadyWindow.Clear();
	_wholeWindow.Clear();

	_state = CyclerState::Applying;
	_stateEnteredAt = a_nowSeconds;
	_lastRetryAt = -1.0e9;

	TryApply(a_nowSeconds);
}

void CyclerCore::TryApply(double a_nowSeconds)
{
	if (a_nowSeconds - _lastRetryAt < _config.retryIntervalSeconds) {
		return;
	}
	_lastRetryAt = a_nowSeconds;

	const std::uint32_t reasons = _api.BlockReasons();
	_current.blockReasonsWorst |= reasons;
	if (_current.applyAttempts == 0) {
		_current.blockReasonsAtRequest = reasons;
	}
	++_current.applyAttempts;

	if (IsTerminalBlock(reasons)) {
		// OpenComposite upscaling blocks CS upscaling for the whole session.
		// Retrying can never succeed.
		Abort("terminal block: " + DescribeBlockReasons(reasons));
		return;
	}

	if (reasons != 0 || !_api.ApplyAllowed()) {
		if (a_nowSeconds - _current.requestedAt >= _config.blockGiveUpSeconds) {
			char buf[192]{};
			std::snprintf(buf, sizeof(buf),
				"giving up on %s after %.1fs blocked (%s)",
				PresetName(_current.to).data(),
				a_nowSeconds - _current.requestedAt,
				DescribeBlockReasons(_current.blockReasonsWorst).c_str());
			Log(buf);
			_current.blockedForSeconds = a_nowSeconds - _current.requestedAt;
			FinishCurrent(a_nowSeconds);
		}
		return;
	}

	_api.SetPreset(_current.to);
	_current.appliedAt = a_nowSeconds;
	_current.blockedForSeconds = a_nowSeconds - _current.requestedAt;
	_current.readbackMatched = (_api.GetPreset() == _current.to);

	_state = CyclerState::Settling;
	_stateEnteredAt = a_nowSeconds;

	char buf[224]{};
	std::snprintf(buf, sizeof(buf),
		"sweep %d [%d] %s -> %s (scale %.3f, %.1f%% px) attempts=%d blocked=%.2fs readback=%s",
		_current.sweep, _current.index,
		PresetName(_current.from).data(), PresetName(_current.to).data(),
		_current.toScale, _current.toPixelFraction * 100.0f,
		_current.applyAttempts, _current.blockedForSeconds,
		_current.readbackMatched ? "ok" : "MISMATCH");
	Log(buf);
}

void CyclerCore::FinishCurrent(double a_nowSeconds)
{
	_current.steady = _steadyWindow.Stats(_config.frameBudgetMs);
	_current.whole = _wholeWindow.Stats(_config.frameBudgetMs);

	_records.push_back(_current);
	if (_recordSink) {
		_recordSink(_current);
	}

	char buf[256]{};
	std::snprintf(buf, sizeof(buf),
		"  settled=%s in %.3fs | steady n=%zu mean=%.2f p95=%.2f p99=%.2f miss=%.1f%%",
		_current.settled ? "yes" : (_current.settleTimedOut ? "TIMEOUT" : "no"),
		_current.SettleLatencySeconds(),
		_current.steady.samples, _current.steady.meanMs,
		_current.steady.p95Ms, _current.steady.p99Ms,
		_current.steady.missRate * 100.0);
	Log(buf);

	++_orderIndex;
	BeginNext(a_nowSeconds);
}

void CyclerCore::Tick(double a_nowSeconds, double a_frameTimeMs)
{
	switch (_state) {
	case CyclerState::Idle:
	case CyclerState::Done:
	case CyclerState::Aborted:
		return;

	case CyclerState::Starting:
		{
			_wholeWindow.Push(a_frameTimeMs);
			const double waited = a_nowSeconds - _stateEnteredAt;
			if (waited < _config.startDelaySeconds) {
				return;
			}

			// Wait for CS to actually accept changes, not just for a timer.
			//
			// kLoadingMenu stays asserted long after the game is playable and
			// for a wildly variable time: 31 s on 2026-08-04, but 115 s on
			// 2026-08-05 from the same save, while frames flowed at a steady
			// 14 ms throughout. A fixed delay cannot straddle that. Tuning it
			// upwards only trades lost transitions for wasted session time, and
			// on 2026-08-05 it still burned the first three visits of sweep 0.
			if (!_api.ApplyAllowed()) {
				if (waited < _config.startMaxWaitSeconds) {
					if (!_loggedStartWait) {
						_loggedStartWait = true;
						Log("start delay elapsed but CS is not accepting changes yet; "
							"waiting for it to clear");
					}
					return;
				}
				char buf[160]{};
				std::snprintf(buf, sizeof(buf),
					"CS still refusing after %.0fs (reasons 0x%X) - starting anyway so the "
					"session is not wasted",
					waited, _api.BlockReasons());
				Log(buf);
			} else if (_loggedStartWait) {
				char buf[96]{};
				std::snprintf(buf, sizeof(buf), "CS accepting changes after %.1fs; starting",
					waited);
				Log(buf);
			}

			BeginNext(a_nowSeconds);
		}
		return;

	case CyclerState::Applying:
		_wholeWindow.Push(a_frameTimeMs);
		TryApply(a_nowSeconds);
		return;

	case CyclerState::Settling:
		_wholeWindow.Push(a_frameTimeMs);
		if (_settle.Push(a_frameTimeMs)) {
			_current.settled = true;
			_current.settledAt = a_nowSeconds;
			_state = CyclerState::Dwelling;
			_stateEnteredAt = a_nowSeconds;
			return;
		}
		if (a_nowSeconds - _stateEnteredAt >= _config.settleTimeoutSeconds) {
			// Record the dwell anyway - a preset that never settles is itself
			// a result worth having.
			_current.settleTimedOut = true;
			_state = CyclerState::Dwelling;
			_stateEnteredAt = a_nowSeconds;
		}
		return;

	case CyclerState::Dwelling:
		_wholeWindow.Push(a_frameTimeMs);
		_steadyWindow.Push(a_frameTimeMs);
		if (a_nowSeconds - _stateEnteredAt >= _config.dwellSeconds) {
			FinishCurrent(a_nowSeconds);
		}
		return;
	}
}

}
