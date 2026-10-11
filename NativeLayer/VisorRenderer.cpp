#include "VisorRenderer.h"
#include "Logging.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <iterator>
#include <windows.h>

using Microsoft::WRL::ComPtr;

namespace
{
	// Full-screen triangle + procedural visor. Coordinates on the visor are
	// expressed as (azimuth, elevation) in degrees on the shell, so all
	// features are attached to the "helmet" and keep their place while the
	// head moves, like a real visor.
	constexpr char kShaderSource[] = R"HLSL(
cbuffer VisorConstants : register(b0)
{
	float4 fovTan;   // tanLeft, tanRight, tanUp, tanDown
	float4 eyeRow0;  // eye->head rotation row 0, w = eye offset x (head frame)
	float4 eyeRow1;  // row 1, w = eye offset y
	float4 eyeRow2;  // row 2, w = eye offset z
	float4 upHead;   // world up in head frame
	float4 sunColor; // rgb, opacity
	float4 tintColor;// rgb, opacity
	float4 shape;    // stripHeight, stripCurve, scratches, dirt
	float4 misc;     // bubbles, seed (0..65535), unused, unused
	float4 geom;     // shell center offset xyz (head frame), size multiplier
	float4 textColor;// rgb, opacity
	float4 textGeom; // height (deg), blur (0..1), hasText, unused
};

Texture2D<float> textTex : register(t0);
SamplerState textSampler : register(s0);

struct VsOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VsOut VSMain(uint id : SV_VertexID)
{
	VsOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

uint Pcg(uint v)
{
	uint s = v * 747796405u + 2891336453u;
	uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}

float4 Rand4(int2 cell, uint salt)
{
	uint seed = (uint)misc.y;
	uint h = Pcg(asuint(cell.x) * 1597334677u ^ Pcg(asuint(cell.y) ^ (seed + salt * 0x9E3779B9u)));
	float4 r;
	r.x = h; h = Pcg(h);
	r.y = h; h = Pcg(h);
	r.z = h; h = Pcg(h);
	r.w = h;
	return r * (1.0 / 4294967296.0);
}

void Over(inout float4 acc, float3 rgb, float a)
{
	a = saturate(a);
	acc.rgb = rgb * a + acc.rgb * (1.0 - a);
	acc.a = a + acc.a * (1.0 - a);
}

static const float PI = 3.14159265;
static const float3 kRadii = float3(0.14, 0.12, 0.13); // real helmet scale, keeps panels outside the shell

float4 PSMain(VsOut i) : SV_Target
{
	float3 dirEye = normalize(float3(lerp(fovTan.x, fovTan.y, i.uv.x), lerp(fovTan.z, fovTan.w, i.uv.y), -1.0));
	float3x3 eyeToHead = float3x3(eyeRow0.xyz, eyeRow1.xyz, eyeRow2.xyz);
	float3 d = normalize(mul(eyeToHead, dirEye));
	float3 o = float3(eyeRow0.w, eyeRow1.w, eyeRow2.w) - geom.xyz;
	float3 radii = kRadii * max(geom.w, 0.2);

	// Ray vs. ellipsoid (origin inside -> take the far root).
	float3 os = o / radii;
	// Offsets tuned for a larger shell can push the eye outside the
	// (now helmet-sized) shell, which turns the visor opaque. Keep the eye inside.
	float osLen = length(os);
	if (osLen > 0.8) { os *= 0.8 / osLen; o = os * radii; }
	float3 ds = d / radii;
	float a = dot(ds, ds);
	float b = 2.0 * dot(os, ds);
	float c = dot(os, os) - 1.0;
	float t = (-b + sqrt(max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
	float3 p = o + d * t;
	float3 n = normalize(p / (radii * radii));

	float az = degrees(atan2(p.x, -p.z));
	float el = degrees(atan2(p.y, length(p.xz)));
	float2 q = float2(az, el);
	float pw = max(length(fwidth(q)), 1e-4);

	float3 up = normalize(upHead.xyz);
	float4 acc = 0;

	// Glass tint, darker at glancing angles (Fresnel-like).
	float cosView = abs(dot(n, d));
	float fres = pow(1.0 - cosView, 3.0);
	Over(acc, tintColor.rgb, tintColor.a + fres * 0.35 * (0.3 + tintColor.a));

	// Faint sky reflection on the inside of the shell.
	float3 refl = reflect(d, -n);
	float sheen = pow(saturate(dot(refl, up)), 6.0) * 0.05;
	acc.rgb += sheen;
	acc.a = saturate(acc.a + sheen * 0.3);

	// Sun strip along the top, lower edge dipping towards the middle.
	float stripHeight = saturate(shape.x);
	float dip = saturate(shape.y) * 20.0 * (1.0 - saturate((az / 80.0) * (az / 80.0)));
	float edge = lerp(45.0, -5.0, stripHeight) - dip;
	float soft = 2.5;
	float stripMask = smoothstep(edge - soft, edge + soft, el);
	float stripRamp = lerp(0.9, 1.0, saturate((el - edge) / 10.0));
	// Boosted so that ~80%+ already blocks the sun completely.
	float sunAlpha = saturate(sunColor.a * 1.25);
	Over(acc, sunColor.rgb, saturate(sunAlpha * stripRamp * (1.0 + sunAlpha * 0.2)) * stripMask);

	// Lettering on the strip (printed on the outside -> mirrored), out of focus.
	if (textGeom.z > 0.5 && textColor.a > 0.0)
	{
		float textH = max(textGeom.x, 0.5);
		float textW = textH * 8.0; // texture aspect 1024x128
		float edge0 = lerp(45.0, -5.0, stripHeight) - saturate(shape.y) * 20.0;
		float centerEl = edge0 + textH * 0.75 + soft;
		float2 uv = float2(0.5 - az / textW, 0.5 - (el - centerEl) / textH);
		if (all(uv > -0.1) && all(uv < 1.1))
		{
			float blurR = textGeom.y * 0.06 + 0.002;
			float2 blurUv = float2(blurR / 8.0, blurR);
			float sum = 0.0;
			float wsum = 0.0;
			[unroll] for (int ty = -2; ty <= 2; ++ty)
			[unroll] for (int tx = -2; tx <= 2; ++tx)
			{
				float w = exp(-(tx * tx + ty * ty) / 4.0);
				sum += textTex.SampleLevel(textSampler, uv + float2(tx, ty) * blurUv * 0.5, 0) * w;
				wsum += w;
			}
			Over(acc, textColor.rgb, textColor.a * (sum / wsum) * stripMask);
		}
	}

	// Glint factor: how the light catches scratches depends on head roll/pitch.
	float glintPhase = atan2(-up.x, up.y) * 4.0 + up.z * 6.0;

	// Out-of-focus dirt blobs.
	float dirt = saturate(shape.w);
	if (dirt > 0.0)
	{
		const float cellSize = 3.0;
		int2 baseCell = int2(floor(q / cellSize));
		[loop] for (int y = -1; y <= 1; ++y)
		[loop] for (int x = -1; x <= 1; ++x)
		{
			int2 cell = baseCell + int2(x, y);
			float4 r = Rand4(cell, 3u);
			if (r.x < dirt * 0.45)
			{
				float4 r2 = Rand4(cell, 4u);
				float2 center = (float2(cell) + r.yz) * cellSize;
				float radius = lerp(0.15, 0.9, r.w * r.w);
				float2 v = q - center;
				float m = exp(-dot(v, v) / (radius * radius));
				Over(acc, float3(0.33, 0.30, 0.26), m * lerp(0.12, 0.4, r2.x));
			}
		}

		// Fine dust specks.
		const float dustCell = 0.8;
		int2 dustBase = int2(floor(q / dustCell));
		[loop] for (int dy = -1; dy <= 1; ++dy)
		[loop] for (int dx = -1; dx <= 1; ++dx)
		{
			int2 cell = dustBase + int2(dx, dy);
			float4 r = Rand4(cell, 5u);
			if (r.x < dirt * 0.12)
			{
				float2 center = (float2(cell) + r.yz) * dustCell;
				float radius = lerp(0.04, 0.12, r.w) + pw;
				float2 v = q - center;
				float m = exp(-dot(v, v) / (radius * radius));
				Over(acc, float3(0.25, 0.23, 0.2), m * 0.35);
			}
		}
	}

	// Bubbles trapped in the material: bright rim + small highlight.
	float bubbles = saturate(misc.x);
	if (bubbles > 0.0)
	{
		const float cellSize = 5.0;
		int2 baseCell = int2(floor(q / cellSize));
		[loop] for (int y = -1; y <= 1; ++y)
		[loop] for (int x = -1; x <= 1; ++x)
		{
			int2 cell = baseCell + int2(x, y);
			float4 r = Rand4(cell, 6u);
			if (r.x < bubbles * 0.35)
			{
				float2 center = (float2(cell) + r.yz) * cellSize;
				float radius = lerp(0.1, 0.4, r.w);
				float2 v = q - center;
				float dist = length(v);
				float ringW = radius * 0.25 + pw;
				float rim = exp(-pow((dist - radius) / ringW, 2.0));
				float2 hlPos = float2(-0.35, 0.35) * radius;
				float hlR = radius * 0.2 + pw;
				float hl = exp(-dot(v - hlPos, v - hlPos) / (hlR * hlR));
				float light = rim * 0.35 + hl * 0.6;
				acc.rgb += light;
				acc.a = saturate(acc.a + rim * 0.2);
			}
		}
	}

	// Fine scratches: thin segments that light up depending on head angle.
	float scratches = saturate(shape.z);
	if (scratches > 0.0)
	{
		const float cellSize = 7.0;
		int2 baseCell = int2(floor(q / cellSize));
		[loop] for (int y = -1; y <= 1; ++y)
		[loop] for (int x = -1; x <= 1; ++x)
		{
			int2 cell = baseCell + int2(x, y);
			float4 r = Rand4(cell, 1u);
			if (r.x < scratches * 0.7)
			{
				float4 r2 = Rand4(cell, 2u);
				float2 center = (float2(cell) + r.yz) * cellSize;
				// Wiping scratches tend to be mostly horizontal.
				float angle = (r.w - 0.5) * (r2.w < 0.7 ? 0.8 : PI);
				float2 dir = float2(cos(angle), sin(angle));
				float halfLen = lerp(0.75, 3.5, r2.x);
				float2 v = q - center;
				float along = clamp(dot(v, dir), -halfLen, halfLen);
				float dist = length(v - dir * along);
				float width = lerp(0.012, 0.035, r2.y);
				float effW = max(width, pw);
				float stroke = saturate(1.0 - dist / effW) * (width / effW);
				stroke *= smoothstep(halfLen, halfLen * 0.5, abs(along));
				float glint = 0.15 + 0.85 * pow(saturate(cos(angle * 2.0 + glintPhase + r2.z * 2.0 * PI)), 12.0);
				float s = stroke * glint;
				acc.rgb += 0.9 * s;
				acc.a = saturate(acc.a + s * 0.15);
			}
		}
	}

	acc.rgb = min(acc.rgb, 1.0);
	return acc;
}
)HLSL";

	struct Constants
	{
		float fovTan[4];
		float eyeRow0[4];
		float eyeRow1[4];
		float eyeRow2[4];
		float upHead[4];
		float sunColor[4];
		float tintColor[4];
		float shape[4];
		float misc[4];
		float geom[4];
		float textColor[4];
		float textGeom[4];
	};
	static_assert(sizeof(Constants) % 16 == 0, "Constant buffer size must be a multiple of 16.");

	std::mutex g_mutex;
	ID3D11Device* g_device = nullptr;
	ComPtr<ID3D11VertexShader> g_vs;
	ComPtr<ID3D11PixelShader> g_ps;
	ComPtr<ID3D11Buffer> g_cb;
	ComPtr<ID3DDeviceContextState> g_state;
	ComPtr<ID3D11SamplerState> g_sampler;
	ComPtr<ID3D11ShaderResourceView> g_textSrv;
	std::wstring g_textCached;
	bool g_initFailed = false;

	constexpr int kTextTexWidth = 1024;
	constexpr int kTextTexHeight = 128;

	// Rasterizes the text with GDI into a single-channel texture (white on black).
	void UpdateTextTextureLocked(ID3D11Device* device, const std::wstring& text, const std::wstring& fontName, bool bold, bool italic)
	{
		const std::wstring cacheKey = text + L"|" + fontName + (bold ? L"|b" : L"|") + (italic ? L"i" : L"");
		if (cacheKey == g_textCached && (g_textSrv || text.empty()))
		{
			return;
		}
		g_textCached = cacheKey;
		g_textSrv.Reset();
		if (text.empty())
		{
			return;
		}

		HDC dc = CreateCompatibleDC(nullptr);
		BITMAPINFO bmi{};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = kTextTexWidth;
		bmi.bmiHeader.biHeight = -kTextTexHeight;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!dc || !bmp || !bits)
		{
			if (bmp) DeleteObject(bmp);
			if (dc) DeleteDC(dc);
			Logging::Log("VisorRenderer: creating text bitmap failed.");
			return;
		}
		HGDIOBJ oldBmp = SelectObject(dc, bmp);
		std::memset(bits, 0, kTextTexWidth * kTextTexHeight * 4);

		// Shrink the font until the text fits the texture width.
		int fontHeight = 112;
		HFONT font = nullptr;
		for (;;)
		{
			font = CreateFontW(-fontHeight, 0, 0, 0, bold ? FW_HEAVY : FW_NORMAL, italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, fontName.empty() ? L"Arial" : fontName.c_str());
			HGDIOBJ oldFont = SelectObject(dc, font);
			SIZE extent{};
			GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
			SelectObject(dc, oldFont);
			if (extent.cx <= kTextTexWidth - 16 || fontHeight <= 16)
			{
				break;
			}
			DeleteObject(font);
			fontHeight -= 8;
		}

		HGDIOBJ oldFont = SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));
		RECT rect{ 0, 0, kTextTexWidth, kTextTexHeight };
		DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		GdiFlush();

