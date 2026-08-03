#include "Reporter.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace csgov {

Reporter::Reporter(std::filesystem::path a_directory, std::string a_sessionStamp) :
	_directory(std::move(a_directory)),
	_stamp(std::move(a_sessionStamp))
{
	std::error_code ec;
	std::filesystem::create_directories(_directory, ec);
}

std::filesystem::path Reporter::Path(std::string_view a_suffix) const
{
	return _directory / (_stamp + std::string{ a_suffix });
}

bool Reporter::OpenTransitions()
{
	_transitions.open(Path("_transitions.csv"));
	if (!_transitions) {
		return false;
	}
	_transitions
		<< "sweep,index,from,to,to_public,scale,pixel_fraction,"
		   "requested_at,applied_at,settled_at,apply_latency_s,settle_latency_s,"
		   "blocked_for_s,apply_attempts,block_at_request,block_worst,"
		   "readback_ok,settled,settle_timed_out,"
		   "steady_n,steady_mean_ms,steady_min_ms,steady_max_ms,steady_p50_ms,"
		   "steady_p95_ms,steady_p99_ms,steady_stddev_ms,steady_miss_rate,"
		   "whole_n,whole_mean_ms,whole_p95_ms,whole_max_ms\n";
	return true;
}

bool Reporter::OpenFrames()
{
	_frames.open(Path("_frames.csv"));
	if (!_frames) {
		return false;
	}
	_frames << "time_s,frame_ms,preset_public,state\n";
	return true;
}

bool Reporter::OpenApiState(std::string_view a_header)
{
	_apiState.open(Path("_apistate.csv"));
	if (!_apiState) {
		return false;
	}
	_apiState << a_header << '\n';
	return true;
}

void Reporter::WriteText(std::string_view a_suffix, std::string_view a_content)
{
	std::ofstream file(Path(a_suffix));
	if (file) {
		file << a_content;
	}
}

void Reporter::WriteApiState(std::string_view a_line)
{
	if (_apiState) {
		_apiState << a_line << '\n';
	}
}

void Reporter::WriteTransition(const TransitionRecord& a_record)
{
	if (!_transitions) {
		return;
	}

	const auto toInfo = FindPreset(a_record.to);
	const std::uint32_t toPublic = toInfo ? toInfo->publicValue : 0;

	_transitions << std::fixed << std::setprecision(4)
				 << a_record.sweep << ',' << a_record.index << ','
				 << PresetName(a_record.from) << ',' << PresetName(a_record.to) << ','
				 << toPublic << ',' << a_record.toScale << ',' << a_record.toPixelFraction << ','
				 << a_record.requestedAt << ',' << a_record.appliedAt << ','
				 << a_record.settledAt << ',' << a_record.ApplyLatencySeconds() << ','
				 << a_record.SettleLatencySeconds() << ',' << a_record.blockedForSeconds << ','
				 << a_record.applyAttempts << ','
				 << '"' << DescribeBlockReasons(a_record.blockReasonsAtRequest) << "\","
				 << '"' << DescribeBlockReasons(a_record.blockReasonsWorst) << "\","
				 << (a_record.readbackMatched ? 1 : 0) << ','
				 << (a_record.settled ? 1 : 0) << ','
				 << (a_record.settleTimedOut ? 1 : 0) << ','
				 << a_record.steady.samples << ',' << a_record.steady.meanMs << ','
				 << a_record.steady.minMs << ',' << a_record.steady.maxMs << ','
				 << a_record.steady.p50Ms << ',' << a_record.steady.p95Ms << ','
				 << a_record.steady.p99Ms << ',' << a_record.steady.stdDevMs << ','
				 << a_record.steady.missRate << ','
				 << a_record.whole.samples << ',' << a_record.whole.meanMs << ','
				 << a_record.whole.p95Ms << ',' << a_record.whole.maxMs << '\n';
	_transitions.flush();  // a crash mid-run must not cost the data already gathered
}

void Reporter::WriteFrame(double a_time, double a_frameTimeMs, std::uint32_t a_presetPublicValue,
	std::string_view a_state)
{
	if (!_frames) {
		return;
	}
	_frames << std::fixed << std::setprecision(4) << a_time << ',' << a_frameTimeMs << ','
			<< a_presetPublicValue << ',' << a_state << '\n';
}

