#include "HousingRenderer.h"
#include "Logging.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr char kShaderSource[] = R"(
cbuffer Constants : register(b0)
{
	row_major float4x4 viewProj;
	float4 eyePos;      // xyz
	float4 sky;         // rgb
	float4 ground;      // rgb
	float4 misc;        // x = encode sRGB in shader, y = has content, z = content needs sRGB decode
};

Texture2D contentTex : register(t0);
SamplerState contentSampler : register(s0);

struct VSIn
{
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float4 albedo : COLOR0;     // rgb linear, a = 1 housing / 0 screen
	float2 material : TEXCOORD0; // x roughness, y metallic
	float2 uv : TEXCOORD1;
};

struct PSIn
{
	float4 pos : SV_POSITION;
	float3 world : TEXCOORD0;
	float3 normal : NORMAL;
	float4 albedo : COLOR0;
	float2 material : TEXCOORD1;
	float2 uv : TEXCOORD2;
};

PSIn VSMain(VSIn input)
{
	PSIn o;
	o.pos = mul(viewProj, float4(input.pos, 1.0));
	o.world = input.pos;
	o.normal = input.normal;
	o.albedo = input.albedo;
	o.material = input.material;
	o.uv = input.uv;
	return o;
}

float3 Hemisphere(float y)
{
	return lerp(ground.rgb, sky.rgb, saturate(y * 0.5 + 0.5));
}

