#include "ApiProbe.h"
#include "CompositorTimer.h"
#include "CSXTransitionApi.h"
#include "Config.h"
#include "Reporter.h"
#include "core/CyclerCore.h"
#include "core/GovernorCore.h"

#include "VRAPI/CSinterface001.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <vector>

CSPluginAPI::ICSInterface001* g_CSInterface = nullptr;

namespace csgov {

namespace {

// Unix epoch milliseconds, UTC. The join key against other tools' logs.
std::uint64_t WallClockMs()
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

std::string TimeStamp()
{
	const auto now = std::chrono::system_clock::now();
	const auto time = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
	localtime_s(&tm, &time);
	char buf[32]{};
	std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
	return buf;
}

// Loaded once at startup. Declared here rather than beside the other globals
// because LiveCSApi reads it, and a namespace-scope name used in an inline
// member function body must already be declared.
PluginConfig g_config;

// Which source supplies GPU time, kept apart because they fail differently.
// Our own hooks are installed once and read 0 until the first submit hands us
// a device; the CS path is a vtable call that crashes outright on a build that
// does not have it (E-34), so it must never be reached by falling through.
bool g_ownGpuTimer = false;
bool g_csGpuTimer = false;

// D-22: CSX's transition-profile API, or null.
//
// Non-null ONLY when getBuildNumber() is an exact match for a build confirmed
// to carry it at slot 23. Never inferred from the revision: ours and theirs
// both say 4, and slot 23 holds a different function in each (E-34, E-36).
CSXInterface001* g_csxTransition = nullptr;
CSXInterface001* TransitionApi() noexcept { return g_csxTransition; }

// D-23b: a preset change waiting for a known-good frame to be captured before
// it is applied. Empty the rest of the time.
std::optional<Preset> g_pendingPreset;

// ---------------------------------------------------------------------------
// Real CS API, behind the same interface the tests fake.
//
// Every method here is reached from FrameSource's per-frame task, which runs on
// the game thread. Keep it that way: CS is not documented as safe to call from
// anywhere else, and nothing in this class does its own marshalling.
// ---------------------------------------------------------------------------
class LiveCSApi final : public ICSApi
{
public:
	bool Available() const override { return g_CSInterface != nullptr; }

	unsigned int BuildNumber() override
	{
		return g_CSInterface ? g_CSInterface->getBuildNumber() : 0;
	}

	Preset GetPreset() override
	{
		if (!g_CSInterface) {
			return Preset::NativeAA;
		}
		const auto raw = static_cast<std::uint32_t>(g_CSInterface->GetUpscalePreset());
		if (const auto info = FindPresetByPublicValue(raw)) {
			return info->preset;
		}
		return Preset::NativeAA;
	}

	// D-22: apply through CS's transition path when the build has it.
	//
	// SetUpscalePreset changes the render scale with no fade, so the relatch is
	// visible - a gridded panel for a split second on every change (E-38). CSX
	// documents the contract for external controllers: preflight, and on kApply
	// schedule the door fade and apply for the current method.
	void SetPreset(Preset a_preset) override
	{
		if (!g_CSInterface) {
			return;
		}
		const auto info = FindPreset(a_preset);
		if (!info) {
			return;
		}
		const auto preset = static_cast<CSPluginAPI::UpscalePreset>(info->publicValue);

		// D-23b: get a KNOWN-GOOD frame in hand before asking for the change.
		//
		// The change is deferred by a frame or two while the capture completes.
		// Capturing after the request was the whole problem: if CS begins
		// tearing down its targets immediately, the frame we copied was already
		// the relatch, and replaying it held the artefact on screen for the
		// entire window instead of hiding it (E-45).
		//
		// Nothing is applied on this call when a capture is needed; the frame
		// loop applies it as soon as one exists.
		if (g_config.transitionHoldFrames > 0 && CompositorTimer::Active() &&
			!CompositorTimer::CaptureReady()) {
			CompositorTimer::ArmCapture();
			g_pendingPreset = a_preset;
			return;
		}

		const auto hold = [](bool a_changing) {
			if (a_changing && g_config.transitionHoldFrames > 0) {
				CompositorTimer::HoldFrames(
					static_cast<std::uint32_t>(g_config.transitionHoldFrames));
			}
		};

		if (auto* csx = TransitionApi()) {
			const auto method = csx->GetUpscaleMethod();
			const bool renderScale = csx->GetRenderAtUpscaleResEnabled();
			const auto profile = csx->GetDLSSProfile();

			const auto decision = static_cast<TransitionDecision>(
				csx->GetVRUpscalingTransitionProfileDecision(method, renderScale, preset, profile));

			switch (decision) {
			case TransitionDecision::NoChange:
				// Already there. Applying anyway would schedule a fade for a
				// change that does not exist - a flash bought for nothing.
				return;
			case TransitionDecision::Blocked:
				// Leave it to the caller's buffer-and-retry. Asking first is the
				// difference between deferring and being refused.
				return;
			case TransitionDecision::Apply:
				hold(true);
				csx->SetVRUpscalingTransitionProfileForMethod(
					method, renderScale, preset, profile);
				return;
			}
			return;
		}

		hold(true);
		g_CSInterface->SetUpscalePreset(preset);
	}

	std::uint32_t BlockReasons() override
	{
		return g_CSInterface ? g_CSInterface->GetVRUpscalingApplyBlockReasons() : 0;
	}

	bool ApplyAllowed() override
	{
		return g_CSInterface && g_CSInterface->IsVRUpscalingProfileApplyAllowed();
	}

