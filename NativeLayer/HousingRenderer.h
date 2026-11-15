#pragma once

#include "HookedFunctions.h"
#include "IpcServer.h"
#include "LightProbe.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <string>
#include <vector>

// Procedural 3D panel housing. Each housed panel gets a mesh built by sweeping
// a rounded-rectangle outline along its housing profile (see IpcServer::HousingProfile).
// Stable mode renders only the housing into panel-space eye quads, preserving the
// separate dashboard. The legacy stereo mode uses head-facing eye quads; the true
// projection mode can also render depth into a runtime-owned D32 target.
//
// Lighting is sim-agnostic: a hemisphere ambient + environment specular
// derived from the app's own eye image (see LightProbe). The view vector
// uses each eye's own position, so housing reflections, fresnel and
// specular highlights shift correctly between the two eyes.
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
		XrPosef contentPose{};
		float contentWidthMeters = 0.0f;
		float contentHeightMeters = 0.0f;
		bool opaqueContent = false;
	};

	// Snapshot while the source swapchain image is still acquired.
	bool UpdateContent(ID3D11Device* device, ID3D11DeviceContext* context,
		const std::string& key, ID3D11Texture2D* source);

	// Called once per frame with keys from all reference spaces.
	void PruneMeshes(const std::vector<std::string>& liveKeys);

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
		bool renderContent = true,
		ID3D11Texture2D* depthTarget = nullptr);

	// Projection planes used for rendering.
	constexpr float kNearZ = 0.02f;
	constexpr float kFarZ = 50.0f;

	void Reset();
}