		std::vector<uint8_t> mask(kTextTexWidth * kTextTexHeight);
		const uint8_t* src = static_cast<const uint8_t*>(bits);
		for (size_t i = 0; i < mask.size(); ++i)
		{
			mask[i] = src[i * 4 + 1];
		}

		SelectObject(dc, oldFont);
		DeleteObject(font);
		SelectObject(dc, oldBmp);
		DeleteObject(bmp);
		DeleteDC(dc);

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTextTexWidth;
		desc.Height = kTextTexHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data{ mask.data(), static_cast<UINT>(kTextTexWidth), 0 };
		ComPtr<ID3D11Texture2D> tex;
		if (FAILED(device->CreateTexture2D(&desc, &data, &tex)) ||
			FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &g_textSrv)))
		{
			Logging::Log("VisorRenderer: creating text texture failed.");
			g_textSrv.Reset();
		}
	}

	XrQuaternionf Conjugate(const XrQuaternionf& q)
	{
		return XrQuaternionf{ -q.x, -q.y, -q.z, q.w };
	}

	XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b)
	{
		return XrQuaternionf{
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
		};
	}

	XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
	{
		const float tx = 2.0f * (q.y * v.z - q.z * v.y);
		const float ty = 2.0f * (q.z * v.x - q.x * v.z);
		const float tz = 2.0f * (q.x * v.y - q.y * v.x);
		return XrVector3f{
			v.x + q.w * tx + (q.y * tz - q.z * ty),
			v.y + q.w * ty + (q.z * tx - q.x * tz),
			v.z + q.w * tz + (q.x * ty - q.y * tx)
		};
	}

	float SrgbToLinear(float c)
	{
		return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}

	bool IsSrgb(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	}

	ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
	{
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "VisorRenderer", nullptr, nullptr,
			entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr))
		{
			std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string();
			Logging::Log("VisorRenderer: shader compile failed (" + std::string(entry) + "): " + message);
			return nullptr;
		}
		return code;
	}

	void ReleaseLocked()
	{
		g_vs.Reset();
		g_ps.Reset();
		g_cb.Reset();
		g_state.Reset();
		g_sampler.Reset();
		g_textSrv.Reset();
		g_textCached.clear();
		g_device = nullptr;
		g_initFailed = false;
	}

	bool EnsureInitializedLocked(ID3D11Device* device)
	{
		if (g_device == device && g_vs && g_ps && g_cb && g_state && g_sampler)
		{
			return true;
		}
		if (g_device == device && g_initFailed)
		{
			return false;
		}

		ReleaseLocked();
		g_device = device;
		g_initFailed = true;

		ComPtr<ID3D11Device1> device1;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
		{
			Logging::Log("VisorRenderer: ID3D11Device1 not available, visor disabled.");
			return false;
		}

		const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
		if (FAILED(device1->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device1), nullptr, &g_state)))
		{
			Logging::Log("VisorRenderer: CreateDeviceContextState failed, visor disabled.");
			return false;
		}

		auto vsCode = Compile("VSMain", "vs_5_0");
		auto psCode = Compile("PSMain", "ps_5_0");
		if (!vsCode || !psCode)
		{
			return false;
		}

		if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs)) ||
			FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &g_ps)))
		{
			Logging::Log("VisorRenderer: creating shaders failed.");
			return false;
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(Constants);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_cb)))
		{
			Logging::Log("VisorRenderer: creating constant buffer failed.");
			return false;
		}

		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampDesc, &g_sampler)))
		{
			Logging::Log("VisorRenderer: creating sampler failed.");
			return false;
		}

		g_initFailed = false;
		Logging::Log("VisorRenderer: initialized.");
		return true;
	}
}