	// D-21: our own timer first, the fork's API only as a fallback.
	//
	// Ours works against whatever Community Shaders the modlist ships, so it is
	// preferred whenever it is running - that is the whole point of moving the
	// brackets here. The CS path stays for the fork, which remains the vehicle
	// for the upstream patch.
	//
	// The CS calls are revision-4-only and guarded by GpuTimingAvailable(); see
	// the note on that function for why calling them unguarded is not a graceful
	// failure but a crash (E-34).
	std::uint64_t GpuTimeUs() const
	{
		if (g_ownGpuTimer) {
			// 0 until the first submit supplies a device, which the caller
			// already treats as "no measurement" rather than "the GPU was idle".
			return CompositorTimer::LastFrameGpuTimeUs();
		}
		if (g_csGpuTimer && g_CSInterface) {
			return g_CSInterface->GetLastFrameGpuTimeUs();
		}
		return 0;
	}

	std::uint64_t GpuFrameIndex() const
	{
		if (g_ownGpuTimer) {
			return CompositorTimer::LastFrameGpuTimeFrameIndex();
		}
		if (g_csGpuTimer && g_CSInterface) {
			return g_CSInterface->GetLastFrameGpuTimeFrameIndex();
		}
		return 0;
	}
};

// ---------------------------------------------------------------------------
// Frame source.
//
// Samples the game's own frame delta and treats each change as a new frame.
// This avoids hooking Present, which needs VR-specific offsets that cannot be
// verified without the game.
//
// Delivers exactly one callback per rendered frame, on the game thread.
//
// A thread owns the pacing; the task does the work. Each iteration posts ONE
// task and then waits for it to run before posting the next. Since SKSE drains
// its queue once per frame, one task per drain means one callback per frame -
// so the frame boundary comes from SKSE's own cadence rather than from us
// trying to infer it.
//
// Two failures this design exists to avoid, both measured:
//
//   1. The task must NEVER re-arm itself. That was tried on 2026-08-04 and hung
//      the game every launch, the moment the load-game menu appeared: SKSE runs
//      tasks added during a drain within that same drain, so a self-re-arming
//      task never yields the main thread. Re-arming from the thread is what
//      makes this safe - the next task is only posted after the previous one
//      has returned, so a drain never sees more than one of ours.
//
//   2. There is no delta-dedup, because dedup was silently destroying the data.
//      Skipping frames whose delta equalled the previous one meant the steadier
//      a preset ran, the fewer of its frames we kept - and what survived was
//      disproportionately jitter. On 2026-08-05 that captured 36% of Balanced's
//      frames against 75% of Quality's in the same 8 s dwell, and reported
//      Quality as FASTER than Balanced despite rendering 28% more pixels.
//      Running once per frame removes the need to detect frames at all.
//
// Health is now reported as capture completeness: the sum of the frametimes we
// observed against wall-clock elapsed. Near 1.0 means we saw every frame. Well
// below 1.0 means the queue is draining less often than once per frame, and the
// data should be distrusted rather than quietly used.
// ---------------------------------------------------------------------------
class FrameSource
{
public:
	using Callback = std::function<void(double nowSeconds, double frameTimeMs)>;

	void Start(Callback a_callback)
	{
		_callback = std::move(a_callback);
		_start = std::chrono::steady_clock::now();
		_running = true;
		_thread = std::thread([this] { Run(); });
	}

	// Ask the pump to stop. Safe to call from inside the callback, which is
	// exactly what happens when the sweep completes.
	void Quiesce() noexcept
	{
		_running = false;
		_pending.store(false, std::memory_order_release);
	}

	// Never call this from inside the callback - the callback runs on the game
	// thread, not the pacing thread, so this is safe there, but joining a thread
	// from within itself terminates the process. That crashed the game twice on
	// 2026-08-03.
	void Join()
	{
		Quiesce();
		if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id()) {
			_thread.join();
		}
	}

	[[nodiscard]] std::uint64_t Frames() const noexcept { return _frames.load(); }

	// Seconds of frametime actually observed, from the deltas themselves.
	[[nodiscard]] double SampledSeconds() const noexcept
	{
		return static_cast<double>(_sampledMicros.load()) / 1.0e6;
	}

	[[nodiscard]] double ElapsedSeconds() const noexcept
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - _start).count();
	}

	// Start the capture-ratio window. Called once, when the sweep actually
	// begins.
	//
	// The window must not include the wait for CS to accept changes: on
	// 2026-08-06 that wait was 80 s of a 333 s session, and counting it as
	// elapsed reported 76.8% capture for a run that had in fact captured
	// essentially every frame (574-577 samples per 8 s dwell against ~576
	// expected). A health metric that cries wolf is worse than none, because
	// the next real warning gets ignored.
	void MarkRunStart() noexcept
	{
		_runStart = std::chrono::steady_clock::now();
		_runStartMicros.store(_sampledMicros.load(), std::memory_order_release);
		_runMarked.store(true, std::memory_order_release);
	}

	// 1.0 means every frame was seen. Anything much lower invalidates the run.
	[[nodiscard]] double CaptureRatio() const noexcept
	{
		if (!_runMarked.load(std::memory_order_acquire)) {
			const double elapsed = ElapsedSeconds();
			return elapsed > 0.0 ? SampledSeconds() / elapsed : 0.0;
		}
		const double elapsed =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - _runStart).count();
		const double sampled =
			static_cast<double>(_sampledMicros.load() - _runStartMicros.load()) / 1.0e6;
		return elapsed > 0.0 ? sampled / elapsed : 0.0;
	}

