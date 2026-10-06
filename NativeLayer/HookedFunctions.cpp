#include "HookedFunctions.h"
#include "ControllerInput.h"
#include "IpcServer.h"
#include "Logging.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
        PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
    };

    // Per-panel overlay swapchain, created lazily against the host's D3D11
    // device once a panel becomes enabled and sized.
    struct PanelSwapchain
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
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

    std::mutex g_swapchainMutex;
    std::unordered_map<std::string, PanelSwapchain> g_panelSwapchains;

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
    PanelSwapchain* EnsurePanelSwapchain(XrSession session, const std::string& key, int32_t width, int32_t height)
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
        if (enumerateFormats != nullptr)
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
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
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

    // Paints a transparent-center, solid-colored picture-frame border (same
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
        quads.reserve(panels.size());

        for (const auto& panel : panels)
        {
            if (!panel.enabled || !panel.hasFrame || panel.frameWidth <= 0 || panel.frameHeight <= 0)
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

        combinedLayers.reserve(frameEndInfo->layerCount + quads.size());
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
        {
            combinedLayers.push_back(frameEndInfo->layers[i]);
        }
        for (const auto& quad : quads)
        {
            combinedLayers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }

        XrFrameEndInfo combinedFrameEndInfo = *frameEndInfo;
        combinedFrameEndInfo.layerCount = static_cast<uint32_t>(combinedLayers.size());
        combinedFrameEndInfo.layers = combinedLayers.data();

        return next(session, &combinedFrameEndInfo);
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
        if (std::strcmp(name, "xrAttachSessionActionSets") == 0)
        {
            *function = reinterpret_cast<PFN_xrVoidFunction>(&Hook_xrAttachSessionActionSets);
            return true;
        }

        return false;
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