float4 PSMain(PSIn input) : SV_TARGET
{
	// Screen opening: rendered as a switched-off display (near-black glossy
	// glass reflecting the ambient light). The panel quad is composited on
	// top of it, so the dashboard appears to be shown on this screen.
	if (input.albedo.a < 0.5)
	{
		float3 sn = normalize(input.normal);
		float3 sv = normalize(eyePos.xyz - input.world);
		if (dot(sn, sv) < 0) sn = -sn;
		float sndv = saturate(dot(sn, sv));
		float sf = 0.04 + 0.96 * pow(1.0 - sndv, 5.0);
		float3 sr = reflect(-sv, sn);
		float3 screen = float3(0.008, 0.009, 0.011) * Hemisphere(sn.y) * 4.0 + Hemisphere(sr.y) * sf;
		if (misc.y > 0.5)
		{
			// Premultiplied panel content over the switched-off display.
			float4 c = contentTex.Sample(contentSampler, input.uv);
			if (misc.z > 0.5)
			{
				c.rgb = pow(saturate(c.rgb), 2.2);
			}
			screen = c.rgb + screen * (1.0 - c.a);
		}
		if (misc.x > 0.5)
		{
			screen = pow(saturate(screen), 1.0 / 2.2);
		}
		return float4(screen, 1.0);
	}

	float3 n = normalize(input.normal);
	float3 v = normalize(eyePos.xyz - input.world);
	if (dot(n, v) < 0) n = -n;

	float roughness = input.material.x;
	float metallic = input.material.y;
	float3 albedo = input.albedo.rgb;

	float3 diffuse = albedo * (1.0 - metallic) * Hemisphere(n.y);

	float3 r = reflect(-v, n);
	float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
	float ndv = saturate(dot(n, v));
	float3 fresnel = f0 + (max(1.0 - roughness, f0) - f0) * pow(1.0 - ndv, 5.0);
	// Rough surfaces see a blurrier, more averaged environment.
	float3 env = lerp(Hemisphere(r.y), (sky.rgb + ground.rgb) * 0.5, roughness);
	float3 specular = env * fresnel * (1.0 - 0.7 * roughness);

	float3 color = diffuse + specular;
	if (misc.x > 0.5)
	{
		color = pow(saturate(color), 1.0 / 2.2);
	}
	return float4(color, 1.0);
}
)";

	struct Vertex
	{
		float pos[3];
		float normal[3];
		float albedo[4];
		float material[2];
		float uv[2];
	};

	struct Constants
	{
		float viewProj[16];
		float eyePos[4];
		float sky[4];
		float ground[4];
		float misc[4];
	};

	std::mutex g_mutex;
	ID3D11Device* g_device = nullptr;
	bool g_initFailed = false;
	ComPtr<ID3DDeviceContextState> g_state;
	ComPtr<ID3D11VertexShader> g_vs;
	ComPtr<ID3D11PixelShader> g_ps;
	ComPtr<ID3D11InputLayout> g_layout;
	ComPtr<ID3D11Buffer> g_cb;
	ComPtr<ID3D11Buffer> g_vb;
	UINT g_vbCapacity = 0;
	ComPtr<ID3D11RasterizerState> g_raster;
	ComPtr<ID3D11DepthStencilState> g_depthState;
	ComPtr<ID3D11BlendState> g_blend;
	ComPtr<ID3D11SamplerState> g_sampler;
	ComPtr<ID3D11Texture2D> g_depth;
	ComPtr<ID3D11DepthStencilView> g_dsv;
	UINT g_depthW = 0;
	UINT g_depthH = 0;

	// Multisampled colour target, resolved into the swapchain image.
	ComPtr<ID3D11Texture2D> g_msaaColor;
	ComPtr<ID3D11RenderTargetView> g_msaaRtv;
	DXGI_FORMAT g_msaaFormat = DXGI_FORMAT_UNKNOWN;
	UINT g_sampleCount = 1;

	// Mipmapped copies of the panel content so minified text stays crisp
	// instead of aliasing.
	struct ContentMips
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11ShaderResourceView> srv;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};
	std::unordered_map<ID3D11Texture2D*, ContentMips> g_contentMips;

	struct Vec3 { float x, y, z; };

	Vec3 Rotate(const XrQuaternionf& q, const Vec3& v)
	{
		const float tx = 2.0f * (q.y * v.z - q.z * v.y);
		const float ty = 2.0f * (q.z * v.x - q.x * v.z);
		const float tz = 2.0f * (q.x * v.y - q.y * v.x);
		return {
			v.x + q.w * tx + (q.y * tz - q.z * ty),
			v.y + q.w * ty + (q.z * tx - q.x * tz),
			v.z + q.w * tz + (q.x * ty - q.y * tx) };
	}

	float SrgbToLinear(float c)
	{
		c = std::clamp(c, 0.0f, 1.0f);
		return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}

	bool IsSrgb(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	}

	bool IsUnormLdr(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
			format == DXGI_FORMAT_R10G10B10A2_UNORM;
	}

	// Emits one quad (two triangles) a-b-c-d with a flat normal.
	void PushQuad(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
		const Vec3& normal, const float albedo[4], float roughness, float metallic)
	{
		const Vec3 corners[6] = { a, b, c, a, c, d };
		for (const auto& p : corners)
		{
			Vertex v{};
			v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
			v.normal[0] = normal.x; v.normal[1] = normal.y; v.normal[2] = normal.z;
			std::memcpy(v.albedo, albedo, sizeof(v.albedo));
			v.material[0] = roughness;
			v.material[1] = metallic;
			out.push_back(v);
		}
	}

	void BuildHousing(const HousingRenderer::Instance& inst, std::vector<Vertex>& out)
	{
		const auto& p = inst.params;
		const float hw = inst.widthMeters * 0.5f;
		const float hh = inst.heightMeters * 0.5f;
		const float bezel = std::clamp(p.bezelWidthMm, 1.0f, 100.0f) * 0.001f;
		const float depth = std::clamp(p.depthMm, 2.0f, 200.0f) * 0.001f;
		// Small raised lip in front of the screen so the bezel has a visible edge.
		const float lip = (std::min)(0.003f, depth * 0.25f);
		const float ow = hw + bezel;
		const float oh = hh + bezel;
		const float roughness = std::clamp(p.roughness, 0.05f, 1.0f);
		const float metallic = std::clamp(p.metallic, 0.0f, 1.0f);
		const float albedo[4] = { SrgbToLinear(p.colorR), SrgbToLinear(p.colorG), SrgbToLinear(p.colorB), 1.0f };
		const float mask[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		auto xf = [&](float x, float y, float z) {
			const Vec3 r = Rotate(inst.pose.orientation, Vec3{ x, y, z });
			return Vec3{ r.x + inst.pose.position.x, r.y + inst.pose.position.y, r.z + inst.pose.position.z };
		};
		auto nf = [&](float x, float y, float z) { return Rotate(inst.pose.orientation, Vec3{ x, y, z }); };

		// Screen surface (switched-off display), slightly behind the lip plane.
		PushQuad(out, xf(-hw, -hh, 0), xf(hw, -hh, 0), xf(hw, hh, 0), xf(-hw, hh, 0), nf(0, 0, 1), mask, 1.0f, 0.0f);
		{
			// UVs for the 6 screen vertices (a,b,c,a,c,d = BL,BR,TR,BL,TR,TL).
			const float uvs[6][2] = {
				{ inst.u0, inst.v1 }, { inst.u1, inst.v1 }, { inst.u1, inst.v0 },
				{ inst.u0, inst.v1 }, { inst.u1, inst.v0 }, { inst.u0, inst.v0 } };
			for (int i = 0; i < 6; ++i)
			{
				out[out.size() - 6 + i].uv[0] = uvs[i][0];
				out[out.size() - 6 + i].uv[1] = uvs[i][1];
			}
		}

		// Front bezel ring (4 trapezoids).
		const Vec3 fwd = nf(0, 0, 1);
		PushQuad(out, xf(-ow, oh, lip), xf(ow, oh, lip), xf(hw, hh, lip), xf(-hw, hh, lip), fwd, albedo, roughness, metallic);
		PushQuad(out, xf(-hw, -hh, lip), xf(hw, -hh, lip), xf(ow, -oh, lip), xf(-ow, -oh, lip), fwd, albedo, roughness, metallic);
		PushQuad(out, xf(-ow, -oh, lip), xf(-ow, oh, lip), xf(-hw, hh, lip), xf(-hw, -hh, lip), fwd, albedo, roughness, metallic);
		PushQuad(out, xf(hw, -hh, lip), xf(hw, hh, lip), xf(ow, oh, lip), xf(ow, -oh, lip), fwd, albedo, roughness, metallic);

		// Inner lip walls facing the screen centre.
		PushQuad(out, xf(-hw, hh, 0), xf(hw, hh, 0), xf(hw, hh, lip), xf(-hw, hh, lip), nf(0, -1, 0), albedo, roughness, metallic);
		PushQuad(out, xf(-hw, -hh, 0), xf(hw, -hh, 0), xf(hw, -hh, lip), xf(-hw, -hh, lip), nf(0, 1, 0), albedo, roughness, metallic);
		PushQuad(out, xf(-hw, -hh, 0), xf(-hw, hh, 0), xf(-hw, hh, lip), xf(-hw, -hh, lip), nf(1, 0, 0), albedo, roughness, metallic);
		PushQuad(out, xf(hw, -hh, 0), xf(hw, hh, 0), xf(hw, hh, lip), xf(hw, -hh, lip), nf(-1, 0, 0), albedo, roughness, metallic);

		// Outer sides from the lip back to the rear.
		const float back = -depth;
		PushQuad(out, xf(-ow, oh, back), xf(ow, oh, back), xf(ow, oh, lip), xf(-ow, oh, lip), nf(0, 1, 0), albedo, roughness, metallic);
		PushQuad(out, xf(-ow, -oh, back), xf(ow, -oh, back), xf(ow, -oh, lip), xf(-ow, -oh, lip), nf(0, -1, 0), albedo, roughness, metallic);
		PushQuad(out, xf(-ow, -oh, back), xf(-ow, oh, back), xf(-ow, oh, lip), xf(-ow, -oh, lip), nf(-1, 0, 0), albedo, roughness, metallic);
		PushQuad(out, xf(ow, -oh, back), xf(ow, oh, back), xf(ow, oh, lip), xf(ow, -oh, lip), nf(1, 0, 0), albedo, roughness, metallic);

		// Back plate.
		PushQuad(out, xf(-ow, -oh, back), xf(ow, -oh, back), xf(ow, oh, back), xf(-ow, oh, back), nf(0, 0, -1), albedo, roughness, metallic);
	}

	// Row-major view-projection for column vectors (clip = M * p).
	void BuildViewProj(const XrPosef& eye, const XrFovf& fov, float out[16])
	{
		const float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
		const float u = tanf(fov.angleUp), d = tanf(fov.angleDown);
		const float n = HousingRenderer::kNearZ, f = HousingRenderer::kFarZ;
		const float proj[16] = {
			2.0f / (r - l), 0, (r + l) / (r - l), 0,
			0, 2.0f / (u - d), (u + d) / (u - d), 0,
			0, 0, f / (n - f), n * f / (n - f),
			0, 0, -1.0f, 0 };

		const XrQuaternionf inv{ -eye.orientation.x, -eye.orientation.y, -eye.orientation.z, eye.orientation.w };
		const Vec3 cx = Rotate(inv, Vec3{ 1, 0, 0 });
		const Vec3 cy = Rotate(inv, Vec3{ 0, 1, 0 });
		const Vec3 cz = Rotate(inv, Vec3{ 0, 0, 1 });
		const Vec3 t = Rotate(inv, Vec3{ -eye.position.x, -eye.position.y, -eye.position.z });
		const float view[16] = {
			cx.x, cy.x, cz.x, t.x,
			cx.y, cy.y, cz.y, t.y,
			cx.z, cy.z, cz.z, t.z,
			0, 0, 0, 1 };

		for (int i = 0; i < 4; ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				float s = 0.0f;
				for (int k = 0; k < 4; ++k)
				{
					s += proj[i * 4 + k] * view[k * 4 + j];
				}
				out[i * 4 + j] = s;
			}
		}
	}

	ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
	{
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "HousingRenderer", nullptr, nullptr,
			entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr))
		{
			std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string();
			Logging::Log("HousingRenderer: shader compile failed (" + std::string(entry) + "): " + message);
			return nullptr;
		}
		return code;
	}

	void ReleaseLocked()
	{
		g_vs.Reset();
		g_ps.Reset();
		g_layout.Reset();
		g_cb.Reset();
		g_vb.Reset();
		g_vbCapacity = 0;
		g_raster.Reset();
		g_depthState.Reset();
		g_blend.Reset();
		g_sampler.Reset();
		g_state.Reset();
		g_dsv.Reset();
		g_depth.Reset();
		g_depthW = g_depthH = 0;
		g_msaaRtv.Reset();
		g_msaaColor.Reset();
		g_msaaFormat = DXGI_FORMAT_UNKNOWN;
		g_sampleCount = 1;
		g_contentMips.clear();
		g_device = nullptr;
		g_initFailed = false;
	}

	bool EnsureInitializedLocked(ID3D11Device* device)
	{
		if (g_device == device && g_vs && g_ps && g_layout && g_cb && g_state)
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
			Logging::Log("HousingRenderer: ID3D11Device1 not available, housings disabled.");
			return false;
		}

		const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
		if (FAILED(device1->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device1), nullptr, &g_state)))
		{
			Logging::Log("HousingRenderer: CreateDeviceContextState failed, housings disabled.");
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
			Logging::Log("HousingRenderer: creating shaders failed.");
			return false;
		}

		const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, albedo), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, material), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		if (FAILED(device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
			vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &g_layout)))
		{
			Logging::Log("HousingRenderer: creating input layout failed.");
			return false;
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(Constants);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_cb)))
		{
			return false;
		}

		D3D11_RASTERIZER_DESC rs{};
		rs.FillMode = D3D11_FILL_SOLID;
		rs.CullMode = D3D11_CULL_NONE;
		rs.DepthClipEnable = TRUE;
		rs.MultisampleEnable = TRUE;
		rs.AntialiasedLineEnable = FALSE;
		D3D11_DEPTH_STENCIL_DESC ds{};
		ds.DepthEnable = TRUE;
		ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		D3D11_BLEND_DESC bs{};
		bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateRasterizerState(&rs, &g_raster)) ||
			FAILED(device->CreateDepthStencilState(&ds, &g_depthState)) ||
			FAILED(device->CreateBlendState(&bs, &g_blend)))
		{
			Logging::Log("HousingRenderer: creating pipeline states failed.");
			return false;
		}

		D3D11_SAMPLER_DESC samp{};
		samp.Filter = D3D11_FILTER_ANISOTROPIC;
		samp.MaxAnisotropy = 8;
		samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samp.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&samp, &g_sampler)))
		{
			return false;
		}

		g_initFailed = false;
		Logging::Log("HousingRenderer: initialized.");
		return true;
	}

	bool EnsureDepthLocked(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT colorFormat)
	{
		if (g_dsv && g_depthW == width && g_depthH == height && g_msaaFormat == colorFormat)
		{
			return true;
		}
		g_dsv.Reset();
		g_depth.Reset();
		g_msaaRtv.Reset();
		g_msaaColor.Reset();
		g_msaaFormat = colorFormat;

		g_sampleCount = 1;
		UINT quality = 0;
		if (SUCCEEDED(device->CheckMultisampleQualityLevels(colorFormat, 4, &quality)) && quality > 0 &&
			SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, 4, &quality)) && quality > 0)
		{
			D3D11_TEXTURE2D_DESC cd{};
			cd.Width = width;
			cd.Height = height;
			cd.MipLevels = 1;
			cd.ArraySize = 1;
			cd.Format = colorFormat;
			cd.SampleDesc.Count = 4;
			cd.Usage = D3D11_USAGE_DEFAULT;
			cd.BindFlags = D3D11_BIND_RENDER_TARGET;
			if (SUCCEEDED(device->CreateTexture2D(&cd, nullptr, &g_msaaColor)) &&
				SUCCEEDED(device->CreateRenderTargetView(g_msaaColor.Get(), nullptr, &g_msaaRtv)))
			{
				g_sampleCount = 4;
			}
			else
			{
				g_msaaRtv.Reset();
				g_msaaColor.Reset();
			}
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_D32_FLOAT;
		desc.SampleDesc.Count = g_sampleCount;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_depth)) ||
			FAILED(device->CreateDepthStencilView(g_depth.Get(), nullptr, &g_dsv)))
		{
			Logging::Log("HousingRenderer: creating depth buffer failed.");
			return false;
		}
		g_depthW = width;
		g_depthH = height;
		Logging::Log("HousingRenderer: render targets " + std::to_string(width) + "x" + std::to_string(height) +
			" samples=" + std::to_string(g_sampleCount) + ".");
		return true;
	}

	// Copies the content into a mipmapped texture and returns its SRV.
	ID3D11ShaderResourceView* GetContentSrvLocked(ID3D11Device* device, ID3D11DeviceContext* context,
		ID3D11Texture2D* content, DXGI_FORMAT format)
	{
		D3D11_TEXTURE2D_DESC src{};
		content->GetDesc(&src);
		auto& entry = g_contentMips[content];
		if (!entry.srv || entry.width != src.Width || entry.height != src.Height || entry.format != format)
		{
			entry = ContentMips{};
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = src.Width;
			desc.Height = src.Height;
			desc.MipLevels = 0;
			desc.ArraySize = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &entry.texture)) ||
				FAILED(device->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.srv)))
			{
				Logging::Log("HousingRenderer: creating content mip texture failed (format=" + std::to_string(format) + ").");
				g_contentMips.erase(content);
				return nullptr;
			}
			entry.width = src.Width;
			entry.height = src.Height;
			entry.format = format;
		}
		context->CopySubresourceRegion(entry.texture.Get(), 0, 0, 0, 0, content, 0, nullptr);
		context->GenerateMips(entry.srv.Get());
		return entry.srv.Get();
	}

	bool EnsureVertexBufferLocked(ID3D11Device* device, UINT vertexCount)
	{
		if (g_vb && g_vbCapacity >= vertexCount)
		{
			return true;
		}
		g_vb.Reset();
		const UINT capacity = (std::max)(vertexCount, 1024u);
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = capacity * sizeof(Vertex);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&desc, nullptr, &g_vb)))
		{
			g_vbCapacity = 0;
			return false;
		}
		g_vbCapacity = capacity;
		return true;
	}
}