private:
	void Run()
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			_running = false;
			logger::error("no SKSE task interface; the cycler cannot be driven");
			return;
		}

		while (_running) {
			_pending.store(true, std::memory_order_release);
			tasks->AddTask([this] { Pump(); });

			// Wait for that task to run before posting another. This is the
			// whole safety property: at most one of our tasks per drain.
			while (_pending.load(std::memory_order_acquire) && _running) {
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		}
	}

	void Pump()
	{
		if (_running) {
			const float delta = RE::GetSecondsSinceLastFrame();
			if (delta > 0.0f) {
				++_frames;
				_sampledMicros.fetch_add(static_cast<std::uint64_t>(delta * 1.0e6f));
				if (_callback) {
					_callback(ElapsedSeconds(), static_cast<double>(delta) * 1000.0);
				}
			}
		}
		_pending.store(false, std::memory_order_release);
	}

	Callback _callback;
	std::thread _thread;
	std::chrono::steady_clock::time_point _start{};
	std::chrono::steady_clock::time_point _runStart{};
	std::atomic_bool _running{ false };
	std::atomic_bool _pending{ false };
	std::atomic_bool _runMarked{ false };
	std::atomic_uint64_t _frames{ 0 };
	std::atomic_uint64_t _sampledMicros{ 0 };
	std::atomic_uint64_t _runStartMicros{ 0 };
};

// ---------------------------------------------------------------------------

LiveCSApi g_api;
std::unique_ptr<CyclerCore> g_cycler;
std::unique_ptr<GovernorCore> g_governor;
std::unique_ptr<Reporter> g_reporter;
FrameSource g_frames;
std::atomic_bool g_finished{ false };
unsigned int g_revisionAcquired = 0;
bool g_gpuTiming = false;
double g_lastApiSample = -1.0;
// Public preset value as last read from CS. Only used once the sweep is over,
// when nothing of ours is driving the lever.
std::uint32_t g_monitorPreset = 0;
// Set once when monitoring starts, if the config asks for a specific preset.
// Cleared when it has been applied - CS refuses changes during loading, so it
// may take several attempts.
int g_pendingMonitorPreset = -1;

// The latest target the governor wanted while CS was refusing changes. Latest
// wins: an older target describes a scene we have already left.
std::optional<Preset> g_bufferedTarget;
// Set on a terminal block, which lasts the whole session. Retrying after one
// would be a spin, not a recovery.
bool g_governorDisabled = false;

// Timestamps of applied changes, for the circuit breaker. There is no in-game
// way to stop the governor, so it has to be able to stop itself.
std::deque<double> g_appliedAt;

// Records an applied change and returns false if the rate has run away, in
// which case the governor is disabled for the session and the preset is left
// wherever it currently is.
bool RegisterApplyAndCheckRate(double a_nowSeconds)
{
	g_appliedAt.push_back(a_nowSeconds);
	while (!g_appliedAt.empty() && a_nowSeconds - g_appliedAt.front() > 60.0) {
		g_appliedAt.pop_front();
	}
	if (static_cast<int>(g_appliedAt.size()) <= g_config.maxChangesPerMinute) {
		return true;
	}

	logger::error("[governor] {} changes in the last minute exceeds MaxChangesPerMinute={}. "
				  "Disabling for this session and leaving the preset alone. This is a runaway, "
				  "not a tuning problem - the timeline CSV has every decision and its reason.",
		g_appliedAt.size(), g_config.maxChangesPerMinute);
	g_governorDisabled = true;
	return false;
}

// ---------------------------------------------------------------------------
// Live GPU-time readout.
//
// One line per second: framerate, frametime, GPU time and the headroom that
// follows from it. This exists to be checked against PrimaShock's overlay - if
// our GPU time tracks its overhead figure across a preset sweep, the timestamp
// bracket in the forked Community Shaders excludes the compositor wait; if ours
// stays flat while PrimaShock's moves, the bracket is wrong and the number is
// just frametime again (design doc D-13).
//
// It is aggregated over a second rather than logged per frame because a per-
// frame line at 72 Hz is unreadable while playing, and the comparison is with
// something a human reads off a HUD.
// ---------------------------------------------------------------------------
struct GpuReadout
{
	double windowStart = -1.0;
	std::uint32_t frames = 0;
	double frameMsSum = 0.0;
	double gpuMsSum = 0.0;
	std::uint32_t gpuSamples = 0;
	std::uint64_t lastGpuFrameIndex = 0;
	std::uint32_t staleFrames = 0;

