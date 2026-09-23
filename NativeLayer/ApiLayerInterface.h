#pragma once

// This project implements an OpenXR API Layer, following the same overall
// architecture as OpenKneeboard: the layer is loaded by the OpenXR loader
// *inside the target application's process* (e.g. iRacing.exe) and wraps the
// application's OpenXR function pointers so it can inject additional
// XrCompositionLayerQuad entries into xrEndFrame without creating a session
// of its own.
//
// Build prerequisites (see ../README.md):
//   - OpenXR SDK headers (openxr.h, openxr_platform.h, loader_interfaces.h)
//     available via $(OpenXrSdkInclude) include path.
//   - MSVC C++ toolchain (Visual Studio "Desktop development with C++").

#include <windows.h>
#include <d3d11.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#ifdef __cplusplus
extern "C" {
#endif

// Entry point the OpenXR loader looks up (exported, see DonutzVrHudLayer.def)
// when it finds this layer referenced by the JSON manifest. Negotiates the
// loader<->layer interface version and hands back our
// xrCreateApiLayerInstance implementation.
XRAPI_ATTR XrResult XRAPI_CALL DonutzVrHud_xrNegotiateLoaderApiLayerInterface(
	const XrNegotiateLoaderInfo* loaderInfo,
	const char* apiLayerName,
	XrNegotiateApiLayerRequest* apiLayerRequest);

#ifdef __cplusplus
}
#endif

namespace DonutzVrHud
{
	// Creates the wrapped XrInstance and stores the "next" dispatch table so
	// hooked functions (see HookedFunctions.h) can forward calls to the real
	// runtime/other layers below us.
	XrResult CreateApiLayerInstance(
		const XrInstanceCreateInfo* info,
		const XrApiLayerCreateInfo* apiLayerInfo,
		XrInstance* instance);

	// Layer-provided implementation of xrGetInstanceProcAddr: returns our own
	// hooked functions for the entry points we care about, and otherwise
	// forwards to the next layer/runtime down the chain.
	XrResult GetInstanceProcAddr(
		XrInstance instance,
		const char* name,
		PFN_xrVoidFunction* function);
}
