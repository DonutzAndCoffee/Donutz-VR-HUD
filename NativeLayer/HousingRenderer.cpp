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
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr char kShaderSource[] = R"(
cbuffer Constants : register(b0)
{
	row_major float4x4 world;
	row_major float4x4 viewProj;
	float4 eyePos;      // xyz, same space as viewProj
	float4 sky;         // rgb
	float4 ground;      // rgb
	float4 misc;        // x = encode sRGB in shader
};

struct VSIn
{
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float4 albedo : COLOR0;      // rgb linear, a = 1 housing / 0 display
	float2 material : TEXCOORD0; // x roughness, y metallic
};

struct PSIn
{
	float4 pos : SV_POSITION;
	float3 world : TEXCOORD0;
	float3 normal : NORMAL;
	float4 albedo : COLOR0;
	float2 material : TEXCOORD1;
};

PSIn VSMain(VSIn input)
{
	PSIn o;
	float4 w = mul(world, float4(input.pos, 1.0));
	o.pos = mul(viewProj, w);
	o.world = w.xyz;
	o.normal = mul((float3x3)world, input.normal);
	o.albedo = input.albedo;
	o.material = input.material;
	return o;
}

float3 Hemisphere(float y)
{
	return lerp(ground.rgb, sky.rgb, saturate(y * 0.5 + 0.5));
}

