#pragma once

#include <d3d11.h>
#include <string>

// Detects the visible (non-transparent) content rectangle of a panel frame,
// e.g. a SimHub overlay that only draws in part of its surface. Uses an
// asynchronous staging readback of the alpha channel every few frames so
// the housing can automatically wrap the actual content instead of the
// full quad.
namespace ContentBounds
{
	struct Rect
	{
		// Normalized texture coordinates, (0,0) = top-left.
		float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
		bool valid = false;
	};

	void Update(const std::string& key, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source);
	Rect Get(const std::string& key);
	void Reset();
}
