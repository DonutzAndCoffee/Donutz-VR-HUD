#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ContentBounds.h"
#include "Logging.h"

#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr int kInterval = 30;      // frames between probes
	constexpr uint8_t kAlphaThreshold = 16;

	struct Entry
	{
		ComPtr<ID3D11Texture2D> staging;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		bool pending = false;
		int frame = 0;
		ContentBounds::Rect rect;
	};

	std::mutex g_mutex;
	std::unordered_map<std::string, Entry> g_entries;

	bool IsFourByteAlphaLast(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			return true;
		default:
			return false;
		}
	}

	void TryRead(const std::string& key, Entry& e, ID3D11DeviceContext* context)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = context->Map(e.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (FAILED(hr))
		{
			return; // still drawing (or failed) - retry next frame
		}

		int minX = static_cast<int>(e.width), minY = static_cast<int>(e.height), maxX = -1, maxY = -1;
		for (UINT y = 0; y < e.height; ++y)
		{
			const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
			for (UINT x = 0; x < e.width; ++x)
			{
				if (row[x * 4 + 3] > kAlphaThreshold)
				{
					minX = std::min(minX, static_cast<int>(x));
					maxX = std::max(maxX, static_cast<int>(x));
					minY = std::min(minY, static_cast<int>(y));
					maxY = std::max(maxY, static_cast<int>(y));
				}
			}
		}
		context->Unmap(e.staging.Get(), 0);
		e.pending = false;

		ContentBounds::Rect r;
		if (maxX >= minX && maxY >= minY)
		{
			r.u0 = static_cast<float>(minX) / e.width;
			r.v0 = static_cast<float>(minY) / e.height;
			r.u1 = static_cast<float>(maxX + 1) / e.width;
			r.v1 = static_cast<float>(maxY + 1) / e.height;
			r.valid = true;
		}

		const bool changed = r.valid != e.rect.valid ||
			std::fabs(r.u0 - e.rect.u0) + std::fabs(r.v0 - e.rect.v0) + std::fabs(r.u1 - e.rect.u1) + std::fabs(r.v1 - e.rect.v1) > 0.002f;
		if (changed)
		{
			e.rect = r;
			Logging::Log("ContentBounds: panel " + key + " content rect u=[" + std::to_string(r.u0) + "," + std::to_string(r.u1) +
				"] v=[" + std::to_string(r.v0) + "," + std::to_string(r.v1) + "] valid=" + std::to_string(r.valid) + ".");
		}
	}
}

namespace ContentBounds
{
	void Update(const std::string& key, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source)
	{
		if (device == nullptr || context == nullptr || source == nullptr)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		Entry& e = g_entries[key];

		if (e.pending && e.staging)
		{
			TryRead(key, e, context);
		}
		if (e.pending || (e.frame++ % kInterval) != 0)
		{
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		if (!IsFourByteAlphaLast(desc.Format) || desc.SampleDesc.Count != 1)
		{
			return;
		}

		if (!e.staging || e.width != desc.Width || e.height != desc.Height || e.format != desc.Format)
		{
			e.staging.Reset();
			D3D11_TEXTURE2D_DESC sd{};
			sd.Width = desc.Width;
			sd.Height = desc.Height;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.Format = desc.Format;
			sd.SampleDesc.Count = 1;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(device->CreateTexture2D(&sd, nullptr, &e.staging)))
			{
				return;
			}
			e.width = desc.Width;
			e.height = desc.Height;
			e.format = desc.Format;
		}

		context->CopySubresourceRegion(e.staging.Get(), 0, 0, 0, 0, source, 0, nullptr);
		e.pending = true;
	}

	Rect Get(const std::string& key)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_entries.find(key);
		return it != g_entries.end() ? it->second.rect : Rect{};
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_entries.clear();
	}
}
