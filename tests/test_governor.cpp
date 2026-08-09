#include "core/GovernorCore.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace csgov;
using Catch::Approx;

namespace {

constexpr double kBudget = 1000.0 / 72.0;  // 13.889 ms

GovernorConfig TestConfig()
{
	GovernorConfig config;
	config.frameBudgetMs = kBudget;
	config.minSamples = 10;
	config.judgeWindowSeconds = 1.0;
	config.evalIntervalSeconds = 0.25;
	config.cooldownSeconds = 1.0;
	return config;
}

// Drives the controller the way the plugin will: feed frames, apply what comes
// back, tell it what happened. Records every decision so a test can assert on
// the sequence rather than on one lucky tick.
struct Harness
{
	GovernorCore core;
	Preset preset = Preset::Balanced;
	double now = 0.0;
	std::vector<GovernorDecision> decisions;
	std::vector<GovernorDecision> changes;

	explicit Harness(GovernorConfig a_config, Preset a_start = Preset::Balanced) :
		core(a_config), preset(a_start)
	{
	}

	// gpuMs <= 0 means the timer produced nothing, i.e. the frametime tier.
	void Run(double a_seconds, double a_frameMs, double a_gpuMs, double a_stepMs = 13.889)
	{
		static std::uint64_t gpuIndex = 0;
		const int steps = static_cast<int>(a_seconds * 1000.0 / a_stepMs);
		for (int i = 0; i < steps; ++i) {
			now += a_stepMs / 1000.0;
			GovernorSample sample;
			sample.nowSeconds = now;
			sample.frameTimeMs = a_frameMs;
			if (a_gpuMs > 0.0) {
				sample.gpuTimeUs = static_cast<std::uint64_t>(a_gpuMs * 1000.0);
				sample.gpuFrameIndex = ++gpuIndex;
			}
			if (auto decision = core.Push(sample, preset)) {
				decisions.push_back(*decision);
				if (decision->action != GovernorAction::Hold) {
					changes.push_back(*decision);
					preset = decision->target;
					core.NotifyApplied(preset, now);
				}
			}
		}
	}
};

}

TEST_CASE("no decision before the window has enough samples", "[governor]")
{
	Harness h{ TestConfig() };
	h.Run(0.1, 13.9, 9.0);
	CHECK(h.decisions.empty());
}

TEST_CASE("spare GPU time climbs", "[governor]")
{
	Harness h{ TestConfig(), Preset::Performance };

	// 9 ms of GPU work against a 13.889 ms budget: nearly 5 ms spare.
	h.Run(20.0, 13.889, 9.0);

	REQUIRE_FALSE(h.changes.empty());
	for (const auto& change : h.changes) {
		CHECK(change.action == GovernorAction::Climb);
		CHECK(change.tier == GovernorTier::Headroom);
	}
	CHECK(PresetScale(h.changes.front().target) > PresetScale(Preset::Performance));
}

TEST_CASE("a large headroom climbs several rungs in one change", "[governor]")
{
	// D-15: one change instead of three. Feeling for the ceiling a rung at a
	// time was forced by censoring, and the headroom tier is not censored.
	auto config = TestConfig();
	Harness h{ config, Preset::UltraPerformance };

	// 6 ms against a 13.889 ms budget - the scene can clearly afford more than
	// the next rung up.
	h.Run(6.0, 13.889, 6.0);

	REQUIRE_FALSE(h.changes.empty());
	const auto& first = h.changes.front();
	CHECK(first.action == GovernorAction::Climb);
	CHECK(PresetScale(first.target) > PresetScale(Preset::Performance));
	CHECK(first.reason.find("rung(s) fit") != std::string::npos);
}

