#pragma once

#include "HookedFunctions.h"
#include "IpcServer.h"
#include "LightProbe.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <string>
#include <vector>

// Procedural 3D panel housing ("Mesh" render mode). Each housed panel gets a
// mesh built by sweeping a rounded-rectangle outline along the panel's
// housing profile (see IpcServer::HousingProfile). Each housing is drawn
// per eye in the panel's own frame with an off-axis projection whose image
// plane is the panel front plane; the result is submitted as two
// eye-specific quad layers (left/right) with exactly the content quad's
// pose/space, so the compositor reprojects housing and content identically.
// The panel content itself stays a separate, sharp quad layer submitted
// after it.
//
// Lighting is sim-agnostic: a hemisphere ambient + environment specular
// derived from the app's own eye image (see LightProbe). The view vector
// uses the head centre for both eyes so highlights do not differ between
// the eyes (no stereo shimmer).
namespace HousingRenderer
{
	struct Instance
	{
		std::string key;       // panel key, used for the cached mesh
		XrPosef pose;          // display centre in the projection layer's space
		float widthMeters;     // display (opening) size
		float heightMeters;
		IpcServer::HousingParams params;
		IpcServer::HousingLeds leds{};
		const IpcServer::HousingProfile* profile = nullptr;
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
		const XrVector3f& headPosition,
		const std::vector<Instance>& instances,
		const donutz::ProbeLighting& lighting,
		ID3D11Texture2D* depthTarget = nullptr);

	// Projection planes used for the depth written to depthTarget
	// (D32_FLOAT, 0 = near, 1 = far), for XrCompositionLayerDepthInfoKHR.
	constexpr float kNearZ = 0.02f;
	constexpr float kFarZ = 50.0f;

	void Reset();
}
