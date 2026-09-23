#include "ApiLayerInterface.h"
#include "HookedFunctions.h"
#include "Logging.h"

#include <cstring>

namespace
{
	// The "next" xrGetInstanceProcAddr from the runtime/layer below us, and
	// the instance handle passed to it, captured during CreateApiLayerInstance
	// so GetInstanceProcAddr can forward unhandled functions.
	PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
}

namespace DonutzVrHud
{
	XrResult CreateApiLayerInstance(
		const XrInstanceCreateInfo* info,
		const XrApiLayerCreateInfo* apiLayerInfo,
		XrInstance* instance)
	{
		if (apiLayerInfo == nullptr || apiLayerInfo->nextInfo == nullptr)
		{
			return XR_ERROR_INITIALIZATION_FAILED;
		}

		// Save the next layer's GetInstanceProcAddr so our hooks (and our own
		// GetInstanceProcAddr) can forward calls that we do not intercept.
		g_nextGetInstanceProcAddr = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;

		XrApiLayerCreateInfo nextLayerInfo = *apiLayerInfo;
		nextLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;

		const PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance =
			apiLayerInfo->nextInfo->nextCreateApiLayerInstance;

		const XrResult result = nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
		if (XR_FAILED(result))
		{
			Logging::Log("CreateApiLayerInstance: nextCreateApiLayerInstance failed with XrResult " + std::to_string(result));
			return result;
		}

		Logging::Log("CreateApiLayerInstance: OpenXR instance created successfully, installing hooks.");
		HookedFunctions::OnInstanceCreated(*instance, g_nextGetInstanceProcAddr);
		return XR_SUCCESS;
	}

	XrResult GetInstanceProcAddr(
		XrInstance instance,
		const char* name,
		PFN_xrVoidFunction* function)
	{
		if (HookedFunctions::TryGetHooked(name, function))
		{
			return XR_SUCCESS;
		}

		if (g_nextGetInstanceProcAddr == nullptr)
		{
			return XR_ERROR_HANDLE_INVALID;
		}

		return g_nextGetInstanceProcAddr(instance, name, function);
	}
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL DonutzVrHud_xrNegotiateLoaderApiLayerInterface(
	const XrNegotiateLoaderInfo* loaderInfo,
	const char* /*apiLayerName*/,
	XrNegotiateApiLayerRequest* apiLayerRequest)
{
	Logging::Log("DonutzVrHud_xrNegotiateLoaderApiLayerInterface: called by OpenXR loader.");

	if (loaderInfo == nullptr || apiLayerRequest == nullptr)
	{
		Logging::Log("DonutzVrHud_xrNegotiateLoaderApiLayerInterface: null loaderInfo/apiLayerRequest.");
		return XR_ERROR_INITIALIZATION_FAILED;
	}

	if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
		loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo))
	{
		return XR_ERROR_INITIALIZATION_FAILED;
	}

	if (apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
		apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
		apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest))
	{
		return XR_ERROR_INITIALIZATION_FAILED;
	}

	if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
		loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
	{
		return XR_ERROR_INITIALIZATION_FAILED;
	}

	apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
	apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
	apiLayerRequest->getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(&DonutzVrHud::GetInstanceProcAddr);
	apiLayerRequest->createApiLayerInstance = reinterpret_cast<PFN_xrCreateApiLayerInstance>(&DonutzVrHud::CreateApiLayerInstance);

	Logging::Log("DonutzVrHud_xrNegotiateLoaderApiLayerInterface: negotiation succeeded.");
	return XR_SUCCESS;
}