TEST_CASE("a multi-rung climb lands inside the hold band, not past it", "[governor]")
{
	// The failure this guards against: a jump so large that the next
	// evaluation immediately descends, which is the oscillation D-1 exists to
	// prevent.
	auto config = TestConfig();
	Harness h{ config, Preset::UltraPerformance };
	h.Run(4.0, 13.889, 6.0);

	REQUIRE_FALSE(h.changes.empty());
	const auto target = h.changes.front().target;

	// Predict the landing the same way the controller did - by multiplying the
	// measured cost of each rung it passed through - and require it to sit
	// inside the band.
	double predicted = 6.0;
	for (Preset p = Preset::UltraPerformance; p != target;) {
		predicted *= h.core.StepRatio(p);
		const auto next = FindPresetByPublicValue(0);
		(void)next;
		// walk up by scale
		Preset best = p;
		float bestScale = 1e9f;
		for (const auto& info : kPresets) {
			if (info.scale > PresetScale(p) && info.scale < bestScale) {
				bestScale = info.scale;
				best = info.preset;
			}
		}
		if (best == p) {
			break;
		}
		p = best;
	}
	CHECK(predicted <= config.frameBudgetMs - config.marginDownMs);
}

TEST_CASE("the frametime tier still probes one rung at a time", "[governor]")
{
	// D-15 changes the headroom tier only. Without GPU time a climb is still a
	// blind probe, and the only safe probe is a small one (D-4).
	auto config = TestConfig();
	config.probeIntervalSeconds = 1.0;
	Harness h{ config, Preset::UltraPerformance };

	h.Run(6.0, 13.889, 0.0);

	REQUIRE_FALSE(h.changes.empty());
	CHECK(h.changes.front().tier == GovernorTier::Frametime);
	CHECK(h.changes.front().target == Preset::Performance);
}

TEST_CASE("GPU time over budget descends", "[governor]")
{
	Harness h{ TestConfig(), Preset::Quality };
	h.Run(10.0, 16.0, 16.0);

	REQUIRE_FALSE(h.changes.empty());
	CHECK(h.changes.front().action == GovernorAction::Descend);
	CHECK(PresetScale(h.changes.front().target) < PresetScale(Preset::Quality));
}

TEST_CASE("a climb that would land past the band is not taken", "[governor]")
{
	// D-16: every rung of a climb is landing-checked, including the first.
	// "Is there spare capacity now" and "will there still be after paying for
	// the rung" are different questions.
	//
	// The check only bites when the climb threshold is loose enough to let a
	// large step be considered, which is where Q-11 is heading: with
	// margin_up 3.0 the threshold blocks first and this never fires.
	auto config = TestConfig();
	config.marginUpMs = 0.5;
	config.landingMarginMs = 1.0;
	Harness h{ config, Preset::Hoshipa };

	// Inside the climb threshold, but one rung from Hoshipa to NativeAA is a
	// 38% jump in pixels and cannot land inside the band.
	h.Run(20.0, 13.889, 13.0);

	for (const auto& change : h.changes) {
		CHECK(change.action != GovernorAction::Climb);
	}
	REQUIRE_FALSE(h.decisions.empty());
	CHECK(h.decisions.back().reason.find("would land past") != std::string::npos);
}

TEST_CASE("the band between the thresholds holds still", "[governor]")
{
	auto config = TestConfig();
	Harness h{ config, Preset::Balanced };

	// Inside [budget - marginUp, budget - marginDown]: neither direction is
	// justified, and doing nothing is the correct output. D-8: success is few
	// correct changes, not many small ones.
	h.Run(30.0, 13.889, kBudget - 0.1);

	CHECK(h.changes.empty());
	REQUIRE_FALSE(h.decisions.empty());
	CHECK(h.decisions.back().action == GovernorAction::Hold);
	CHECK(h.decisions.back().reason.find("band") != std::string::npos);
}

TEST_CASE("never climbs past the top or descends past the bottom", "[governor]")
{
	{
		Harness h{ TestConfig(), Preset::NativeAA };
		h.Run(30.0, 13.889, 5.0);
		CHECK(h.changes.empty());
		CHECK(h.decisions.back().reason.find("maximum quality") != std::string::npos);
	}
	{
		Harness h{ TestConfig(), Preset::UltraPerformance };
		h.Run(30.0, 30.0, 30.0);
		CHECK(h.changes.empty());
		CHECK(h.decisions.back().reason.find("cheapest preset") != std::string::npos);
	}
}