float4 PSMain(PSIn input) : SV_TARGET
{
	float3 n = normalize(input.normal);
	float3 v = normalize(eyePos.xyz - input.world);
	if (dot(n, v) < 0) n = -n;
	float ndv = saturate(dot(n, v));
	float3 r = reflect(-v, n);
	float3 color;

	if (input.albedo.a < 0.5)
	{
		// Switched-off display: near-black glossy glass.
		float f = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
		color = float3(0.004, 0.004, 0.005) + Hemisphere(r.y) * f * 0.6;
	}
	else
	{
		float roughness = input.material.x;
		float metallic = input.material.y;
		float3 albedo = input.albedo.rgb;
		// Key light from above plus hemisphere ambient.
		float key = saturate(dot(n, normalize(float3(0.2, 1.0, 0.4)))) * 0.35;
		float3 diffuse = albedo * (1.0 - metallic) * (Hemisphere(n.y) + key * sky.rgb);
		float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
		float3 fresnel = f0 + (max(1.0 - roughness, f0) - f0) * pow(1.0 - ndv, 5.0);
		float3 env = lerp(Hemisphere(r.y), (sky.rgb + ground.rgb) * 0.5, roughness);
		float3 specular = env * fresnel * (1.0 - 0.7 * roughness);
		float3 h = normalize(v + normalize(float3(0.2, 1.0, 0.4)));
		float spec = pow(saturate(dot(n, h)), lerp(256.0, 8.0, roughness)) * (1.0 - roughness) * 0.5;
		color = (diffuse + specular + spec * sky.rgb) * lerp(0.6, 1.0, ndv);
	}

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
	};

	struct Constants
	{
		float world[16];
		float viewProj[16];
		float eyePos[4];
		float sky[4];
		float ground[4];
		float misc[4];
	};

	struct MeshCache
	{
		ComPtr<ID3D11Buffer> vb;
		UINT vertexCount = 0;
		std::string signature;
	};

	std::mutex g_mutex;
	ID3D11Device* g_device = nullptr;
	bool g_initFailed = false;
	ComPtr<ID3DDeviceContextState> g_state;
	ComPtr<ID3D11VertexShader> g_vs;
	ComPtr<ID3D11PixelShader> g_ps;
	ComPtr<ID3D11InputLayout> g_layout;
	ComPtr<ID3D11Buffer> g_cb;
	ComPtr<ID3D11RasterizerState> g_raster;
	ComPtr<ID3D11DepthStencilState> g_depthState;
	ComPtr<ID3D11BlendState> g_blend;
	ComPtr<ID3D11Texture2D> g_depth;
	ComPtr<ID3D11DepthStencilView> g_dsv;
	UINT g_depthW = 0;
	UINT g_depthH = 0;
	ComPtr<ID3D11Texture2D> g_msaaColor;
	ComPtr<ID3D11RenderTargetView> g_msaaRtv;
	DXGI_FORMAT g_msaaFormat = DXGI_FORMAT_UNKNOWN;
	UINT g_sampleCount = 1;
	std::unordered_map<std::string, MeshCache> g_meshes;

	struct Vec3 { float x, y, z; };

	Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	Vec3 Normalize(const Vec3& v)
	{
		const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		return len > 1e-9f ? Vec3{ v.x / len, v.y / len, v.z / len } : Vec3{ 0, 0, 1 };
	}

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

	void PushTri(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c,
		const float albedo[4], float roughness, float metallic)
	{
		const Vec3 n = Normalize(Cross(Sub(b, a), Sub(c, a)));
		for (const Vec3* p : { &a, &b, &c })
		{
			Vertex v{};
			v.pos[0] = p->x; v.pos[1] = p->y; v.pos[2] = p->z;
			v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
			std::memcpy(v.albedo, albedo, sizeof(v.albedo));
			v.material[0] = roughness;
			v.material[1] = metallic;
			out.push_back(v);
		}
	}

	void PushQuad(std::vector<Vertex>& out, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
		const float albedo[4], float roughness, float metallic)
	{
		PushTri(out, a, b, c, albedo, roughness, metallic);
		PushTri(out, a, c, d, albedo, roughness, metallic);
	}

	// Rounded rectangle outline (CCW seen from +z), always the same point
	// count for a given segment count so consecutive rings can be swept.
	std::vector<Vec3> Outline(float halfW, float halfH, float radius, float z, uint32_t segments)
	{
		radius = std::clamp(radius, 0.0f, (std::min)(halfW, halfH));
		const float cx[4] = { halfW - radius, -(halfW - radius), -(halfW - radius), halfW - radius };
		const float cy[4] = { halfH - radius, halfH - radius, -(halfH - radius), -(halfH - radius) };
		std::vector<Vec3> pts;
		pts.reserve(4 * (segments + 1));
		for (int corner = 0; corner < 4; ++corner)
		{
			for (uint32_t s = 0; s <= segments; ++s)
			{
				const float a = (corner + static_cast<float>(s) / segments) * 1.5707963f;
				pts.push_back({ cx[corner] + radius * cosf(a), cy[corner] + radius * sinf(a), z });
			}
		}
		return pts;
	}

	void SweepRings(std::vector<Vertex>& out, const std::vector<Vec3>& a, const std::vector<Vec3>& b,
		const float albedo[4], float roughness, float metallic)
	{
		const size_t n = a.size();
		for (size_t i = 0; i < n; ++i)
		{
			const size_t j = (i + 1) % n;
			PushQuad(out, a[i], a[j], b[j], b[i], albedo, roughness, metallic);
		}
	}

	void BuildMesh(const HousingRenderer::Instance& inst, std::vector<Vertex>& out)
	{
		const auto& p = inst.params;
		const float hw = inst.widthMeters * 0.5f;
		const float hh = inst.heightMeters * 0.5f;
		const float bezel = std::clamp(p.bezelWidthMm, 1.0f, 100.0f) * 0.001f;
		const float depth = std::clamp(p.depthMm, 2.0f, 200.0f) * 0.001f;
		const float ow = hw + bezel;
		const float oh = hh + bezel;
		const float roughness = std::clamp(p.roughness, 0.05f, 1.0f);
		const float metallic = std::clamp(p.metallic, 0.0f, 1.0f);
		const float albedo[4] = { SrgbToLinear(p.colorR), SrgbToLinear(p.colorG), SrgbToLinear(p.colorB), 1.0f };
		const float glass[4] = { 0.004f, 0.004f, 0.005f, 0.0f };

		uint32_t segments = 8;
		float frontRadius = std::clamp(p.cornerRadiusMm, 0.0f, 200.0f) * 0.001f;
		float backRadius = frontRadius;
		std::vector<IpcServer::HousingProfilePoint> points;
		if (inst.profile != nullptr)
		{
			points = inst.profile->points;
			segments = std::clamp(inst.profile->cornerSegments, 1u, 32u);
			if (inst.profile->frontCornerRadiusMm > 0.0f) frontRadius = inst.profile->frontCornerRadiusMm * 0.001f;
			if (inst.profile->backCornerRadiusMm > 0.0f) backRadius = inst.profile->backCornerRadiusMm * 0.001f;
		}
		float zMin = 0.0f, zMax = 0.0f;
		for (const auto& pt : points)
		{
			zMin = (std::min)(zMin, pt.zPercent);
			zMax = (std::max)(zMax, pt.zPercent);
		}
		if (points.size() < 2 || zMax - zMin < 1.0f)
		{
			points = { { 0.0f, 0.0f }, { 0.0f, -100.0f } };
		}

		// Recessed display opening + inner lip.
		const float recess = HousingRenderer::kRecessMeters;
		const float openRadius = (std::max)(0.0f, frontRadius - bezel);
		const auto openFront = Outline(hw, hh, openRadius, 0.0f, segments);
		const auto openBack = Outline(hw, hh, openRadius, -recess, segments);
		SweepRings(out, openFront, openBack, albedo, roughness, metallic);

		// Profile rings from front to back.
		std::vector<std::vector<Vec3>> rings;
		for (const auto& pt : points)
		{
			const float t = std::clamp(-pt.zPercent / 100.0f, 0.0f, 1.0f);
			const float inset = (std::max)(0.0f, pt.insetMm) * 0.001f;
			const float radius = (std::max)(0.0f, frontRadius + (backRadius - frontRadius) * t - inset);
			rings.push_back(Outline((std::max)(hw, ow - inset), (std::max)(hh, oh - inset), radius,
				std::clamp(pt.zPercent, -100.0f, 0.0f) * 0.01f * depth, segments));
		}

		// Front bezel face: display opening -> first profile ring.
		SweepRings(out, openFront, rings.front(), albedo, roughness, metallic);
		for (size_t r = 1; r < rings.size(); ++r)
		{
			SweepRings(out, rings[r - 1], rings[r], albedo, roughness, metallic);
		}

		// Back cap.
		const auto& back = rings.back();
		const Vec3 centre{ 0.0f, 0.0f, back.front().z };
		for (size_t i = 0; i < back.size(); ++i)
		{
			PushTri(out, centre, back[(i + 1) % back.size()], back[i], albedo, roughness, metallic);
		}

		// Opaque dark display surface at the bottom of the recess.
		const Vec3 dc{ 0.0f, 0.0f, -recess };
		for (size_t i = 0; i < openBack.size(); ++i)
		{
			PushTri(out, dc, openBack[i], openBack[(i + 1) % openBack.size()], glass, 0.15f, 0.0f);
		}
	}

	std::string Signature(const HousingRenderer::Instance& inst)
	{
		const auto& p = inst.params;
		std::string s = std::to_string(inst.widthMeters) + "|" + std::to_string(inst.heightMeters) + "|" +
			std::to_string(p.bezelWidthMm) + "|" + std::to_string(p.depthMm) + "|" + std::to_string(p.cornerRadiusMm) + "|" +
			std::to_string(p.colorR) + "|" + std::to_string(p.colorG) + "|" + std::to_string(p.colorB) + "|" +
			std::to_string(p.roughness) + "|" + std::to_string(p.metallic);
		if (inst.profile != nullptr)
		{
			s += "|r" + std::to_string(inst.profile->revision) + "|n" + std::to_string(inst.profile->points.size());
		}
		return s;
	}

	MeshCache* GetMeshLocked(ID3D11Device* device, const HousingRenderer::Instance& inst)
	{
		const std::string signature = Signature(inst);
		auto& entry = g_meshes[inst.key];
		if (entry.vb && entry.signature == signature)
		{
			return &entry;
		}

		std::vector<Vertex> vertices;
		BuildMesh(inst, vertices);
		entry = MeshCache{};
		if (vertices.empty())
		{
			return nullptr;
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA data{ vertices.data() };
		if (FAILED(device->CreateBuffer(&desc, &data, &entry.vb)))
		{
			Logging::Log("HousingRenderer: creating mesh vertex buffer failed.");
			g_meshes.erase(inst.key);
			return nullptr;
		}
		entry.vertexCount = static_cast<UINT>(vertices.size());
		entry.signature = signature;
		Logging::Log("HousingRenderer: mesh rebuilt (triangles=" + std::to_string(vertices.size() / 3) + ").");
		return &entry;
	}

	// Row-major rigid transform for column vectors (p' = M * p).
	void BuildWorld(const XrPosef& pose, float out[16])
	{
		const Vec3 cx = Rotate(pose.orientation, Vec3{ 1, 0, 0 });
		const Vec3 cy = Rotate(pose.orientation, Vec3{ 0, 1, 0 });
		const Vec3 cz = Rotate(pose.orientation, Vec3{ 0, 0, 1 });
		const float m[16] = {
			cx.x, cy.x, cz.x, pose.position.x,
			cx.y, cy.y, cz.y, pose.position.y,
			cx.z, cy.z, cz.z, pose.position.z,
			0, 0, 0, 1 };
		std::memcpy(out, m, sizeof(m));
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
		g_raster.Reset();
		g_depthState.Reset();
		g_blend.Reset();
		g_state.Reset();
		g_dsv.Reset();
		g_depth.Reset();
		g_depthW = g_depthH = 0;
		g_msaaRtv.Reset();
		g_msaaColor.Reset();
		g_msaaFormat = DXGI_FORMAT_UNKNOWN;
		g_sampleCount = 1;
		g_meshes.clear();
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

		g_initFailed = false;
		Logging::Log("HousingRenderer: initialized.");
		return true;
	}

	bool EnsureTargetsLocked(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT colorFormat)
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
		const XrVector3f& headPosition,
		const std::vector<Instance>& instances,
		const donutz::ProbeLighting& lighting,
		ID3D11Texture2D* /*depthTarget*/)
	{
		if (device == nullptr || context == nullptr || target == nullptr || width == 0 || height == 0)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		if (!EnsureInitializedLocked(device) || !EnsureTargetsLocked(device, width, height, rtvFormat))
		{
			return false;
		}

		ComPtr<ID3D11DeviceContext1> context1;
		if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
		{
			return false;
		}

		{
			std::unordered_set<std::string> live;
			for (const auto& inst : instances) live.insert(inst.key);
			for (auto it = g_meshes.begin(); it != g_meshes.end();)
			{
				it = live.count(it->first) == 0 ? g_meshes.erase(it) : std::next(it);
			}
		}

		std::vector<std::pair<MeshCache*, const Instance*>> draws;
		for (const auto& inst : instances)
		{
			if (MeshCache* mesh = GetMeshLocked(device, inst))
			{
				draws.emplace_back(mesh, &inst);
			}
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
		constants.eyePos[0] = headPosition.x;
		constants.eyePos[1] = headPosition.y;
		constants.eyePos[2] = headPosition.z;
		constants.sky[0] = lighting.skyR; constants.sky[1] = lighting.skyG; constants.sky[2] = lighting.skyB;
		constants.ground[0] = lighting.groundR; constants.ground[1] = lighting.groundG; constants.ground[2] = lighting.groundB;
		constants.misc[0] = (!IsSrgb(rtvFormat) && IsUnormLdr(rtvFormat)) ? 1.0f : 0.0f;

		ComPtr<ID3DDeviceContextState> previousState;
		context1->SwapDeviceContextState(g_state.Get(), &previousState);

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
		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;
		context->OMSetRenderTargets(1, rtvs, g_dsv.Get());
		context->OMSetDepthStencilState(g_depthState.Get(), 0);
		context->OMSetBlendState(g_blend.Get(), nullptr, 0xFFFFFFFF);
		context->RSSetState(g_raster.Get());
		context->RSSetViewports(1, &viewport);
		context->IASetInputLayout(g_layout.Get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_vs.Get(), nullptr, 0);
		context->VSSetConstantBuffers(0, 1, cbs);
		context->PSSetShader(g_ps.Get(), nullptr, 0);
		context->PSSetConstantBuffers(0, 1, cbs);

		for (const auto& [mesh, inst] : draws)
		{
			BuildWorld(inst->pose, constants.world);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				std::memcpy(mapped.pData, &constants, sizeof(constants));
				context->Unmap(g_cb.Get(), 0);
			}
			ID3D11Buffer* vbs[] = { mesh->vb.Get() };
			context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
			context->Draw(mesh->vertexCount, 0);
		}

		ID3D11RenderTargetView* nullRtv[] = { nullptr };
		context->OMSetRenderTargets(1, nullRtv, nullptr);
		if (g_sampleCount > 1)
		{
			context->ResolveSubresource(target, 0, g_msaaColor.Get(), 0, rtvFormat);
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
