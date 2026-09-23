#pragma once

#include <windows.h>
#include <d3d11.h>

#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>

// Hooks the small set of OpenXR functions this layer cares about:
//  - xrCreateSession / xrDestroySession: track the app's session + D3D11
//    device so we know where to create our own overlay swapchains.
//  - xrEndFrame: append our own XrCompositionLayerQuad entries (built from
//    the panel data received over IPC, see IpcServer.h) to the layer array
//    the application submits, then forward to the real xrEndFrame.
//
// NOTE: this is the architectural skeleton described in NativeLayer/README.md.
// The D3D11 shared-texture binding and the actual quad construction are
// marked with TODOs; wire them up once the OpenXR SDK headers/toolchain are
// available locally to iterate against a real runtime.
namespace HookedFunctions
{
	void OnInstanceCreated(XrInstance instance, PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr);

	// Returns true and populates *function if `name` is one of the entry
	// points we override; otherwise returns false so the caller forwards to
	// the next layer/runtime.
	bool TryGetHooked(const char* name, PFN_xrVoidFunction* function);

	// Current cockpit anchor pose (in the local/world-locked reference
	// space), as last computed by RecenterAnchor(). Used by
	// ControllerInput.cpp to compose a panel's world-space pose the same
	// way Hook_xrEndFrame does, for hand-to-panel distance checks and grab
	// offset math. Returns identity + hasAnchor=false before the first
	// recenter.
	XrPosef GetAnchorPose(bool* hasAnchor);
}
