#include "core/Stats.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace csgov;
using Catch::Approx;

TEST_CASE("percentile uses nearest rank", "[stats]")
{
	const std::vector<double> sorted{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	CHECK(PercentileSorted(sorted, 0.0) == Approx(1.0));
	CHECK(PercentileSorted(sorted, 50.0) == Approx(5.0));
	CHECK(PercentileSorted(sorted, 95.0) == Approx(10.0));
	CHECK(PercentileSorted(sorted, 100.0) == Approx(10.0));
	CHECK(PercentileSorted({}, 50.0) == Approx(0.0));
	CHECK(PercentileSorted({ 42.0 }, 95.0) == Approx(42.0));
}

TEST_CASE("stats summarise a window", "[stats]")
{
	const auto stats = ComputeStats({ 10.0, 12.0, 14.0, 11.0, 13.0 }, 0.0);
	CHECK(stats.samples == 5);
	CHECK(stats.meanMs == Approx(12.0));
	CHECK(stats.minMs == Approx(10.0));
	CHECK(stats.maxMs == Approx(14.0));
	CHECK(stats.p50Ms == Approx(12.0));
	CHECK(stats.stdDevMs == Approx(1.4142).margin(0.001));
}

TEST_CASE("miss rate counts frames over budget", "[stats]")
{
	// 72 Hz budget. Two of five frames miss.
	const double budget = 1000.0 / 72.0;  // 13.889
	const auto stats = ComputeStats({ 12.0, 13.0, 14.0, 15.0, 13.5 }, budget);
	CHECK(stats.missRate == Approx(0.4));
}

TEST_CASE("empty stats are inert rather than undefined", "[stats]")
{
	const auto stats = ComputeStats({}, 13.89);
	CHECK_FALSE(stats.Valid());
	CHECK(stats.samples == 0);
	CHECK(stats.meanMs == Approx(0.0));
}

TEST_CASE("frame window is bounded and drops junk", "[stats]")
{
	FrameWindow window{ 3 };
	window.Push(10.0);
	window.Push(11.0);
	window.Push(12.0);
	window.Push(13.0);  // evicts the oldest

	CHECK(window.Size() == 3);
	CHECK(window.Full());
	const auto samples = window.Samples();
	CHECK(samples.front() == Approx(11.0));
	CHECK(samples.back() == Approx(13.0));

	// Zero and negative frametimes are meaningless and must not enter stats.
	window.Push(0.0);
	window.Push(-5.0);
	CHECK(window.Size() == 3);
}

TEST_CASE("settle detector requires a sustained quiet run", "[stats][settle]")
{
	SettleDetector::Config config;
	config.minSamples = 5;
	config.toleranceMs = 1.0;
	config.warmupSamples = 2;
	SettleDetector detector{ config };

	// Warmup frames are ignored entirely - this is the transition spike.
	CHECK_FALSE(detector.Push(40.0));
	CHECK_FALSE(detector.Push(35.0));

	CHECK_FALSE(detector.Push(13.0));
	CHECK_FALSE(detector.Push(13.2));
	CHECK_FALSE(detector.Push(13.1));
	CHECK_FALSE(detector.Push(13.3));
	CHECK(detector.Push(13.0));  // fifth in-band sample settles it
	CHECK(detector.Settled());

	// Once settled it stays settled until reset.
	CHECK_FALSE(detector.Push(30.0));
	CHECK(detector.Settled());
}

TEST_CASE("an outlier restarts the settle run", "[stats][settle]")
{
	SettleDetector::Config config;
	config.minSamples = 4;
	config.toleranceMs = 1.0;
	config.warmupSamples = 0;
	SettleDetector detector{ config };

	CHECK_FALSE(detector.Push(13.0));
	CHECK_FALSE(detector.Push(13.1));
	CHECK_FALSE(detector.Push(13.0));
	CHECK_FALSE(detector.Push(25.0));  // spike - run restarts here
	CHECK_FALSE(detector.Settled());

	// Needs a fresh run of four, so three more must not be enough.
	CHECK_FALSE(detector.Push(25.1));
	CHECK_FALSE(detector.Push(25.0));
	CHECK(detector.Push(25.2));
	CHECK(detector.Settled());
}

TEST_CASE("settle detector never settles on noise", "[stats][settle]")
{
	SettleDetector::Config config;
	config.minSamples = 5;
	config.toleranceMs = 0.5;
	config.warmupSamples = 0;
	SettleDetector detector{ config };

	// Alternating far apart - a frametime that is oscillating badly should
	// never be reported as stable.
	for (int i = 0; i < 50; ++i) {
		const double value = (i % 2 == 0) ? 12.0 : 20.0;
		CHECK_FALSE(detector.Push(value));
	}
	CHECK_FALSE(detector.Settled());
}

TEST_CASE("reset clears settle state", "[stats][settle]")
{
	SettleDetector::Config config;
	config.minSamples = 2;
	config.warmupSamples = 0;
	SettleDetector detector{ config };

	detector.Push(13.0);
	detector.Push(13.0);
	REQUIRE(detector.Settled());

	detector.Reset();
	CHECK_FALSE(detector.Settled());
	CHECK(detector.SamplesSeen() == 0);
}

TEST_CASE("drop rate separates real misses from jitter at the cap", "[stats]")
{
	// The 2026-08-03 failure: at a locked 72 Hz the frametime sits ON the
	// budget, so symmetric jitter puts about half the samples fractionally
	// above it. Counting those as misses reported 33-44% stutter in a scene
	// that felt perfect. dropRate must not be fooled the same way.
	constexpr double budget = 1000.0 / 72.0;  // 13.889 ms

	SECTION("jitter around the cap is not a drop")
	{
		std::vector<double> samples;
		for (int i = 0; i < 100; ++i) {
			samples.push_back(budget + (i % 2 == 0 ? 0.05 : -0.05));
		}
		const auto stats = ComputeStats(samples, budget);

		// Half the samples are over budget...
		REQUIRE(stats.missRate == Approx(0.5).margin(0.01));
		// ...and none of them missed an interval.
		REQUIRE(stats.dropRate == Approx(0.0));
	}

	SECTION("frames that took a second interval are drops")
	{
		std::vector<double> samples(90, budget);
		samples.insert(samples.end(), 10, budget * 2.0);
		const auto stats = ComputeStats(samples, budget);

		REQUIRE(stats.dropRate == Approx(0.1));
	}

	SECTION("a scene genuinely over budget shows both")
	{
		const std::vector<double> samples(100, budget * 1.8);
		const auto stats = ComputeStats(samples, budget);

		REQUIRE(stats.missRate == Approx(1.0));
		REQUIRE(stats.dropRate == Approx(1.0));
	}

	SECTION("no budget means neither is computed")
	{
		const std::vector<double> samples(10, 20.0);
		const auto stats = ComputeStats(samples, 0.0);

		REQUIRE(stats.missRate == Approx(0.0));
		REQUIRE(stats.dropRate == Approx(0.0));
	}
}
