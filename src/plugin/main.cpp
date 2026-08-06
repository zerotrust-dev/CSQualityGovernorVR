#include "ApiProbe.h"
#include "Config.h"
#include "Reporter.h"
#include "core/CyclerCore.h"

#include "VRAPI/CSinterface001.h"

#include <atomic>
#include <chrono>
#include <thread>

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

	void SetPreset(Preset a_preset) override
	{
		if (!g_CSInterface) {
			return;
		}
		const auto info = FindPreset(a_preset);
		if (!info) {
			return;
		}
		g_CSInterface->SetUpscalePreset(
			static_cast<CSPluginAPI::UpscalePreset>(info->publicValue));
	}

	std::uint32_t BlockReasons() override
	{
		return g_CSInterface ? g_CSInterface->GetVRUpscalingApplyBlockReasons() : 0;
	}

	bool ApplyAllowed() override
	{
		return g_CSInterface && g_CSInterface->IsVRUpscalingProfileApplyAllowed();
	}

	// Revision 4, forked provider only. Guarded by the caller through
	// GpuTimingAvailable(); see the note on that function for why calling this
	// unguarded is not a graceful failure.
	std::uint64_t GpuTimeUs() const
	{
		return g_CSInterface ? g_CSInterface->GetLastFrameGpuTimeUs() : 0;
	}

	std::uint64_t GpuFrameIndex() const
	{
		return g_CSInterface ? g_CSInterface->GetLastFrameGpuTimeFrameIndex() : 0;
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

PluginConfig g_config;
LiveCSApi g_api;
std::unique_ptr<CyclerCore> g_cycler;
std::unique_ptr<Reporter> g_reporter;
FrameSource g_frames;
std::atomic_bool g_finished{ false };
unsigned int g_revisionAcquired = 0;
bool g_gpuTiming = false;
double g_lastApiSample = -1.0;
// Public preset value as last read from CS. Only used once the sweep is over,
// when nothing of ours is driving the lever.
std::uint32_t g_monitorPreset = 0;

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

	if (g_reporter) {
		g_reporter->Finish(g_cycler->Records(), g_config.cycler.frameBudgetMs);
		logger::info("results written to {}", g_reporter->Directory().string());
	}
	logger::info("cycler finished in state {}", CyclerStateName(state));
	if (g_config.monitorAfterSweep) {
		logger::info("monitoring: still recording frames, GPU time and API state for the rest of "
					 "the session. The preset is no longer being changed.");
	}
}

void OnFrame(double a_now, double a_frameTimeMs)
{
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

	// Sample the whole readable API surface at ~2 Hz for the entire run, so
	// drift in anything other than the preset is visible afterwards without
	// anyone having watched for it.
	if (g_reporter && a_now - g_lastApiSample >= 0.5) {
		g_lastApiSample = a_now;
		const auto snap = ApiSnapshot::Capture(g_CSInterface, g_gpuTiming);
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
		g_revisionAcquired);
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
				g_gpuTiming = GpuTimingAvailable(g_CSInterface, g_revisionAcquired);
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