TEST_CASE("cooldown paces changes at least as slowly as the actuator settles",
	"[governor]")
{
	auto config = TestConfig();
	config.cooldownSeconds = 3.0;
	Harness h{ config, Preset::UltraPerformance };

	h.Run(20.0, 13.889, 6.0);

	REQUIRE(h.changes.size() >= 2);
	// E-2: settle is ~1.0 s, so changing faster than the cooldown means judging
	// the previous transition rather than the scene (D-8).
	for (std::size_t i = 1; i < h.changes.size(); ++i) {
		CHECK(h.changes[i].atSeconds - h.changes[i - 1].atSeconds >= config.cooldownSeconds);
	}
}

TEST_CASE("a stale GPU frame index is not counted as a new sample", "[governor]")
{
	Harness h{ TestConfig(), Preset::Balanced };
	auto& core = h.core;

	// Same index every frame: the timer produced one reading and then stopped.
	// Reusing it would manufacture samples, which is how a whole ladder got
	// inverted once before (E-10).
	for (int i = 0; i < 200; ++i) {
		GovernorSample sample;
		sample.nowSeconds = 0.0139 * (i + 1);
		sample.frameTimeMs = 13.889;
		sample.gpuTimeUs = 9000;
		sample.gpuFrameIndex = 7;  // never advances
		(void)core.Push(sample, h.preset);
	}
	CHECK(core.Tier() == GovernorTier::Frametime);
}

TEST_CASE("falls back to the frametime tier when GPU time is absent", "[governor]")
{
	Harness h{ TestConfig(), Preset::Balanced };

	// Capped and clean: censored per D-7b, so climbing can only be a probe.
	h.Run(5.0, 13.889, 0.0);

	REQUIRE_FALSE(h.decisions.empty());
	CHECK(h.decisions.back().tier == GovernorTier::Frametime);
	CHECK(h.decisions.back().censored);
}

TEST_CASE("frametime tier descends on drops without any GPU time", "[governor]")
{
	Harness h{ TestConfig(), Preset::Quality };

	// Well past 1.5x budget: dropped frames, not merely over budget (D-7a).
	h.Run(5.0, 30.0, 0.0);

	REQUIRE_FALSE(h.changes.empty());
	CHECK(h.changes.front().action == GovernorAction::Descend);
	CHECK(h.changes.front().tier == GovernorTier::Frametime);
	CHECK(h.changes.front().reason.find("drop rate") != std::string::npos);
}

TEST_CASE("a failed probe doubles the probe interval", "[governor]")
{
	auto config = TestConfig();
	config.probeIntervalSeconds = 2.0;
	config.probeIntervalMaxSeconds = 60.0;
	Harness h{ config, Preset::Performance };

	const double before = h.core.ProbeIntervalSeconds();

	// Censored, so it probes upward blind; then the scene drops frames, so the
	// probe is judged a failure.
	h.Run(6.0, 13.889, 0.0);
	h.Run(6.0, 30.0, 0.0);

	CHECK(h.core.ProbeIntervalSeconds() > before);
}

TEST_CASE("replays a censored ladder the way the real captures behave", "[governor]")
{
	// The shape measured on 2026-08-06 (E-17): four presets pinned at the cap
	// with frametimes that cannot be told apart, and GPU times that can.
	// A frametime-only controller sees nothing to act on here; the headroom
	// tier must still find the spare capacity.
	struct Rung
	{
		Preset preset;
		double frameMs;
		double gpuMs;
	};
	const std::vector<Rung> ladder{
		{ Preset::UltraPerformance, 13.89, 11.73 },
		{ Preset::Performance, 13.96, 12.47 },
		{ Preset::Balanced, 13.85, 13.19 },
		{ Preset::Quality, 13.84, 14.00 },
	};

	const auto config = TestConfig();
	const double climbAt = config.frameBudgetMs - config.marginUpMs;
	const double descendAt = config.frameBudgetMs - config.marginDownMs;

	for (const auto& rung : ladder) {
		Harness h{ config, rung.preset };
		h.Run(8.0, rung.frameMs, rung.gpuMs);

		REQUIRE_FALSE(h.decisions.empty());
		const auto& last = h.decisions.back();

		// Whatever it decides, it must decide it on GPU time: every one of these
		// frametimes is censored and they are indistinguishable from each other
		// (E-1). This is the case a frametime-driven controller cannot see.
		CHECK(last.tier == GovernorTier::Headroom);
		CHECK(last.censored);
		CHECK(last.p95GpuMs > 0.0);

		if (rung.gpuMs < climbAt) {
			REQUIRE_FALSE(h.changes.empty());
			CHECK(h.changes.front().action == GovernorAction::Climb);
		} else if (rung.gpuMs > descendAt) {
			REQUIRE_FALSE(h.changes.empty());
			CHECK(h.changes.front().action == GovernorAction::Descend);
		} else {
			CHECK(h.changes.empty());
		}
	}
}

