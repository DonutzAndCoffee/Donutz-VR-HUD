using System.Runtime.InteropServices;
using CefSharp;
using CefSharp.OffScreen;
using CefSharp.Structs;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.DXGI;
using InteropUnmanagedType = System.Runtime.InteropServices.UnmanagedType;

namespace Donutz_VR_HUD.Sources
{
    /// <summary>
    /// Renders a web-based dashboard (e.g. a SimHub URL dashboard) off-screen via
    /// CefSharp/CEF (Chromium Embedded Framework) and streams it into a D3D11
    /// texture, the same way OpenKneeboard's ChromiumPageSource does: CEF is run
    /// windowless (Off-Screen Rendering, OSR) with GPU shared-texture output
    /// enabled, so frames never touch a real, DWM-composited window and are
    /// copied GPU-to-GPU via <see cref="IRenderHandler.OnAcceleratedPaint"/>
    /// instead of round-tripping through a CPU bitmap/PNG capture like the
    /// previous WebView2-based implementation did. This avoids the occlusion/
    /// throttling/DWM-cloaking issues that caused black frames, static images
    /// and white "transparent" backgrounds with WebView2.
    /// </summary>
    public sealed class WebDashboardSource : IOverlaySource
    {
        [DllImport("d3d11.dll")]
        private static extern int D3D11CreateDevice(
            IntPtr pAdapter,
            int driverType,
            IntPtr software,
            uint flags,
            IntPtr pFeatureLevels,
            uint featureLevels,
            uint sdkVersion,
            out IntPtr ppDevice,
            out int pFeatureLevel,
            out IntPtr ppImmediateContext);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(InteropUnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr hObject);

        private const int D3D_DRIVER_TYPE_HARDWARE = 1;
        private const uint D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
        private const uint D3D11_SDK_VERSION = 7;

        private readonly D3D11 _d3d11 = D3D11.GetApi();

        private nint _devicePtr;
        private nint _contextPtr;
        private ComPtr<ID3D11Device> _device;
        private ComPtr<ID3D11Device1> _device1;
        private ComPtr<ID3D11DeviceContext> _context;
        private ComPtr<ID3D11Texture2D> _texture;
        private nint _sharedHandle;
        private string _sharedHandleName = string.Empty;

        private int _width;
        private int _height;

        private ChromiumWebBrowser? _browser;
        private CefRenderHandler? _renderHandler;

        public nint DevicePtr => _devicePtr;
        public nint ContextPtr => _contextPtr;
        public event Action<string, int, int>? FrameArrived;

        /// <summary>
        /// Starts rendering the given URL off-screen at the given pixel resolution
        /// via CEF/CefSharp windowless (OSR) rendering and begins streaming
        /// frames via <see cref="FrameArrived"/>.
        /// </summary>
        public Task StartAsync(string url, int width = 1024, int height = 1024)
        {
            _width = width;
            _height = height;

            CreateDeviceAndTexture();

            var windowInfo = CefSharp.WindowInfo.Create();
            windowInfo.SetAsWindowless(IntPtr.Zero);
            windowInfo.SharedTextureEnabled = true;

            var browserSettings = new BrowserSettings
            {
                WindowlessFrameRate = 30,
                BackgroundColor = 0 // fully transparent (AARRGGBB), like OpenKneeboard's browser tabs
            };

            _renderHandler = new CefRenderHandler(this, width, height);

            _browser = new ChromiumWebBrowser(
                address: url,
                browserSettings: browserSettings,
                requestContext: null,
                automaticallyCreateBrowser: false)
            {
                RenderHandler = _renderHandler
            };

            _browser.CreateBrowser(windowInfo, browserSettings);

            return Task.CompletedTask;
        }

        /// <summary>Navigates the dashboard to a different URL without restarting the source.</summary>
        public Task NavigateAsync(string url)
        {
            _browser?.Load(url);
            return Task.CompletedTask;
        }

        /// <summary>Forces a reload of the current dashboard URL.</summary>
        public void Reload() => _browser?.Reload();

        /// <summary>
        /// Raises <see cref="FrameArrived"/> after a CEF-rendered frame has
        /// been copied into the shared D3D11 texture exposed to the native
        /// OpenXR layer.
        /// </summary>
        private void OnFrameCopied()
        {
            FrameArrived?.Invoke(_sharedHandleName, _width, _height);
        }

