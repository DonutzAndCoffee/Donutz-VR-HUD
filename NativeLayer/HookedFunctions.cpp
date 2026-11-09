#include "HookedFunctions.h"
#include "ControllerInput.h"
#include "IpcServer.h"
#include "Logging.h"
#include "VisorRenderer.h"
#include "HousingRenderer.h"
#include "ContentBounds.h"
#include "LightProbe.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// This hook chain intercepts the host application's (e.g. iRacing.exe)
// OpenXR calls to inject additional XrCompositionLayerQuad entries built
// from panel data received over IPC (see IpcServer.h), without the host
// ever knowing this layer exists. See NativeLayer/README.md for the overall
// architecture (modeled after OpenKneeboard).
namespace
{
    struct NextDispatch
    {
        PFN_xrCreateSession xrCreateSession = nullptr;
        PFN_xrDestroySession xrDestroySession = nullptr;
        PFN_xrEndFrame xrEndFrame = nullptr;
        PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
        PFN_xrDestroySwapchain xrDestroySwapchain = nullptr;
        PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
        PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
        PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
        PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
        PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
        PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
        PFN_xrDestroySpace xrDestroySpace = nullptr;
        PFN_xrLocateSpace xrLocateSpace = nullptr;
        PFN_xrLocateViews xrLocateViews = nullptr;
        PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
    };

