#pragma once

#include "core/CyclerCore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace csgov {

struct PluginConfig
{
	CyclerConfig cycler = CyclerConfig::Default();

	// Automatically arm the sweep once the player is in-world. With this off
	// the sweep only starts on the hotkey.
	bool autoStart = true;
	// DirectInput scan code. 0x58 = F12. Toggles arm/abort.
	std::uint32_t hotkey = 0x58;

	bool writeCsv = true;
	bool writePerFrameCsv = false;  // large; only for deep analysis
	bool logToFile = true;

	// Keep recording frames after the sweep finishes, for the rest of the play
	// session. Without this the capture covers only the sweep - 4m38s of a much
	// longer session on 2026-08-06 - and anything seen while simply playing has
	// no counterpart in the data.
	bool monitorAfterSweep = true;

	// Run the controller and record every decision. Phase 4: the first live run
	// must not also be the first test.
	bool shadowGovernor = true;
	bool writeTimelineCsv = true;

	// Let the controller actually change the preset. Phase 5.
	//
	// Off by default, and deliberately separate from shadowGovernor: turning
	// the observer on is free, turning the actuator on is not, and the two
	// should never be the same switch.
	//
	// It only ever acts once the sweep has finished. Two owners on one lever
	// has bitten this project repeatedly, and the cycler owns it until then.
	bool applyGovernor = false;

	// Simple mode: the rule as a person states it, so a person can judge it.
	//
	// "Whenever I see 15-20% overhead I think we are ok to go up one preset" is
	// not a question replay can answer, because it is really asking what the
	// result LOOKS like. This does exactly that and nothing else.
	//
	// The percentage is read against the MEAN GPU time, which is what the
	// readout prints - so the number you act on is the number you saw. That is
	// also why it is an experiment and not a shipping controller: the mean hides
	// the tail, and a locked framerate is lost at the tail.
	bool simpleMode = false;
	double simpleClimbHeadroomPct = 20.0;
	double simpleDescendFps = 70.0;

	// D-25 + D-26: the controller the project arrived at. Climb when spare
	// capacity clears the measured price of the next rung plus the margin;
	// descend when frames actually arrive late.
	//
	// The margin is the ONLY number a user should ever set: the price is
	// measured per rung per session, so it already absorbs resolution, headset,
	// GPU and scene. A raw threshold would need a different value on every rung
	// and every system - which is the knob that produced 14% and hunted.
	bool adaptiveMode = false;
	double climbMarginPct = 5.0;
	double descendMissRatePct = 5.0;
	double climbCleanSeconds = 8.0;

	// The Community Shaders build the preset ladder was verified against.
	//
	// Presets.h hardcodes each quality mode's resolution scale, copied by hand
	// from CS's Upscaling.h, and the VR API exposes no way to ask for them - it
	// can set and get the preset, not its scale. So every pixel fraction, the
	// cost model, and the score against the optimum rest on a table that a CS
	// update can silently invalidate. PL3.17's notes describe a "major VR
	// Render Scale upgrade", so this is a live risk on the coming MGO release
	// candidates rather than a theoretical one.
	//
	// This does not verify the scales - nothing available can. It detects that
	// CS changed underneath the table, which is the honest check we can make,
	// and turns a silent wrong number into a loud refusal. Set to 0 to disable
	// the check; raise it after re-verifying the ladder against the new source.
	int verifiedCsBuild = 9;
	// What to do when the build differs. Refusing is the safe default: wrong
	// pixel fractions do not look wrong, they just quietly rank everything.
	bool requireVerifiedCsBuild = true;

	// The build whose vtable actually carries our revision-4 GPU timing methods.
	//
	// Deliberately SEPARATE from verifiedCsBuild, because the two facts are
	// independent and tying them to one number is a trap. CSX 3.18 reports
	// build 11 and its resolution scales match ours exactly - so the ladder is
	// verified and verifiedCsBuild should say 11 - but it has no timing methods
	// at all, and calling them crashes the game (E-34). One number cannot mean
	// both "the scales are right" and "our fork's vtable is present".
	//
	// Only ever set this to a build of OUR fork.
	int gpuTimingCsBuild = 9;