TEST_CASE("a step's cost is measured, not assumed", "[governor]")
{
	// D-18. The controller starts from a seed measured on real sweeps and
	// replaces it with what the step actually costs here. A single fitted k
	// could not do this: the implied k across the ladder runs from 0.21 to
	// 2.27, so it misfits whichever end it is not fitted to (E-27).
	auto config = TestConfig();
	config.stepRatioAlpha = 1.0;  // adopt the observation whole, for a clean assertion
	// One rung at a time: only an adjacent change measures a step, and with
	// several rungs affordable the controller would take them in one move and
	// learn nothing.
	config.maxClimbRungs = 1;
	Harness h{ config, Preset::Balanced };

	const double seeded = h.core.StepRatio(Preset::Balanced);
	CHECK(seeded > 1.0);

	// Climb from Balanced, then present a cost 30% higher at the new preset.
	h.Run(4.0, 13.889, 9.0);
	REQUIRE_FALSE(h.changes.empty());
	CHECK(h.changes.front().target == Preset::Quality);
	h.Run(4.0, 13.889, 9.0 * 1.30);

	CHECK(h.core.StepRatio(Preset::Balanced) == Approx(1.30).margin(0.02));
}

TEST_CASE("only adjacent steps teach a step's cost", "[governor]")
{
	// A multi-rung move measures the product of several steps. Attributing it
	// to one would corrupt every prediction that uses it.
	auto config = TestConfig();
	config.stepRatioAlpha = 1.0;
	Harness h{ config, Preset::UltraPerformance };

	const double before = h.core.StepRatio(Preset::UltraPerformance);
	h.Run(4.0, 13.889, 5.0);  // large headroom: a multi-rung climb
	REQUIRE_FALSE(h.changes.empty());
	REQUIRE(h.changes.front().target != Preset::Performance);  // skipped a rung
	h.Run(4.0, 13.889, 9.0);

	CHECK(h.core.StepRatio(Preset::UltraPerformance) == Approx(before));
}

TEST_CASE("an implausible ratio is rejected rather than believed", "[governor]")
{
	// A scene that changes during the transition produces a ratio that is not a
	// measurement of the step. Keeping the previous number beats adopting it,
	// because it feeds the next climb's landing.
	auto config = TestConfig();
	config.stepRatioAlpha = 1.0;
	Harness h{ config, Preset::Balanced };

	const double before = h.core.StepRatio(Preset::Balanced);
	h.Run(4.0, 13.889, 9.0);
	REQUIRE_FALSE(h.changes.empty());
	// GPU time halves after climbing - impossible for a step, so discard it.
	h.Run(4.0, 13.889, 4.5);

	CHECK(h.core.StepRatio(Preset::Balanced) == Approx(before));
}

TEST_CASE("the first measurement replaces the seed rather than blending", "[governor]")
{
	// The seeds come from one machine. Treating them as a prior worth averaging
	// against would leave most of that machine in someone else's estimate after
	// their first transition. Smoothing is for repeated observations of the
	// same thing, not for a measurement against a guess.
	auto config = TestConfig();
	config.stepRatioAlpha = 0.3;  // slow smoothing, so blending would be visible
	config.maxClimbRungs = 1;     // an adjacent change is what measures a step
	Harness h{ config, Preset::Balanced };

	const double seed = h.core.StepRatio(Preset::Balanced);
	REQUIRE(seed > 1.05);

	h.Run(4.0, 13.889, 9.0);
	REQUIRE_FALSE(h.changes.empty());
	REQUIRE(h.changes.front().target == Preset::Quality);
	h.Run(4.0, 13.889, 9.0 * 1.30);

	// Adopted whole, not 30% of the way from the seed.
	CHECK(h.core.StepRatio(Preset::Balanced) == Approx(1.30).margin(0.02));
}