        /// <summary>
        /// Copies raw BGRA pixels (the <see cref="IRenderHandler.OnPaint"/>
        /// fallback path, used when the GPU shared-texture path is
        /// unavailable) into the shared texture via UpdateSubresource.
        /// </summary>
        private unsafe void CopyPixelsToTexture(IntPtr buffer, int width, int height)
        {
            var stride = width * 4;
            _context.UpdateSubresource(
                new ComPtr<ID3D11Resource>((ID3D11Resource*)_texture.Handle),
                0,
                null,
                (void*)buffer,
                (uint)stride,
                0);

            // Ensure the GPU write is actually submitted before the native
            // OpenXR layer (a different process) opens the shared handle and
            // copies from it - otherwise it can read stale/empty data even
            // though our copy above "succeeded".
            _context.Flush();

            OnFrameCopied();
        }

        /// <summary>
        /// Opens CEF's per-frame shared texture handle (see
        /// <see cref="AcceleratedPaintInfo.SharedTextureHandle"/>) with
        /// <c>ID3D11Device1::OpenSharedResource</c> and copies it GPU-to-GPU
        /// into our own shared texture via <c>CopyResource</c>, exactly like
        /// OpenKneeboard's ChromiumPageSource_RenderHandler::OnAcceleratedPaint.
        /// The incoming handle/resource must not be cached across calls - CEF
        /// recycles a pool of textures and hands back a possibly different
        /// handle on every frame.
        /// </summary>
        private unsafe void CopyAcceleratedTextureToTexture(IntPtr sharedTextureHandle)
        {
            if (_device1.Handle is null || sharedTextureHandle == IntPtr.Zero)
            {
                return;
            }

            ComPtr<ID3D11Texture2D> sourceTexture = default;
            var iid = ID3D11Texture2D.Guid;
            var hr = _device1.OpenSharedResource1((void*)sharedTextureHandle, &iid, (void**)sourceTexture.GetAddressOf());
            if (hr < 0)
            {
                return;
            }

            using (sourceTexture)
            {
                _context.CopyResource(
                    new ComPtr<ID3D11Resource>((ID3D11Resource*)_texture.Handle),
                    new ComPtr<ID3D11Resource>((ID3D11Resource*)sourceTexture.Handle));
            }

            // Ensure the GPU-to-GPU copy is actually submitted before the
            // native OpenXR layer (a different process) opens the shared
            // handle and copies from it - otherwise it can read stale/empty
            // data even though our copy above "succeeded".
            _context.Flush();

            OnFrameCopied();
        }

        private unsafe void CreateDeviceAndTexture()
        {
            var hr = D3D11CreateDevice(
                IntPtr.Zero,
                D3D_DRIVER_TYPE_HARDWARE,
                IntPtr.Zero,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                IntPtr.Zero,
                0,
                D3D11_SDK_VERSION,
                out _devicePtr,
                out _,
                out _contextPtr);

            if (hr < 0)
            {
                Marshal.ThrowExceptionForHR(hr);
            }

            _device = new ComPtr<ID3D11Device>((ID3D11Device*)_devicePtr);
            _context = new ComPtr<ID3D11DeviceContext>((ID3D11DeviceContext*)_contextPtr);
            _device1 = _device.QueryInterface<ID3D11Device1>();

            var textureDesc = new Texture2DDesc
            {
                Width = (uint)_width,
                Height = (uint)_height,
                MipLevels = 1,
                ArraySize = 1,
                Format = Format.FormatB8G8R8A8Unorm,
                SampleDesc = new Silk.NET.DXGI.SampleDesc(1, 0),
                Usage = Usage.Default,
                BindFlags = (uint)BindFlag.ShaderResource,
                CPUAccessFlags = 0,
                // The texture must be a shared NT-handle resource so its
                // underlying GPU memory (not just a same-process pointer)
                // can be opened by the native OpenXR layer running inside
                // the host application's (e.g. iRacing.exe) separate
                // process, via IDXGIResource1::CreateSharedHandle /
                // ID3D11Device1::OpenSharedResource1.
                MiscFlags = (uint)(ResourceMiscFlag.Shared | ResourceMiscFlag.SharedNthandle)
            };

            ComPtr<ID3D11Texture2D> texture = default;
            _device.CreateTexture2D(in textureDesc, null, ref texture).ThrowOnError();
            _texture = texture;

            CreateSharedHandle();
        }

