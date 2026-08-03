#include "core/CyclerCore.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace csgov;
using Catch::Approx;

namespace {

class FakeApi : public ICSApi
{
public:
	bool available = true;
	unsigned int build = 1234;
	Preset preset = Preset::NativeAA;
	std::uint32_t blockMask = 0;
	bool allowed = true;
	int setCalls = 0;
	// If set, SetPreset silently refuses - models a readback mismatch.
	bool ignoreSet = false;

	bool Available() const override { return available; }
	unsigned int BuildNumber() override { return build; }
	Preset GetPreset() override { return preset; }
	void SetPreset(Preset a_preset) override
	{
		++setCalls;
		if (!ignoreSet) {
			preset = a_preset;
		}
	}
	std::uint32_t BlockReasons() override { return blockMask; }
	bool ApplyAllowed() override { return allowed && blockMask == 0; }
};

CyclerConfig FastConfig(int a_sweeps = 1)
{
	CyclerConfig config = CyclerConfig::Default();
	config.startDelaySeconds = 1.0;
	config.dwellSeconds = 1.0;
	config.settleTimeoutSeconds = 2.0;
	config.retryIntervalSeconds = 0.1;
	config.blockGiveUpSeconds = 3.0;
	config.sweeps = a_sweeps;
	config.settle.minSamples = 3;
	config.settle.warmupSamples = 1;
	config.settle.toleranceMs = 1.0;
	return config;
}

// Drives the core at a fixed cadence with a steady frametime.
void Run(CyclerCore& a_core, double& a_now, double a_seconds, double a_frameMs = 13.0,
	double a_step = 1.0 / 90.0)
{
	const double end = a_now + a_seconds;
	while (a_now < end) {
		a_now += a_step;
		a_core.Tick(a_now, a_frameMs);
	}
}

}

TEST_CASE("refuses to start without the CS interface", "[cycler]")
{
	FakeApi api;
	api.available = false;
	CyclerCore core{ api, FastConfig() };

	core.Start(0.0);
	CHECK(core.State() == CyclerState::Aborted);
	CHECK(api.setCalls == 0);
}

TEST_CASE("waits out the start delay before touching anything", "[cycler]")
{
	FakeApi api;
	CyclerCore core{ api, FastConfig() };

	double now = 0.0;
	core.Start(now);
	CHECK(core.State() == CyclerState::Starting);

	Run(core, now, 0.5);
	CHECK(core.State() == CyclerState::Starting);
	CHECK(api.setCalls == 0);  // must not disturb the game early

	Run(core, now, 0.8);
	CHECK(api.setCalls >= 1);
}

TEST_CASE("a full sweep visits every preset once, in ladder order", "[cycler]")
{
	FakeApi api;
	auto config = FastConfig(1);
	CyclerCore core{ api, config };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 60.0);

	REQUIRE(core.State() == CyclerState::Done);
	const auto& records = core.Records();
	REQUIRE(records.size() == config.order.size());

	for (std::size_t i = 0; i < records.size(); ++i) {
		CHECK(records[i].to == config.order[i]);
		CHECK(records[i].sweep == 0);
		CHECK(records[i].readbackMatched);
		CHECK(records[i].settled);
	}
	// Cheapest first.
	CHECK(records.front().to == Preset::UltraPerformance);
	CHECK(records.back().to == Preset::NativeAA);
}

TEST_CASE("multiple sweeps repeat the ladder", "[cycler]")
{
	FakeApi api;
	auto config = FastConfig(3);
	CyclerCore core{ api, config };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 200.0);

	REQUIRE(core.State() == CyclerState::Done);
	CHECK(core.Records().size() == config.order.size() * 3);
	CHECK(core.Records().back().sweep == 2);
}

TEST_CASE("records capture settle latency and steady-state stats", "[cycler]")
{
	FakeApi api;
	CyclerCore core{ api, FastConfig(1) };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 60.0, 12.5);

	const auto& first = core.Records().front();
	CHECK(first.settled);
	CHECK(first.SettleLatencySeconds() > 0.0);
	CHECK(first.steady.samples > 0);
	CHECK(first.steady.meanMs == Approx(12.5).margin(0.001));
	CHECK(first.steady.missRate == Approx(0.0));  // 12.5 ms is inside a 72 Hz budget
	CHECK(first.whole.samples >= first.steady.samples);
}

TEST_CASE("blocked applies retry without advancing", "[cycler][blocking]")
{
	FakeApi api;
	api.blockMask = static_cast<std::uint32_t>(BlockReason::LoadingMenu);
	CyclerCore core{ api, FastConfig(1) };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 2.0);

	CHECK(core.State() == CyclerState::Applying);
	CHECK(api.setCalls == 0);  // never applied while blocked

	// Clear the block; it should now proceed.
	api.blockMask = 0;
	Run(core, now, 1.0);
	CHECK(api.setCalls >= 1);
	CHECK(core.State() != CyclerState::Applying);
}

TEST_CASE("block reasons are retained on the record", "[cycler][blocking]")
{
	FakeApi api;
	api.blockMask = static_cast<std::uint32_t>(BlockReason::RelatchPending);
	CyclerCore core{ api, FastConfig(1) };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 1.5);
	api.blockMask = 0;
	Run(core, now, 60.0);

	const auto& first = core.Records().front();
	CHECK(first.applyAttempts > 1);
	CHECK((first.blockReasonsWorst &
			  static_cast<std::uint32_t>(BlockReason::RelatchPending)) != 0);
	CHECK(first.blockedForSeconds > 0.0);
}