    // Per-panel overlay swapchain, created lazily against the host's D3D11
    // device once a panel becomes enabled and sized.
    struct PanelSwapchain
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        int64_t format = 0;
        std::vector<ID3D11Texture2D*> images;
    };

    std::mutex g_mutex;
    NextDispatch g_next;
    XrSession g_appSession = XR_NULL_HANDLE;
    XrSpace g_viewSpace = XR_NULL_HANDLE;

    // World-locked reference space the panels are anchored to (analogous to
    // a dashboard element fixed in the cockpit rather than to the headset).
    // Created alongside g_viewSpace in Hook_xrCreateSession.
    XrSpace g_localSpace = XR_NULL_HANDLE;

    // The panel's yaw-only pose (in g_localSpace) at the moment of the last
    // recenter, i.e. where the "cockpit" is considered to be relative to the
    // room. Combined with each panel's authored offset in Hook_xrEndFrame.
    // Defaults to the local space's origin/identity until the user recenters.
    XrPosef g_anchorPose{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
    bool g_hasAnchor = false;
    std::mutex g_anchorMutex;

    ID3D11Device* g_d3dDevice = nullptr;
    ID3D11DeviceContext* g_d3dContext = nullptr;
    std::atomic<bool> g_depthLayerSupported{ false };

    std::mutex g_swapchainMutex;
    std::unordered_map<std::string, PanelSwapchain> g_panelSwapchains;

    // Swapchains created by the host application itself (not ours - our
    // own swapchains call g_next directly and never pass through the
    // hooks). Tracked so the housing light probe (LightProbe.cpp) can read
    // back the image the app just submitted in its projection layer.
    struct AppSwapchain
    {
        int64_t format = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arraySize = 1;
        uint32_t sampleCount = 1;
        std::vector<ID3D11Texture2D*> images;
        uint32_t lastAcquiredIndex = 0;
    };

    std::mutex g_appSwapchainMutex;
    std::unordered_map<XrSwapchain, AppSwapchain> g_appSwapchains;

    bool TryGetAppSwapchainImage(XrSwapchain swapchain, ID3D11Texture2D** texture, int64_t* format);

    std::string GuidToKey(const IpcServer::PanelIdBytes& id)
    {
        return std::string(reinterpret_cast<const char*>(id.bytes), sizeof(id.bytes));
    }

    const XrGraphicsBindingD3D11KHR* FindD3D11Binding(const void* next)
    {
        const auto* header = reinterpret_cast<const XrBaseInStructure*>(next);
        while (header != nullptr)
        {
            if (header->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            {
                return reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(header);
            }
            header = header->next;
        }
        return nullptr;
    }

    XrQuaternionf QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b)
    {
        return XrQuaternionf{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        };
    }

    XrVector3f QuatRotateVector(const XrQuaternionf& q, const XrVector3f& v)
    {
        // v' = q * v * q^-1, expanded for a unit quaternion.
        const XrVector3f qv{ q.x, q.y, q.z };
        const float qw = q.w;
        const XrVector3f t{
            2.0f * (qv.y * v.z - qv.z * v.y),
            2.0f * (qv.z * v.x - qv.x * v.z),
            2.0f * (qv.x * v.y - qv.y * v.x)
        };
        return XrVector3f{
            v.x + qw * t.x + (qv.y * t.z - qv.z * t.y),
            v.y + qw * t.y + (qv.z * t.x - qv.x * t.z),
            v.z + qw * t.z + (qv.x * t.y - qv.y * t.x)
        };
    }

    // Extracts only the yaw (rotation around the world "up"/Y axis) from a
    // full head orientation, discarding pitch/roll. This is what makes
    // recentering feel natural: leaning or looking up/down shouldn't tilt
    // the cockpit-anchored panels.
    XrQuaternionf YawOnly(const XrQuaternionf& q)
    {
        const float sinYawCosPitch = 2.0f * (q.w * q.y + q.x * q.z);
        const float cosYawCosPitch = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
        const float yaw = atan2f(sinYawCosPitch, cosYawCosPitch);
        const float half = yaw * 0.5f;
        return XrQuaternionf{ 0.0f, sinf(half), 0.0f, cosf(half) };
    }

    // Recomputes g_anchorPose from the current head pose (in g_localSpace),
    // keeping only yaw so the panels stay upright and "in the car" from the
    // driver's current forward-facing direction and position.
    void RecenterAnchor(XrSession session, XrSpace viewSpace, XrSpace localSpace, XrTime time)
    {
        PFN_xrLocateSpace locateSpace;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            locateSpace = g_next.xrLocateSpace;
        }

        if (locateSpace == nullptr || viewSpace == XR_NULL_HANDLE || localSpace == XR_NULL_HANDLE)
        {
            return;
        }

        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(locateSpace(viewSpace, localSpace, time, &location)))
        {
            Logging::Log("RecenterAnchor: xrLocateSpace failed.");
            return;
        }

        constexpr XrSpaceLocationFlags kRequiredFlags =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & kRequiredFlags) != kRequiredFlags)
        {
            Logging::Log("RecenterAnchor: head pose not valid/tracked yet, skipping.");
            return;
        }

        XrPosef newAnchor{};
        newAnchor.orientation = YawOnly(location.pose.orientation);
        newAnchor.position = location.pose.position;

        std::lock_guard<std::mutex> lock(g_anchorMutex);
        g_anchorPose = newAnchor;
        g_hasAnchor = true;
        Logging::Log("RecenterAnchor: anchored panels at position=(" +
            std::to_string(newAnchor.position.x) + ", " + std::to_string(newAnchor.position.y) + ", " + std::to_string(newAnchor.position.z) + ").");
    }

    // Ensures a swapchain exists for the given panel at the requested pixel
    // size, (re)creating it if missing or if the size changed. Returns
    // nullptr if the swapchain could not be created (e.g. no D3D11 device
    // captured yet).
    PanelSwapchain* EnsurePanelSwapchain(XrSession session, const std::string& key, int32_t width, int32_t height, int64_t forcedFormat = 0)
    {
        if (width <= 0 || height <= 0)
        {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(g_swapchainMutex);
        auto it = g_panelSwapchains.find(key);
        if (it != g_panelSwapchains.end() && it->second.width == width && it->second.height == height)
        {
            return &it->second;
        }

        PFN_xrCreateSwapchain createSwapchain;
        PFN_xrEnumerateSwapchainImages enumerateImages;
        PFN_xrDestroySwapchain destroySwapchain;
        PFN_xrEnumerateSwapchainFormats enumerateFormats;
        {
            std::lock_guard<std::mutex> dispatchLock(g_mutex);
            createSwapchain = g_next.xrCreateSwapchain;
            enumerateImages = g_next.xrEnumerateSwapchainImages;
            destroySwapchain = g_next.xrDestroySwapchain;
            enumerateFormats = g_next.xrEnumerateSwapchainFormats;
        }

        if (createSwapchain == nullptr || enumerateImages == nullptr)
        {
            return nullptr;
        }

        if (it != g_panelSwapchains.end())
        {
            if (destroySwapchain != nullptr && it->second.swapchain != XR_NULL_HANDLE)
            {
                destroySwapchain(it->second.swapchain);
            }
            g_panelSwapchains.erase(it);
        }

        int64_t chosenFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (forcedFormat != 0)
        {
            chosenFormat = forcedFormat;
        }
        else if (enumerateFormats != nullptr)
        {
            uint32_t formatCount = 0;
            enumerateFormats(session, 0, &formatCount, nullptr);
            if (formatCount > 0)
            {
                std::vector<int64_t> formats(formatCount);
                XrResult formatsResult = enumerateFormats(session, formatCount, &formatCount, formats.data());
                if (XR_SUCCEEDED(formatsResult))
                {
                    // Prefer formats matching the BGRA shared textures created by the
                    // managed sources (Sources/*.cs use Format.FormatB8G8R8A8Unorm).
                    // ID3D11DeviceContext::CopyResource requires identical formats
                    // between source and destination; a mismatch here silently fails
                    // the copy (leaving the quad black) instead of returning an error.
                    static const int64_t kPreferredFormats[] = {
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                        DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                    };

                    bool found = false;
                    for (int64_t preferred : kPreferredFormats)
                    {
                        if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
                        {
                            chosenFormat = preferred;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        chosenFormat = formats.front();
                    }
                    Logging::Log("EnsurePanelSwapchain: chosen swapchain format=" + std::to_string(chosenFormat) +
                        " (runtime reported " + std::to_string(formatCount) + " formats).");
                }
                else
                {
                    Logging::Log("EnsurePanelSwapchain: xrEnumerateSwapchainFormats (2nd call) failed, XrResult=" +
                        std::to_string(formatsResult) + ".");
                }
            }
            else
            {
                Logging::Log("EnsurePanelSwapchain: xrEnumerateSwapchainFormats returned formatCount=0.");
            }
        }

        XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        createInfo.usageFlags = forcedFormat == DXGI_FORMAT_D32_FLOAT
            ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
            : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = chosenFormat;
        createInfo.sampleCount = 1;
        createInfo.width = static_cast<uint32_t>(width);
        createInfo.height = static_cast<uint32_t>(height);
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrResult createResult = createSwapchain(session, &createInfo, &swapchain);
        if (XR_FAILED(createResult))
        {
            Logging::Log("EnsurePanelSwapchain: xrCreateSwapchain failed, XrResult=" + std::to_string(createResult) +
                " (w=" + std::to_string(width) + " h=" + std::to_string(height) + ").");
            return nullptr;
        }

        uint32_t imageCount = 0;
        enumerateImages(swapchain, 0, &imageCount, nullptr);
        if (imageCount == 0)
        {
            Logging::Log("EnsurePanelSwapchain: xrEnumerateSwapchainImages returned imageCount=0.");
            if (destroySwapchain != nullptr)
            {
                destroySwapchain(swapchain);
            }
            return nullptr;
        }

        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        XrResult enumResult = enumerateImages(
            swapchain,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(enumResult))
        {
            Logging::Log("EnsurePanelSwapchain: xrEnumerateSwapchainImages (2nd call) failed, XrResult=" + std::to_string(enumResult) + ".");
            if (destroySwapchain != nullptr)
            {
                destroySwapchain(swapchain);
            }
            return nullptr;
        }

        Logging::Log("EnsurePanelSwapchain: created swapchain successfully (w=" + std::to_string(width) + " h=" + std::to_string(height) + ", images=" + std::to_string(imageCount) + ").");

        PanelSwapchain panel;
        panel.swapchain = swapchain;
        panel.width = width;
        panel.height = height;
        panel.format = chosenFormat;
        panel.images.reserve(images.size());
        for (const auto& image : images)
        {
            panel.images.push_back(image.texture);
        }

        auto result = g_panelSwapchains.emplace(key, std::move(panel));
        return &result.first->second;
    }

    // Fills a swapchain image with a flat color via a small CPU-side
    // staging texture + CopyResource, used for the controller marker/laser
    // visualization quads (no CEF frame involved, unlike the HUD panels).
    // The color is only re-uploaded when it actually changes (tracked by
    // the caller) to avoid a staging texture round-trip every frame.
    void FillSolidColorTexture(ID3D11Texture2D* target, int32_t width, int32_t height, const ControllerInput::VisualColor& color)
    {
        if (g_d3dDevice == nullptr || g_d3dContext == nullptr || target == nullptr)
        {
            return;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, &staging)))
        {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_d3dContext->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped)))
        {
            return;
        }

        const uint8_t b = static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f);
        const uint8_t g = static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f);
        const uint8_t r = static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f);
        const uint8_t a = static_cast<uint8_t>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f);

        auto* rowBase = static_cast<uint8_t*>(mapped.pData);
        for (int32_t y = 0; y < height; ++y)
        {
            uint8_t* row = rowBase + static_cast<size_t>(y) * mapped.RowPitch;
            for (int32_t x = 0; x < width; ++x)
            {
                uint8_t* pixel = row + static_cast<size_t>(x) * 4;
                pixel[0] = b;
                pixel[1] = g;
                pixel[2] = r;
                pixel[3] = a;
            }
        }
        g_d3dContext->Unmap(staging.Get(), 0);
        g_d3dContext->CopyResource(target, staging.Get());
    }

    // Ensures a 4x4 solid-color swapchain exists for the given key (one per
    // marker/laser visual, e.g. "ctrl_marker_left"), (re)painting it with
    // the given color whenever the color changes. Small fixed size is fine
    // since XrCompositionLayerQuad scales the sub-image to worldSize.
    PanelSwapchain* EnsureVisualSwapchain(XrSession session, const std::string& key, const ControllerInput::VisualColor& color)
    {
        constexpr int32_t kVisualTextureSize = 4;
        PanelSwapchain* swapchain = EnsurePanelSwapchain(session, key, kVisualTextureSize, kVisualTextureSize);
        if (swapchain == nullptr)
        {
            return nullptr;
        }

        PFN_xrAcquireSwapchainImage acquireImage;
        PFN_xrWaitSwapchainImage waitImage;
        PFN_xrReleaseSwapchainImage releaseImage;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            acquireImage = g_next.xrAcquireSwapchainImage;
            waitImage = g_next.xrWaitSwapchainImage;
            releaseImage = g_next.xrReleaseSwapchainImage;
        }

        if (acquireImage == nullptr || waitImage == nullptr || releaseImage == nullptr || swapchain->images.empty())
        {
            return nullptr;
        }

        uint32_t imageIndex = 0;
        if (XR_FAILED(acquireImage(swapchain->swapchain, nullptr, &imageIndex)))
        {
            return nullptr;
        }

        XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_SUCCEEDED(waitImage(swapchain->swapchain, &waitInfo)))
        {
            FillSolidColorTexture(swapchain->images[imageIndex], kVisualTextureSize, kVisualTextureSize, color);
        }

        XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        releaseImage(swapchain->swapchain, &releaseInfo);

        return swapchain;
    }

    // ---- Quad-based panel housing -------------------------------------
    // Every housing face is its own XrCompositionLayerQuad, exactly like the
    // panel content itself. The runtime composites quads at display rate
    // with the latest head pose, so the housing stays as sharp and stable
    // as an unhoused panel. Faces are submitted back-to-front (outer walls,
    // switched-off screen, content, recess walls, front bezel), a correct
    // painter's order for a box seen from the front.

    // Colour held by each face swapchain, so the tiny texture is only
    // re-uploaded when the (8-bit quantised) lighting actually changes.
    struct HousingFaceColor
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t packed = 0xFFFFFFFFu;
        uint32_t target = 0xFFFFFFFFu;
        size_t painted = 0;
    };
    std::unordered_map<std::string, HousingFaceColor> g_housingFaceColors;

    float LinearToSrgbF(float c)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    }

    float SrgbToLinearF(float c)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }

    PanelSwapchain* EnsureHousingFaceSwapchain(XrSession session, const std::string& key, const ControllerInput::VisualColor& color)
    {
        auto toByte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        const uint32_t packed = (toByte(color.r) << 16) | (toByte(color.g) << 8) | toByte(color.b);
        auto& cached = g_housingFaceColors[key];
        if (cached.swapchain != XR_NULL_HANDLE && cached.packed == packed)
        {
            PanelSwapchain* existing = EnsurePanelSwapchain(session, key, 4, 4);
            if (existing != nullptr && existing->swapchain == cached.swapchain)
            {
                return existing;
            }
        }

        PanelSwapchain* swapchain = EnsureVisualSwapchain(session, key, color);
        if (swapchain == nullptr)
        {
            return nullptr;
        }

        // Every image of the ring must receive the new colour before the
        // swapchain is considered up to date.
        if (cached.swapchain != swapchain->swapchain || cached.target != packed)
        {
            cached.swapchain = swapchain->swapchain;
            cached.target = packed;
            cached.painted = 0;
        }
        if (++cached.painted >= swapchain->images.size())
        {
            cached.packed = packed;
        }
        return swapchain;
    }

	// Bezel front face, rendered as one quad with a per-pixel shaded
	// texture: rounded outer corners (anti-aliased alpha), a bevelled outer
	// edge and a small lip towards the display, a subtle vertical light
	// gradient and an image-based specular highlight that follows the
	// viewer's head. The display opening is transparent.
	constexpr int32_t kBezelTextureMaxSize = 384;

	// Shades (sRGB) for the flat face and the bevels of each side
	// (0 = top, 1 = bottom, 2 = left, 3 = right).
	struct BezelShading
	{
		float flat[3];
		float outer[4][3];
		float inner[4][3];
	};

	struct BezelGeometry
	{
		float hw, hh;        // display half extents (m)
		float ow, oh;        // outer half extents (m)
		float radius;        // outer corner radius (m)
		float outerBevel;    // outer bevel width (m)
		float innerBevel;    // inner lip width (m)
	};

	XrVector3f NormalizeVec(const XrVector3f& v)
	{
		const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		return len > 1e-6f ? XrVector3f{ v.x / len, v.y / len, v.z / len } : XrVector3f{ 0.0f, 0.0f, 1.0f };
	}

	float DotVec(const XrVector3f& a, const XrVector3f& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	XrVector3f InverseRotate(const XrQuaternionf& q, const XrVector3f& v)
	{
		return QuatRotateVector(XrQuaternionf{ -q.x, -q.y, -q.z, q.w }, v);
	}

	// Hemisphere ambient + environment reflection from the image light probe
	// plus a Blinn-Phong highlight of the "sky" (light from above, in the
	// panel's space). `normalLocal`/`eyeLocal`/`pointLocal` are in panel
	// coordinates. Returns an sRGB-encoded colour.
	ControllerInput::VisualColor ShadeHousingSurface(const IpcServer::HousingParams& p, const donutz::ProbeLighting& l,
		const XrQuaternionf& panelOrientation, const XrVector3f& normalLocal, const XrVector3f& eyeLocal, const XrVector3f& pointLocal)
	{
		const XrVector3f n = NormalizeVec(QuatRotateVector(panelOrientation, normalLocal));
		const XrVector3f v = NormalizeVec(QuatRotateVector(panelOrientation,
			XrVector3f{ eyeLocal.x - pointLocal.x, eyeLocal.y - pointLocal.y, eyeLocal.z - pointLocal.z }));
		const XrVector3f lightDir{ 0.0f, 1.0f, 0.0f };

		const float t = std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f);
		const float sky[3] = { l.skyR, l.skyG, l.skyB };
		const float ground[3] = { l.groundR, l.groundG, l.groundB };
		const float albedo[3] = { SrgbToLinearF(p.colorR), SrgbToLinearF(p.colorG), SrgbToLinearF(p.colorB) };
		const float rough = std::clamp(p.roughness, 0.05f, 1.0f);
		const float metal = std::clamp(p.metallic, 0.0f, 1.0f);

		// Reflected view direction picks sky or ground for the environment term.
		const float nv = DotVec(n, v);
		const XrVector3f refl{ 2.0f * nv * n.x - v.x, 2.0f * nv * n.y - v.y, 2.0f * nv * n.z - v.z };
		const float tr = std::clamp(refl.y * 0.5f + 0.5f, 0.0f, 1.0f);
		const float tEnv = tr + (t - tr) * rough;

		const XrVector3f h = NormalizeVec(XrVector3f{ lightDir.x + v.x, lightDir.y + v.y, lightDir.z + v.z });
		const float nh = (std::max)(DotVec(n, h), 0.0f);
		const float nl = (std::max)(DotVec(n, lightDir), 0.0f);
		const float r4 = rough * rough * rough * rough;
		const float shininess = std::clamp(2.0f / (std::max)(r4, 1e-4f) - 2.0f, 2.0f, 2000.0f);
		const float specLobe = powf(nh, shininess) * (shininess + 8.0f) / (8.0f * 3.14159265f) * nl;
		const float vh = std::clamp(DotVec(v, h), 0.0f, 1.0f);
		const float fresnelT = powf(1.0f - vh, 5.0f);

		float rgb[3];
		for (int i = 0; i < 3; ++i)
		{
			const float hemi = ground[i] + (sky[i] - ground[i]) * t;
			const float env = ground[i] + (sky[i] - ground[i]) * tEnv;
			const float f0 = 0.04f + (albedo[i] - 0.04f) * metal;
			const float fresnel = f0 + (1.0f - f0) * fresnelT;
			const float diffuse = albedo[i] * (1.0f - metal) * hemi;
			const float specular = env * fresnel * (1.0f - 0.7f * rough) + sky[i] * fresnel * (std::min)(specLobe, 40.0f) * 0.25f;
			rgb[i] = diffuse + specular;
		}

		ControllerInput::VisualColor c;
		c.r = LinearToSrgbF(rgb[0]);
		c.g = LinearToSrgbF(rgb[1]);
		c.b = LinearToSrgbF(rgb[2]);
		c.a = 1.0f;
		return c;
	}

	void FillBezelTexture(ID3D11Texture2D* target, int32_t width, int32_t height, const BezelGeometry& g, const BezelShading& s)
	{
		if (g_d3dDevice == nullptr || g_d3dContext == nullptr || target == nullptr)
		{
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(width);
		desc.Height = static_cast<UINT>(height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
		if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, &staging)))
		{
			return;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(g_d3dContext->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped)))
		{
			return;
		}

		const float pixel = (std::min)(2.0f * g.ow / width, 2.0f * g.oh / height);
		auto* rowBase = static_cast<uint8_t*>(mapped.pData);
		for (int32_t py = 0; py < height; ++py)
		{
			uint8_t* row = rowBase + static_cast<size_t>(py) * mapped.RowPitch;
			const float y = (0.5f - (py + 0.5f) / height) * 2.0f * g.oh;
			const float ay = fabsf(y);
			for (int32_t px = 0; px < width; ++px)
			{
				uint8_t* out = row + static_cast<size_t>(px) * 4;
				const float x = ((px + 0.5f) / width - 0.5f) * 2.0f * g.ow;
				const float ax = fabsf(x);

				// Display opening.
				if (ax < g.hw && ay < g.hh)
				{
					out[0] = out[1] = out[2] = out[3] = 0;
					continue;
				}

				// Rounded-rectangle signed distance to the outer edge.
				const float qx = ax - (g.ow - g.radius);
				const float qy = ay - (g.oh - g.radius);
				const float mx = (std::max)(qx, 0.0f);
				const float my = (std::max)(qy, 0.0f);
				(void)mx; (void)my; // 45 deg chamfer matching the side-shell corner strip
				const float d = (std::max)({ ax - g.ow, ay - g.oh, (qx + qy) * 0.70710678f - g.radius });
				const float alpha = std::clamp(1.5f - d / pixel, 0.0f, 1.0f); // opaque up to the edge, overlaps the walls
				if (alpha <= 0.0f)
				{
					out[0] = out[1] = out[2] = out[3] = 0;
					continue;
				}

				const int sideX = x < 0.0f ? 2 : 3;
				const int sideY = y < 0.0f ? 1 : 0;

				// Outer bevel: blend towards the side normals, radially in the corners.
				float wx, wy;
				if (qx > 0.0f && qy > 0.0f)
				{
					wx = qx; wy = qy;
				}
				else if (qx > qy)
				{
					wx = 1.0f; wy = 0.0f;
				}
				else
				{
					wx = 0.0f; wy = 1.0f;
				}
				const float wsum = wx + wy;
				wx /= wsum; wy /= wsum;
				const float wo = std::clamp(1.0f + d / g.outerBevel, 0.0f, 1.0f);

				// Inner lip towards the display.
				const float dx = ax - g.hw;
				const float dy = ay - g.hh;
				const float di = sqrtf((std::max)(dx, 0.0f) * (std::max)(dx, 0.0f) + (std::max)(dy, 0.0f) * (std::max)(dy, 0.0f));
				const float wi = std::clamp(1.0f - di / g.innerBevel, 0.0f, 1.0f);
				const int innerSide = dx > dy ? sideX : sideY;

				// Subtle top-to-bottom light falloff across the face.
				const float gradient = 1.0f + 0.10f * (y / g.oh);

				float rgb[3];
				for (int i = 0; i < 3; ++i)
				{
					float c = s.flat[i];
					c += (s.inner[innerSide][i] - c) * wi;
					const float outer = s.outer[sideX][i] * wx + s.outer[sideY][i] * wy;
					c += (outer - c) * wo;
					rgb[i] = std::clamp(c * gradient, 0.0f, 1.0f) * alpha;
				}
				out[0] = static_cast<uint8_t>(rgb[2] * 255.0f + 0.5f);
				out[1] = static_cast<uint8_t>(rgb[1] * 255.0f + 0.5f);
				out[2] = static_cast<uint8_t>(rgb[0] * 255.0f + 0.5f);
				out[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
			}
		}
		g_d3dContext->Unmap(staging.Get(), 0);
		g_d3dContext->CopyResource(target, staging.Get());
	}

	// Repaints the bezel texture only when its (8-bit quantised) shading or
	// geometry changed; every image of the swapchain ring gets the update.
	PanelSwapchain* EnsureBezelSwapchain(XrSession session, const std::string& key, int32_t width, int32_t height,
		const BezelGeometry& geometry, const BezelShading& shading)
	{
		PanelSwapchain* swapchain = EnsurePanelSwapchain(session, key, width, height);
		if (swapchain == nullptr || swapchain->images.empty())
		{
			return nullptr;
		}

		uint32_t tag = 2166136261u;
		auto mix = [&tag](uint32_t v) { tag = (tag ^ v) * 16777619u; };
		auto q8 = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		for (int i = 0; i < 3; ++i)
		{
			mix(q8(shading.flat[i]));
			for (int side = 0; side < 4; ++side)
			{
				mix(q8(shading.outer[side][i]));
				mix(q8(shading.inner[side][i]));
			}
		}
		const float geo[7] = { geometry.hw, geometry.hh, geometry.ow, geometry.oh, geometry.radius, geometry.outerBevel, geometry.innerBevel };
		for (float f : geo)
		{
			mix(static_cast<uint32_t>(f * 100000.0f));
		}

		auto& cached = g_housingFaceColors[key];
		if (cached.swapchain == swapchain->swapchain && cached.packed == tag)
		{
			return swapchain;
		}

		PFN_xrAcquireSwapchainImage acquireImage;
		PFN_xrWaitSwapchainImage waitImage;
		PFN_xrReleaseSwapchainImage releaseImage;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			acquireImage = g_next.xrAcquireSwapchainImage;
			waitImage = g_next.xrWaitSwapchainImage;
			releaseImage = g_next.xrReleaseSwapchainImage;
		}
		if (acquireImage == nullptr || waitImage == nullptr || releaseImage == nullptr)
		{
			return nullptr;
		}
		uint32_t imageIndex = 0;
		if (XR_FAILED(acquireImage(swapchain->swapchain, nullptr, &imageIndex)))
		{
			return nullptr;
		}
		XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		const bool painted = XR_SUCCEEDED(waitImage(swapchain->swapchain, &waitInfo));
		if (painted)
		{
			FillBezelTexture(swapchain->images[imageIndex], width, height, geometry, shading);
		}
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		releaseImage(swapchain->swapchain, &releaseInfo);
		if (!painted)
		{
			return nullptr;
		}

		if (cached.swapchain != swapchain->swapchain || cached.target != tag)
		{
			cached.swapchain = swapchain->swapchain;
			cached.target = tag;
			cached.painted = 0;
		}
		if (++cached.painted >= swapchain->images.size())
		{
			cached.packed = tag;
		}
		return swapchain;
	}

	// All solid-colour housing faces of a panel share one atlas swapchain
	// (one 4x4 cell per face) so a housing only needs three swapchains;
	// runtimes limit the swapchain count (XR_ERROR_LIMIT_REACHED).
	constexpr int32_t kAtlasCell = 16;
	constexpr int32_t kAtlasCells = 16;

	PanelSwapchain* EnsureHousingAtlasSwapchain(XrSession session, const std::string& key,
		const std::vector<ControllerInput::VisualColor>& colors)
	{
		const int32_t width = kAtlasCell * kAtlasCells;
		PanelSwapchain* swapchain = EnsurePanelSwapchain(session, key, width, kAtlasCell);
		if (swapchain == nullptr || swapchain->images.empty() || g_d3dDevice == nullptr || g_d3dContext == nullptr)
		{
			return nullptr;
		}

		uint32_t tag = 2166136261u;
		auto q8 = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		for (const auto& c : colors)
		{
			tag = (tag ^ ((q8(c.r) << 16) | (q8(c.g) << 8) | q8(c.b))) * 16777619u;
		}
		auto& cached = g_housingFaceColors[key];
		if (cached.swapchain == swapchain->swapchain && cached.packed == tag)
		{
			return swapchain;
		}

		PFN_xrAcquireSwapchainImage acquireImage;
		PFN_xrWaitSwapchainImage waitImage;
		PFN_xrReleaseSwapchainImage releaseImage;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			acquireImage = g_next.xrAcquireSwapchainImage;
			waitImage = g_next.xrWaitSwapchainImage;
			releaseImage = g_next.xrReleaseSwapchainImage;
		}
		if (acquireImage == nullptr || waitImage == nullptr || releaseImage == nullptr)
		{
			return nullptr;
		}
		uint32_t imageIndex = 0;
		if (XR_FAILED(acquireImage(swapchain->swapchain, nullptr, &imageIndex)))
		{
			return nullptr;
		}
		XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		waitInfo.timeout = 100000000; // 100 ms, never stall xrEndFrame
		bool painted = false;
		const XrResult waitResult = waitImage(swapchain->swapchain, &waitInfo);
		if (waitResult != XR_SUCCESS)
		{
			Logging::Log("EnsureHousingAtlasSwapchain: xrWaitSwapchainImage returned " + std::to_string(waitResult) + ".");
		}
		if (waitResult == XR_SUCCESS)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(width);
			desc.Height = static_cast<UINT>(kAtlasCell);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&desc, nullptr, &staging)) &&
				SUCCEEDED(g_d3dContext->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped)))
			{
				auto* rowBase = static_cast<uint8_t*>(mapped.pData);
				for (int32_t y = 0; y < kAtlasCell; ++y)
				{
					uint8_t* row = rowBase + static_cast<size_t>(y) * mapped.RowPitch;
					for (int32_t x = 0; x < width; ++x)
					{
						const size_t cell = static_cast<size_t>(x / kAtlasCell);
						const ControllerInput::VisualColor c = cell < colors.size() ? colors[cell] : ControllerInput::VisualColor{ 0.0f, 0.0f, 0.0f, 1.0f };
						uint8_t* pixel = row + static_cast<size_t>(x) * 4;
						pixel[0] = q8(c.b);
						pixel[1] = q8(c.g);
						pixel[2] = q8(c.r);
						pixel[3] = 255;
					}
				}
				g_d3dContext->Unmap(staging.Get(), 0);
				g_d3dContext->CopyResource(swapchain->images[imageIndex], staging.Get());
				painted = true;
			}
		}
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		releaseImage(swapchain->swapchain, &releaseInfo);
		if (!painted)
		{
			return nullptr;
		}

		if (cached.swapchain != swapchain->swapchain || cached.target != tag)
		{
			cached.swapchain = swapchain->swapchain;
			cached.target = tag;
			cached.painted = 0;
		}
		if (++cached.painted >= swapchain->images.size())
		{
			cached.packed = tag;
		}
		return swapchain;
	}

	// Hemisphere ambient
    // light probe. Returns an sRGB-encoded colour.
    ControllerInput::VisualColor ShadeHousingFace(const IpcServer::HousingParams& p, const donutz::ProbeLighting& l, float normalY, bool screen)
    {
        const float t = std::clamp(normalY * 0.5f + 0.5f, 0.0f, 1.0f);
        const float hemi[3] = {
            l.groundR + (l.skyR - l.groundR) * t,
            l.groundG + (l.skyG - l.groundG) * t,
            l.groundB + (l.skyB - l.groundB) * t };
        const float avg[3] = { (l.skyR + l.groundR) * 0.5f, (l.skyG + l.groundG) * 0.5f, (l.skyB + l.groundB) * 0.5f };

        float rgb[3];
        if (screen)
        {
            // Switched-off display: near-black glass with a faint reflection.
            for (int i = 0; i < 3; ++i)
            {
                rgb[i] = hemi[i] * 0.03f + avg[i] * 0.04f;
            }
        }
        else
        {
            const float albedo[3] = { SrgbToLinearF(p.colorR), SrgbToLinearF(p.colorG), SrgbToLinearF(p.colorB) };
            const float rough = std::clamp(p.roughness, 0.05f, 1.0f);
            const float metal = std::clamp(p.metallic, 0.0f, 1.0f);
            for (int i = 0; i < 3; ++i)
            {
                const float diffuse = albedo[i] * (1.0f - metal) * hemi[i];
                const float f0 = 0.04f + (albedo[i] - 0.04f) * metal;
                const float env = hemi[i] + (avg[i] - hemi[i]) * rough;
                rgb[i] = diffuse + env * f0 * (1.0f - 0.7f * rough);
            }
        }

        ControllerInput::VisualColor c;
        c.r = LinearToSrgbF(rgb[0]);
        c.g = LinearToSrgbF(rgb[1]);
        c.b = LinearToSrgbF(rgb[2]);
        c.a = 1.0f;
        return c;
    }

    // Builds the housing faces around a display of width x height centred
    // at `pose` (the display/content plane). `back` receives the faces drawn
    // before the panel content, `front` those drawn after it. The bezel's
    // front plane sits kDisplayRecessMeters in front of the display, so the
    // display appears set into the housing.
    void BuildHousingQuads(XrSession session, const std::string& key, const XrCompositionLayerQuad& contentQuad,
        const XrPosef& pose, float width, float height, const IpcServer::HousingParams& p,
        const donutz::ProbeLighting& lighting, const XrPosef& headPose,
        std::vector<XrCompositionLayerQuad>& back, std::vector<XrCompositionLayerQuad>& front)
    {
        constexpr float kDisplayRecessMeters = 0.001f;
        const float hw = width * 0.5f;
        const float hh = height * 0.5f;
        const float bezel = std::clamp(p.bezelWidthMm, 1.0f, 100.0f) * 0.001f;
        const float depth = std::clamp(p.depthMm, 2.0f, 200.0f) * 0.001f;
        const float r = kDisplayRecessMeters;
        const float ow = hw + bezel;
        const float oh = hh + bezel;
        // Limited so the corner never cuts into the display backing below.
        const float radius = std::clamp(p.cornerRadiusMm * 0.001f, 0.0f, (std::min)({ bezel * 1.5f, ow, oh }));

        // Viewer position in panel coordinates, for view-dependent shading.
        const XrVector3f viewerLocal = InverseRotate(pose.orientation, XrVector3f{
            headPose.position.x - pose.position.x,
            headPose.position.y - pose.position.y,
            headPose.position.z - pose.position.z });
        // Shading uses a fixed nominal viewpoint in front of the panel:
        // view-dependent shading would repaint every housing texture on each
        // head movement, and the compositor then shows that one frame late
        // (visible as flicker). Only the light probe changes the shading.
        const XrVector3f eyeLocal{ 0.0f, 0.1f, 0.6f };

        constexpr float s = 0.70710678f;
        const XrQuaternionf identity{ 0.0f, 0.0f, 0.0f, 1.0f };
        const XrQuaternionf faceUp{ -s, 0.0f, 0.0f, s };    // +z -> +y
        const XrQuaternionf faceDown{ s, 0.0f, 0.0f, s };   // +z -> -y
        const XrQuaternionf faceLeft{ 0.0f, -s, 0.0f, s };  // +z -> -x
        const XrQuaternionf faceRight{ 0.0f, s, 0.0f, s };  // +z -> +x

        auto face = [&](std::vector<XrCompositionLayerQuad>& out, const char* name,
            XrVector3f center, const XrQuaternionf& rot, float w, float h, XrVector3f normal, bool screen)
        {
            PanelSwapchain* swapchain = EnsureHousingFaceSwapchain(session, key + "_housing_" + name,
                screen ? ShadeHousingFace(p, lighting, QuatRotateVector(pose.orientation, normal).y, true)
                       : ShadeHousingSurface(p, lighting, pose.orientation, normal, eyeLocal, center));
            if (swapchain == nullptr)
            {
                return;
            }
            XrCompositionLayerQuad q = contentQuad;
            q.layerFlags = 0;
            q.subImage.swapchain = swapchain->swapchain;
            q.subImage.imageRect.offset = { 0, 0 };
            q.subImage.imageRect.extent = { swapchain->width, swapchain->height };
            q.subImage.imageArrayIndex = 0;
            q.pose.orientation = QuatMultiply(pose.orientation, rot);
            const XrVector3f c = QuatRotateVector(pose.orientation, center);
            q.pose.position = { pose.position.x + c.x, pose.position.y + c.y, pose.position.z + c.z };
            q.size = { w, h };
            out.push_back(q);
        };

        // Behind the content: outer walls (bezel front plane back to the
        // rear of the housing), then the switched-off display.
        const float wallZ = (r - depth) * 0.5f;
        const float wallLen = depth + r;
        // Walls are shortened by the corner radius so they don't stick out
        // past the rounded bezel corners.
        // Corner strips are tangent to the rounded bezel outline (circumscribed),
        // so walls extend by r*tan(45deg/2) and meet them without a gap.
        constexpr float kCornerTan = 0.41421356f;
        const float wallW = 2.0f * (ow - radius * (1.0f - kCornerTan));
        const float wallH = 2.0f * (oh - radius * (1.0f - kCornerTan));
        // Only faces pointing towards the viewer are submitted (quads are
        // double-sided and the box is convex), which keeps the layer count
        // low. Seen from behind, the whole shell is drawn after the content
        // so the back plate covers it.
        const bool viewerBehind = viewerLocal.z < -depth;
        std::vector<XrCompositionLayerQuad> rearShell;
        XrCompositionLayerQuad backPlate{};
        bool hasBackPlate = false;
        std::vector<XrCompositionLayerQuad>& shell = viewerBehind ? rearShell : back;
        auto facesViewer = [&](const XrVector3f& center, const XrVector3f& normal)
        {
            (void)center; (void)normal;
            return true;
        };
        auto wall = [&](const char* name, XrVector3f center, const XrQuaternionf& rot, float w, float h, XrVector3f normal)
        {
            if (w > 0.0005f && h > 0.0005f)
            {
                face(shell, name, center, rot, w, h, normal, false);
            }
        };
        wall("outer_top", { 0.0f, oh, wallZ }, faceUp, wallW, wallLen, { 0.0f, 1.0f, 0.0f });
        wall("outer_bottom", { 0.0f, -oh, wallZ }, faceDown, wallW, wallLen, { 0.0f, -1.0f, 0.0f });
        wall("outer_left", { -ow, 0.0f, wallZ }, faceLeft, wallLen, wallH, { -1.0f, 0.0f, 0.0f });
        wall("outer_right", { ow, 0.0f, wallZ }, faceRight, wallLen, wallH, { 1.0f, 0.0f, 0.0f });

        // Rounded corners of the side shell, approximated by narrow strips.
        if (radius > 0.0005f)
        {
            constexpr int kCornerSegments = 1;
            constexpr float kHalfPi = 1.57079633f;
            const float delta = kHalfPi / kCornerSegments;
            const float chord = 2.0f * radius * kCornerTan * 1.12f;
            const float inset = radius;
            const float signs[4][2] = { { 1.0f, 1.0f }, { -1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f } };
            for (int corner = 0; corner < 4; ++corner)
            {
                const float cx = signs[corner][0] * (ow - radius);
                const float cy = signs[corner][1] * (oh - radius);
                for (int seg = 0; seg < kCornerSegments; ++seg)
                {
                    const float angle = corner * kHalfPi + (seg + 0.5f) * delta;
                    const float nx = cosf(angle);
                    const float ny = sinf(angle);
                    const XrVector3f center{ cx + nx * inset, cy + ny * inset, wallZ };
                    const XrVector3f normal{ nx, ny, 0.0f };
                    // Quad axes: X = tangent, Y = panel +z, Z = outward normal.
                    const float m00 = -ny, m01 = 0.0f, m02 = nx;
                    const float m10 = nx, m11 = 0.0f, m12 = ny;
                    const float m20 = 0.0f, m21 = 1.0f, m22 = 0.0f;
                    XrQuaternionf rot{};
                    const float trace = m00 + m11 + m22;
                    if (trace > 0.0f)
                    {
                        const float s4 = sqrtf(trace + 1.0f) * 2.0f;
                        rot = { (m21 - m12) / s4, (m02 - m20) / s4, (m10 - m01) / s4, 0.25f * s4 };
                    }
                    else if (m00 > m11 && m00 > m22)
                    {
                        const float s4 = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
                        rot = { 0.25f * s4, (m01 + m10) / s4, (m02 + m20) / s4, (m21 - m12) / s4 };
                    }
                    else if (m11 > m22)
                    {
                        const float s4 = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
                        rot = { (m01 + m10) / s4, 0.25f * s4, (m12 + m21) / s4, (m02 - m20) / s4 };
                    }
                    else
                    {
                        const float s4 = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
                        rot = { (m02 + m20) / s4, (m12 + m21) / s4, 0.25f * s4, (m10 - m01) / s4 };
                    }
                    if (facesViewer(center, normal))
                    {
                        const std::string name = "corner_" + std::to_string(corner) + "_" + std::to_string(seg);
                        // All strips share one swapchain (same colour) to stay within the
                        // runtime's swapchain limit.
                        (void)name;
                        face(shell, "corner", center, rot, chord, wallLen, XrVector3f{ 1.0f, 0.0f, 0.0f }, false);
                    }
                }
            }
        }

        // Rounded back plate, only visible from behind.
        if (viewerBehind)
        {
            BezelGeometry backGeometry{};
            backGeometry.ow = ow;
            backGeometry.oh = oh;
            backGeometry.radius = radius;
            backGeometry.outerBevel = (std::max)((std::min)(bezel * 0.35f, 0.003f), 0.0005f);
            backGeometry.innerBevel = 0.0003f;
            const XrVector3f backCenter{ 0.0f, 0.0f, -depth };
            const ControllerInput::VisualColor c = ShadeHousingSurface(p, lighting, pose.orientation, { 0.0f, 0.0f, -1.0f }, eyeLocal, backCenter);
            BezelShading backShading{};
            const float rgb[3] = { c.r, c.g, c.b };
            for (int i = 0; i < 3; ++i)
            {
                backShading.flat[i] = rgb[i];
                for (int side = 0; side < 4; ++side)
                {
                    backShading.outer[side][i] = rgb[i] * 0.85f;
                    backShading.inner[side][i] = rgb[i];
                }
            }
            const int32_t bw = ow >= oh ? 256 : (std::max)(32, static_cast<int32_t>(256 * ow / oh));
            const int32_t bh = oh >= ow ? 256 : (std::max)(32, static_cast<int32_t>(256 * oh / ow));
            PanelSwapchain* backSwapchain = EnsureBezelSwapchain(session, key + "_housing_backplate", bw, bh, backGeometry, backShading);
            if (backSwapchain != nullptr)
            {
                XrCompositionLayerQuad q = contentQuad;
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.subImage.swapchain = backSwapchain->swapchain;
                q.subImage.imageRect.offset = { 0, 0 };
                q.subImage.imageRect.extent = { backSwapchain->width, backSwapchain->height };
                q.subImage.imageArrayIndex = 0;
                q.pose.orientation = QuatMultiply(pose.orientation, XrQuaternionf{ 0.0f, 1.0f, 0.0f, 0.0f });
                const XrVector3f bc = QuatRotateVector(pose.orientation, backCenter);
                q.pose.position = { pose.position.x + bc.x, pose.position.y + bc.y, pose.position.z + bc.z };
                q.size = { 2.0f * ow, 2.0f * oh };
                // Pushed after the bezel below (see end of function).
                backPlate = q;
                hasBackPlate = true;
            }
        }
        // Switched-off display backing: slightly behind the content plane so
        // runtimes that depth-test quads cannot z-fight it with the content,
        // and reaching halfway under the bezel so it closes the recess edges
        // seen at an angle without poking out of rounded outer corners.
        constexpr float kBackingOffsetMeters = 0.0005f;
        face(back, "screen", { 0.0f, 0.0f, -kBackingOffsetMeters }, identity, 2.0f * hw + bezel, 2.0f * hh + bezel, { 0.0f, 0.0f, 1.0f }, true);

        		// In front of the content: the bezel as one quad, 1 mm in front of
		// the display plane, per-pixel shaded (bevels, gradient, highlight)
		// with rounded outer corners.
		BezelGeometry geometry{};
		geometry.hw = hw;
		geometry.hh = hh;
		geometry.ow = ow;
		geometry.oh = oh;
		geometry.radius = radius;
		geometry.outerBevel = (std::max)((std::min)(bezel * 0.35f, 0.003f), 0.0005f);
		geometry.innerBevel = (std::max)((std::min)(bezel * 0.2f, 0.0015f), 0.0003f);

		const XrVector3f faceCenter{ 0.0f, 0.0f, r };
		auto shade = [&](const XrVector3f& normal, float* out)
		{
			const ControllerInput::VisualColor c = ShadeHousingSurface(p, lighting, pose.orientation, normal, eyeLocal, faceCenter);
			out[0] = c.r; out[1] = c.g; out[2] = c.b;
		};
		BezelShading shading{};
		shade({ 0.0f, 0.0f, 1.0f }, shading.flat);
		const XrVector3f sideDirs[4] = { { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
		for (int side = 0; side < 4; ++side)
		{
			const XrVector3f& d = sideDirs[side];
			shade({ d.x, d.y, 1.0f }, shading.outer[side]);
			shade({ -d.x * 0.8f, -d.y * 0.8f, 1.0f }, shading.inner[side]);
		}

		const int32_t texWidth = ow >= oh ? kBezelTextureMaxSize
			: (std::max)(64, static_cast<int32_t>(kBezelTextureMaxSize * ow / oh));
		const int32_t texHeight = oh >= ow ? kBezelTextureMaxSize
			: (std::max)(64, static_cast<int32_t>(kBezelTextureMaxSize * oh / ow));
		PanelSwapchain* bezelSwapchain = EnsureBezelSwapchain(session, key + "_housing_bezel", texWidth, texHeight, geometry, shading);
		if (bezelSwapchain != nullptr)
		{
			XrCompositionLayerQuad q = contentQuad;
			q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			q.subImage.swapchain = bezelSwapchain->swapchain;
			q.subImage.imageRect.offset = { 0, 0 };
			q.subImage.imageRect.extent = { bezelSwapchain->width, bezelSwapchain->height };
			q.subImage.imageArrayIndex = 0;
			q.pose.orientation = pose.orientation;
			const XrVector3f c = QuatRotateVector(pose.orientation, faceCenter);
			q.pose.position = { pose.position.x + c.x, pose.position.y + c.y, pose.position.z + c.z };
			q.size = { 2.0f * ow, 2.0f * oh };
			front.push_back(q);
		}

        front.insert(front.end(), rearShell.begin(), rearShell.end());
        if (hasBackPlate)
        {
            front.push_back(backPlate);
        }
	}


    // Paints a transparent-center
    // visual idea as the desktop edit-mode highlight in
    // Sources/PanelHighlightSource.cs) used to mark the panel currently
    // grabbed by a VR controller.
    void FillFrameTexture(ID3D11Texture2D* target, int32_t width, int32_t height, const ControllerInput::VisualColor& color)
    {
        if (g_d3dDevice == nullptr || g_d3dContext == nullptr || target == nullptr)
        {
            return;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, &staging)))
        {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_d3dContext->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped)))
        {
            return;
        }

        const uint8_t b = static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f);
        const uint8_t g = static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f);
        const uint8_t r = static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f);
        const uint8_t a = static_cast<uint8_t>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f);

        const int32_t smallerSide = (width < height) ? width : height;
        const int32_t borderThickness = (smallerSide / 40 > 1) ? (smallerSide / 40) : 1;

        auto* rowBase = static_cast<uint8_t*>(mapped.pData);
        for (int32_t y = 0; y < height; ++y)
        {
            uint8_t* row = rowBase + static_cast<size_t>(y) * mapped.RowPitch;
            const bool inBorderRow = (y < borderThickness) || (y >= height - borderThickness);
            for (int32_t x = 0; x < width; ++x)
            {
                const bool inBorderCol = (x < borderThickness) || (x >= width - borderThickness);
                uint8_t* pixel = row + static_cast<size_t>(x) * 4;
                if (inBorderRow || inBorderCol)
                {
                    pixel[0] = b;
                    pixel[1] = g;
                    pixel[2] = r;
                    pixel[3] = a;
                }
                else
                {
                    pixel[0] = 0;
                    pixel[1] = 0;
                    pixel[2] = 0;
                    pixel[3] = 0;
                }
            }
        }
        g_d3dContext->Unmap(staging.Get(), 0);
        g_d3dContext->CopyResource(target, staging.Get());
    }

    // Ensures a solid-colored frame-border swapchain exists for the given
    // key (one per hand, e.g. "ctrl_grab_highlight_left"), repainting it
    // whenever the color changes. Larger than the marker/laser textures so
    // the border stays crisp when stretched to panel size.
    PanelSwapchain* EnsureFrameSwapchain(XrSession session, const std::string& key, const ControllerInput::VisualColor& color)
    {
        constexpr int32_t kFrameTextureSize = 64;
        PanelSwapchain* swapchain = EnsurePanelSwapchain(session, key, kFrameTextureSize, kFrameTextureSize);
        if (swapchain == nullptr)
        {
            return nullptr;
        }

        PFN_xrAcquireSwapchainImage acquireImage;
        PFN_xrWaitSwapchainImage waitImage;
        PFN_xrReleaseSwapchainImage releaseImage;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            acquireImage = g_next.xrAcquireSwapchainImage;
            waitImage = g_next.xrWaitSwapchainImage;
            releaseImage = g_next.xrReleaseSwapchainImage;
        }

        if (acquireImage == nullptr || waitImage == nullptr || releaseImage == nullptr || swapchain->images.empty())
        {
            return nullptr;
        }

        uint32_t imageIndex = 0;
        if (XR_FAILED(acquireImage(swapchain->swapchain, nullptr, &imageIndex)))
        {
            return nullptr;
        }

        XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_SUCCEEDED(waitImage(swapchain->swapchain, &waitInfo)))
        {
            FillFrameTexture(swapchain->images[imageIndex], kFrameTextureSize, kFrameTextureSize, color);
        }

        XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        releaseImage(swapchain->swapchain, &releaseInfo);

        return swapchain;
    }

    // Copies the panel's latest shared D3D11 texture into the given
    // swapchain image using the host's device context. The texture is
    // shared cross-process by *name* (see IDXGIResource1::CreateSharedHandle
    // on the sender side, this app's Sources/*.cs), since a raw NT handle
    // value is only valid within the process that created it. This process
    // opens the same underlying GPU resource by name via
    // ID3D11Device1::OpenSharedResourceByName.
    void UpdatePanelTexture(const IpcServer::PanelState& panel, ID3D11Texture2D* target)
    {
        if (g_d3dDevice == nullptr || g_d3dContext == nullptr || target == nullptr || !panel.hasFrame || panel.sharedHandleName.empty())
        {
            Logging::Log("UpdatePanelTexture: skipped (device=" + std::to_string(g_d3dDevice != nullptr) +
                " context=" + std::to_string(g_d3dContext != nullptr) +
                " target=" + std::to_string(target != nullptr) +
                " hasFrame=" + std::to_string(panel.hasFrame) +
                " nameEmpty=" + std::to_string(panel.sharedHandleName.empty()) + ").");
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        HRESULT hr = g_d3dDevice->QueryInterface(IID_PPV_ARGS(&device1));
        if (FAILED(hr))
        {
            Logging::Log("UpdatePanelTexture: QueryInterface(ID3D11Device1) failed, hr=0x" + std::to_string(static_cast<unsigned long>(hr)) + ".");
            return;
        }

        std::wstring nameCopy = panel.sharedHandleName;
        std::string nameNarrow(nameCopy.begin(), nameCopy.end());

        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        hr = device1->OpenSharedResourceByName(
                panel.sharedHandleName.c_str(),
                DXGI_SHARED_RESOURCE_READ,
                IID_PPV_ARGS(&sharedTexture));
        if (FAILED(hr))
        {
            Logging::Log("UpdatePanelTexture: OpenSharedResourceByName('" + nameNarrow + "') failed, hr=0x" + std::to_string(static_cast<unsigned long>(hr)) + ".");
            return;
        }

        g_d3dContext->CopyResource(target, sharedTexture.Get());
        Logging::Log(Logging::LogLevel::Debug, "UpdatePanelTexture: successfully copied shared texture '" + nameNarrow + "'.");
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSession(
        XrInstance instance,
        const XrSessionCreateInfo* createInfo,
        XrSession* session)
    {
        PFN_xrCreateSession next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrCreateSession;
        }

        const XrResult result = next(instance, createInfo, session);
        if (XR_SUCCEEDED(result))
        {
            PFN_xrCreateReferenceSpace createReferenceSpace;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_appSession = *session;

                if (const XrGraphicsBindingD3D11KHR* binding = FindD3D11Binding(createInfo->next))
                {
                    g_d3dDevice = binding->device;
                    if (g_d3dDevice != nullptr)
                    {
                        g_d3dDevice->GetImmediateContext(&g_d3dContext);
                    }
                }

                createReferenceSpace = g_next.xrCreateReferenceSpace;
            }

            if (createReferenceSpace != nullptr)
            {
                XrReferenceSpaceCreateInfo spaceCreateInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
                spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                spaceCreateInfo.poseInReferenceSpace.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                spaceCreateInfo.poseInReferenceSpace.position = { 0.0f, 0.0f, 0.0f };

                XrSpace viewSpace = XR_NULL_HANDLE;
                if (XR_SUCCEEDED(createReferenceSpace(*session, &spaceCreateInfo, &viewSpace)))
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_viewSpace = viewSpace;
                }
                else
                {
                    Logging::Log("Hook_xrCreateSession: failed to create VIEW reference space; overlay will fall back to the host layer space.");
                }

                // LOCAL is a world-locked space (fixed relative to the room/
                // seated origin), unlike VIEW which moves and rotates with
                // the headset. Panels default to this space so they behave
                // like a fixed cockpit element instead of a head-locked HUD.
                XrReferenceSpaceCreateInfo localCreateInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
                localCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                localCreateInfo.poseInReferenceSpace.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                localCreateInfo.poseInReferenceSpace.position = { 0.0f, 0.0f, 0.0f };

                XrSpace localSpace = XR_NULL_HANDLE;
                if (XR_SUCCEEDED(createReferenceSpace(*session, &localCreateInfo, &localSpace)))
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_localSpace = localSpace;
                }
                else
                {
                    Logging::Log("Hook_xrCreateSession: failed to create LOCAL reference space; overlay will fall back to head-locked (VIEW) mode.");
                }

                ControllerInput::OnSessionCreated(*session, localSpace);
            }
        }

        return result;
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session)
    {
        PFN_xrDestroySession next;
        PFN_xrDestroySpace destroySpace;
        XrSpace viewSpaceToDestroy = XR_NULL_HANDLE;
        XrSpace localSpaceToDestroy = XR_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrDestroySession;
            destroySpace = g_next.xrDestroySpace;
            if (session == g_appSession)
            {
                g_appSession = XR_NULL_HANDLE;
                g_d3dDevice = nullptr;
                g_d3dContext = nullptr;
                viewSpaceToDestroy = g_viewSpace;
                g_viewSpace = XR_NULL_HANDLE;
                localSpaceToDestroy = g_localSpace;
                g_localSpace = XR_NULL_HANDLE;
                VisorRenderer::Reset();
                HousingRenderer::Reset();
                ContentBounds::Reset();
                donutz::LightProbe::Reset();
            }
        }

        {
            std::lock_guard<std::mutex> anchorLock(g_anchorMutex);
            g_hasAnchor = false;
        }

        ControllerInput::OnSessionDestroyed();

        if (destroySpace != nullptr && viewSpaceToDestroy != XR_NULL_HANDLE)
        {
            destroySpace(viewSpaceToDestroy);
        }

        if (destroySpace != nullptr && localSpaceToDestroy != XR_NULL_HANDLE)
        {
            destroySpace(localSpaceToDestroy);
        }

        {
            std::lock_guard<std::mutex> lock(g_swapchainMutex);
            PFN_xrDestroySwapchain destroySwapchain;
            {
                std::lock_guard<std::mutex> dispatchLock(g_mutex);
                destroySwapchain = g_next.xrDestroySwapchain;
            }
            if (destroySwapchain != nullptr)
            {
                for (auto& [key, panel] : g_panelSwapchains)
                {
                    if (panel.swapchain != XR_NULL_HANDLE)
                    {
                        destroySwapchain(panel.swapchain);
                    }
                }
            }
            g_panelSwapchains.clear();
        }

        return next(session);
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo)
    {
        PFN_xrEndFrame next;
        PFN_xrAcquireSwapchainImage acquireImage;
        PFN_xrWaitSwapchainImage waitImage;
        PFN_xrReleaseSwapchainImage releaseImage;
        XrSpace viewSpace;
        XrSpace localSpace;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrEndFrame;
            acquireImage = g_next.xrAcquireSwapchainImage;
            waitImage = g_next.xrWaitSwapchainImage;
            releaseImage = g_next.xrReleaseSwapchainImage;
            viewSpace = g_viewSpace;
            localSpace = g_localSpace;
        }

        if (session != g_appSession || frameEndInfo == nullptr || acquireImage == nullptr || waitImage == nullptr || releaseImage == nullptr)
        {
            return next(session, frameEndInfo);
        }

        ControllerInput::Update(session, localSpace, frameEndInfo->displayTime);

        if (IpcServer::ConsumeRecenterRequested())
        {
            RecenterAnchor(session, viewSpace, localSpace, frameEndInfo->displayTime);
        }
        else
        {
            bool needsInitialAnchor = false;
            {
                std::lock_guard<std::mutex> lock(g_anchorMutex);
                needsInitialAnchor = !g_hasAnchor;
            }
            if (needsInitialAnchor)
            {
                // First frame after session start: anchor the panels to the
                // driver's initial head position/facing so they don't appear
                // at the LOCAL space's raw origin (which may be far from the
                // seated position) before the user explicitly recenters.
                RecenterAnchor(session, viewSpace, localSpace, frameEndInfo->displayTime);
            }
        }

        const auto panels = IpcServer::GetPanelsSnapshot();

        static int s_frameLogCounter = 0;
        if ((s_frameLogCounter++ % 90) == 0)
        {
            Logging::Log(Logging::LogLevel::Debug, "Hook_xrEndFrame: panels snapshot count=" + std::to_string(panels.size()) +
                " viewSpaceValid=" + std::to_string(viewSpace != XR_NULL_HANDLE) +
                " appLayerCount=" + std::to_string(frameEndInfo->layerCount));
            for (const auto& p : panels)
            {
                Logging::Log(Logging::LogLevel::Debug, "  panel: enabled=" + std::to_string(p.enabled) +
                    " hasFrame=" + std::to_string(p.hasFrame) +
                    " w=" + std::to_string(p.frameWidth) + " h=" + std::to_string(p.frameHeight) +
                    " nameEmpty=" + std::to_string(p.sharedHandleName.empty()));
            }
        }

        // Storage for the quads and their referenced sub-images must outlive
        // the xrEndFrame call below.
        std::vector<XrCompositionLayerQuad> quads;
        std::vector<const XrCompositionLayerBaseHeader*> combinedLayers;
        quads.reserve(panels.size() * 14);
        bool anyHousedPanel = false;
        const donutz::ProbeLighting housingLighting = donutz::LightProbe::GetLighting();
        // Head pose in LOCAL space for view-dependent housing highlights.
        XrPosef headInLocal{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
        bool headInLocalValid = false;
        {
            PFN_xrLocateSpace locateSpace;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                locateSpace = g_next.xrLocateSpace;
            }
            XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
            if (locateSpace != nullptr && viewSpace != XR_NULL_HANDLE && localSpace != XR_NULL_HANDLE &&
                XR_SUCCEEDED(locateSpace(viewSpace, localSpace, frameEndInfo->displayTime, &location)) &&
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
            {
                headInLocal = location.pose;
                headInLocalValid = true;
            }
        }
        std::vector<XrCompositionLayerQuad> housingBack;
        std::vector<XrCompositionLayerQuad> housingFront;
        // Mesh-mode housings, rendered later into one projection layer.
        struct MeshHousing
        {
            XrSpace space;
            HousingRenderer::Instance instance;
            float pixelsPerMeter;
        };
        std::vector<MeshHousing> meshHousings;
        float meshRenderScale = 0.5f;

        for (const auto& panel : panels)
        {
            if (panel.isVisor || !panel.enabled || !panel.hasFrame || panel.frameWidth <= 0 || panel.frameHeight <= 0)
            {
                continue;
            }

            const std::string key = GuidToKey(panel.panelId);
            PanelSwapchain* panelSwapchain = EnsurePanelSwapchain(session, key, panel.frameWidth, panel.frameHeight);
            if (panelSwapchain == nullptr || panelSwapchain->images.empty())
            {
                Logging::Log("Hook_xrEndFrame: EnsurePanelSwapchain failed for panel (w=" + std::to_string(panel.frameWidth) + " h=" + std::to_string(panel.frameHeight) + ").");
                continue;
            }

            uint32_t imageIndex = 0;
            if (XR_FAILED(acquireImage(panelSwapchain->swapchain, nullptr, &imageIndex)))
            {
                Logging::Log("Hook_xrEndFrame: xrAcquireSwapchainImage failed.");
                continue;
            }

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(waitImage(panelSwapchain->swapchain, &waitInfo)))
            {
                XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                releaseImage(panelSwapchain->swapchain, &releaseInfo);
                continue;
            }

            UpdatePanelTexture(panel, panelSwapchain->images[imageIndex]);
            if (panel.hasHousing)
            {
                ContentBounds::Update(key, g_d3dDevice, g_d3dContext, panelSwapchain->images[imageIndex]);
            }

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            releaseImage(panelSwapchain->swapchain, &releaseInfo);

            XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
            // Without this flag the compositor treats the quad's texture as
            // fully opaque, so transparent (alpha=0) CEF pixels are shown as
            // solid black instead of letting the scene behind the panel show
            // through. The managed sources write premultiplied-alpha BGRA
            // (CEF's default OSR output), matching
            // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT semantics.
            // Panels can opt into panel.opaqueBackground to disable
            // blending on purpose (e.g. a dashboard skin authored with a
            // black background that should stay solid black, not blend).
            quad.layerFlags = panel.opaqueBackground
                ? 0
                : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

            // Panels normally stay fixed relative to the driver's cockpit
            // anchor (LOCAL space). panel.headLocked opts a panel back into
            // following the headset (VIEW space), e.g. for a HUD element
            // that should always be in view regardless of head movement.
            const bool useLocalSpace = !panel.headLocked && g_localSpace != XR_NULL_HANDLE;
            quad.space = useLocalSpace
                ? g_localSpace
                : (viewSpace != XR_NULL_HANDLE
                    ? viewSpace
                    : (frameEndInfo->layers[0] != nullptr ? frameEndInfo->layers[0]->space : XR_NULL_HANDLE));
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = panelSwapchain->swapchain;
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { panel.frameWidth, panel.frameHeight };

            // Panel-authored offset (relative to the cockpit anchor), as a
            // pose in its own right.
            XrPosef offsetPose{};
            offsetPose.position = { panel.positionX, panel.positionY, panel.positionZ };

            // Convert degrees around X/Y/Z to a quaternion (ZYX order).
            const float radX = panel.rotationDegX * 0.0174532924f * 0.5f;
            const float radY = panel.rotationDegY * 0.0174532924f * 0.5f;
            const float radZ = panel.rotationDegZ * 0.0174532924f * 0.5f;
            const float cx = cosf(radX), sx = sinf(radX);
            const float cy = cosf(radY), sy = sinf(radY);
            const float cz = cosf(radZ), sz = sinf(radZ);
            offsetPose.orientation.w = cx * cy * cz + sx * sy * sz;
            offsetPose.orientation.x = sx * cy * cz - cx * sy * sz;
            offsetPose.orientation.y = cx * sy * cz + sx * cy * sz;
            offsetPose.orientation.z = cx * cy * sz - sx * sy * cz;

            if (useLocalSpace)
            {
                // Compose the panel's authored offset with the cockpit
                // anchor pose (both expressed in g_localSpace), so the panel
                // is world-locked like a real dashboard element: moving/
                // turning the head no longer moves the panel, only
                // recentering (or a future explicit "move cockpit" action)
                // does.
                XrPosef anchor;
                {
                    std::lock_guard<std::mutex> lock(g_anchorMutex);
                    anchor = g_anchorPose;
                }

                quad.pose.orientation = QuatMultiply(anchor.orientation, offsetPose.orientation);
                const XrVector3f rotatedOffset = QuatRotateVector(anchor.orientation, offsetPose.position);
                quad.pose.position = {
                    anchor.position.x + rotatedOffset.x,
                    anchor.position.y + rotatedOffset.y,
                    anchor.position.z + rotatedOffset.z
                };
            }
            else
            {
                // Head-locked (or no LOCAL space available): panel offset
                // directly in VIEW/app space, following the headset.
                quad.pose = offsetPose;
            }

            quad.size.width = panel.widthMeters;
            quad.size.height = panel.heightMeters;

            if (panel.hasHousing && !panel.isVisor && quad.space != XR_NULL_HANDLE)
            {
                // Wrap the actually visible content (e.g. a SimHub overlay
                // drawn only in part of its transparent surface), not the
                // full quad.
                XrPosef screenPose = quad.pose;
                float screenWidth = panel.widthMeters;
                float screenHeight = panel.heightMeters;
                const ContentBounds::Rect content = ContentBounds::Get(key);
                if (content.valid)
                {
                    const float cu = (content.u0 + content.u1) * 0.5f - 0.5f;
                    const float cv = 0.5f - (content.v0 + content.v1) * 0.5f;
                    const XrVector3f shift = QuatRotateVector(quad.pose.orientation,
                        XrVector3f{ cu * panel.widthMeters, cv * panel.heightMeters, 0.0f });
                    screenPose.position = { quad.pose.position.x + shift.x, quad.pose.position.y + shift.y, quad.pose.position.z + shift.z };
                    screenWidth = (content.u1 - content.u0) * panel.widthMeters;
                    screenHeight = (content.v1 - content.v0) * panel.heightMeters;
                }

                if (panel.housing.renderMode == 1 && g_d3dDevice != nullptr && g_d3dContext != nullptr)
                {
                    // Mesh mode: housing goes into a shared projection layer
                    // in the same space as the content quad.
                    MeshHousing mesh{};
                    mesh.space = quad.space;
                    mesh.instance.key = key;
                    mesh.instance.pose = screenPose;
                    mesh.instance.widthMeters = screenWidth;
                    mesh.instance.heightMeters = screenHeight;
                    mesh.instance.params = panel.housing;
                    mesh.instance.profile = &panel.housingProfile;
                    mesh.pixelsPerMeter = panel.widthMeters > 0.001f
                        ? static_cast<float>(panel.frameWidth) / panel.widthMeters
                        : 2048.0f;
                    meshHousings.push_back(std::move(mesh));
                    meshRenderScale = (std::max)(meshRenderScale, std::clamp(panel.housing.renderScale, 0.5f, 2.0f));
                    quads.push_back(quad);
                    anyHousedPanel = true;
                    continue;
                }

                // The panel content stays its own, unmodified quad (full
                // sharpness and runtime reprojection); the housing is built
                // from extra flat quads around it.
                housingBack.clear();
                housingFront.clear();
                XrPosef headPose{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
                if (quad.space == localSpace && headInLocalValid)
                {
                    headPose = headInLocal;
                }
                else if (quad.space == localSpace)
                {
                    // Fallback: assume the viewer looks straight at the panel.
                    const XrVector3f front = QuatRotateVector(screenPose.orientation, XrVector3f{ 0.0f, 0.0f, 0.6f });
                    headPose.position = { screenPose.position.x + front.x, screenPose.position.y + front.y, screenPose.position.z + front.z };
                }
                BuildHousingQuads(session, key, quad, screenPose, screenWidth, screenHeight,
                    panel.housing, housingLighting, headPose, housingBack, housingFront);
                quads.insert(quads.end(), housingBack.begin(), housingBack.end());
                quads.push_back(quad);
                quads.insert(quads.end(), housingFront.begin(), housingFront.end());
                anyHousedPanel = true;
                continue;
            }

            quads.push_back(quad);
        }

        // Controller marker + laser visualization quads (see
        // ControllerInput::GetVisuals). Poses are already expressed in
        // g_localSpace (ControllerInput::Update is called with localSpace
        // below), matching the panels' own world-locked space.
        if (g_localSpace != XR_NULL_HANDLE)
        {
            ControllerInput::ControllerVisual visuals[2];
            ControllerInput::GetVisuals(visuals);

            static const char* kHandKeys[2] = { "ctrl_marker_left", "ctrl_marker_right" };
            static const char* kLaserKeys[2] = { "ctrl_laser_left", "ctrl_laser_right" };
            static const char* kGrabHighlightKeys[2] = { "ctrl_grab_highlight_left", "ctrl_grab_highlight_right" };
            static const char* kHitCursorKeys[2] = { "ctrl_hit_cursor_left", "ctrl_hit_cursor_right" };

            for (int hand = 0; hand < 2; ++hand)
            {
                const auto& visual = visuals[hand];
                if (!visual.visible)
                {
                    continue;
                }

                if (PanelSwapchain* markerSwapchain = EnsureVisualSwapchain(session, kHandKeys[hand], visual.markerColor))
                {
                    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space = g_localSpace;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = markerSwapchain->swapchain;
                    quad.subImage.imageRect.offset = { 0, 0 };
                    quad.subImage.imageRect.extent = { markerSwapchain->width, markerSwapchain->height };
                    quad.pose = visual.markerPose;
                    quad.size.width = visual.markerSizeMeters;
                    quad.size.height = visual.markerSizeMeters;
                    quads.push_back(quad);
                }

                if (visual.laserLengthMeters > 0.0f)
                {
                    if (PanelSwapchain* laserSwapchain = EnsureVisualSwapchain(session, kLaserKeys[hand], visual.laserColor))
                    {
                        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        quad.space = g_localSpace;
                        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        quad.subImage.swapchain = laserSwapchain->swapchain;
                        quad.subImage.imageRect.offset = { 0, 0 };
                        quad.subImage.imageRect.extent = { laserSwapchain->width, laserSwapchain->height };
                        quad.pose = visual.laserPose;
                        quad.size.width = visual.laserWidthMeters;
                        quad.size.height = visual.laserLengthMeters;
                        quads.push_back(quad);
                    }
                }

                if (visual.grabHighlightVisible)
                {
                    static const ControllerInput::VisualColor kGrabHighlightColor{ 1.0f, 0.85f, 0.1f, 0.9f };
                    if (PanelSwapchain* frameSwapchain = EnsureFrameSwapchain(session, kGrabHighlightKeys[hand], kGrabHighlightColor))
                    {
                        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        quad.space = g_localSpace;
                        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        quad.subImage.swapchain = frameSwapchain->swapchain;
                        quad.subImage.imageRect.offset = { 0, 0 };
                        quad.subImage.imageRect.extent = { frameSwapchain->width, frameSwapchain->height };
                        quad.pose = visual.grabHighlightPose;
                        quad.size.width = visual.grabHighlightWidthMeters;
                        quad.size.height = visual.grabHighlightHeightMeters;
                        quads.push_back(quad);
                    }
                }

                if (visual.hitCursorVisible)
                {
                    if (PanelSwapchain* cursorSwapchain = EnsureVisualSwapchain(session, kHitCursorKeys[hand], visual.hitCursorColor))
                    {
                        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        quad.space = g_localSpace;
                        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        quad.subImage.swapchain = cursorSwapchain->swapchain;
                        quad.subImage.imageRect.offset = { 0, 0 };
                        quad.subImage.imageRect.extent = { cursorSwapchain->width, cursorSwapchain->height };
                        quad.pose = visual.hitCursorPose;
                        quad.size.width = visual.hitCursorSizeMeters;
                        quad.size.height = visual.hitCursorSizeMeters;
                        quads.push_back(quad);
                    }
                }
            }
        }

        // Procedural helmet visor (see VisorRenderer.h): rendered per eye
        // with the app's own projection views, then submitted on top of
        // everything as an extra projection layer, since a real visor is
        // the closest thing to the driver's eyes.
        constexpr uint32_t kMaxVisorViews = 4;
        XrCompositionLayerProjection visorLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerProjectionView visorViews[kMaxVisorViews]{};
        bool hasVisorLayer = false;
        {
            const IpcServer::PanelState* visorPanel = nullptr;
            for (const auto& panel : panels)
            {
                if (panel.isVisor)
                {
                    visorPanel = &panel;
                    break;
                }
            }

            // Animated hinge: enabled = closed (0), disabled = open (1).
            // Eased over ~0.25 s; when open the raised glass edge stays visible at the top.
            static float s_visorOpen = 1.0f;
            static LARGE_INTEGER s_visorLastTick{};
            {
                LARGE_INTEGER now{};
                LARGE_INTEGER freq{};
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&freq);
                float dt = s_visorLastTick.QuadPart != 0
                    ? static_cast<float>(now.QuadPart - s_visorLastTick.QuadPart) / static_cast<float>(freq.QuadPart)
                    : 0.0f;
                s_visorLastTick = now;
                dt = std::clamp(dt, 0.0f, 0.1f);
                const float target = (visorPanel != nullptr && visorPanel->enabled) ? 0.0f : 1.0f;
                const float step = dt / 0.25f;
                s_visorOpen = s_visorOpen < target
                    ? (std::min)(target, s_visorOpen + step)
                    : (std::max)(target, s_visorOpen - step);
            }
            const float visorLinear = s_visorOpen;
            const float visorEased = visorLinear * visorLinear * (3.0f - 2.0f * visorLinear);

            const XrCompositionLayerProjection* appProjection = nullptr;
            for (uint32_t i = 0; visorPanel != nullptr && i < frameEndInfo->layerCount; ++i)
            {
                const auto* layer = frameEndInfo->layers[i];
                if (layer != nullptr && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                {
                    appProjection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                    break;
                }
            }

            if (visorPanel != nullptr && (s_frameLogCounter % 300) == 1)
            {
                Logging::Log("Hook_xrEndFrame: visor panel present (appProjection=" +
                    std::to_string(appProjection != nullptr) +
                    ", viewCount=" + std::to_string(appProjection != nullptr ? appProjection->viewCount : 0) +
                    ", device=" + std::to_string(g_d3dDevice != nullptr) +
                    ", sunOpacity=" + std::to_string(visorPanel->visor.sunOpacity) +
                    ", tintOpacity=" + std::to_string(visorPanel->visor.tintOpacity) + ").");
            }

            if (visorPanel != nullptr && appProjection != nullptr && appProjection->viewCount >= 2 &&
                appProjection->viewCount <= kMaxVisorViews && g_d3dDevice != nullptr && g_d3dContext != nullptr)
            {
                const auto& left = appProjection->views[0].pose;
                const auto& right = appProjection->views[1].pose;

                // Head = midpoint between the eyes, orientation halfway
                // between both eyes (handles canted displays).
                XrPosef headPose{};
                headPose.position = {
                    (left.position.x + right.position.x) * 0.5f,
                    (left.position.y + right.position.y) * 0.5f,
                    (left.position.z + right.position.z) * 0.5f };
                XrQuaternionf r = right.orientation;
                const XrQuaternionf& l = left.orientation;
                if (l.x * r.x + l.y * r.y + l.z * r.z + l.w * r.w < 0.0f)
                {
                    r = { -r.x, -r.y, -r.z, -r.w };
                }
                XrQuaternionf mid{ l.x + r.x, l.y + r.y, l.z + r.z, l.w + r.w };
                const float len = sqrtf(mid.x * mid.x + mid.y * mid.y + mid.z * mid.z + mid.w * mid.w);
                headPose.orientation = len > 0.0f
                    ? XrQuaternionf{ mid.x / len, mid.y / len, mid.z / len, mid.w / len }
                    : l;

                bool allViewsOk = true;
                for (uint32_t v = 0; v < appProjection->viewCount && allViewsOk; ++v)
                {
                    const auto& appView = appProjection->views[v];

                    // Half the app's eye resolution is plenty for the
                    // mostly smooth visor and keeps the cost negligible.
                    const int32_t w = std::clamp(appView.subImage.imageRect.extent.width / 2, 64, 2048);
                    const int32_t h = std::clamp(appView.subImage.imageRect.extent.height / 2, 64, 2048);

                    const std::string key = "visor_view_" + std::to_string(v);
                    PanelSwapchain* swapchain = EnsurePanelSwapchain(session, key, w, h);
                    if (swapchain == nullptr || swapchain->images.empty())
                    {
                        allViewsOk = false;
                        break;
                    }

                    uint32_t imageIndex = 0;
                    if (XR_FAILED(acquireImage(swapchain->swapchain, nullptr, &imageIndex)))
                    {
                        allViewsOk = false;
                        break;
                    }

                    XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                    waitInfo.timeout = XR_INFINITE_DURATION;
                    if (XR_SUCCEEDED(waitImage(swapchain->swapchain, &waitInfo)))
                    {
                        allViewsOk = VisorRenderer::RenderEye(
                            g_d3dDevice, g_d3dContext, swapchain->images[imageIndex],
                            static_cast<DXGI_FORMAT>(swapchain->format),
                            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                            visorPanel->visor, headPose, appView.pose, appView.fov, visorEased);
                    }
                    else
                    {
                        allViewsOk = false;
                    }

                    XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    releaseImage(swapchain->swapchain, &releaseInfo);

                    visorViews[v] = XrCompositionLayerProjectionView{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                    visorViews[v].pose = appView.pose;
                    visorViews[v].fov = appView.fov;
                    visorViews[v].subImage.swapchain = swapchain->swapchain;
                    visorViews[v].subImage.imageRect.offset = { 0, 0 };
                    visorViews[v].subImage.imageRect.extent = { w, h };
                    visorViews[v].subImage.imageArrayIndex = 0;
                }

                if (allViewsOk)
                {
                    visorLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    visorLayer.space = appProjection->space;
                    visorLayer.viewCount = appProjection->viewCount;
                    visorLayer.views = visorViews;
                    hasVisorLayer = true;
                }
                else if ((s_frameLogCounter % 90) == 1)
                {
                    Logging::Log("Hook_xrEndFrame: visor rendering failed for at least one view, skipping visor layer.");
                }
            }
        }

		// Mesh housings: rendered per eye with an off-axis projection whose
		// image plane is the panel's own front plane, then submitted as two
		// eye-specific quad layers with exactly the panel's pose/space/size.
		// The compositor therefore treats housing and content identically
		// (same layer type, same space, same reprojection) - no dependence
		// on how the runtime composites a second projection layer.
		constexpr uint32_t kHousingViews = 2;
		// Distance of the housing quad's image plane behind the panel front.
		constexpr float kHousingPlaneOffset = 0.005f;
		std::vector<XrCompositionLayerQuad> meshQuads;
		if (!meshHousings.empty())
		{
			PFN_xrLocateViews locateViews;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				locateViews = g_next.xrLocateViews;
			}

			const XrSpace housingSpace = meshHousings.front().space;
			XrView locatedViews[kHousingViews];
			for (auto& lv : locatedViews) lv = XrView{ XR_TYPE_VIEW };
			bool locatedOk = false;
			if (locateViews != nullptr)
			{
				XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
				locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				locateInfo.displayTime = frameEndInfo->displayTime;
				locateInfo.space = housingSpace;
				XrViewState viewState{ XR_TYPE_VIEW_STATE };
				uint32_t located = 0;
				constexpr XrViewStateFlags kViewValid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
				locatedOk = XR_SUCCEEDED(locateViews(session, &locateInfo, &viewState, kHousingViews, &located, locatedViews)) &&
					located == kHousingViews && (viewState.viewStateFlags & kViewValid) == kViewValid;
			}

			if (locatedOk)
			{
				XrVector3f headCentre{ 0.0f, 0.0f, 0.0f };
				for (const auto& lv : locatedViews)
				{
					headCentre.x += lv.pose.position.x * 0.5f;
					headCentre.y += lv.pose.position.y * 0.5f;
					headCentre.z += lv.pose.position.z * 0.5f;
				}

				meshQuads.reserve(meshHousings.size() * kHousingViews);
				for (const auto& mesh : meshHousings)
				{
					if (mesh.space != housingSpace)
					{
						continue;
					}
					const auto& inst = mesh.instance;
					const XrQuaternionf invRot{ -inst.pose.orientation.x, -inst.pose.orientation.y,
						-inst.pose.orientation.z, inst.pose.orientation.w };
					const auto toPanel = [&](const XrVector3f& p) {
						return QuatRotateVector(invRot, XrVector3f{ p.x - inst.pose.position.x,
							p.y - inst.pose.position.y, p.z - inst.pose.position.z });
					};

					// Quad large enough to also contain the perspective
					// footprint of the housing's back part.
					const float bezel = std::clamp(inst.params.bezelWidthMm, 1.0f, 100.0f) * 0.001f;
					const float depth = std::clamp(inst.params.depthMm, 2.0f, 200.0f) * 0.001f;
					const float margin = bezel + depth * 1.5f;
					const float quadW = inst.widthMeters + 2.0f * margin;
					const float quadH = inst.heightMeters + 2.0f * margin;
					const int32_t w = std::clamp(static_cast<int32_t>(quadW * mesh.pixelsPerMeter * meshRenderScale), 64, 4096);
					const int32_t h = std::clamp(static_cast<int32_t>(quadH * mesh.pixelsPerMeter * meshRenderScale), 64, 4096);

					// Render in the panel's own frame (panel pose = identity).
					std::vector<HousingRenderer::Instance> single(1, inst);
					single.front().pose = XrPosef{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
					const XrVector3f headLocal = toPanel(headCentre);

					XrCompositionLayerQuad eyeQuads[kHousingViews];
					bool ok = true;
					for (uint32_t v = 0; v < kHousingViews && ok; ++v)
					{
						const XrVector3f eye = toPanel(locatedViews[v].pose.position);
						// Image plane sits a bit behind the panel front so the
						// housing quad never shares the content quad's plane
						// (compositor z-fighting -> flicker).
						const float planeZ = -kHousingPlaneOffset;
						const float dz = eye.z - planeZ;
						if (dz < 0.03f)
						{
							// Eye (almost) in or behind the panel plane: no
							// sensible planar projection.
							ok = false;
							break;
						}
						// Off-axis frustum through the quad rectangle at z = planeZ.
						XrFovf fov{};
						fov.angleLeft = atanf((-quadW * 0.5f - eye.x) / dz);
						fov.angleRight = atanf((quadW * 0.5f - eye.x) / dz);
						fov.angleUp = atanf((quadH * 0.5f - eye.y) / dz);
						fov.angleDown = atanf((-quadH * 0.5f - eye.y) / dz);
						const XrPosef eyePose{ { 0.0f, 0.0f, 0.0f, 1.0f }, eye };

						PanelSwapchain* color = EnsurePanelSwapchain(session, inst.key + (v == 0 ? "_housingL" : "_housingR"), w, h);
						if (color == nullptr || color->images.empty())
						{
							ok = false;
							break;
						}
						uint32_t colorIndex = 0;
						if (XR_FAILED(acquireImage(color->swapchain, nullptr, &colorIndex)))
						{
							ok = false;
							break;
						}
						XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
						waitInfo.timeout = XR_INFINITE_DURATION;
						const bool colorReady = XR_SUCCEEDED(waitImage(color->swapchain, &waitInfo));

						ok = colorReady && HousingRenderer::RenderEye(
							g_d3dDevice, g_d3dContext, color->images[colorIndex],
							static_cast<DXGI_FORMAT>(color->format),
							static_cast<uint32_t>(w), static_cast<uint32_t>(h),
							eyePose, fov, headLocal, single, housingLighting, nullptr);

						XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
						releaseImage(color->swapchain, &releaseInfo);

						XrCompositionLayerQuad& q = eyeQuads[v];
						q = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
						q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
						q.space = mesh.space;
						q.eyeVisibility = v == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
						q.subImage.swapchain = color->swapchain;
						q.subImage.imageRect.offset = { 0, 0 };
						q.subImage.imageRect.extent = { w, h };
						q.subImage.imageArrayIndex = 0;
						q.pose = inst.pose;
						const XrVector3f back = QuatRotateVector(inst.pose.orientation, XrVector3f{ 0.0f, 0.0f, planeZ });
						q.pose.position = { inst.pose.position.x + back.x, inst.pose.position.y + back.y, inst.pose.position.z + back.z };
						q.size.width = quadW;
						q.size.height = quadH;
					}

					if (ok)
					{
						meshQuads.push_back(eyeQuads[0]);
						meshQuads.push_back(eyeQuads[1]);
					}
					else if ((s_frameLogCounter % 90) == 1)
					{
						Logging::Log("Hook_xrEndFrame: mesh housing skipped for one panel (eye behind panel plane or swapchain/render failure).");
					}
				}

				if ((s_frameLogCounter % 300) == 1 && !meshQuads.empty())
				{
					const auto& q = meshQuads.front();
					Logging::Log("Hook_xrEndFrame: mesh housing quads=" + std::to_string(meshQuads.size()) +
						" size=" + std::to_string(q.size.width) + "x" + std::to_string(q.size.height) +
						" tex=" + std::to_string(q.subImage.imageRect.extent.width) + "x" + std::to_string(q.subImage.imageRect.extent.height) +
						" eyeL=(" + std::to_string(locatedViews[0].pose.position.x) + ", " + std::to_string(locatedViews[0].pose.position.y) + ", " +
						std::to_string(locatedViews[0].pose.position.z) + ").");
				}
			}
			else if ((s_frameLogCounter % 90) == 1)
			{
				Logging::Log("Hook_xrEndFrame: mesh housing skipped (xrLocateViews in panel space failed).");
			}
		}

        // Feed the image light probe (app's first eye image) that shades the
        // housing faces; the result is used from the next frame on.
        if (anyHousedPanel && g_d3dDevice != nullptr && g_d3dContext != nullptr)
        {
            for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
            {
                const auto* layer = frameEndInfo->layers[i];
                if (layer != nullptr && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                {
                    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                    if (projection->viewCount >= 1)
                    {
                        const auto& sub = projection->views[0].subImage;
                        ID3D11Texture2D* appTexture = nullptr;
                        int64_t appFormat = 0;
                        if (TryGetAppSwapchainImage(sub.swapchain, &appTexture, &appFormat))
                        {
                            donutz::LightProbe::Update(g_d3dDevice, g_d3dContext, appTexture, sub.imageArrayIndex, appFormat);
                        }
                    }
                    break;
                }
            }
        }

        combinedLayers.reserve(frameEndInfo->layerCount + quads.size() + meshQuads.size() + 2);
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
        {
            combinedLayers.push_back(frameEndInfo->layers[i]);
        }
        for (const auto& quad : meshQuads)
        {
            combinedLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }
        for (const auto& quad : quads)
        {
            combinedLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }
        if (hasVisorLayer)
        {
            combinedLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&visorLayer));
        }

        XrFrameEndInfo combinedFrameEndInfo = *frameEndInfo;
        combinedFrameEndInfo.layerCount = static_cast<uint32_t>(combinedLayers.size());
        if ((s_frameLogCounter % 180) == 2)
        {
            Logging::Log("Hook_xrEndFrame: submitting " + std::to_string(combinedLayers.size()) + " layers (" + std::to_string(quads.size()) + " quads).");
        }
        combinedFrameEndInfo.layers = combinedLayers.data();

        const XrResult endResult = next(session, &combinedFrameEndInfo);
        static int s_endFailLogs = 0;
        if (XR_FAILED(endResult) && s_endFailLogs < 20)
        {
            ++s_endFailLogs;
            Logging::Log("Hook_xrEndFrame: runtime rejected " + std::to_string(combinedLayers.size()) + " layers, XrResult=" + std::to_string(endResult) + ".");
        }
        if (endResult == XR_ERROR_LAYER_LIMIT_EXCEEDED)
        {
            // Never freeze the headset: fall back to the app's own layers.
            return next(session, frameEndInfo);
        }
        return endResult;
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain)
    {
        PFN_xrCreateSwapchain next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrCreateSwapchain;
        }

        const XrResult result = next(session, createInfo, swapchain);
        if (XR_SUCCEEDED(result) && createInfo != nullptr && swapchain != nullptr &&
            (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0)
        {
            AppSwapchain info;
            info.format = createInfo->format;
            info.width = createInfo->width;
            info.height = createInfo->height;
            info.arraySize = createInfo->arraySize;
            info.sampleCount = createInfo->sampleCount;
            std::lock_guard<std::mutex> lock(g_appSwapchainMutex);
            g_appSwapchains[*swapchain] = std::move(info);
        }
        return result;
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain swapchain)
    {
        PFN_xrDestroySwapchain next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrDestroySwapchain;
        }

        {
            std::lock_guard<std::mutex> lock(g_appSwapchainMutex);
            g_appSwapchains.erase(swapchain);
        }
        return next(swapchain);
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrEnumerateSwapchainImages(
        XrSwapchain swapchain, uint32_t capacity, uint32_t* countOutput, XrSwapchainImageBaseHeader* images)
    {
        PFN_xrEnumerateSwapchainImages next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrEnumerateSwapchainImages;
        }

        const XrResult result = next(swapchain, capacity, countOutput, images);
        if (XR_SUCCEEDED(result) && capacity > 0 && images != nullptr && countOutput != nullptr &&
            images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)
        {
            std::lock_guard<std::mutex> lock(g_appSwapchainMutex);
            auto it = g_appSwapchains.find(swapchain);
            if (it != g_appSwapchains.end())
            {
                const auto* d3dImages = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
                it->second.images.assign(*countOutput, nullptr);
                for (uint32_t i = 0; i < *countOutput; ++i)
                {
                    it->second.images[i] = d3dImages[i].texture;
                }
            }
        }
        return result;
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(
        XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index)
    {
        PFN_xrAcquireSwapchainImage next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrAcquireSwapchainImage;
        }

        const XrResult result = next(swapchain, acquireInfo, index);
        if (XR_SUCCEEDED(result) && index != nullptr)
        {
            std::lock_guard<std::mutex> lock(g_appSwapchainMutex);
            auto it = g_appSwapchains.find(swapchain);
            if (it != g_appSwapchains.end())
            {
                it->second.lastAcquiredIndex = *index;
            }
        }
        return result;
    }

    // Returns the D3D11 texture the app most recently acquired (= the image
    // it just rendered and submitted) for one of its own swapchains.
    bool TryGetAppSwapchainImage(XrSwapchain swapchain, ID3D11Texture2D** texture, int64_t* format)
    {
        std::lock_guard<std::mutex> lock(g_appSwapchainMutex);
        auto it = g_appSwapchains.find(swapchain);
        if (it == g_appSwapchains.end() || it->second.lastAcquiredIndex >= it->second.images.size())
        {
            return false;
        }
        *texture = it->second.images[it->second.lastAcquiredIndex];
        *format = it->second.format;
        return *texture != nullptr;
    }

    XRAPI_ATTR XrResult XRAPI_CALL Hook_xrAttachSessionActionSets(
        XrSession session,
        const XrSessionActionSetsAttachInfo* attachInfo)
    {
        PFN_xrAttachSessionActionSets next;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            next = g_next.xrAttachSessionActionSets;
        }

        if (next == nullptr)
        {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        return ControllerInput::OnAttachSessionActionSets(next, session, attachInfo);
    }
}