	void Add(double a_now, double a_frameTimeMs, std::uint64_t a_gpuUs,
		std::uint64_t a_gpuFrameIndex, double a_budgetMs, std::string_view a_presetName)
	{
		if (windowStart < 0.0) {
			windowStart = a_now;
		}
		++frames;
		frameMsSum += a_frameTimeMs;
		if (a_gpuUs > 0) {
			if (a_gpuFrameIndex != lastGpuFrameIndex) {
				lastGpuFrameIndex = a_gpuFrameIndex;
			} else {
				++staleFrames;
			}
			gpuMsSum += static_cast<double>(a_gpuUs) / 1000.0;
			++gpuSamples;
		}

		const double elapsed = a_now - windowStart;
		if (elapsed < 1.0 || frames == 0) {
			return;
		}

		const double meanFrameMs = frameMsSum / frames;
		const double fps = meanFrameMs > 0.0 ? 1000.0 / meanFrameMs : 0.0;
		if (gpuSamples == 0) {
			logger::info("[readout] {} | {:.1f} fps | frame {:.2f} ms | gpu n/a", a_presetName, fps,
				meanFrameMs);
		} else {
			const double meanGpuMs = gpuMsSum / gpuSamples;
			// Percentages are formatted by hand: fmt v12 rejects the "%"
			// presentation type at compile time.
			const double headroom = a_budgetMs > 0.0 ? (1.0 - meanGpuMs / a_budgetMs) * 100.0 : 0.0;
			logger::info(
				"[readout] {} | {:.1f} fps | frame {:.2f} ms | gpu {:.2f} ms | headroom {:.1f}% | "
				"gpu frames {} of {} ({} repeated)",
				a_presetName, fps, meanFrameMs, meanGpuMs, headroom, gpuSamples, frames,
				staleFrames);
		}

		windowStart = a_now;
		frames = 0;
		frameMsSum = 0.0;
		gpuMsSum = 0.0;
		gpuSamples = 0;
		staleFrames = 0;
	}
};

GpuReadout g_readout;

std::filesystem::path OutputDirectory()
{
	// Next to the SKSE log, i.e. Documents/My Games/Skyrim VR/SKSE/.
	//
	// Deliberately NOT a relative "Data/..." path: under Mod Organizer's
	// virtual filesystem those writes land in the Overwrite folder, which is
	// easy to miss and easy to lose. The captures are the entire point of this
	// plugin, so they go somewhere unambiguous and next to the log the user is
	// already reading.
	if (const auto dir = logger::log_directory()) {
		return *dir / "CSQualityGovernorVR";
	}
	return std::filesystem::current_path() / "CSQualityGovernorVR";
}

void OnRecord(const TransitionRecord& a_record)
{
	if (g_reporter) {
		g_reporter->WriteTransition(a_record);
	}
}

void OnLog(std::string_view a_message)
{
	logger::info("{}", a_message);
}

void FinishIfDone()
{
	if (g_finished.load() || !g_cycler) {
		return;
	}
	const auto state = g_cycler->State();
	if (state != CyclerState::Done && state != CyclerState::Aborted) {
		return;
	}

	g_finished = true;

	// In monitor mode the pump keeps running: the sweep is a short window inside
	// a play session, and stopping here is what left the 2026-08-06 run with
	// 4m38s of data for a session that continued long afterwards. Everything
	// observed in that gap - including a reported 34% headroom that our capture
	// never saw - had no counterpart to compare against.
	//
	// Quiesce, do NOT join: this runs on the sampling thread. The thread exits
	// on its own once the flag is observed.
	if (!g_config.monitorAfterSweep) {
		g_frames.Quiesce();
	}

	// Capture completeness decides whether anything above is worth reading.
	// Below ~0.9 the queue drained less often than once per frame, and every
	// per-preset number carries a load-dependent sampling bias - the defect that
	// inverted Quality against Balanced on 2026-08-05.
	// Percentages are formatted by hand rather than with the "%" presentation
	// type: fmt v12 rejects it at compile time, and this is the sort of thing
	// that is not worth a second CI round trip to be clever about.
	const double ratio = g_frames.CaptureRatio();
	logger::info("frame source: {} frames, {:.1f}s sampled of {:.1f}s elapsed (capture {:.1f}%)",
		g_frames.Frames(), g_frames.SampledSeconds(), g_frames.ElapsedSeconds(), ratio * 100.0);
	if (ratio < 0.9) {
		logger::error("capture ratio {:.1f}% - fewer frames seen than rendered. Per-preset "
					  "comparisons in this run are biased and should not be trusted.",
			ratio * 100.0);
	}

	// D-23's cost, reported rather than assumed. `replayed` is the working path
	// - the held copy handed back during a relatch. `withheld` means there was
	// no copy to give and the frame was dropped instead, which OpenComposite
	// shows as black; a non-zero count there is a defect worth chasing.
	if (const auto replayed = CompositorTimer::FramesReplayed(),
		withheld = CompositorTimer::FramesWithheld();
		replayed > 0 || withheld > 0) {
		logger::info("transition hold: {} submit(s) replayed from the held frame, {} withheld "
					 "with no copy available",
			replayed, withheld);
	}

	if (g_reporter) {
		g_reporter->Finish(g_cycler->Records(), g_config.cycler.frameBudgetMs);
		logger::info("results written to {}", g_reporter->Directory().string());
	}
	logger::info("cycler finished in state {}", CyclerStateName(state));
	if (g_config.monitorAfterSweep) {
		logger::info("monitoring: still recording frames, GPU time and API state for the rest of "
					 "the session.");
		// One deliberate exception to "the preset is no longer being changed":
		// the sweep ends on the cheapest rung, so free play afterwards would
		// never reach the load where the thresholds decide. Choosing a
		// demanding preset here is how a marginal session gets captured during
		// ordinary play rather than by sending someone somewhere expensive.
		if (g_config.monitorPreset >= 0) {
			g_pendingMonitorPreset = g_config.monitorPreset;
		} else {
			logger::info("  preset left as the sweep finished it");
		}
	}
}

