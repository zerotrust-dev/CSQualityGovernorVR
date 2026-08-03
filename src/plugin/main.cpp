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
// Samples the game's own frame delta from a dedicated thread and treats each
// change as a new frame. This avoids hooking Present, which needs VR-specific
// offsets that cannot be verified without the game.
//
// The trade-off is honest and logged: at a perfectly locked framerate two
// consecutive deltas could be bit-identical and undercount. Sample health is
// reported so undercounting is visible rather than silent. If it proves a
// problem in practice, replace this with a swapchain Present hook - the rest of
// the plugin does not care where frametimes come from.
// ---------------------------------------------------------------------------
class FrameSource
{
public:
	using Callback = std::function<void(double nowSeconds, double frameTimeMs)>;

	void Start(Callback a_callback)
	{
		_callback = std::move(a_callback);
		_running = true;
		_thread = std::thread([this] { Run(); });
	}

	void Stop()
	{
		_running = false;
		if (_thread.joinable()) {
			_thread.join();
		}
	}

	[[nodiscard]] std::uint64_t Frames() const noexcept { return _frames.load(); }
	[[nodiscard]] std::uint64_t Polls() const noexcept { return _polls.load(); }

private:
	void Run()
	{
		using clock = std::chrono::steady_clock;
		const auto start = clock::now();
		float lastDelta = -1.0f;

		while (_running) {
			++_polls;
			const float delta = RE::GetSecondsSinceLastFrame();
			if (delta > 0.0f && delta != lastDelta) {
				lastDelta = delta;
				++_frames;
				const double now =
					std::chrono::duration<double>(clock::now() - start).count();
				if (_callback) {
					_callback(now, static_cast<double>(delta) * 1000.0);
				}
			}
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	}

	Callback _callback;
	std::thread _thread;
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
	g_frames.Stop();

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

	if (g_config.writeCsv && !g_reporter->OpenTransitions()) {
		logger::error("could not open transitions CSV; results will not be saved");
	}
	if (g_config.writePerFrameCsv && !g_reporter->OpenFrames()) {
		logger::warn("could not open per-frame CSV");
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
				g_CSInterface = static_cast<CSPluginAPI::ICSInterface001*>(
					request.GetApiFunction(CSPluginAPI::CSInterfaceRevision));
			}
			if (g_CSInterface) {
				logger::info("acquired Community Shaders interface, build {}",
					g_CSInterface->getBuildNumber());
			} else {
				logger::error(
					"Community Shaders interface unavailable - is the Particle Lights Fork "
					"installed, and does it expose API revision {}?",
					CSPluginAPI::CSInterfaceRevision);
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

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	const auto configPath = std::filesystem::path{ "Data" } / "SKSE" / "Plugins" /
	                        "CSQualityGovernorVR.ini";
	csgov::g_config = csgov::PluginConfig::Load(configPath);

	logger::info("CSQualityGovernorVR loaded");

	SKSE::GetMessagingInterface()->RegisterListener(csgov::OnMessage);
	return true;
}
