#pragma once

#include <d3d11.h>
#include <cstdint>

namespace donutz
{
	struct ProbeLighting
	{
		// Linear RGB, roughly 0..1 (scene-referred from the app image).
		float skyR = 0.05f, skyG = 0.05f, skyB = 0.06f;
		float groundR = 0.03f, groundG = 0.03f, groundB = 0.03f;
		float avgLum = 0.04f;
		bool valid = false;
	};

	// Sim-agnostic ambient light estimate derived from the app's own rendered
	// eye image: downsample on the GPU, read back a tiny mip asynchronously,
	// split upper/lower halves into sky/ground colours and smooth over time.
	namespace LightProbe
	{
		void Update(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
					uint32_t arrayIndex, int64_t format);
		ProbeLighting GetLighting();
		void Reset();
	}
}