// ---------------------------------------------------------------------------
// Shadow mode.
//
// The controller sees everything and touches nothing. Every evaluation is
// written to the timeline CSV, including the ones that decided to hold, because
// "why did it not act" is as much a question as "why did it act" - and a
// controller that only logs its changes cannot answer either.
//
// Two details that make the shadow decisions comparable to live ones:
//
//   - a preset change made by anything else (the cycler, or the player in the
//     CS menu) resets the window. Otherwise a decision would be taken on
//     samples from two different presets, which is the same defect as
//     averaging across scenes.
//   - a decision it would have acted on still starts its cooldown, via
//     NotifyApplied. Nothing is applied; this only stops it re-deciding the
//     same thing every 0.5 s and produces the pacing a live run would have.
// ---------------------------------------------------------------------------
void RunShadowGovernor(double a_now, double a_frameTimeMs, std::uint64_t a_gpuUs,
	std::uint64_t a_gpuFrame, Preset a_preset, std::uint32_t a_presetPublicValue,
	bool a_monitoring)
{
	if (!g_governor) {
		return;
	}

	static Preset lastSeenPreset = a_preset;
	if (a_preset != lastSeenPreset) {
		g_governor->Reset(a_now);
		lastSeenPreset = a_preset;
	}

	GovernorSample sample;
	sample.nowSeconds = a_now;
	sample.frameTimeMs = a_frameTimeMs;
	sample.gpuTimeUs = a_gpuUs;
	sample.gpuFrameIndex = a_gpuFrame;

	const auto decision = g_governor->Push(sample, a_preset);
	if (!decision) {
		return;
	}

	std::string_view outcome = "hold";

	if (decision->action != GovernorAction::Hold) {
		// Live only after the sweep. The cycler owns the lever until then, and
		// two owners on one lever is a failure this project has met more than
		// once.
		const bool live = g_config.applyGovernor && a_monitoring && !g_governorDisabled;

		if (!live) {
			outcome = "shadow";
			logger::info("[governor] WOULD {} {} -> {} | {} | tier={} p95gpu={:.2f}ms "
						 "headroom={:.2f}ms p95frame={:.2f}ms drops={:.1f}% n={}",
				GovernorActionName(decision->action), PresetName(a_preset),
				PresetName(decision->target), decision->reason,
				GovernorTierName(decision->tier), decision->p95GpuMs, decision->headroomMs,
				decision->p95FrameMs, decision->dropRate * 100.0, decision->samples);
			// Pacing only, so the shadow cadence resembles a live one.
			g_governor->NotifyApplied(decision->target, a_now);
		} else {
			const auto blocked = g_api.BlockReasons();
			if (IsTerminalBlock(blocked)) {
				// OpenComposite upscaling has taken the lever for the session.
				// Say so once and stop, rather than retrying forever.
				logger::error("[governor] disabled for this session: {}",
					DescribeBlockReasons(blocked));
				g_governorDisabled = true;
				outcome = "terminal-block";
			} else if (!g_api.ApplyAllowed()) {
				// Buffer the latest desired target and retry, as the API
				// documentation prescribes. Latest wins: an older target is
				// stale by definition.
				g_bufferedTarget = decision->target;
				outcome = "deferred";
			} else {
				g_api.SetPreset(decision->target);
				const auto readback = g_api.GetPreset();
				const bool matched = readback == decision->target;
				logger::info("[governor] {} {} -> {}{} | {} | p95gpu={:.2f}ms headroom={:.2f}ms",
					GovernorActionName(decision->action), PresetName(a_preset),
					PresetName(decision->target),
					matched ? "" : " (READBACK MISMATCH)", decision->reason,
					decision->p95GpuMs, decision->headroomMs);
				outcome = matched ? "applied" : "readback-mismatch";
				g_bufferedTarget.reset();
				// Only now: a refused apply must not start a cooldown against a
				// change that never happened.
				g_governor->NotifyApplied(decision->target, a_now);
				RegisterApplyAndCheckRate(a_now);
			}
		}
	}

	if (g_config.writeTimelineCsv && g_reporter) {
		const auto target = FindPreset(decision->target);
		g_reporter->WriteTimeline(WallClockMs(), *decision, a_presetPublicValue,
			target ? target->publicValue : a_presetPublicValue, outcome);
	}
}