void Reporter::Finish(const std::vector<TransitionRecord>& a_records, double a_budgetMs)
{
	std::ofstream summary(Path("_summary.txt"));
	if (summary) {
		summary << "CS Quality Governor - cycler session " << _stamp << "\n";
		if (!_sessionInfo.empty()) {
			summary << _sessionInfo << "\n";
		}
		summary << "frame budget: " << std::fixed << std::setprecision(3) << a_budgetMs
				<< " ms\ntransitions recorded: " << a_records.size() << "\n\n";

		// Aggregate every visit to the same preset across all sweeps. Repeated
		// visits are what distinguish a real difference from run-to-run noise.
		struct Agg
		{
			int visits = 0;
			double meanSum = 0.0;
			double p95Sum = 0.0;
			double missSum = 0.0;
			double settleSum = 0.0;
			int settledCount = 0;
			int timeoutCount = 0;
			int readbackFail = 0;
			int blockedVisits = 0;
			double worstP95 = 0.0;
			float scale = 1.0f;

			// Spread across repeat visits. A mean without this is untrustworthy:
			// 13/14/15 and 14/14/14 both average to 14, but only one of them is
			// a measurement.
			double minMean = std::numeric_limits<double>::max();
			double maxMean = 0.0;

			// Split by sweep direction. With serpentine traversal, ascending and
			// descending sweeps see the same preset at opposite ends of the
			// session, so the gap between them is the session drift.
			double ascSum = 0.0;
			int ascCount = 0;
			double descSum = 0.0;
			int descCount = 0;
		};

		std::map<std::uint32_t, Agg> byPreset;  // keyed by public value, ordered
		for (const auto& record : a_records) {
			const auto info = FindPreset(record.to);
			auto& agg = byPreset[info ? info->publicValue : 0];
			++agg.visits;
			agg.scale = record.toScale;
			agg.meanSum += record.steady.meanMs;
			agg.p95Sum += record.steady.p95Ms;
			agg.missSum += record.steady.missRate;
			agg.worstP95 = std::max(agg.worstP95, record.steady.p95Ms);
			agg.minMean = std::min(agg.minMean, record.steady.meanMs);
			agg.maxMean = std::max(agg.maxMean, record.steady.meanMs);
			if ((record.sweep % 2) == 0) {
				agg.ascSum += record.steady.meanMs;
				++agg.ascCount;
			} else {
				agg.descSum += record.steady.meanMs;
				++agg.descCount;
			}
			if (record.settled) {
				++agg.settledCount;
				agg.settleSum += record.SettleLatencySeconds();
			}
			if (record.settleTimedOut) {
				++agg.timeoutCount;
			}
			if (!record.readbackMatched) {
				++agg.readbackFail;
			}
			if (record.applyAttempts > 1) {
				++agg.blockedVisits;
			}
		}

		summary << "preset            scale  visits  mean_ms   spread    drift   p95_ms  "
				   "worst_p95   miss%  settle_s  timeouts  rbfail  blocked\n";
		summary << "-------------------------------------------------------------------------"
				   "-------------------------------------------------\n";
		for (const auto& [publicValue, agg] : byPreset) {
			const auto info = FindPresetByPublicValue(publicValue);
			const double n = agg.visits > 0 ? static_cast<double>(agg.visits) : 1.0;
			const double spread =
				agg.visits > 0 && agg.maxMean >= agg.minMean ? agg.maxMean - agg.minMean : 0.0;
			// Only meaningful once both directions have been seen, i.e. from the
			// second sweep onwards. Reported as 0 rather than a fabricated number.
			const double drift = (agg.ascCount > 0 && agg.descCount > 0) ?
			                         (agg.ascSum / agg.ascCount) - (agg.descSum / agg.descCount) :
			                         0.0;
			summary << std::left << std::setw(18)
					<< (info ? std::string{ info->name } : std::string{ "?" }) << std::right
					<< std::setw(5) << std::setprecision(2) << agg.scale << std::setw(8)
					<< agg.visits << std::setw(9) << std::setprecision(2) << (agg.meanSum / n)
					<< std::setw(9) << spread << std::setw(9) << std::showpos << drift
					<< std::noshowpos << std::setw(9) << (agg.p95Sum / n) << std::setw(11)
					<< agg.worstP95
					<< std::setw(8) << std::setprecision(1) << (agg.missSum / n * 100.0)
					<< std::setw(10) << std::setprecision(3)
					<< (agg.settledCount > 0 ? agg.settleSum / agg.settledCount : 0.0)
					<< std::setw(10) << agg.timeoutCount << std::setw(8) << agg.readbackFail
					<< std::setw(9) << agg.blockedVisits << "\n";
		}

		summary << "\nNotes\n"
				<< "  p95/worst_p95 matter more than mean: a locked framerate is lost at the\n"
				<< "  tail, not on average.\n"
				<< "  miss% is the share of frames exceeding the budget.\n"
				<< "  spread is max-min of the per-visit mean. If spread is comparable to the\n"
				<< "  gap between two presets, those presets are not distinguishable in this\n"
				<< "  scene and the ranking between them means nothing.\n"
				<< "  drift is ascending-sweep mean minus descending-sweep mean. Near zero is\n"
				<< "  a stable scene. Large and same-signed across every preset means the\n"
				<< "  session drifted and the absolute numbers are not comparable to another\n"
				<< "  session - though the ranking within this one still survives, which is\n"
				<< "  the reason for reversing alternate sweeps.\n"
				<< "  settle_s is time from the API call to a stable frametime.\n"
				<< "  rbfail counts visits where GetUpscalePreset() did not return the value\n"
				<< "  just set - if non-zero, the API is not doing what it claims.\n";
	}

	if (_transitions) {
		_transitions.flush();
		_transitions.close();
	}
	if (_frames) {
		_frames.flush();
		_frames.close();
	}
	if (_apiState) {
		_apiState.flush();
		_apiState.close();
	}
}

}
