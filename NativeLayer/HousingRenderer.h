#pragma once

#include "HookedFunctions.h"
#include "IpcServer.h"
#include "LightProbe.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <vector>

// Procedural 3D panel housing. Each housed panel gets a box-shaped bezel
// whose screen opening matches the panel quad exactly; the bezel grows
// outward by bezelWidthMm and backward by depthMm. All housings are drawn
// per eye into one premultiplied-alpha projection layer submitted after the
// panel quads. The screen opening is first written into the depth buffer
// only (alpha 0), so the back of the housing never covers the panel content
// while the raised bezel lip still occludes the screen edges correctly.
//
// Lighting is sim-agnostic: a hemisphere ambient + environment specular
// derived from the app's own eye image (see LightProbe), so housings go dark
// at night or in tunnels without any sun telemetry.
namespace HousingRenderer
{
	struct Instance
	{
		XrPosef pose;          // panel centre in the projection layer's space
		float widthMeters;
		float heightMeters;
		IpcServer::HousingParams params;
		// Panel content shown on the housing's screen (premultiplied BGRA
		// swapchain image of the panel). Rendering it here instead of as a
		// separate quad layer guarantees that screen and housing share the
		// exact same stereo depth.
		ID3D11Texture2D* content = nullptr;
		int64_t contentFormat = 0;
		float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	};

	bool RenderEye(
		ID3D11Device* device,
		ID3D11DeviceContext* context,
		ID3D11Texture2D* target,
		DXGI_FORMAT rtvFormat,
		uint32_t width,
		uint32_t height,
		const XrPosef& eyePose,
		const XrFovf& fov,
		const std::vector<Instance>& instances,
		const donutz::ProbeLighting& lighting,
		ID3D11Texture2D* depthTarget = nullptr);

	// Projection planes used for the depth written to depthTarget
	// (D32_FLOAT, 0 = near, 1 = far), for XrCompositionLayerDepthInfoKHR.
	constexpr float kNearZ = 0.02f;
	constexpr float kFarZ = 50.0f;

	void Reset();
}