void OnFrame(double a_now, double a_frameTimeMs)
{
	// D-21: keep asking for the compositor until the runtime publishes one.
	//
	// The attempt at CS-interface time is too early - OpenComposite had not
	// handed one out, so the first run of this measured nothing at all. There
	// is no "VR is ready" event to wait on, and this is the only place that
	// runs once per frame. Install() is a cheap flag check once it succeeds,
	// and gives up after a bounded number of attempts if it never does.
	// D-23b: a change is waiting on a good frame. Once the capture is in hand,
	// apply it - the deferral is a frame or two, which nothing downstream can
	// tell from an ordinary decision cadence.
	if (g_pendingPreset && CompositorTimer::CaptureReady()) {
		const auto target = *g_pendingPreset;
		g_pendingPreset.reset();
		g_api.SetPreset(target);
	}

	if (g_config.useOwnGpuTimer && !g_ownGpuTimer && CompositorTimer::Install()) {
		g_ownGpuTimer = true;
		g_csGpuTimer = false;
		g_gpuTiming = true;
		logger::info("GPU timing is live: our own compositor brackets (D-21)");
	}

	if (!g_cycler) {
		return;
	}

	// After the sweep the cycler is left alone but everything else keeps
	// recording, so the capture covers the whole play session rather than the
	// sweep window.
	const bool monitoring = g_finished.load();
	if (!monitoring) {
		const bool wasStarting = g_cycler->State() == CyclerState::Starting;
		g_cycler->Tick(a_now, a_frameTimeMs);
		if (wasStarting && g_cycler->State() != CyclerState::Starting) {
			// The wait for CS is over; start the capture-ratio window here.
			g_frames.MarkRunStart();
		}
	}

	const std::string_view state =
		monitoring ? std::string_view{ "Monitoring" } : CyclerStateName(g_cycler->State());

	// Apply the monitor preset once CS will accept it. Retried on the same 2 Hz
	// tick as the API sample rather than spun on, because a refused apply
	// during a loading screen is normal and not an error.
	if (monitoring && g_pendingMonitorPreset >= 0 && a_now - g_lastApiSample >= 0.5) {
		if (const auto wanted = FindPresetByPublicValue(static_cast<std::uint32_t>(g_pendingMonitorPreset))) {
			if (g_api.ApplyAllowed()) {
				g_api.SetPreset(wanted->preset);
				logger::info("monitoring at {} (MonitorPreset={}), so free play reaches the load "
							 "where the thresholds decide",
					wanted->name, g_pendingMonitorPreset);
				g_pendingMonitorPreset = -1;
			}
		} else {
			logger::warn("MonitorPreset={} is not a valid public preset value; ignoring",
				g_pendingMonitorPreset);
			g_pendingMonitorPreset = -1;
		}
	}

	// A target buffered while CS was refusing changes. Retried on the 2 Hz tick
	// rather than spun on, and dropped once it lands.
	if (monitoring && g_bufferedTarget && !g_governorDisabled &&
		a_now - g_lastApiSample >= 0.5 && g_api.ApplyAllowed()) {
		g_api.SetPreset(*g_bufferedTarget);
		logger::info("[governor] applied buffered target {} once CS allowed it",
			PresetName(*g_bufferedTarget));
		g_governor->NotifyApplied(*g_bufferedTarget, a_now);
		g_bufferedTarget.reset();
		RegisterApplyAndCheckRate(a_now);
	}

	// Sample the whole readable API surface at ~2 Hz for the entire run, so
	// drift in anything other than the preset is visible afterwards without
	// anyone having watched for it.
	if (g_reporter && a_now - g_lastApiSample >= 0.5) {
		g_lastApiSample = a_now;
		// g_csGpuTimer, NOT g_gpuTiming. The snapshot reads GPU time straight
		// off the CS interface, so the question here is "does THAT interface
		// have the methods", not "do we have timing from somewhere". Passing
		// the latter crashed the game the instant our own timer went live and
		// flipped it true against a CS build with no such method (E-37).
		const auto snap = ApiSnapshot::Capture(g_CSInterface, g_csGpuTimer);
		// Cheap side effect worth having: this snapshot already read the preset,
		// so monitor mode gets it without calling the API every frame.
		g_monitorPreset = snap.upscalePreset;
		g_reporter->WriteApiState(std::format("{},{:.3f},{},{},{:.3f}", WallClockMs(), a_now, state,
			snap.CsvValues(), a_frameTimeMs));
		// A game that exits without unwinding takes any buffered rows with it,
		// and a monitored session can run for hours. Bound the loss to ~0.5 s.
		g_reporter->Flush();
	}

	// One read per frame, on the game thread with everything else. Both getters
	// are plain atomic loads inside Community Shaders, so this costs nothing and
	// keeps GPU time aligned with the frametime it belongs to.
	const std::uint64_t gpuUs = g_gpuTiming ? g_api.GpuTimeUs() : 0;
	const std::uint64_t gpuFrame = g_gpuTiming ? g_api.GpuFrameIndex() : 0;

	// While monitoring, the preset is whatever CS actually has - nothing is
	// driving it any more, but the player can change it from the CS menu. It
	// comes from the 2 Hz snapshot rather than a per-frame API call.
	const auto info = monitoring ? FindPresetByPublicValue(g_monitorPreset)
								 : FindPreset(g_cycler->CurrentTarget());
	if (g_config.writePerFrameCsv && g_reporter) {
		g_reporter->WriteFrame(WallClockMs(), a_now, a_frameTimeMs, info ? info->publicValue : 0,
			state, gpuUs, gpuFrame);
	}

	g_readout.Add(a_now, a_frameTimeMs, gpuUs, gpuFrame, g_config.cycler.frameBudgetMs,
		info ? info->name : "?");

	RunShadowGovernor(a_now, a_frameTimeMs, gpuUs, gpuFrame, info ? info->preset : Preset::NativeAA,
		info ? info->publicValue : 0, monitoring);

	if (!monitoring) {
		FinishIfDone();
	}
}

