#pragma once

#include "VRAPI/CSinterface001.h"

#include <cstdint>

namespace csgov {

// CSX 3.18's interface, declared with ITS vtable layout rather than ours.
//
// Slots 1-22 are identical between the two headers, checked declaration by
// declaration against `csx-3-VR`. Slot 23 is where they diverge, and it is the
// reason this file exists:
//
//     slot 23   ours : GetLastFrameGpuTimeUs()
//     slot 23   CSX  : GetVRUpscalingTransitionProfileDecision(...)
//
// Both are "revision 4". Calling the preflight through our own interface
// declaration would therefore call our GPU-time getter's slot on a build that
// has a completely different function there - which is precisely the crash of
// E-34, and the reason a whole interface is redeclared here rather than a
// convenient cast being taken.
//
// Every signature below is copied verbatim from their header, in source order,
// so the compiler lays out the same vtable they compiled against. A pointer is
// only ever cast to this type once the build number is an EXACT match for one
// known to be a CSX build with this method - never "at least", never inferred
// from the revision.
struct CSXInterface001
{
	virtual unsigned int getBuildNumber() = 0;

	virtual bool GetSSSEnabled() = 0;
	virtual void SetSSSEnabled(bool enabled) = 0;

	virtual bool GetSSGIEnabled() = 0;
	virtual void SetSSGIEnabled(bool enabled) = 0;

	virtual bool GetVolumetricLightingExteriorEnabled() = 0;
	virtual void SetVolumetricLightingExteriorEnabled(bool enabled) = 0;

	virtual CSPluginAPI::UpscalePreset GetUpscalePreset() = 0;
	virtual void SetUpscalePreset(CSPluginAPI::UpscalePreset preset) = 0;

	virtual bool GetLightLimitFixContactShadowsEnabled() = 0;
	virtual void SetLightLimitFixContactShadowsEnabled(bool enabled) = 0;

	virtual CSPluginAPI::DLSSProfile GetDLSSProfile() = 0;
	virtual void SetDLSSProfile(CSPluginAPI::DLSSProfile profile) = 0;

	virtual bool GetRenderAtUpscaleResEnabled() = 0;
	virtual void SetRenderAtUpscaleResEnabled(bool enabled) = 0;
	virtual bool GetRenderAtUpscaleResActive() = 0;

	virtual void SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled,
		CSPluginAPI::UpscalePreset preset, CSPluginAPI::DLSSProfile profile) = 0;

	virtual CSPluginAPI::UpscaleMethod GetUpscaleMethod() = 0;
	virtual void SetUpscaleMethod(CSPluginAPI::UpscaleMethod method) = 0;
	virtual void SetVRUpscalingTransitionProfileForMethod(CSPluginAPI::UpscaleMethod method,
		bool renderScaleModeEnabled, CSPluginAPI::UpscalePreset preset,
		CSPluginAPI::DLSSProfile profile) = 0;

	virtual std::uint32_t GetVRUpscalingApplyBlockReasons() = 0;
	virtual bool IsVRUpscalingProfileApplyAllowed() = 0;

	// Slot 23. The one we came for.
	virtual std::uint32_t GetVRUpscalingTransitionProfileDecision(
		CSPluginAPI::UpscaleMethod method, bool renderScaleModeEnabled,
		CSPluginAPI::UpscalePreset preset, CSPluginAPI::DLSSProfile profile) = 0;
};

// Their enum, by value rather than by name, so a rename upstream cannot
// silently change what we compare against.
enum class TransitionDecision : std::uint32_t {
	Blocked = 0,   // buffer the desired profile and retry later
	NoChange = 1,  // already matches - the caller must NOT schedule a fade
	Apply = 2,     // schedule the door fade, then apply for the method
};

}
