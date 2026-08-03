#include "ApiProbe.h"
#include "Config.h"
#include "Reporter.h"
#include "core/CyclerCore.h"

#include "VRAPI/CSinterface001.h"

#include <atomic>
#include <chrono>

CSPluginAPI::ICSInterface001* g_CSInterface = nullptr;

namespace csgov {

namespace {

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
// Every method here is reached from FrameSource's pump, which runs on the game
// thread. Keep it that way: CS is not documented as safe to call from anywhere
// else, and nothing in this class does its own marshalling.
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
};

// ---------------------------------------------------------------------------
// Frame source.
//
// Samples the game's own frame delta and treats each change as a new frame.
// This avoids hooking Present, which needs VR-specific offsets that cannot be
// verified without the game.
//
// It runs as a task that re-arms itself: SKSE drains its task queue once per
// frame on the game thread, and a task added from inside the drain runs on the
// next one. So this pumps at frame rate, on the main thread, without a Present
// hook and without a thread of our own.
//
// That last part is the point. This used to be a dedicated polling thread,
// which was wrong twice over:
//
//   - Every CS API call was made off the game thread. CS is not documented to
//     be safe against that, and we were doing it on every single tick.
//   - Ending the sweep called join() on the sampling thread from inside that
//     same thread, which terminates the process. It crashed the game twice on
//     2026-08-03, deterministically, at the moment the sweep completed.
//
// Both are structural consequences of owning a thread, so the thread is gone
// rather than guarded.
//
// The delta-dedup is kept: at a perfectly locked framerate two consecutive
// deltas can be bit-identical and undercount. That is why frames and polls are
// both counted and logged - if the queue ever drains at some rate other than
// once per frame, the ratio says so instead of the error hiding in the data.
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
		Schedule();
	}

	// Stops the pump after the current tick. Safe to call from inside the
	// callback, which is exactly what happens when the sweep completes.
	void Quiesce() noexcept { _running = false; }

	[[nodiscard]] std::uint64_t Frames() const noexcept { return _frames.load(); }
	[[nodiscard]] std::uint64_t Polls() const noexcept { return _polls.load(); }

private:
	void Schedule()
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			// Nothing can drive us, so say so rather than appearing to run.
			_running = false;
			logger::error("no SKSE task interface; the cycler cannot be driven");
			return;
		}
		tasks->AddTask([this] { Pump(); });
	}

	void Pump()
	{
		if (!_running) {
			return;
		}

		++_polls;
		const float delta = RE::GetSecondsSinceLastFrame();
		if (delta > 0.0f && delta != _lastDelta) {
			_lastDelta = delta;
			++_frames;
			const double now =
				std::chrono::duration<double>(std::chrono::steady_clock::now() - _start).count();
			if (_callback) {
				_callback(now, static_cast<double>(delta) * 1000.0);
			}
		}

		// Re-arm last, and only if still running, so a callback that quiesces
		// us is the final tick.
		if (_running) {
			Schedule();
		}
	}

	Callback _callback;
	std::chrono::steady_clock::time_point _start{};
	float _lastDelta{ -1.0f };  // main thread only
	std::atomic_bool _running{ false };
	std::atomic_uint64_t _frames{ 0 };
	std::atomic_uint64_t _polls{ 0 };
};

// ---------------------------------------------------------------------------

PluginConfig g_config;
LiveCSApi g_api;
std::unique_ptr<CyclerCore> g_cycler;
std::unique_ptr<Reporter> g_reporter;
FrameSource g_frames;
std::atomic_bool g_finished{ false };
unsigned int g_revisionAcquired = 0;
double g_lastApiSample = -1.0;

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
	// Runs inside the pump's own callback, so this only asks it not to re-arm.
	g_frames.Quiesce();

	logger::info("frame source: {} frames from {} polls", g_frames.Frames(), g_frames.Polls());

	if (g_reporter) {
		g_reporter->Finish(g_cycler->Records(), g_config.cycler.frameBudgetMs);
		logger::info("results written to {}", g_reporter->Directory().string());
	}
	logger::info("cycler finished in state {}", CyclerStateName(state));
}

void OnFrame(double a_now, double a_frameTimeMs)
{
	if (!g_cycler || g_finished.load()) {
		return;
	}

	g_cycler->Tick(a_now, a_frameTimeMs);

	// Sample the whole readable API surface at ~2 Hz for the entire run, so
	// drift in anything other than the preset is visible afterwards without
	// anyone having watched for it.
	if (g_reporter && a_now - g_lastApiSample >= 0.5) {
		g_lastApiSample = a_now;
		const auto snap = ApiSnapshot::Capture(g_CSInterface);
		g_reporter->WriteApiState(std::format("{:.3f},{},{},{:.3f}", a_now,
			CyclerStateName(g_cycler->State()), snap.CsvValues(), a_frameTimeMs));
	}

	if (g_config.writePerFrameCsv && g_reporter) {
		const auto info = FindPreset(g_cycler->CurrentTarget());
		g_reporter->WriteFrame(a_now, a_frameTimeMs, info ? info->publicValue : 0,
			CyclerStateName(g_cycler->State()));
	}

	FinishIfDone();
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
			std::format("time_s,cycler_state,{},frame_ms", header.CsvHeaderSuffix("api")));
	}

	g_cycler = std::make_unique<CyclerCore>(g_api, g_config.cycler);
	g_cycler->SetRecordSink(OnRecord);
	g_cycler->SetLogSink(OnLog);

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
