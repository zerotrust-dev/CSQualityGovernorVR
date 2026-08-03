#include "Stats.h"

#include <algorithm>
#include <cmath>

namespace csgov {

double PercentileSorted(const std::vector<double>& a_sorted, double a_percentile) noexcept
{
	if (a_sorted.empty()) {
		return 0.0;
	}
	if (a_sorted.size() == 1) {
		return a_sorted.front();
	}

	const double clamped = std::clamp(a_percentile, 0.0, 100.0);
	// Nearest-rank: rank = ceil(p/100 * N), 1-based.
	const auto n = static_cast<double>(a_sorted.size());
	auto rank = static_cast<std::size_t>(std::ceil(clamped / 100.0 * n));
	if (rank == 0) {
		rank = 1;
	}
	if (rank > a_sorted.size()) {
		rank = a_sorted.size();
	}
	return a_sorted[rank - 1];
}

FrameStats ComputeStats(std::vector<double> a_samplesMs, double a_budgetMs) noexcept
{
	FrameStats stats;
	if (a_samplesMs.empty()) {
		return stats;
	}

	std::sort(a_samplesMs.begin(), a_samplesMs.end());

	stats.samples = a_samplesMs.size();
	stats.minMs = a_samplesMs.front();
	stats.maxMs = a_samplesMs.back();

	double sum = 0.0;
	for (const double value : a_samplesMs) {
		sum += value;
	}
	stats.meanMs = sum / static_cast<double>(stats.samples);

	double variance = 0.0;
	for (const double value : a_samplesMs) {
		const double delta = value - stats.meanMs;
		variance += delta * delta;
	}
	stats.stdDevMs = std::sqrt(variance / static_cast<double>(stats.samples));

	stats.p50Ms = PercentileSorted(a_samplesMs, 50.0);
	stats.p95Ms = PercentileSorted(a_samplesMs, 95.0);
	stats.p99Ms = PercentileSorted(a_samplesMs, 99.0);

	if (a_budgetMs > 0.0) {
		const auto misses = std::count_if(a_samplesMs.begin(), a_samplesMs.end(),
			[a_budgetMs](double value) { return value > a_budgetMs; });
		stats.missRate = static_cast<double>(misses) / static_cast<double>(stats.samples);

		const double dropMs = a_budgetMs * kDropThresholdFactor;
		const auto drops = std::count_if(a_samplesMs.begin(), a_samplesMs.end(),
			[dropMs](double value) { return value > dropMs; });
		stats.dropRate = static_cast<double>(drops) / static_cast<double>(stats.samples);
	}

	return stats;
}

FrameWindow::FrameWindow(std::size_t a_capacity) noexcept :
	_capacity(a_capacity == 0 ? 1 : a_capacity)
{}

void FrameWindow::Push(double a_frameTimeMs)
{
	if (!(a_frameTimeMs > 0.0)) {
		return;  // drop non-positive and NaN
	}
	_samples.push_back(a_frameTimeMs);
	while (_samples.size() > _capacity) {
		_samples.pop_front();
	}
}

void FrameWindow::Clear() noexcept
{
	_samples.clear();
}

FrameStats FrameWindow::Stats(double a_budgetMs) const noexcept
{
	return ComputeStats(Samples(), a_budgetMs);
}

std::vector<double> FrameWindow::Samples() const
{
	return { _samples.begin(), _samples.end() };
}

SettleDetector::SettleDetector() noexcept :
	SettleDetector(Config{})
{}

SettleDetector::SettleDetector(Config a_config) noexcept :
	_config(a_config)
{
	if (_config.minSamples == 0) {
		_config.minSamples = 1;
	}
}

void SettleDetector::Reset() noexcept
{
	_run.clear();
	_seen = 0;
	_settled = false;
}

bool SettleDetector::Push(double a_frameTimeMs)
{
	if (_settled) {
		return false;
	}
	if (!(a_frameTimeMs > 0.0)) {
		return false;
	}

	++_seen;
	if (_seen <= _config.warmupSamples) {
		return false;  // skip the transition spike itself
	}

	_run.push_back(a_frameTimeMs);

	// Median of the current run.
	std::vector<double> sorted(_run);
	std::sort(sorted.begin(), sorted.end());
	const double median = PercentileSorted(sorted, 50.0);

	// Any sample outside the band restarts the run from this frame.
	const bool withinBand = std::all_of(_run.begin(), _run.end(),
		[median, this](double value) { return std::fabs(value - median) <= _config.toleranceMs; });

	if (!withinBand) {
		_run.assign(1, a_frameTimeMs);
		return false;
	}

	if (_run.size() >= _config.minSamples) {
		_settled = true;
		return true;
	}

	return false;
}

}