void BeginSession()
{
	const auto stamp = TimeStamp();
	g_reporter = std::make_unique<Reporter>(OutputDirectory(), stamp);

	std::string info = std::format(
		"CS build {} | target {:.1f} Hz (budget {:.3f} ms) | sweeps {} | dwell {:.1f}s",
		g_api.BuildNumber(), g_config.targetHz, g_config.cycler.frameBudgetMs,
		g_config.cycler.sweeps, g_config.cycler.dwellSeconds);
	g_reporter->SetSessionInfo(info);
	logger::info("{}", info);

	// Probe the API before touching anything. If this section looks wrong,
	// nothing after it is worth reading - and that is a result in itself,
	// available without waiting for a sweep to complete.
	const auto probe = ApiProbe::Run(g_CSInterface, CSPluginAPI::CSInterfaceRevision,
		g_revisionAcquired, static_cast<unsigned int>(g_config.gpuTimingCsBuild));
	const auto probeText = probe.Format();
	for (auto&& line : std::views::split(probeText, '\n')) {
		const std::string_view view{ line.begin(), line.end() };
		if (!view.empty()) {
			logger::info("{}", view);
		}
	}
	g_reporter->WriteText("_apiprobe.txt", probeText);
	if (!probe.AllOk()) {
		logger::error("API probe reported problems - see the CHECK lines above");
	}

	{
		// What produced this capture. Written into the file rather than kept in
		// someone's head: the two captures already committed are implicitly
		// "MGO 4.0beta RC2 + CS PL3.15" and nothing in them says so, which is a
		// mistake worth making only once now that release candidates are
		// arriving. The ladder is included because it is hardcoded and CS cannot
		// be asked for it, so a later replay has no other way to tell whether
		// the scales it assumes are the ones that were in force.
		std::vector<std::string> provenance;
		provenance.push_back(std::format("cs_build={}", g_api.BuildNumber()));
		provenance.push_back(std::format("cs_api_revision={}", g_revisionAcquired));
		provenance.push_back(std::format("verified_cs_build={}", g_config.verifiedCsBuild));
		provenance.push_back(std::format("gpu_timing_cs_build={}", g_config.gpuTimingCsBuild));
		// Which timer, not just whether. A capture taken through our own
		// brackets and one taken through the fork's are different instruments,
		// and D-21 exists precisely because they may not agree yet.
		provenance.push_back(std::format("gpu_timing={}",
			g_ownGpuTimer ? "own" : (g_csGpuTimer ? "cs-fork" : "none")));
		provenance.push_back(std::format("frame_budget_ms={:.3f}", g_config.cycler.frameBudgetMs));
		provenance.push_back(std::format("apply_governor={}", g_config.applyGovernor ? 1 : 0));
		std::string ladder;
		for (const auto& info : kPresets) {
			if (!ladder.empty()) {
				ladder += ',';
			}
			ladder += std::format("{}:{:.6f}", info.name, info.scale);
		}
		provenance.push_back("preset_scales=" + ladder);
		g_reporter->SetProvenance(std::move(provenance));
	}

	if (g_config.writeCsv && !g_reporter->OpenTransitions()) {
		logger::error("could not open transitions CSV; results will not be saved");
	}
	if (g_config.writePerFrameCsv && !g_reporter->OpenFrames()) {
		logger::warn("could not open per-frame CSV");
	}
	{
		const ApiSnapshot header{};
		g_reporter->OpenApiState(
			std::format("wall_ms,time_s,cycler_state,{},frame_ms", header.CsvHeaderSuffix("api")));
	}

	if (g_config.shadowGovernor) {
		GovernorConfig governorConfig;
		governorConfig.frameBudgetMs = g_config.cycler.frameBudgetMs;
		g_governor = std::make_unique<GovernorCore>(governorConfig);

		if (g_config.writeTimelineCsv && !g_reporter->OpenTimeline()) {
			logger::warn("could not open timeline CSV; decisions will only reach the log");
		}
		logger::info("governor active ({}): climb below {:.2f} ms p95 GPU, descend above {:.2f} "
					 "ms, budget {:.3f} ms",
			g_config.applyGovernor ? "APPLYING after the sweep" : "shadow, applying nothing",
			governorConfig.frameBudgetMs - governorConfig.marginUpMs,
			governorConfig.frameBudgetMs - governorConfig.marginDownMs,
			governorConfig.frameBudgetMs);
		logger::info("thresholds fitted 2026-08-08 by replay across a light and a marginal "
					 "session (D-10c, E-23); climbs cost up to {} rungs, priced from what each "
					 "step has actually cost here (D-18)",
			governorConfig.maxClimbRungs);
	}

	g_cycler = std::make_unique<CyclerCore>(g_api, g_config.cycler);
	g_cycler->SetRecordSink(OnRecord);
	g_cycler->SetLogSink(OnLog);

	// The transitions CSV carries elapsed seconds only. This line is its anchor
	// to wall clock, so a transition can be located in another tool's log
	// without re-deriving the offset from the filename stamp, which is only
	// accurate to the second.
	logger::info("clock anchor: wall_ms={} at time_s=0 (UTC epoch milliseconds)", WallClockMs());

	g_frames.Start(OnFrame);
	g_cycler->Start(0.0);
}

