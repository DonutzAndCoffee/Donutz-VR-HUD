#pragma once

#include "HookedFunctions.h"
#include "IpcServer.h"

#include <d3d11.h>
#include <dxgiformat.h>

// Procedural "helmet visor" renderer. Ray-traces a virtual, head-attached
// ellipsoidal visor shell per eye in a full-screen pixel shader and writes a
// premultiplied-alpha image (glass tint + edge darkening, sun strip at the
// top, view-dependent scratches, bubbles and out-of-focus dirt). The images
// are submitted by Hook_xrEndFrame as an extra XrCompositionLayerProjection
// reusing the app's own per-eye poses/FOVs, so no placement is needed.
//
// All D3D11 work runs on the host app's immediate context inside
// xrEndFrame; the app's pipeline state is preserved via
// ID3D11DeviceContext1::SwapDeviceContextState.
namespace VisorRenderer
{
	// Renders one eye of the visor into `target` (a swapchain image).
	// headPose/eyePose/fov are expressed in the app projection layer's
	// space; that space is assumed to be gravity-aligned (+Y up), which is
	// true for LOCAL/STAGE spaces used by sims.
	bool RenderEye(
		ID3D11Device* device,
		ID3D11DeviceContext* context,
		ID3D11Texture2D* target,
		DXGI_FORMAT rtvFormat,
		uint32_t width,
		uint32_t height,
		const IpcServer::VisorParams& params,
		const XrPosef& headPose,
		const XrPosef& eyePose,
		const XrFovf& fov);

	// Releases all GPU resources (e.g. on session destruction).
	void Reset();
}
