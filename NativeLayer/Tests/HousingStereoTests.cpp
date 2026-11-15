// Standalone WARP regression test; compile with HousingRenderer.cpp (see README).
#include "..\HookedFunctions.cpp"
#include <d3d11sdklayers.h>
#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <iostream>
#include <limits>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Test
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	const XrSession session = reinterpret_cast<XrSession>(1);
	const XrSpace local = reinterpret_cast<XrSpace>(2);
	const XrSpace view = reinterpret_cast<XrSpace>(3);
	const XrSpace app = reinterpret_cast<XrSpace>(4);
	XrPosef head{ { 0, 0, 0, 1 }, {} };
	XrPosef appOrigin{ { 0, 0.17364818f, 0, 0.98480775f }, { 0.13f, -0.07f, 0.18f } };
	std::vector<IpcServer::PanelState> panels;
	ContentBounds::Rect bounds{};
	XrCompositionLayerProjection original{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView originalViews[2]{};
	constexpr int kSourceW = 320, kSourceH = 240;
	bool expectScene = true;
	bool profileFrame = false;
	bool projectionFrame = false;
	bool projectionDepthFrame = false;
	bool editHudBudgetFrame = false;
	bool markerFrame = false;
	int failDepthCreateView = -1;
	int depthCreates = 0;
	bool projectionBudgetFrame = false;
	bool stableFrame = false;
	bool budgetFrame = false;
	bool stereoBudgetFrame = false;
	bool rejectFrame = false;
	bool controllersVisible = false;
	size_t profileCoverage = 0;
	int endCalls = 0;
	int failAcquire = -1;
	int creates = 0;
	int checkedFrames = 0;
	size_t rayChecks = 0;
	size_t displayChecks = 0;
	size_t emptyChecks = 0;
	size_t planeChecks = 0;
	std::vector<std::string> logs;

	struct Swapchain
	{
		ComPtr<ID3D11Texture2D> texture;
		int state = 0;
		int viewIndex = -1;
		bool source = false;
		bool depth = false;
		DXGI_FORMAT format;
	};
	std::map<XrSwapchain, Swapchain> swapchains;
	uintptr_t nextHandle = 10;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;

	XMMATRIX Matrix(const XrPosef& pose)
	{
		return XMMatrixRotationQuaternion(XMVectorSet(pose.orientation.x, pose.orientation.y,
			pose.orientation.z, pose.orientation.w)) *
			XMMatrixTranslation(pose.position.x, pose.position.y, pose.position.z);
	}

	XrPosef Pose(FXMMATRIX matrix)
	{
		XMVECTOR scale, orientation, translation;
		Require(XMMatrixDecompose(&scale, &orientation, &translation, matrix), "Rigid pose decomposition failed");
		XMFLOAT4 q;
		XMFLOAT3 p;
		XMStoreFloat4(&q, orientation);
		XMStoreFloat3(&p, translation);
		return { { q.x, q.y, q.z, q.w }, { p.x, p.y, p.z } };
	}

	XMMATRIX ToApp(XrSpace space)
	{
		const XMMATRIX inverseApp = XMMatrixInverse(nullptr, Matrix(appOrigin));
		if (space == local) return inverseApp;
		if (space == view) return Matrix(head) * inverseApp;
		Require(space == app, "Unexpected space");
		return XMMatrixIdentity();
	}

	XRAPI_ATTR XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime, XrSpaceLocation* output)
	{
		output->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		output->pose = Pose(ToApp(space) * XMMatrixInverse(nullptr, ToApp(base)));
		return XR_SUCCESS;
	}

	// Canted eye in app space for a head pose given in LOCAL space.
	XMMATRIX EyeInApp(int eye, const XrPosef& headPose)
	{
		const float side = eye == 0 ? -1.0f : 1.0f;
		return XMMatrixRotationY(side * 0.045f) * XMMatrixTranslation(side * 0.032f, 0, 0) *
			Matrix(headPose) * XMMatrixInverse(nullptr, Matrix(appOrigin));
	}

	XRAPI_ATTR XrResult XRAPI_CALL LocateViews(XrSession, const XrViewLocateInfo* info,
		XrViewState* state, uint32_t capacity, uint32_t* count, XrView* views)
	{
		Require(capacity == 2, "Housing requested wrong eye count");
		*count = 2;
		state->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		for (int i = 0; i < 2; ++i)
		{
			views[i].pose = Pose(EyeInApp(i, head) * XMMatrixInverse(nullptr, ToApp(info->space)));
			views[i].fov = originalViews[i].fov;
		}
		return XR_SUCCESS;
	}

	void Paint(Swapchain& chain, const float color[4])
	{
		ComPtr<ID3D11RenderTargetView> rtv;
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		desc.Format = chain.format;
		desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		Require(SUCCEEDED(device->CreateRenderTargetView(chain.texture.Get(), &desc, &rtv)), "Source RTV failed");
		context->ClearRenderTargetView(rtv.Get(), color);
	}

	XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* handle)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = info->width;
		desc.Height = info->height;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		Swapchain chain{};
		chain.depth = (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
		if (chain.depth && depthCreates++ == failDepthCreateView) return XR_ERROR_RUNTIME_FAILURE;
		desc.Format = info->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_TYPELESS :
			static_cast<DXGI_FORMAT>(info->format);
		desc.BindFlags = chain.depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		chain.format = static_cast<DXGI_FORMAT>(info->format);
		chain.source = info->width == kSourceW && info->height == kSourceH;
		if (!chain.source && !chain.depth)
		{
			int colors = 0;
			for (const auto& pair : swapchains)
				if (!pair.second.source && !pair.second.depth) ++colors;
			chain.viewIndex = colors % 2;
		}
		Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &chain.texture)), "Runtime texture creation failed");
		*handle = reinterpret_cast<XrSwapchain>(nextHandle++);
		swapchains.emplace(*handle, std::move(chain));
		++creates;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrSwapchain handle)
	{
		Require(swapchains.at(handle).state == 0, "Destroying an acquired image");
		swapchains.erase(handle);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL Enumerate(XrSwapchain handle, uint32_t capacity, uint32_t* count,
		XrSwapchainImageBaseHeader* images)
	{
		*count = 1;
		if (capacity > 0) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)->texture = swapchains.at(handle).texture.Get();
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL Formats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats)
	{
		*count = 1;
		if (capacity > 0) formats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL Acquire(XrSwapchain handle, const XrSwapchainImageAcquireInfo*, uint32_t* index)
	{
		auto& chain = swapchains.at(handle);
		if (!chain.source && !chain.depth && chain.viewIndex == failAcquire) return XR_ERROR_RUNTIME_FAILURE;
		Require(chain.state == 0, "Image acquired twice");
		chain.state = 1;
		*index = 0;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL Wait(XrSwapchain handle, const XrSwapchainImageWaitInfo*)
	{
		auto& chain = swapchains.at(handle);
		Require(chain.state == 1, "Image wait without acquisition");
		chain.state = 2;
		if (chain.source)
		{
			std::vector<uint32_t> pixels(kSourceW * kSourceH, 0);
			for (int y = 0; y < kSourceH; ++y)
				for (int x = 0; x < kSourceW; ++x)
					if (!bounds.valid || (x >= bounds.u0 * kSourceW && x < bounds.u1 * kSourceW &&
						y >= bounds.v0 * kSourceH && y < bounds.v1 * kSourceH)) pixels[y * kSourceW + x] = 0xffffff00;
			context->UpdateSubresource(chain.texture.Get(), 0, nullptr, pixels.data(), kSourceW * 4, 0);
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL Release(XrSwapchain handle, const XrSwapchainImageReleaseInfo*)
	{
		auto& chain = swapchains.at(handle);
		Require(chain.state == 2, "Image release without successful wait");
		chain.state = 0;
		if (chain.source)
		{
			// A released runtime image is no longer ours to sample.
			const float recycled[4] = { 1, 0, 1, 1 };
			Paint(chain, recycled);
		}
		return XR_SUCCESS;
	}

	struct Triangle
	{
		XMVECTOR a, b, c;
		bool display;
	};
	std::vector<Triangle> triangles;

	void Rectangle(FXMMATRIX transform, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d, bool display = false)
	{
		const auto point = [&](XMFLOAT3 p) { return XMVector3TransformCoord(XMLoadFloat3(&p), transform); };
		triangles.push_back({ point(a), point(b), point(c), display });
		triangles.push_back({ point(a), point(c), point(d), display });
	}

	XrSpace SpaceOf(const IpcServer::PanelState& panel)
	{
		return panel.headLocked ? view : local;
	}

	// Housing pose in the panel's own space, centred on the cropped content.
	XMMATRIX HousingInSpace(const IpcServer::PanelState& panel)
	{
		const float u0 = bounds.valid ? bounds.u0 : 0, u1 = bounds.valid ? bounds.u1 : 1;
		const float v0 = bounds.valid ? bounds.v0 : 0, v1 = bounds.valid ? bounds.v1 : 1;
		const float cx = ((u0 + u1) / 2 - 0.5f) * panel.widthMeters;
		const float cy = (0.5f - (v0 + v1) / 2) * panel.heightMeters;
		return XMMatrixTranslation(cx, cy, 0) * XMMatrixRotationY(panel.rotationDegY * XM_PI / 180) *
			XMMatrixTranslation(panel.positionX, panel.positionY, panel.positionZ);
	}

	void ReferenceGeometry(const IpcServer::PanelState& panel)
	{
		triangles.clear();
		const float u0 = bounds.valid ? bounds.u0 : 0, u1 = bounds.valid ? bounds.u1 : 1;
		const float v0 = bounds.valid ? bounds.v0 : 0, v1 = bounds.valid ? bounds.v1 : 1;
		const XMMATRIX housing = HousingInSpace(panel) * ToApp(SpaceOf(panel));
		const float w = (u1 - u0) * panel.widthMeters / 2;
		const float h = (v1 - v0) * panel.heightMeters / 2;
		const float left = w + panel.housing.bezelLeftMm / 1000;
		const float right = w + panel.housing.bezelRightMm / 1000;
		const float bottom = h + panel.housing.bezelBottomMm / 1000;
		const float top = h + panel.housing.bezelTopMm / 1000;
		const float d = -panel.housing.depthMm / 1000;
		const float r = -panel.housing.displayRecessMm / 1000;
		// Independent analytic rectangular shell, lip, back and display.
		Rectangle(housing, { -left, -bottom, d }, { right, -bottom, d }, { right, top, d }, { -left, top, d });
		Rectangle(housing, { -left, -bottom, 0 }, { -left, top, 0 }, { -left, top, d }, { -left, -bottom, d });
		Rectangle(housing, { right, -bottom, 0 }, { right, top, 0 }, { right, top, d }, { right, -bottom, d });
		Rectangle(housing, { -left, -bottom, 0 }, { right, -bottom, 0 }, { right, -bottom, d }, { -left, -bottom, d });
		Rectangle(housing, { -left, top, 0 }, { right, top, 0 }, { right, top, d }, { -left, top, d });
		Rectangle(housing, { -left, -bottom, 0 }, { -w, -bottom, 0 }, { -w, h, 0 }, { -left, h, 0 });
		Rectangle(housing, { w, -bottom, 0 }, { right, -bottom, 0 }, { right, h, 0 }, { w, h, 0 });
		Rectangle(housing, { -w, h, 0 }, { w, h, 0 }, { w, top, 0 }, { -w, top, 0 });
		Rectangle(housing, { -w, -bottom, 0 }, { w, -bottom, 0 }, { w, -h, 0 }, { -w, -h, 0 });
		Rectangle(housing, { -w, -h, 0 }, { -w, h, 0 }, { -w, h, r }, { -w, -h, r });
		Rectangle(housing, { w, -h, 0 }, { w, h, 0 }, { w, h, r }, { w, -h, r });
		Rectangle(housing, { -w, -h, 0 }, { w, -h, 0 }, { w, -h, r }, { -w, -h, r });
		Rectangle(housing, { -w, h, 0 }, { w, h, 0 }, { w, h, r }, { -w, h, r });
		Rectangle(housing, { -w, -h, r }, { w, -h, r }, { w, h, r }, { -w, h, r }, true);
	}

	std::vector<uint32_t> Read(ID3D11Texture2D* texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		const UINT width = desc.Width, height = desc.Height;
		desc.BindFlags = desc.MiscFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging;
		Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Readback texture creation failed");
		context->CopyResource(staging.Get(), texture);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Readback map failed");
		std::vector<uint32_t> result(width * height);
		for (UINT y = 0; y < height; ++y)
			std::memcpy(result.data() + y * width, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * 4);
		context->Unmap(staging.Get(), 0);
		return result;
	}

	std::vector<float> ReadDepth(ID3D11Texture2D* texture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		const UINT width = desc.Width, height = desc.Height;
		desc.BindFlags = desc.MiscFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging;
		Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Depth readback texture creation failed");
		context->CopyResource(staging.Get(), texture);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Depth readback map failed");
		std::vector<float> result(width * height);
		for (UINT y = 0; y < height; ++y)
			std::memcpy(result.data() + y * width, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * sizeof(float));
		context->Unmap(staging.Get(), 0);
		return result;
	}

	XMVECTOR HeadCentreInApp()
	{
		return (XMVector3TransformCoord(XMVectorZero(), EyeInApp(0, head)) +
			XMVector3TransformCoord(XMVectorZero(), EyeInApp(1, head))) / 2;
	}

	// One stereo housing: both eye quads share a square plane through the
	// housing that faces the head. Every texel must show what a ray from the
	// current eye (not the host's older submitted view) through it hits.
	// Returns the distance from the head to the quad.
	float CheckStereo(const XrCompositionLayerQuad& left, const XrCompositionLayerQuad& right)
	{
		Require(left.eyeVisibility == XR_EYE_VISIBILITY_LEFT && right.eyeVisibility == XR_EYE_VISIBILITY_RIGHT,
			"Stereo housing eye visibility invalid");
		Require(left.space == right.space && std::memcmp(&left.pose, &right.pose, sizeof(left.pose)) == 0 &&
			left.size.width == left.size.height && std::memcmp(&left.size, &right.size, sizeof(left.size)) == 0,
			"Eye quads do not share one square plane");
		const IpcServer::PanelState* panel = nullptr;
		const XMVECTOR position = XMVectorSet(left.pose.position.x, left.pose.position.y, left.pose.position.z, 1);
		for (const auto& candidate : panels)
		{
			const int mode = candidate.housing.renderMode;
			if ((mode != 2 && mode != 4) || SpaceOf(candidate) != left.space) continue;
			const float planeZ = mode == 4 ? -std::clamp(candidate.housing.displayRecessMm, 0.0f, 20.0f) / 1000
				: (0.006f - candidate.housing.depthMm / 1000) / 2;
			const XMVECTOR housingCentre = XMVector3TransformCoord(
				XMVectorSet(0, 0, planeZ, 1), HousingInSpace(candidate));
			if (XMVectorGetX(XMVector3Length(housingCentre - position)) < 0.00001f) panel = &candidate;
		}
		Require(panel != nullptr, "Stereo quad is not centred on its housing");
		const XMMATRIX quadToApp = Matrix(left.pose) * ToApp(left.space);
		const XMVECTOR toHead = HeadCentreInApp() - XMVector3TransformCoord(XMVectorZero(), quadToApp);
		const XMVECTOR normal = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), quadToApp);
		const float facing = XMVectorGetX(XMVector3Dot(normal, XMVector3Normalize(toHead)));
		if (panel->housing.renderMode == 4)
		{
			const XMVECTOR display = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), HousingInSpace(*panel) * ToApp(left.space));
			if (XMVectorGetX(XMVector3Dot(display, XMVector3Normalize(toHead))) >= 0.5f)
				Require(XMVectorGetX(XMVector3Dot(normal, display)) > 0.99999f, "Display-plane stereo quad left the display plane");
			else
				Require(fabsf(facing - 0.5f) < 0.0001f, "Display-plane stereo quad tilt is not clamped at 60 degrees");
			++planeChecks;
		}
		else Require(facing > 0.99999f, "Stereo quad does not face the head");
		ReferenceGeometry(*panel);
		int visibleSamples = 0, yellowSamples = 0;
		for (int eye = 0; eye < 2; ++eye)
		{
			const auto& quad = eye == 0 ? left : right;
			Require(quad.layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT && quad.next == nullptr,
				"Stereo quad flags changed");
			auto& chain = swapchains.at(quad.subImage.swapchain);
			Require(chain.state == 0, "Submitted image is still acquired");
			D3D11_TEXTURE2D_DESC desc{};
			chain.texture->GetDesc(&desc);
			const XrRect2Di& rect = quad.subImage.imageRect;
			Require(rect.offset.x == 0 && rect.offset.y == 0 && rect.extent.width == rect.extent.height &&
				rect.extent.width == static_cast<int32_t>(desc.Width) && rect.extent.height == static_cast<int32_t>(desc.Height),
				"Stereo image rectangle invalid");
			const int size = rect.extent.width;
			const auto color = Read(chain.texture.Get());
			const XMVECTOR origin = XMVector3TransformCoord(XMVectorZero(), EyeInApp(eye, head));
			const auto trace = [&](float px, float py, XMVECTOR& point) {
				const XMVECTOR onQuad = XMVector3TransformCoord(XMVectorSet((px / size - 0.5f) * quad.size.width,
					(0.5f - py / size) * quad.size.height, 0, 1), quadToApp);
				const XMVECTOR direction = XMVector3Normalize(onQuad - origin);
				float nearest = std::numeric_limits<float>::max();
				const Triangle* hit = nullptr;
				for (const auto& triangle : triangles)
				{
					float distance;
					if (TriangleTests::Intersects(origin, direction, triangle.a, triangle.b, triangle.c, distance) && distance < nearest)
					{
						nearest = distance;
						hit = &triangle;
					}
				}
				if (hit) point = origin + direction * nearest;
				return hit;
			};
			for (int y = 3; y < size; y += 5)
			{
				for (int x = 2; x < size; x += 5)
				{
					XMVECTOR point, unused;
					const Triangle* hit = trace(x + 0.5f, y + 0.5f, point);
					// Coverage is only compared away from the silhouette in the image.
					int neighbours = 0;
					for (float dy : { -4.0f, 0.0f, 4.0f })
						for (float dx : { -4.0f, 0.0f, 4.0f })
							if (trace(x + 0.5f + dx, y + 0.5f + dy, unused)) ++neighbours;
					const uint32_t pixel = color[y * size + x];
					const bool yellow = ((pixel >> 16) & 255) > 180 && ((pixel >> 8) & 255) > 180 && (pixel & 255) < 40;
					if (yellow) ++yellowSamples;
					if (hit)
					{
						++visibleSamples;
						if (neighbours == 9)
						{
							Require((pixel >> 24) > 200, "Housing silhouette differs from independent ray intersections");
							++rayChecks;
						}
						const float edgeDistance = (std::min)({
							XMVectorGetX(XMVector3LinePointDistance(hit->a, hit->b, point)),
							XMVectorGetX(XMVector3LinePointDistance(hit->b, hit->c, point)),
							XMVectorGetX(XMVector3LinePointDistance(hit->c, hit->a, point)) });
						if (edgeDistance > 0.006f)
						{
							Require(yellow == hit->display, "Display pixels shifted relative to housing or incorrect occlusion");
							if (hit->display) ++displayChecks;
						}
					}
					else if (neighbours == 0)
					{
						Require((pixel >> 24) == 0, "Stereo image is not transparent outside the housing");
						++emptyChecks;
					}
				}
			}
		}
		Require(visibleSamples > 5, "Housing has disappeared in a rotated view");
		if (fabsf(panel->rotationDegY) < 60) Require(yellowSamples > 5, "Display texture missing or sampled after release");
		if (panel->rotationDegY == 180) Require(yellowSamples == 0, "Display visible through back cap");
		return XMVectorGetX(XMVector3Length(toHead));
	}

	void CheckStereoPairs(const XrFrameEndInfo* info, uint32_t first, size_t count)
	{
		float previous = std::numeric_limits<float>::max();
		for (size_t i = 0; i < count; ++i)
		{
			const auto* left = info->layers[first + i * 2];
			const auto* right = info->layers[first + i * 2 + 1];
			Require(left->type == XR_TYPE_COMPOSITION_LAYER_QUAD && right->type == XR_TYPE_COMPOSITION_LAYER_QUAD,
				"Stereo housing is not submitted as compositor-placed quads");
			const float distance = CheckStereo(*reinterpret_cast<const XrCompositionLayerQuad*>(left),
				*reinterpret_cast<const XrCompositionLayerQuad*>(right));
			Require(distance <= previous, "Stereo housings are not composed far to near");
			previous = distance;
		}
	}

	XRAPI_ATTR XrResult XRAPI_CALL End(XrSession, const XrFrameEndInfo* info)
	{
		++endCalls;
		Require(info->layers[0] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&original), "Host layer was modified");
		Require(info->layerCount <= g_maxCompositionLayers, "Edit mode exceeded runtime composition layer limit");
		if (rejectFrame) return XR_ERROR_LAYER_LIMIT_EXCEEDED;
		if (markerFrame)
		{
			Require(info->layerCount == 5, "Experimental housing is not one eye quad pair plus two depth references");
			const auto& panel = panels[0];
			XMVECTOR probes[2];
			for (int i = 0; i < 2; ++i)
				probes[i] = XMVector3TransformCoord(XMVectorSet(panel.widthMeters * (i == 0 ? -0.25f : 0.25f),
					panel.heightMeters / 2 + 0.06f, -panel.housing.displayRecessMm / 1000 + (i == 0 ? 0.1f : -0.1f), 1),
					HousingInSpace(panel) * ToApp(SpaceOf(panel)));
			const XMVECTOR probe = probes[0];
			const uint32_t expected[2] = { 0xffff0000, 0xff00ff00 };
			for (int eye = 0; eye < 2; ++eye)
			{
				Require(info->layers[1 + eye]->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "Experimental housing is not a quad layer");
				const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1 + eye]);
				Require(quad.eyeVisibility == (eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT),
					"Experimental housing eye visibility invalid");
				const int size = quad.subImage.imageRect.extent.width;
				const XMMATRIX appToQuad = XMMatrixInverse(nullptr, Matrix(quad.pose) * ToApp(quad.space));
				XMFLOAT3 e, p;
				XMStoreFloat3(&e, XMVector3TransformCoord(XMVector3TransformCoord(XMVectorZero(), EyeInApp(eye, head)), appToQuad));
				XMStoreFloat3(&p, XMVector3TransformCoord(probe, appToQuad));
				const float t = e.z / (e.z - p.z);
				const int x = static_cast<int>(((e.x + (p.x - e.x) * t) / quad.size.width + 0.5f) * size);
				const int y = static_cast<int>((0.5f - (e.y + (p.y - e.y) * t) / quad.size.height) * size);
				Require(x > 8 && y > 8 && x < size - 8 && y < size - 8, "Depth probe marker lies outside the eye quad");
				const auto pixels = Read(swapchains.at(quad.subImage.swapchain).texture.Get());
				Require(pixels[y * size + x] == expected[eye], "Depth probe marker missing, misplaced or assigned to the wrong eye");
			}
			for (int i = 0; i < 2; ++i)
			{
				Require(info->layers[3 + i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "Depth reference is not a quad layer");
				const auto& reference = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[3 + i]);
				const XMVECTOR referencePosition = XMVector3TransformCoord(XMVectorSet(reference.pose.position.x,
					reference.pose.position.y, reference.pose.position.z, 1), ToApp(reference.space));
				Require(reference.eyeVisibility == XR_EYE_VISIBILITY_BOTH && reference.size.width == 0.01f &&
					XMVectorGetX(XMVector3Length(referencePosition - probes[i])) < 0.00001f,
					"Depth reference quad is not at its probe's 3D position");
			}
		}
		else if (editHudBudgetFrame)
		{
			Require(info->layerCount == 2 && info->layers[1]->type == XR_TYPE_COMPOSITION_LAYER_QUAD,
				"Edit-mode helper HUD was omitted when the layer budget was tight");
			const auto& hud = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
			Require(hud.space == view && fabsf(hud.pose.position.x - 0.17f) < 0.00001f &&
				fabsf(hud.pose.position.y - 0.03f) < 0.00001f && fabsf(hud.pose.position.z + 0.45f) < 0.00001f,
				"The submitted edit helper layer was not the shortcut HUD");
		}
		else if (projectionFrame || projectionDepthFrame)
		{
			Require(info->layerCount == 3 && info->layers[1]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION,
				"True stereo housing was not submitted as a projection layer alongside the dashboard");
			const auto& scene = *reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[1]);
			Require(scene.space == local && scene.viewCount == 2 && scene.next == nullptr,
				"True stereo housing projection metadata changed");
			for (uint32_t v = 0; v < scene.viewCount; ++v)
			{
				const auto& view = scene.views[v];
				const XrPosef expected = Pose(EyeInApp(v, head) * Matrix(appOrigin));
				const XrPosef stale = Pose(Matrix(originalViews[v].pose) * Matrix(appOrigin));
				Require(fabsf(expected.position.x - stale.position.x) > 0.001f,
					"Test no longer distinguishes runtime eye poses from the host's submitted ones");
				const float orientationDot = view.pose.orientation.x * expected.orientation.x +
					view.pose.orientation.y * expected.orientation.y + view.pose.orientation.z * expected.orientation.z +
					view.pose.orientation.w * expected.orientation.w;
				const XMVECTOR actualPosition = XMVectorSet(view.pose.position.x, view.pose.position.y, view.pose.position.z, 0);
				const XMVECTOR expectedPosition = XMVectorSet(expected.position.x, expected.position.y, expected.position.z, 0);
				Require(view.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW &&
					XMVectorGetX(XMVector3Length(actualPosition - expectedPosition)) < 0.00001f &&
					fabsf(orientationDot) > 0.99999f &&
					view.fov.angleLeft == originalViews[v].fov.angleLeft &&
					view.fov.angleRight == originalViews[v].fov.angleRight &&
					view.fov.angleUp == originalViews[v].fov.angleUp &&
					view.fov.angleDown == originalViews[v].fov.angleDown,
					"Housing projection did not use the runtime's current eye view");
				if (projectionDepthFrame)
				{
					Require(view.next != nullptr, "Depth-capable projection view omitted its depth info");
					const auto& depthInfo = *reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(view.next);
					const auto& depthChain = swapchains.at(depthInfo.subImage.swapchain);
					D3D11_TEXTURE2D_DESC depthDesc{};
					depthChain.texture->GetDesc(&depthDesc);
					Require(depthChain.depth && depthDesc.Format == DXGI_FORMAT_D32_FLOAT &&
						depthInfo.minDepth == 0.0f && depthInfo.maxDepth == 1.0f &&
						depthInfo.nearZ == HousingRenderer::kNearZ && depthInfo.farZ == HousingRenderer::kFarZ,
						"Projection depth target or depth metadata is invalid");
					const auto depths = ReadDepth(depthChain.texture.Get());
					Require(std::any_of(depths.begin(), depths.end(), [](float depth) { return depth >= 0.0f && depth < 1.0f; }),
						"Housing geometry did not write compositor depth");
				}
				else Require(view.next == nullptr, "Depth info was submitted while depth support was disabled");
				const auto pixels = Read(swapchains.at(view.subImage.swapchain).texture.Get());
				const size_t coverage = std::count_if(pixels.begin(), pixels.end(), [](uint32_t pixel) { return (pixel >> 24) > 127; });
				Require(coverage > 20, "True stereo housing projection rendered no geometry");
			}
			Require(info->layers[2]->type == XR_TYPE_COMPOSITION_LAYER_QUAD &&
				reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[2])->eyeVisibility == XR_EYE_VISIBILITY_BOTH &&
				reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[2])->subImage.imageRect.extent.width == kSourceW,
				"True stereo mode changed or downsampled the separate dashboard");
		}
		else if (projectionBudgetFrame)
		{
			Require(info->layerCount == 2 && info->layers[1]->type == XR_TYPE_COMPOSITION_LAYER_QUAD,
				"Projection housing exceeded the layer limit instead of preserving the dashboard");
			const auto& dashboard = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
			Require(dashboard.eyeVisibility == XR_EYE_VISIBILITY_BOTH &&
				dashboard.subImage.imageRect.extent.width == kSourceW,
				"Dashboard was lost when the projection housing did not fit the layer budget");
		}
		else if (stableFrame)
		{
			size_t stableCount = 0, stereoCount = 0, contentCount = 0, housingCount = 0;
			for (const auto& panel : panels)
				if (panel.housing.renderMode == 1) ++stableCount;
				else ++stereoCount;
			Require(info->layerCount == 1 + stableCount * 3 + stereoCount * 2, "Mixed mode layer count changed");
			CheckStereoPairs(info, 1, stereoCount);
			for (uint32_t i = 1 + static_cast<uint32_t>(stereoCount) * 2; i < info->layerCount; ++i)
			{
				Require(info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "Stable mesh is not a quad layer");
				const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
				Require(quad.next == nullptr, "Stable quad unexpectedly submits depth");
				if (quad.eyeVisibility == XR_EYE_VISIBILITY_BOTH)
				{
					Require(quad.subImage.imageRect.extent.width == kSourceW && quad.subImage.imageRect.extent.height == kSourceH,
						"Stable dashboard was downsampled or rendered into the housing image");
					++contentCount;
				}
				else
				{
					Require(quad.eyeVisibility == XR_EYE_VISIBILITY_LEFT || quad.eyeVisibility == XR_EYE_VISIBILITY_RIGHT,
						"Housing eye visibility invalid");
					const auto pixels = Read(swapchains.at(quad.subImage.swapchain).texture.Get());
					const size_t coverage = std::count_if(pixels.begin(), pixels.end(), [](uint32_t pixel) { return (pixel >> 24) > 127; });
					Require(coverage > 20, "Stable mesh housing did not render");
					if (quad.eyeVisibility == XR_EYE_VISIBILITY_LEFT) profileCoverage = coverage;
					++housingCount;
				}
				bool matched = false;
				for (const auto& panel : panels)
				{
					if (panel.housing.renderMode != 1 || quad.space != (panel.headLocked ? view : local)) continue;
					const float yaw = panel.rotationDegY * XM_PI / 180;
					const XMMATRIX panelPose = XMMatrixRotationY(yaw) * XMMatrixTranslation(panel.positionX, panel.positionY, panel.positionZ);
					const bool content = quad.eyeVisibility == XR_EYE_VISIBILITY_BOTH;
					const float cu = bounds.valid ? (bounds.u0 + bounds.u1) / 2 - 0.5f : 0;
					const float cv = bounds.valid ? 0.5f - (bounds.v0 + bounds.v1) / 2 : 0;
					const XMMATRIX expected = XMMatrixTranslation(content ? 0 : cu * panel.widthMeters,
						content ? 0 : cv * panel.heightMeters, content ? -panel.housing.displayRecessMm / 1000 : -0.005f) * panelPose;
					const XMMATRIX diff = Matrix(quad.pose) - expected;
					bool equal = true;
					for (int row = 0; row < 4; ++row)
						equal = equal && XMVector4Less(XMVectorAbs(diff.r[row]), XMVectorReplicate(0.000002f));
					if (!equal) continue;
					if (content) Require(quad.size.width == panel.widthMeters && quad.size.height == panel.heightMeters,
						"Stable dashboard size changed");
					matched = true;
				}
				Require(matched, "Stable crop/recess/space pose changed from the panel-space baseline");
			}
			Require(contentCount == stableCount && housingCount == stableCount * 2,
				"Stable and stereo rendering were not independently selected");
		}
		else if (stereoBudgetFrame)
		{
			// Room for one eye pair only: the nearer housing keeps it, the other shows its dashboard.
			Require(info->layerCount == 4, "Stereo layer budget not used for one housing plus one dashboard");
			CheckStereoPairs(info, 1, 1);
			Require(info->layers[1]->space == local && info->layers[3]->space == view &&
				info->layers[3]->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "Wrong stereo housing dropped from layer budget");
			const auto* content = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[3]);
			Require(content->eyeVisibility == XR_EYE_VISIBILITY_BOTH && content->subImage.imageRect.extent.width == kSourceW,
				"Dropped stereo housing lost its dashboard");
		}
		else if (budgetFrame)
		{
			Require(info->layerCount == 16, "Layer budget was not fully used for content and controllers");
			for (uint32_t i = 1; i < info->layerCount; ++i)
			{
				Require(info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "Unexpected budgeted layer type");
				const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
				if (i <= panels.size())
					Require(quad->subImage.imageRect.extent.width == kSourceW, "Decorative housing displaced dashboard content");
				else
					Require(quad->subImage.imageRect.extent.width != kSourceW, "Controller visuals were lost from layer budget");
			}
		}
		else if (profileFrame)
		{
			Require(info->layerCount == 3 && info->layers[1]->type == XR_TYPE_COMPOSITION_LAYER_QUAD,
				"Profile selection fell back to the plain dashboard quad");
			const auto* leftEye = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
			Require(leftEye->eyeVisibility == XR_EYE_VISIBILITY_LEFT, "Profile frame is not a stereo housing");
			const auto pixels = Read(swapchains.at(leftEye->subImage.swapchain).texture.Get());
			profileCoverage = std::count_if(pixels.begin(), pixels.end(), [](uint32_t pixel) { return (pixel >> 24) > 127; });
		}
		else if (expectScene)
		{
			Require(info->layerCount == 1 + panels.size() * 2, "Dashboard submitted separately or unexpected layer count");
			CheckStereoPairs(info, 1, panels.size());
		}
		else
		{
			Require(info->layerCount == 1 + panels.size(), "Failed scene must preserve all dashboard content");
			for (uint32_t i = 1; i < info->layerCount; ++i)
				Require(info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD &&
					reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i])->eyeVisibility == XR_EYE_VISIBILITY_BOTH,
					"A partially rendered stereo housing was submitted");
		}
		for (const auto& pair : swapchains) Require(pair.second.state == 0, "Leaked acquired image");
		++checkedFrames;
		return XR_SUCCESS;
	}

	void Frame()
	{
		// The host rendered from an older tracking sample than xrLocateViews reports now.
		const XrPosef staleHead = Pose(XMMatrixRotationY(0.04f) * XMMatrixTranslation(0.03f, -0.01f, 0.02f) * Matrix(head));
		for (int i = 0; i < 2; ++i)
		{
			auto& output = originalViews[i];
			output = XrCompositionLayerProjectionView{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			output.pose = Pose(EyeInApp(i, staleHead));
			output.fov = { -0.77f + i * 0.04f, 0.83f + i * 0.03f, 0.72f, -0.68f };
			output.subImage.imageRect = { { 11, 17 }, { 640, 480 } };
		}
		original.space = app;
		original.viewCount = 2;
		original.views = originalViews;
		const auto* base = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&original);
		XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
		info.displayTime = 123456789;
		info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		info.layerCount = 1;
		info.layers = &base;
		const D3D11_VIEWPORT viewport{ 3, 4, 17, 19, 0.2f, 0.9f };
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
		const int previousEndCalls = endCalls;
		Require(Hook_xrEndFrame(session, &info) == (rejectFrame ? XR_ERROR_LAYER_LIMIT_EXCEEDED : XR_SUCCESS), "End frame result changed");
		Require(endCalls == previousEndCalls + 1, "xrEndFrame called twice for one frame");
		D3D11_VIEWPORT restored{};
		UINT count = 1;
		context->RSGetViewports(&count, &restored);
		D3D11_PRIMITIVE_TOPOLOGY topology;
		context->IAGetPrimitiveTopology(&topology);
		Require(count == 1 && std::memcmp(&restored, &viewport, sizeof(viewport)) == 0 &&
			topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "Application D3D pipeline state was not restored");
	}
}