	// D-21: measure GPU time from our own compositor hooks rather than from a
	// forked Community Shaders, so timing works against whatever build the
	// modlist ships.
	//
	// Off only as an escape hatch. With it off, and without the fork installed,
	// the controller runs on the censored frametime tier (E-1) where four
	// presets read the same 13.9 ms and every climb is a blind probe.
	bool useOwnGpuTimer = true;

	// D-22: the build confirmed to carry CSX's transition-profile API.
	//
	// Exact match only. That API's preflight sits at vtable slot 23, where our
	// own header has GetLastFrameGpuTimeUs - both call themselves revision 4,
	// so nothing but an exact build number distinguishes them (E-34, E-36).
	// 0 disables the path and falls back to SetUpscalePreset.
	int transitionApiCsBuild = 11;

	// D-23: frames to withhold across the render-target relatch a preset change
	// causes, so the runtime reprojects the last good frame instead of
	// presenting a reallocation in progress.
	//
	// Measured, not guessed. A change costs a ~85 ms stall plus one frame doing
	// 66-86 ms of GPU work, and takes 10-11 frames to return to normal.
	// Counting from the moment we apply: frames 1-3 are still valid old-preset
	// frames, 4-6 are the stall and the relatch, 7-10 are valid but slow. 8
	// covers the bad ones with margin without withholding the recovery.
	//
	// RC2 and RC3 measure identically here - same stall, same spike, same
	// recovery. Only the appearance differed, because PL3.15's HAM bug painted
	// the gap white. 0 disables it.
	int transitionHoldFrames = 8;

	// Circuit breaker: disable the governor for the session if it applies more
	// changes than this in any rolling minute.
	//
	// The replay predicts 2-3 a minute and D-8 wants few correct changes, so a
	// sustained double-digit rate means something is wrong - a bad threshold, a
	// scene the cost model mispredicts, or an oscillation. In a headset the
	// alternative to stopping by itself is quitting the game, since editing an
	// ini is not an option mid-session.
	int maxChangesPerMinute = 12;

	// Public preset value to select once monitoring begins, or -1 to leave
	// whatever the sweep ended on.
	//
	// The sweep ends on the cheapest rung, so free play afterwards runs with
	// large headroom and never exercises the region where the thresholds
	// decide - 71% of the 2026-08-08 session sat above 30% headroom for exactly
	// this reason. Setting a demanding preset here produces marginal load
	// during ordinary play, which is the alternative to asking someone to go
	// and stand somewhere expensive.
	int monitorPreset = -1;

	// Refresh rate the frame budget is derived from.
	double targetHz = 72.0;

	[[nodiscard]] static PluginConfig Load(const std::filesystem::path& a_path);
	void ApplyDerived();
};

// Minimal INI reader. Intentionally dependency-free and forgiving: an
// unreadable or absent file yields defaults rather than a failure, because a
// diagnostic tool that refuses to run is worse than one running on defaults.
class IniFile
{
public:
	bool Load(const std::filesystem::path& a_path);

	[[nodiscard]] bool Has(std::string_view a_key) const;
	[[nodiscard]] std::string GetString(std::string_view a_key, std::string a_default) const;
	[[nodiscard]] double GetDouble(std::string_view a_key, double a_default) const;
	[[nodiscard]] int GetInt(std::string_view a_key, int a_default) const;
	[[nodiscard]] bool GetBool(std::string_view a_key, bool a_default) const;
	[[nodiscard]] std::uint32_t GetHex(std::string_view a_key, std::uint32_t a_default) const;

private:
	std::map<std::string, std::string, std::less<>> _values;
};

}