namespace HookedFunctions
{
    void OnInstanceCreated(XrInstance instance, PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        nextGetInstanceProcAddr(instance, "xrCreateSession", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrCreateSession));
        nextGetInstanceProcAddr(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrDestroySession));
        nextGetInstanceProcAddr(instance, "xrEndFrame", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrEndFrame));
        nextGetInstanceProcAddr(instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrCreateSwapchain));
        nextGetInstanceProcAddr(instance, "xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrDestroySwapchain));
        nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrEnumerateSwapchainImages));
        nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrEnumerateSwapchainFormats));
        nextGetInstanceProcAddr(instance, "xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrAcquireSwapchainImage));
        nextGetInstanceProcAddr(instance, "xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrWaitSwapchainImage));
        nextGetInstanceProcAddr(instance, "xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrReleaseSwapchainImage));
        nextGetInstanceProcAddr(instance, "xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrCreateReferenceSpace));
        nextGetInstanceProcAddr(instance, "xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrDestroySpace));
        nextGetInstanceProcAddr(instance, "xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrLocateSpace));
        nextGetInstanceProcAddr(instance, "xrLocateViews", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrLocateViews));
        nextGetInstanceProcAddr(instance, "xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction*>(&g_next.xrAttachSessionActionSets));

        Logging::Log("OnInstanceCreated: hooks installed, starting IPC server.");

        // Start (or ensure) the IPC server that the WPF configuration app
        // connects to in order to push panel transforms and frame data.
        IpcServer::EnsureStarted();

        ControllerInput::OnInstanceCreated(instance, nextGetInstanceProcAddr);
    }

    bool TryGetHooked(const char* name, PFN_xrVoidFunction* function)
    {
        if (std::strcmp(name, "xrCreateSession") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateSession);
            return true;
        }
        if (std::strcmp(name, "xrDestroySession") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrDestroySession);
            return true;
        }
        if (std::strcmp(name, "xrEndFrame") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrEndFrame);
            return true;
        }
        if (std::strcmp(name, "xrCreateSwapchain") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrCreateSwapchain);
            return true;
        }
        if (std::strcmp(name, "xrDestroySwapchain") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrDestroySwapchain);
            return true;
        }
        if (std::strcmp(name, "xrEnumerateSwapchainImages") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrEnumerateSwapchainImages);
            return true;
        }
        if (std::strcmp(name, "xrAcquireSwapchainImage") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrAcquireSwapchainImage);
            return true;
        }
        if (std::strcmp(name, "xrAttachSessionActionSets") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrAttachSessionActionSets);
            return true;
        }

        return false;
    }

    void SetDepthLayerSupported(bool supported)
    {
        g_depthLayerSupported = supported;
    }

    XrPosef GetAnchorPose(bool* hasAnchor)
    {
        std::lock_guard<std::mutex> lock(g_anchorMutex);
        if (hasAnchor != nullptr)
        {
            *hasAnchor = g_hasAnchor;
        }
        return g_anchorPose;
    }
}