namespace Logging
{
	void Log(LogLevel level, const std::string& message)
	{
		Test::logs.push_back(message);
		if (level == LogLevel::Info && message.find("HousingRenderer:") == 0) std::cout << message << '\n';
	}
}
namespace IpcServer
{
	void EnsureStarted() {}
	std::vector<PanelState> GetPanelsSnapshot() { return Test::panels; }
	bool ConsumeRecenterRequested() { return false; }
	LedColors GetLedColors() { return {}; }
}
namespace ContentBounds
{
	void Update(const std::string&, ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*) {}
	Rect Get(const std::string&) { return Test::bounds; }
	void Reset() {}
}
namespace ControllerInput
{
	void Update(XrSession, XrSpace, XrTime) {}
	void GetVisuals(ControllerVisual out[2])
	{
		for (int i = 0; i < 2; ++i)
		{
			out[i] = {};
			if (!Test::controllersVisible) continue;
			out[i].visible = true;
			out[i].markerPose = { { 0, 0, 0, 1 }, { 0.1f * i, 0, -0.5f } };
			out[i].laserPose = out[i].grabHighlightPose = out[i].hitCursorPose = out[i].markerPose;
			out[i].laserLengthMeters = 0.5f;
			out[i].grabHighlightVisible = out[i].hitCursorVisible = true;
			out[i].grabHighlightWidthMeters = 0.4f;
			out[i].grabHighlightHeightMeters = 0.24f;
		}
	}
	void OnInstanceCreated(XrInstance, PFN_xrGetInstanceProcAddr) {}
	void OnSessionCreated(XrSession, XrSpace) {}
	void OnSessionDestroyed() {}
	XrResult OnAttachSessionActionSets(PFN_xrAttachSessionActionSets next, XrSession session, const XrSessionActionSetsAttachInfo* info)
	{ return next(session, info); }
	XrResult OnSuggestInteractionProfileBindings(PFN_xrSuggestInteractionProfileBindings next, XrInstance instance,
		const XrInteractionProfileSuggestedBinding* info) { return next(instance, info); }
	XrResult OnSyncActions(PFN_xrSyncActions next, XrSession session, const XrActionsSyncInfo* info) { return next(session, info); }
}
namespace VisorRenderer
{
	void Reset() {}
	bool RenderEye(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, DXGI_FORMAT, uint32_t, uint32_t,
		const IpcServer::VisorParams&, const XrPosef&, const XrPosef&, const XrFovf&, float) { return false; }
}
namespace donutz::LightProbe
{
	void Reset() {}
	ProbeLighting GetLighting() { return { 0.5f, 0.5f, 0.5f, 0.15f, 0.15f, 0.15f, 0.3f, true }; }
	void Update(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, uint32_t, int64_t) {}
}