TEST_CASE("a persistent block gives up rather than hanging the sweep", "[cycler][blocking]")
{
	FakeApi api;
	api.blockMask = static_cast<std::uint32_t>(BlockReason::LoadingMenu);
	auto config = FastConfig(1);
	config.blockGiveUpSeconds = 2.0;
	CyclerCore core{ api, config };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 5.0);

	// It abandoned the first preset and moved on instead of stalling forever.
	CHECK_FALSE(core.Records().empty());
	CHECK(core.Records().front().appliedAt == Approx(0.0));
}

TEST_CASE("OpenComposite blocking aborts the whole run", "[cycler][blocking]")
{
	FakeApi api;
	api.blockMask = static_cast<std::uint32_t>(BlockReason::OpenCompositeUpscaling);
	CyclerCore core{ api, FastConfig(1) };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 5.0);

	// Retrying could never succeed, so it must stop rather than spin.
	CHECK(core.State() == CyclerState::Aborted);
	CHECK(api.setCalls == 0);
}

TEST_CASE("readback mismatch is recorded, not hidden", "[cycler]")
{
	FakeApi api;
	api.ignoreSet = true;  // CS accepts the call but the value does not stick
	CyclerCore core{ api, FastConfig(1) };

	double now = 0.0;
	core.Start(now);
	Run(core, now, 20.0);

	REQUIRE_FALSE(core.Records().empty());
	CHECK_FALSE(core.Records().front().readbackMatched);
	CHECK(api.setCalls > 0);
}

TEST_CASE("frametime that never settles still produces a record", "[cycler]")
{
	FakeApi api;
	auto config = FastConfig(1);
	config.settle.toleranceMs = 0.1;
	CyclerCore core{ api, config };

	double now = 0.0;
	core.Start(now);

	// Oscillate wildly so the detector can never settle.
	const double end = now + 30.0;
	int i = 0;
	while (now < end) {
		now += 1.0 / 90.0;
		core.Tick(now, (i++ % 2 == 0) ? 11.0 : 22.0);
	}

	REQUIRE_FALSE(core.Records().empty());
	const auto& first = core.Records().front();
	CHECK_FALSE(first.settled);
	CHECK(first.settleTimedOut);
	CHECK(first.steady.samples > 0);  // dwell still sampled
}

TEST_CASE("records reach the sink as they complete", "[cycler]")
{
	FakeApi api;
	CyclerCore core{ api, FastConfig(1) };

	int delivered = 0;
	core.SetRecordSink([&](const TransitionRecord&) { ++delivered; });

	double now = 0.0;
	core.Start(now);
	Run(core, now, 60.0);

	CHECK(delivered == static_cast<int>(core.Records().size()));
	CHECK(delivered > 0);
}

TEST_CASE("start is idempotent", "[cycler]")
{
	FakeApi api;
	CyclerCore core{ api, FastConfig(1) };

	double now = 0.0;
	core.Start(now);
	core.Start(now + 0.1);  // second call must not restart or double-arm
	Run(core, now, 60.0);

	CHECK(core.Records().size() == CyclerConfig::Default().order.size());
}

TEST_CASE("serpentine traversal reverses the ladder on odd sweeps", "[cycler]")
{
	// Guards against the change silently doing nothing: with serpentine off the
	// order repeats, with it on sweep 1 must be the exact reverse of sweep 0.
	// If it ever regresses, every per-preset number becomes confounded with
	// session drift again, which is invisible in the output.
	const auto orderOfSweep = [](bool a_serpentine, int a_sweep) {
		FakeApi api;
		CyclerConfig config = FastConfig(a_sweep + 1);
		config.serpentine = a_serpentine;
		CyclerCore core{ api, config };

		std::vector<Preset> seen;
		core.SetRecordSink([&](const TransitionRecord& a_record) {
			if (a_record.sweep == a_sweep) {
				seen.push_back(a_record.to);
			}
		});

		double now = 0.0;
		core.Start(now);
		Run(core, now, 400.0);
		return seen;
	};

	const auto ladder = CyclerConfig::Default().order;
	REQUIRE(ladder.size() > 2);

	SECTION("sweep 0 always runs the ladder forwards")
	{
		REQUIRE(orderOfSweep(true, 0) == ladder);
	}

	SECTION("sweep 1 runs it backwards")
	{
		std::vector<Preset> reversed{ ladder.rbegin(), ladder.rend() };
		REQUIRE(orderOfSweep(true, 1) == reversed);
	}

	SECTION("sweep 2 runs it forwards again")
	{
		REQUIRE(orderOfSweep(true, 2) == ladder);
	}

	SECTION("disabling serpentine keeps every sweep forwards")
	{
		REQUIRE(orderOfSweep(false, 1) == ladder);
	}

	SECTION("every preset is still visited exactly once per sweep")
	{
		auto sorted = orderOfSweep(true, 1);
		auto expected = ladder;
		const auto byValue = [](Preset a_lhs, Preset a_rhs) {
			return static_cast<std::uint32_t>(a_lhs) < static_cast<std::uint32_t>(a_rhs);
		};
		std::sort(sorted.begin(), sorted.end(), byValue);
		std::sort(expected.begin(), expected.end(), byValue);
		REQUIRE(sorted == expected);
	}
}