void OnMessage(SKSE::MessagingInterface::Message* a_message)
{
	if (!a_message) {
		return;
	}

	switch (a_message->type) {
	case SKSE::MessagingInterface::kPostLoad:
		{
			// The CS interface is only valid to request after PostLoad.
			auto* messaging = SKSE::GetMessagingInterface();
			CSPluginAPI::CSMessage request{};
			messaging->Dispatch(CSPluginAPI::CSMessage::kMessage_GetInterface, &request,
				sizeof(request), CSPluginAPI::CSPluginName);
			if (request.GetApiFunction) {
				// Negotiate downwards. Asking only for the newest revision and
				// giving up would report "unavailable" on a build that would
				// have answered revision 1 or 2 perfectly well - a false
				// negative that is expensive to diagnose from the game side.
				for (unsigned int revision = CSPluginAPI::CSInterfaceRevision; revision >= 1;
					--revision) {
					if (auto* iface = static_cast<CSPluginAPI::ICSInterface001*>(
							request.GetApiFunction(revision))) {
						g_CSInterface = iface;
						g_revisionAcquired = revision;
						break;
					}
					logger::warn("CS API revision {} not available, trying {}", revision,
						revision - 1);
				}
			}
			if (g_CSInterface) {
				logger::info("acquired Community Shaders interface: revision {} (requested {}), "
							 "build {}",
					g_revisionAcquired, CSPluginAPI::CSInterfaceRevision,
					g_CSInterface->getBuildNumber());
				if (g_revisionAcquired < 3) {
					logger::warn(
						"revision {} lacks GetVRUpscalingApplyBlockReasons and "
						"IsVRUpscalingProfileApplyAllowed; block detection will be blind",
						g_revisionAcquired);
				}

				// The ladder is hardcoded and CS cannot be asked for it, so a CS
				// that moved underneath us is checked for by build number. This
				// detects "CS changed", not "the scales changed" - the second is
				// not answerable through this API - and it is deliberately loud
				// because the failure it guards is silent: wrong pixel fractions
				// rank every preset wrongly without looking wrong anywhere.
				const auto build = g_CSInterface->getBuildNumber();
				if (g_config.verifiedCsBuild > 0 &&
					build != static_cast<unsigned int>(g_config.verifiedCsBuild)) {
					logger::error(
						"CS build {} is not the build the preset ladder was verified against "
						"({}). Presets.h hardcodes each quality mode's resolution scale and "
						"this API cannot report them, so every pixel fraction may now be "
						"wrong.",
						build, g_config.verifiedCsBuild);
					if (g_config.requireVerifiedCsBuild) {
						logger::error(
							"refusing to apply preset changes. Re-verify the scales in CS's "
							"Upscaling.h against Presets.h, then set VerifiedCsBuild={} in the "
							"ini. Set RequireVerifiedCsBuild=0 to override.",
							build);
						g_config.applyGovernor = false;
					} else {
						logger::warn("RequireVerifiedCsBuild=0: continuing on an unverified "
									 "ladder. Captures from this session are suspect.");
					}
				}

				// The GPU timer is the whole basis of the headroom tier. Without
				// it the controller still runs, on censored frametime (E-1), and
				// climbs become blind probes - a real degradation that used to be
				// reported at warn level among ordinary startup chatter.
				if (g_revisionAcquired < 4) {
					logger::error(
						"revision {} has no GPU timing. The controller falls back to the "
						"frametime tier, which is censored at the compositor cap, so climbs "
						"become blind probes and no headroom number is trustworthy. Install "
						"the forked Community Shaders build if you want the headroom tier.",
						g_revisionAcquired);
				}
				// D-21: try our own brackets first. They work against whatever
				// CS the modlist ships, so the fork stops being something Rik
				// has to run and goes back to being only the upstream patch.
				// D-22. Exact build match, for the same reason as everything
				// else here: their slot 23 is the preflight, ours is the GPU
				// timer, and both builds answer "revision 4".
				if (g_config.transitionApiCsBuild > 0 &&
					build == static_cast<unsigned int>(g_config.transitionApiCsBuild)) {
					g_csxTransition = reinterpret_cast<CSXInterface001*>(g_CSInterface);
					logger::info("preset changes will use CS's transition-profile API "
								 "(build {}): preflight, then the door fade, instead of "
								 "setting the preset underneath the renderer",
						build);
				} else {
					logger::info("preset changes will use SetUpscalePreset (build {} is not the "
								 "transition-API build {}); a change may show the relatch",
						build, g_config.transitionApiCsBuild);
				}

				g_ownGpuTimer = g_config.useOwnGpuTimer && CompositorTimer::Install();
				if (g_ownGpuTimer) {
					logger::info("GPU timing from our own compositor hooks (D-21); works "
								 "against whatever Community Shaders the modlist ships");
				}
				g_csGpuTimer = !g_ownGpuTimer &&
				               GpuTimingAvailable(g_CSInterface, g_revisionAcquired,
								   static_cast<unsigned int>(g_config.gpuTimingCsBuild));
				g_gpuTiming = g_ownGpuTimer || g_csGpuTimer;
				if (g_gpuTiming) {
					logger::info("GPU timing available (revision {}, build {}): headroom will be "
								 "measured rather than inferred",
						g_revisionAcquired, g_CSInterface->getBuildNumber());
				} else {
					logger::info(
						"no GPU timing on this provider (revision {}, build {}); running on the "
						"frametime tier, which is correct but conservative near the cap",
						g_revisionAcquired, g_CSInterface->getBuildNumber());
				}
			} else {
				logger::error(
					"Community Shaders interface unavailable at any revision - is the Particle "
					"Lights Fork installed and loaded before this plugin?");
			}
			break;
		}

	case SKSE::MessagingInterface::kDataLoaded:
		if (g_config.autoStart) {
			BeginSession();
		} else {
			logger::info("auto-start disabled; waiting for hotkey 0x{:X}", g_config.hotkey);
		}
		break;

	default:
		break;
	}
}

}
}

namespace csgov {
namespace {

// Without this no log file is written at all and every logger call silently
// goes nowhere. That cost the live view of the first two real runs.
void InitLogging()
{
	auto path = logger::log_directory();
	if (!path) {
		return;
	}
	*path /= "CSQualityGovernorVR.log";

	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
	auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);  // a crash must not cost the log
	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
}

}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	csgov::InitLogging();

	const auto configPath = std::filesystem::path{ "Data" } / "SKSE" / "Plugins" /
	                        "CSQualityGovernorVR.ini";
	csgov::g_config = csgov::PluginConfig::Load(configPath);

	logger::info("CSQualityGovernorVR loaded");

	SKSE::GetMessagingInterface()->RegisterListener(csgov::OnMessage);
	return true;
}