int main()
{
	try
	{
		using namespace Test;
		Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
			D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
			&device, nullptr, &context)), "WARP/debug device creation failed");
		ComPtr<ID3D11InfoQueue> debug;
		Require(SUCCEEDED(device.As(&debug)), "D3D11 debug queue unavailable");
		g_appSession = session;
		g_viewSpace = view;
		g_localSpace = local;
		g_d3dDevice = device.Get();
		g_d3dContext = context.Get();
		g_hasAnchor = true;
		g_anchorPose = { { 0, 0, 0, 1 }, {} };
		g_next.xrEndFrame = End;
		g_next.xrCreateSwapchain = Create;
		g_next.xrDestroySwapchain = Destroy;
		g_next.xrEnumerateSwapchainImages = Enumerate;
		g_next.xrEnumerateSwapchainFormats = Formats;
		g_next.xrAcquireSwapchainImage = Acquire;
		g_next.xrWaitSwapchainImage = Wait;
		g_next.xrReleaseSwapchainImage = Release;
		g_next.xrLocateSpace = LocateSpace;
		g_next.xrLocateViews = LocateViews;
		IpcServer::PanelState panel;
		panel.enabled = panel.hasFrame = panel.hasHousing = true;
		panel.panelId.bytes[0] = 1;
		panel.positionZ = -0.8f;
		panel.widthMeters = 0.4f;
		panel.heightMeters = 0.24f;
		panel.frameWidth = kSourceW;
		panel.frameHeight = kSourceH;
		panel.housing = { 0, 20, 80, 10, 0.3f, 0.3f, 0.3f, 0.8f, 0, 2, 18, 12, 8, 24, 0, 40, {}, 2, 1 };
		panels = { panel };
		Frame();
		const int firstCreates = creates;
		Frame();
		Require(creates == firstCreates, "Unchanged frame recreated swapchains");
		for (float yaw : { 35.0f, 90.0f, 180.0f })
		{
			panels[0].rotationDegY = yaw;
			Frame();
		}
		panels[0].rotationDegY = 20;
		head = Pose(XMMatrixRotationY(0.18f) * XMMatrixTranslation(0.09f, 0.025f, 0.04f));
		Frame();
		Require(creates == firstCreates, "Panel rotation or head movement recreated stereo swapchains");
		const XrPosef movedHead = head;
		panels[0].housing.renderMode = 4;
		Frame();
		const int planeCreates = creates;
		for (float yaw : { 0.0f, 35.0f, 75.0f, 90.0f, 180.0f })
		{
			panels[0].rotationDegY = yaw;
			Frame();
		}
		panels[0].rotationDegY = 35;
		head = Pose(XMMatrixRotationY(-0.1f) * XMMatrixTranslation(-0.05f, 0.03f, 0.02f));
		Frame();
		Require(creates == planeCreates, "Display-plane stereo recreated swapchains for rotation or head movement");
		Require(planeChecks == 7, "Display-plane stereo frames were not verified");
		panels[0].housing.renderMode = 5;
		markerFrame = true;
		Frame();
		markerFrame = false;
		panels[0].housing.renderMode = 2;
		panels[0].rotationDegY = 20;
		head = movedHead;
		bounds = { 0.125f, 0.25f, 0.75f, 0.875f, true };
		Frame();
		panel.panelId.bytes[0] = 2;
		panel.headLocked = true;
		panel.positionX = 0.36f;
		panel.positionY = -0.18f;
		panel.positionZ = -1.1f;
		panels.push_back(panel);
		Frame();
		head = Pose(XMMatrixRotationY(-0.15f) * XMMatrixTranslation(-0.06f, -0.02f, 0.07f));
		Frame();
		expectScene = false;
		failAcquire = 1;
		Frame();
		failAcquire = -1;
		profileFrame = true;
		panels.resize(1);
		head = { { 0, 0, 0, 1 }, {} };
		bounds = {};
		panels[0].rotationDegY = 0;
		auto& profile = panels[0].housingProfile;
		profile = { 8, 6, 8, { { 1.5f, 0 }, { 0.45f, -4 }, { 0, -12 }, { 0, -75 },
			{ 0.6f, -90 }, { 2, -98 }, { 4, -100 } }, 1 };
		Frame();
		const size_t roundedCoverage = profileCoverage;
		profile = { 2, 2, 4, { { 0, 0 }, { 0, -100 } }, 2 };
		Frame();
		Require(profileCoverage > roundedCoverage + 5, "Rounded and flat profiles have the same silhouette");
		profile = { 8, 6, 8, { { 1.5f, 0 }, { 0.45f, -4 }, { 0, -12 }, { 0, -75 },
			{ 0.6f, -90 }, { 2, -98 }, { 4, -100 } }, 3 };
		Frame();
		Require(profileCoverage == roundedCoverage, "Switching back to Rounded did not rebuild the profile");
		profileFrame = false;
		panels[0].housing.renderMode = 3;
		projectionFrame = true;
		Frame();
		projectionFrame = false;
		projectionBudgetFrame = true;
		g_maxCompositionLayers = 2;
		Frame();
		g_maxCompositionLayers = 16;
		projectionBudgetFrame = false;
		projectionFrame = true;
		failDepthCreateView = depthCreates + 1;
		HookedFunctions::SetDepthLayerSupported(true);
		Frame();
		failDepthCreateView = -1;
		projectionDepthFrame = true;
		Frame();
		HookedFunctions::SetDepthLayerSupported(false);
		projectionDepthFrame = false;
		projectionFrame = false;
		const IpcServer::PanelState housingPanel = panels[0];
		IpcServer::PanelState contentPanel{};
		contentPanel.enabled = contentPanel.hasFrame = true;
		contentPanel.panelId.bytes[0] = 10;
		contentPanel.positionZ = -0.8f;
		contentPanel.widthMeters = 0.4f;
		contentPanel.heightMeters = 0.24f;
		contentPanel.frameWidth = kSourceW;
		contentPanel.frameHeight = kSourceH;
		IpcServer::PanelState editHud = contentPanel;
		editHud.panelId.bytes[0] = 11;
		editHud.headLocked = true;
		editHud.nonInteractive = true;
		editHud.positionX = 0.17f;
		editHud.positionY = 0.03f;
		editHud.positionZ = -0.45f;
		panels = { contentPanel, editHud };
		editHudBudgetFrame = true;
		g_maxCompositionLayers = 2;
		Frame();
		g_maxCompositionLayers = 16;
		editHudBudgetFrame = false;
		stableFrame = true;
		panels = { housingPanel };
		panels[0].housing.renderMode = 1;
		panels[0].panelId.bytes[0] = 5;
		profile.frontCornerRadiusMm = 80;
		profile.backCornerRadiusMm = 60;
		++profile.revision;
		const int beforeStable = creates;
		Frame();
		Require(creates == beforeStable + 3, "Stable mesh allocated stereo swapchains");
		const int stableCreates = creates;
		Frame();
		Require(creates == stableCreates, "Stable mesh recreated swapchains each frame");
		const size_t stableRoundCoverage = profileCoverage;
		profile = { 2, 2, 4, { { 0, 0 }, { 0, -100 } }, 4 };
		Frame();
		Require(profileCoverage > stableRoundCoverage, "Stable mesh lost profile selection");
		panels[0].housing.renderScale = 0.5f;
		bounds = { 0.125f, 0.25f, 0.75f, 0.875f, true };
		panels[0].rotationDegY = 20;
		Frame();
		panel.panelId.bytes[0] = 2;
		panel.housing.renderMode = 1;
		panels.push_back(panel);
		Frame();
		head = Pose(XMMatrixRotationY(0.18f) * XMMatrixTranslation(0.09f, 0.025f, 0.04f));
		Frame();
		panels[1].housing.renderMode = 2;
		Frame();
		panels[0].housing.renderMode = 2;
		stableFrame = false;
		expectScene = true;
		Frame();
		stereoBudgetFrame = true;
		g_maxCompositionLayers = 4;
		Frame();
		stereoBudgetFrame = false;
		panels.resize(1);
		budgetFrame = true;
		controllersVisible = true;
		g_maxCompositionLayers = 16;
		for (int i = 0; i < 7; ++i)
		{
			auto copy = panels[0];
			copy.panelId.bytes[0] = static_cast<uint8_t>(i + 1);
			copy.housing.renderMode = 0;
			if (i == 0) panels[0] = copy;
			else panels.push_back(copy);
		}
		Frame();
		Frame();
		rejectFrame = true;
		Frame();
		rejectFrame = false;
		Require(rayChecks > 40000 && displayChecks > 20000 && emptyChecks > 60000, "Insufficient geometry/display verification");
		HousingRenderer::PruneMeshes({});
		HousingRenderer::Reset();
		for (const auto& pair : swapchains) Require(pair.second.state == 0, "Outstanding image at teardown");
		for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i)
		{
			SIZE_T size = 0;
			debug->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> bytes(size);
			auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
			debug->GetMessage(i, message, &size);
			if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
			{
				std::cerr << message->pDescription << '\n';
				Require(false, "D3D11 debug layer reported a warning/error");
			}
		}
		g_panelSwapchains.clear();
		swapchains.clear();
		std::cout << "PASS: " << checkedFrames << " frames, " << rayChecks << " independent housing rays, " << displayChecks <<
			" textured display samples, " << emptyChecks << " transparent texels; true stereo projection uses current runtime eye poses, submits optional depth, keeps the dashboard separate, and falls back under layer pressure; "
			"head-facing and display-plane eye quads, 90/180-degree views, canted eyes, moving head, LOCAL/VIEW spaces, cropped content, typeless color, "
			"Round/Flat/Round profile changes, edit-mode and stereo layer budgeting, far-to-near order, "
			"stable full-resolution dashboard, independent scale, mixed stable/stereo modes, mode switching, "
			"single end-frame call, owned snapshots, failure fallback, image lifetime, stable swapchains, clean D3D11 debug layer\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
