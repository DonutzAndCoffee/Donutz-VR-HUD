#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "LightProbe.h"
#include "Logging.h"

#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace donutz
{
	namespace
	{
		constexpr int kStagingCount = 3;
		constexpr int kFrameInterval = 2;     // probe every N frames
		constexpr float kSmoothing = 0.2f;    // EMA factor per readback

		enum class Encoding
		{
			Unsupported,
			Unorm8Rgba,
			Unorm8Bgra,
			Unorm10,
			Half4,
		};

		struct FormatInfo
		{
			Encoding encoding = Encoding::Unsupported;
			DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
			bool srgbEncoded = false;
		};

		FormatInfo Classify(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return { Encoding::Unorm8Rgba, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true };
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return { Encoding::Unorm8Bgra, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true };
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return { Encoding::Unorm10, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, true };
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				return { Encoding::Half4, DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, false };
			default:
				return {};
			}
		}

		float HalfToFloat(uint16_t h)
		{
			const uint32_t sign = (h & 0x8000u) << 16;
			uint32_t exp = (h >> 10) & 0x1Fu;
			uint32_t mant = h & 0x3FFu;
			uint32_t bits;
			if (exp == 0)
			{
				if (mant == 0)
				{
					bits = sign;
				}
				else
				{
					exp = 127 - 15 + 1;
					while ((mant & 0x400u) == 0)
					{
						mant <<= 1;
						--exp;
					}
					mant &= 0x3FFu;
					bits = sign | (exp << 23) | (mant << 13);
				}
			}
			else if (exp == 31)
			{
				bits = sign | 0x7F800000u | (mant << 13);
			}
			else
			{
				bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
			}
			float f;
			std::memcpy(&f, &bits, sizeof(f));
			return f;
		}

		float SrgbToLinear(float c)
		{
			return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		struct State
		{
			ID3D11Device* device = nullptr;
			UINT width = 0;
			UINT height = 0;
			DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
			FormatInfo info;
			UINT mipLevels = 0;
			UINT readMip = 0;
			UINT readWidth = 0;
			UINT readHeight = 0;
			ComPtr<ID3D11Texture2D> resolveTexture;   // only for MSAA sources
			ComPtr<ID3D11Texture2D> mipTexture;
			ComPtr<ID3D11ShaderResourceView> mipSrv;
			ComPtr<ID3D11Texture2D> staging[kStagingCount];
			bool pending[kStagingCount] = {};
			int writeIndex = 0;
			uint64_t frame = 0;
			bool failed = false;
			ProbeLighting lighting;
			std::chrono::steady_clock::time_point lastLog{};
		};

		std::mutex g_mutex;
		State g_state;

		void ReleaseResources(State& s)
		{
			s.resolveTexture.Reset();
			s.mipSrv.Reset();
			s.mipTexture.Reset();
			for (int i = 0; i < kStagingCount; ++i)
			{
				s.staging[i].Reset();
				s.pending[i] = false;
			}
			s.device = nullptr;
			s.width = s.height = 0;
			s.sourceFormat = DXGI_FORMAT_UNKNOWN;
			s.failed = false;
		}

		bool EnsureResources(State& s, ID3D11Device* device, const D3D11_TEXTURE2D_DESC& srcDesc, DXGI_FORMAT srcFormat)
		{
			if (s.device == device && s.width == srcDesc.Width && s.height == srcDesc.Height && s.sourceFormat == srcFormat)
			{
				return !s.failed;
			}

			ReleaseResources(s);
			s.device = device;
			s.width = srcDesc.Width;
			s.height = srcDesc.Height;
			s.sourceFormat = srcFormat;
			s.info = Classify(srcFormat);
			if (s.info.encoding == Encoding::Unsupported)
			{
				Logging::Log("LightProbe: unsupported app swapchain format " + std::to_string(static_cast<int>(srcFormat)) + ", using neutral lighting.");
				s.failed = true;
				return false;
			}

			if (srcDesc.SampleDesc.Count > 1)
			{
				D3D11_TEXTURE2D_DESC rd = {};
				rd.Width = srcDesc.Width;
				rd.Height = srcDesc.Height;
				rd.MipLevels = 1;
				rd.ArraySize = 1;
				rd.Format = s.info.typeless;
				rd.SampleDesc.Count = 1;
				rd.Usage = D3D11_USAGE_DEFAULT;
				if (FAILED(device->CreateTexture2D(&rd, nullptr, &s.resolveTexture)))
				{
					s.failed = true;
					return false;
				}
			}

			UINT levels = 1;
			for (UINT w = srcDesc.Width, h = srcDesc.Height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2))
			{
				++levels;
			}
			s.mipLevels = levels;

			// Read a mip around 8x8: small enough to be cheap, large enough to
			// separate sky (upper half) from ground/cockpit (lower half).
			s.readMip = 0;
			UINT rw = srcDesc.Width, rh = srcDesc.Height;
			while ((rw > 8 || rh > 8) && s.readMip + 1 < levels)
			{
				rw = std::max(1u, rw / 2);
				rh = std::max(1u, rh / 2);
				++s.readMip;
			}
			s.readWidth = rw;
			s.readHeight = rh;

			D3D11_TEXTURE2D_DESC md = {};
			md.Width = srcDesc.Width;
			md.Height = srcDesc.Height;
			md.MipLevels = levels;
			md.ArraySize = 1;
			md.Format = s.info.typeless;
			md.SampleDesc.Count = 1;
			md.Usage = D3D11_USAGE_DEFAULT;
			md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
			if (FAILED(device->CreateTexture2D(&md, nullptr, &s.mipTexture)))
			{
				Logging::Log("LightProbe: failed to create mip texture.");
				s.failed = true;
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = s.info.view;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = levels;
			if (FAILED(device->CreateShaderResourceView(s.mipTexture.Get(), &sd, &s.mipSrv)))
			{
				Logging::Log("LightProbe: failed to create SRV.");
				s.failed = true;
				return false;
			}

			D3D11_TEXTURE2D_DESC st = {};
			st.Width = rw;
			st.Height = rh;
			st.MipLevels = 1;
			st.ArraySize = 1;
			st.Format = s.info.typeless;
			st.SampleDesc.Count = 1;
			st.Usage = D3D11_USAGE_STAGING;
			st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < kStagingCount; ++i)
			{
				if (FAILED(device->CreateTexture2D(&st, nullptr, &s.staging[i])))
				{
					s.failed = true;
					return false;
				}
			}

			Logging::Log("LightProbe: initialized " + std::to_string(srcDesc.Width) + "x" + std::to_string(srcDesc.Height) +
						 " reading mip " + std::to_string(s.readMip) + " (" + std::to_string(rw) + "x" + std::to_string(rh) + ").");
			return true;
		}

		void DecodePixel(const State& s, const uint8_t* row, UINT x, float& r, float& g, float& b)
		{
			switch (s.info.encoding)
			{
			case Encoding::Unorm8Rgba:
			case Encoding::Unorm8Bgra:
			{
				const uint8_t* p = row + x * 4;
				float c0 = p[0] / 255.0f, c1 = p[1] / 255.0f, c2 = p[2] / 255.0f;
				if (s.info.encoding == Encoding::Unorm8Bgra)
				{
					std::swap(c0, c2);
				}
				r = c0; g = c1; b = c2;
				break;
			}
			case Encoding::Unorm10:
			{
				uint32_t v;
				std::memcpy(&v, row + x * 4, 4);
				r = (v & 0x3FFu) / 1023.0f;
				g = ((v >> 10) & 0x3FFu) / 1023.0f;
				b = ((v >> 20) & 0x3FFu) / 1023.0f;
				break;
			}
			case Encoding::Half4:
			{
				uint16_t h[4];
				std::memcpy(h, row + x * 8, 8);
				r = HalfToFloat(h[0]); g = HalfToFloat(h[1]); b = HalfToFloat(h[2]);
				break;
			}
			default:
				r = g = b = 0.0f;
				return;
			}

			if (s.info.srgbEncoded)
			{
				r = SrgbToLinear(r); g = SrgbToLinear(g); b = SrgbToLinear(b);
			}
			r = std::clamp(std::isfinite(r) ? r : 0.0f, 0.0f, 4.0f);
			g = std::clamp(std::isfinite(g) ? g : 0.0f, 0.0f, 4.0f);
			b = std::clamp(std::isfinite(b) ? b : 0.0f, 0.0f, 4.0f);
		}

		void TryReadback(State& s, ID3D11DeviceContext* context, int index)
		{
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			const HRESULT hr = context->Map(s.staging[index].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING || FAILED(hr))
			{
				return;
			}

			float sky[3] = {}, ground[3] = {};
			int skyN = 0, groundN = 0;
			const UINT half = std::max(1u, s.readHeight / 2);
			for (UINT y = 0; y < s.readHeight; ++y)
			{
				const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
				for (UINT x = 0; x < s.readWidth; ++x)
				{
					float r, g, b;
					DecodePixel(s, row, x, r, g, b);
					float* target = (y < half) ? sky : ground;
					target[0] += r; target[1] += g; target[2] += b;
					(y < half ? skyN : groundN)++;
				}
			}
			context->Unmap(s.staging[index].Get(), 0);
			s.pending[index] = false;

			if (skyN == 0) { skyN = 1; }
			if (groundN == 0) { std::memcpy(ground, sky, sizeof(sky)); groundN = skyN; }
			for (int i = 0; i < 3; ++i)
			{
				sky[i] /= skyN;
				ground[i] /= groundN;
			}
			const float lum = 0.5f * (0.2126f * sky[0] + 0.7152f * sky[1] + 0.0722f * sky[2]) +
							  0.5f * (0.2126f * ground[0] + 0.7152f * ground[1] + 0.0722f * ground[2]);

			ProbeLighting& l = s.lighting;
			const float a = l.valid ? kSmoothing : 1.0f;
			l.skyR += (sky[0] - l.skyR) * a;
			l.skyG += (sky[1] - l.skyG) * a;
			l.skyB += (sky[2] - l.skyB) * a;
			l.groundR += (ground[0] - l.groundR) * a;
			l.groundG += (ground[1] - l.groundG) * a;
			l.groundB += (ground[2] - l.groundB) * a;
			l.avgLum += (lum - l.avgLum) * a;
			l.valid = true;

			const auto now = std::chrono::steady_clock::now();
			if (now - s.lastLog > std::chrono::seconds(1))
			{
				s.lastLog = now;
				std::ostringstream msg;
				msg.precision(3);
				msg << "LightProbe: sky=(" << l.skyR << "," << l.skyG << "," << l.skyB << ") ground=("
					<< l.groundR << "," << l.groundG << "," << l.groundB << ") lum=" << l.avgLum;
				Logging::Log(Logging::LogLevel::Debug, msg.str());
			}
		}
	}

	namespace LightProbe
	{
		void Update(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
					uint32_t arrayIndex, int64_t format)
		{
			if (device == nullptr || context == nullptr || source == nullptr)
			{
				return;
			}

			std::lock_guard<std::mutex> lock(g_mutex);
			State& s = g_state;

			// Always drain finished readbacks so results arrive promptly.
			for (int i = 0; i < kStagingCount; ++i)
			{
				if (s.pending[i] && s.staging[i])
				{
					TryReadback(s, context, i);
				}
			}

			if ((s.frame++ % kFrameInterval) != 0)
			{
				return;
			}

			D3D11_TEXTURE2D_DESC desc = {};
			source->GetDesc(&desc);
			if (!EnsureResources(s, device, desc, static_cast<DXGI_FORMAT>(format)))
			{
				return;
			}
			if (s.pending[s.writeIndex])
			{
				return; // ring full - GPU is behind, skip this probe
			}

			const UINT srcSub = D3D11CalcSubresource(0, std::min<UINT>(arrayIndex, desc.ArraySize - 1), desc.MipLevels);
			if (s.resolveTexture)
			{
				context->ResolveSubresource(s.resolveTexture.Get(), 0, source, srcSub, s.info.view);
				context->CopySubresourceRegion(s.mipTexture.Get(), 0, 0, 0, 0, s.resolveTexture.Get(), 0, nullptr);
			}
			else
			{
				context->CopySubresourceRegion(s.mipTexture.Get(), 0, 0, 0, 0, source, srcSub, nullptr);
			}
			context->GenerateMips(s.mipSrv.Get());
			context->CopySubresourceRegion(s.staging[s.writeIndex].Get(), 0, 0, 0, 0,
										   s.mipTexture.Get(), D3D11CalcSubresource(s.readMip, 0, s.mipLevels), nullptr);
			s.pending[s.writeIndex] = true;
			s.writeIndex = (s.writeIndex + 1) % kStagingCount;
		}

		ProbeLighting GetLighting()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			return g_state.lighting;
		}

		void Reset()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			ReleaseResources(g_state);
			g_state.lighting = ProbeLighting{};
			g_state.frame = 0;
			g_state.writeIndex = 0;
		}
	}
}