        /// <summary>
        /// Obtains a cross-process NT shared handle for <see cref="_texture"/>
        /// via IDXGIResource1::CreateSharedHandle, so the native OpenXR layer
        /// (running inside the host VR application's process) can open the
        /// same underlying GPU resource with
        /// ID3D11Device1::OpenSharedResource1. A raw same-process pointer (as
        /// previously returned) is meaningless in another process.
        /// </summary>
        private unsafe void CreateSharedHandle()
        {
            if (_sharedHandle != IntPtr.Zero)
            {
                CloseHandle(_sharedHandle);
                _sharedHandle = IntPtr.Zero;
            }

            using var resource = _texture.QueryInterface<IDXGIResource1>();

            void* handle = null;
            // IDXGIResource1::CreateSharedHandle's dwAccess parameter expects
            // the DXGI_SHARED_RESOURCE_* flags (READ | WRITE), not a generic
            // Windows access right like GENERIC_ALL. Passing GENERIC_ALL here
            // caused the receiving process's OpenSharedResourceByName call to
            // fail with E_INVALIDARG.
            const uint DxgiSharedResourceRead = 0x80000000;
            const uint DxgiSharedResourceWrite = 0x00000001;
            const uint DxgiSharedResourceReadWrite = DxgiSharedResourceRead | DxgiSharedResourceWrite;
            SecurityAttributes* securityAttributes = null;
            _sharedHandleName = $"Local\\DonutzVrHud_{Guid.NewGuid():N}";
            fixed (char* name = _sharedHandleName)
            {
                resource.CreateSharedHandle(
                    securityAttributes,
                    DxgiSharedResourceReadWrite,
                    name,
                    &handle).ThrowOnError();
            }

            _sharedHandle = (nint)handle;
        }

        public void Dispose()
        {
            _browser?.Dispose();
            _browser = null;
            _renderHandler = null;

            DisposeTexture();

            DisposeDevice1();

            if (_contextPtr != IntPtr.Zero)
            {
                Marshal.Release(_contextPtr);
            }

            if (_devicePtr != IntPtr.Zero)
            {
                Marshal.Release(_devicePtr);
            }
        }

        private unsafe void DisposeTexture()
        {
            if (_sharedHandle != IntPtr.Zero)
            {
                CloseHandle(_sharedHandle);
                _sharedHandle = IntPtr.Zero;
            }

            if (_texture.Handle is not null)
            {
                _texture.Dispose();
            }
        }

        private unsafe void DisposeDevice1()
        {
            if (_device1.Handle is not null)
            {
                _device1.Dispose();
            }
        }

        /// <summary>
        /// <see cref="IRenderHandler"/> implementation bridging CEF's OSR
        /// paint callbacks into <see cref="WebDashboardSource"/>'s D3D11
        /// shared texture, mirroring OpenKneeboard's
        /// ChromiumPageSource_RenderHandler: the accelerated (GPU shared
        /// texture) path is preferred, with the CPU bitmap path
        /// (<see cref="OnPaint"/>) only used as a fallback for configurations
        /// where GPU shared-texture OSR is unavailable.
        /// </summary>
        private sealed class CefRenderHandler : IRenderHandler
        {
            private readonly WebDashboardSource _owner;
            private readonly int _width;
            private readonly int _height;

            public CefRenderHandler(WebDashboardSource owner, int width, int height)
            {
                _owner = owner;
                _width = width;
                _height = height;
            }

            public ScreenInfo? GetScreenInfo() => new ScreenInfo { DeviceScaleFactor = 1.0F };

            public Rect GetViewRect() => new(0, 0, _width, _height);

            public bool GetScreenPoint(int viewX, int viewY, out int screenX, out int screenY)
            {
                screenX = viewX;
                screenY = viewY;
                return false;
            }

            public void OnAcceleratedPaint(PaintElementType type, Rect dirtyRect, AcceleratedPaintInfo acceleratedPaintInfo)
            {
                if (type != PaintElementType.View)
                {
                    return;
                }

                _owner.CopyAcceleratedTextureToTexture(acceleratedPaintInfo.SharedTextureHandle);
            }

            public void OnPaint(PaintElementType type, Rect dirtyRect, IntPtr buffer, int width, int height)
            {
                if (type != PaintElementType.View)
                {
                    return;
                }

                _owner.CopyPixelsToTexture(buffer, width, height);
            }

            public void OnCursorChange(IntPtr cursor, CefSharp.Enums.CursorType type, CursorInfo customCursorInfo)
            {
            }

            public bool StartDragging(IDragData dragData, CefSharp.Enums.DragOperationsMask mask, int x, int y) => false;

            public void UpdateDragCursor(CefSharp.Enums.DragOperationsMask operation)
            {
            }

            public void OnPopupShow(bool show)
            {
            }

            public void OnPopupSize(Rect rect)
            {
            }

            public void OnImeCompositionRangeChanged(CefSharp.Structs.Range selectedRange, Rect[] characterBounds)
            {
            }

            public void OnVirtualKeyboardRequested(IBrowser browser, CefSharp.Enums.TextInputMode inputMode)
            {
            }

            public void Dispose()
            {
            }
        }
    }

    internal static class HResultExtensions
    {
        public static void ThrowOnError(this int hr)
        {
            if (hr < 0)
            {
                Marshal.ThrowExceptionForHR(hr);
            }
        }
    }
}