namespace VisorRenderer
{
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
		const XrFovf& fov)
	{
		if (device == nullptr || context == nullptr || target == nullptr || width == 0 || height == 0)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		if (!EnsureInitializedLocked(device))
		{
			return false;
		}

		ComPtr<ID3D11DeviceContext1> context1;
		if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
		{
			return false;
		}

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = rtvFormat;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11RenderTargetView> rtv;
		if (FAILED(device->CreateRenderTargetView(target, &rtvDesc, &rtv)))
		{
			Logging::Log("VisorRenderer: CreateRenderTargetView failed (format=" + std::to_string(rtvFormat) + ").");
			return false;
		}

		// Eye relative to the head (center between the eyes).
		const XrQuaternionf headInv = Conjugate(headPose.orientation);
		const XrQuaternionf eyeToHead = Multiply(headInv, eyePose.orientation);
		const XrVector3f eyeOffset = Rotate(headInv, XrVector3f{
			eyePose.position.x - headPose.position.x,
			eyePose.position.y - headPose.position.y,
			eyePose.position.z - headPose.position.z });
		const XrVector3f upHead = Rotate(headInv, XrVector3f{ 0.0f, 1.0f, 0.0f });

		// Columns of the eye->head rotation matrix are the rotated basis vectors.
		const XrVector3f ex = Rotate(eyeToHead, XrVector3f{ 1.0f, 0.0f, 0.0f });
		const XrVector3f ey = Rotate(eyeToHead, XrVector3f{ 0.0f, 1.0f, 0.0f });
		const XrVector3f ez = Rotate(eyeToHead, XrVector3f{ 0.0f, 0.0f, 1.0f });

		const bool srgb = IsSrgb(rtvFormat);
		auto color = [srgb](float c) { c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c); return srgb ? SrgbToLinear(c) : c; };

		Constants constants{};
		constants.fovTan[0] = tanf(fov.angleLeft);
		constants.fovTan[1] = tanf(fov.angleRight);
		constants.fovTan[2] = tanf(fov.angleUp);
		constants.fovTan[3] = tanf(fov.angleDown);
		constants.eyeRow0[0] = ex.x; constants.eyeRow0[1] = ey.x; constants.eyeRow0[2] = ez.x; constants.eyeRow0[3] = eyeOffset.x;
		constants.eyeRow1[0] = ex.y; constants.eyeRow1[1] = ey.y; constants.eyeRow1[2] = ez.y; constants.eyeRow1[3] = eyeOffset.y;
		constants.eyeRow2[0] = ex.z; constants.eyeRow2[1] = ey.z; constants.eyeRow2[2] = ez.z; constants.eyeRow2[3] = eyeOffset.z;
		constants.upHead[0] = upHead.x; constants.upHead[1] = upHead.y; constants.upHead[2] = upHead.z;
		constants.sunColor[0] = color(params.sunR); constants.sunColor[1] = color(params.sunG); constants.sunColor[2] = color(params.sunB);
		constants.sunColor[3] = params.sunOpacity;
		constants.tintColor[0] = color(params.tintR); constants.tintColor[1] = color(params.tintG); constants.tintColor[2] = color(params.tintB);
		constants.tintColor[3] = params.tintOpacity;
		constants.shape[0] = params.stripHeight;
		constants.shape[1] = params.stripCurve;
		constants.shape[2] = params.scratches;
		constants.shape[3] = params.dirt;
		constants.misc[0] = params.bubbles;
		constants.misc[1] = static_cast<float>(params.seed % 65536u);
		constants.geom[0] = params.offsetX;
		constants.geom[1] = params.offsetY;
		constants.geom[2] = params.offsetZ;
		constants.geom[3] = params.size > 0.0f ? params.size : 1.0f;

		size_t textLen = 0;
		while (textLen < std::size(params.text) && params.text[textLen] != L'\0')
		{
			++textLen;
		}
		size_t fontLen = 0;
		while (fontLen < std::size(params.fontName) && params.fontName[fontLen] != L'\0')
		{
			++fontLen;
		}
		UpdateTextTextureLocked(device, std::wstring(params.text, textLen), std::wstring(params.fontName, fontLen), params.bold != 0, params.italic != 0);
		constants.textColor[0] = color(params.textR); constants.textColor[1] = color(params.textG); constants.textColor[2] = color(params.textB);
		constants.textColor[3] = params.textOpacity;
		constants.textGeom[0] = 2.0f + 10.0f * params.textSize;
		constants.textGeom[1] = params.textBlur;
		constants.textGeom[2] = g_textSrv ? 1.0f : 0.0f;

		ComPtr<ID3DDeviceContextState> previousState;
		context1->SwapDeviceContextState(g_state.Get(), &previousState);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			std::memcpy(mapped.pData, &constants, sizeof(constants));
			context->Unmap(g_cb.Get(), 0);
		}

		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(width);
		viewport.Height = static_cast<float>(height);
		viewport.MaxDepth = 1.0f;

		ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
		ID3D11Buffer* cbs[] = { g_cb.Get() };
		context->OMSetRenderTargets(1, rtvs, nullptr);
		context->RSSetViewports(1, &viewport);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_vs.Get(), nullptr, 0);
		context->PSSetShader(g_ps.Get(), nullptr, 0);
		context->PSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[] = { g_textSrv.Get() };
		ID3D11SamplerState* samplers[] = { g_sampler.Get() };
		context->PSSetShaderResources(0, 1, srvs);
		context->PSSetSamplers(0, 1, samplers);
		context->Draw(3, 0);

		ID3D11ShaderResourceView* nullSrv[] = { nullptr };
		context->PSSetShaderResources(0, 1, nullSrv);

		ID3D11RenderTargetView* nullRtv[] = { nullptr };
		context->OMSetRenderTargets(1, nullRtv, nullptr);

		context1->SwapDeviceContextState(previousState.Get(), nullptr);
		return true;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ReleaseLocked();
	}
}