namespace HousingRenderer
{
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
		ID3D11Texture2D* depthTarget)
	{
		if (device == nullptr || context == nullptr || target == nullptr || width == 0 || height == 0)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		if (!EnsureInitializedLocked(device) || !EnsureDepthLocked(device, width, height, rtvFormat))
		{
			return false;
		}

		ComPtr<ID3D11DeviceContext1> context1;
		if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
		{
			return false;
		}

		std::vector<Vertex> vertices;
		std::vector<UINT> starts;
		vertices.reserve(instances.size() * 14 * 6);
		for (const auto& inst : instances)
		{
			starts.push_back(static_cast<UINT>(vertices.size()));
			BuildHousing(inst, vertices);
		}
		starts.push_back(static_cast<UINT>(vertices.size()));
		if (!EnsureVertexBufferLocked(device, static_cast<UINT>(vertices.size())))
		{
			return false;
		}

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = rtvFormat;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11RenderTargetView> rtv;
		if (FAILED(device->CreateRenderTargetView(target, &rtvDesc, &rtv)))
		{
			Logging::Log("HousingRenderer: CreateRenderTargetView failed (format=" + std::to_string(rtvFormat) + ").");
			return false;
		}

		Constants constants{};
		BuildViewProj(eyePose, fov, constants.viewProj);
		constants.eyePos[0] = eyePose.position.x;
		constants.eyePos[1] = eyePose.position.y;
		constants.eyePos[2] = eyePose.position.z;
		constants.sky[0] = lighting.skyR; constants.sky[1] = lighting.skyG; constants.sky[2] = lighting.skyB;
		constants.ground[0] = lighting.groundR; constants.ground[1] = lighting.groundG; constants.ground[2] = lighting.groundB;
		constants.misc[0] = (!IsSrgb(rtvFormat) && IsUnormLdr(rtvFormat)) ? 1.0f : 0.0f;

		ComPtr<ID3DDeviceContextState> previousState;
		context1->SwapDeviceContextState(g_state.Get(), &previousState);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (!vertices.empty() && SUCCEEDED(context->Map(g_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
			context->Unmap(g_vb.Get(), 0);
		}

		const float clear[4] = { 0, 0, 0, 0 };
		ID3D11RenderTargetView* colorRtv = g_sampleCount > 1 ? g_msaaRtv.Get() : rtv.Get();
		context->ClearRenderTargetView(colorRtv, clear);
		context->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(width);
		viewport.Height = static_cast<float>(height);
		viewport.MaxDepth = 1.0f;

		ID3D11RenderTargetView* rtvs[] = { colorRtv };
		ID3D11Buffer* cbs[] = { g_cb.Get() };
		ID3D11Buffer* vbs[] = { g_vb.Get() };
		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;
		context->OMSetRenderTargets(1, rtvs, g_dsv.Get());
		context->OMSetDepthStencilState(g_depthState.Get(), 0);
		context->OMSetBlendState(g_blend.Get(), nullptr, 0xFFFFFFFF);
		context->RSSetState(g_raster.Get());
		context->RSSetViewports(1, &viewport);
		context->IASetInputLayout(g_layout.Get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
		context->VSSetShader(g_vs.Get(), nullptr, 0);
		context->VSSetConstantBuffers(0, 1, cbs);
		context->PSSetShader(g_ps.Get(), nullptr, 0);
		context->PSSetConstantBuffers(0, 1, cbs);
		ID3D11SamplerState* samplers[] = { g_sampler.Get() };
		context->PSSetSamplers(0, 1, samplers);

		for (size_t i = 0; i < instances.size(); ++i)
		{
			const auto& inst = instances[i];
			ID3D11ShaderResourceView* srv = nullptr;
			bool contentSrgb = false;
			if (inst.content != nullptr)
			{
				DXGI_FORMAT f = static_cast<DXGI_FORMAT>(inst.contentFormat);
				srv = GetContentSrvLocked(device, context, inst.content, f);
				// UNORM content written by the sources is sRGB-encoded; a
				// linear-light (sRGB/float) target needs it decoded.
				contentSrgb = !IsSrgb(f) && constants.misc[0] < 0.5f;
			}

			constants.misc[1] = srv != nullptr ? 1.0f : 0.0f;
			constants.misc[2] = contentSrgb ? 1.0f : 0.0f;
			if (SUCCEEDED(context->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				std::memcpy(mapped.pData, &constants, sizeof(constants));
				context->Unmap(g_cb.Get(), 0);
			}
			ID3D11ShaderResourceView* srvs[] = { srv };
			context->PSSetShaderResources(0, 1, srvs);
			context->Draw(starts[i + 1] - starts[i], starts[i]);
		}

		ID3D11ShaderResourceView* nullSrv[] = { nullptr };
		context->PSSetShaderResources(0, 1, nullSrv);

		ID3D11RenderTargetView* nullRtv[] = { nullptr };
		context->OMSetRenderTargets(1, nullRtv, nullptr);

		if (g_sampleCount > 1)
		{
			context->ResolveSubresource(target, 0, g_msaaColor.Get(), 0, rtvFormat);
		}

		// Depth for the runtime's positional reprojection: a single-sample
		// depth-only pass straight into the depth swapchain image.
		if (depthTarget != nullptr)
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
			dd.Format = DXGI_FORMAT_D32_FLOAT;
			dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			ComPtr<ID3D11DepthStencilView> depthDsv;
			if (SUCCEEDED(device->CreateDepthStencilView(depthTarget, &dd, &depthDsv)))
			{
				context->ClearDepthStencilView(depthDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
				context->OMSetRenderTargets(0, nullptr, depthDsv.Get());
				context->PSSetShader(nullptr, nullptr, 0);
				context->Draw(static_cast<UINT>(vertices.size()), 0);
				context->OMSetRenderTargets(0, nullptr, nullptr);
			}
		}

		context1->SwapDeviceContextState(previousState.Get(), nullptr);
		return true;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ReleaseLocked();
	}
}
