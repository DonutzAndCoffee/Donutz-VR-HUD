using System.Runtime.InteropServices;
using System.Windows.Threading;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.DXGI;
using InteropUnmanagedType = System.Runtime.InteropServices.UnmanagedType;

namespace Donutz_VR_HUD.Sources
{
    /// <summary>
    /// Renders a simple, self-contained test pattern (colored quadrants plus a
    /// crosshair and a moving marker) into a shared D3D11 texture, without any
    /// dependency on WebView2 or window capture. Useful to verify that the
    /// OpenXR overlay pipeline itself (native layer, quad placement, shared
    /// texture handoff) works, independent of whatever real content (SimHub
    /// dashboard, captured window) is later shown.
    /// </summary>
    public sealed class TestPatternSource : IOverlaySource
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
        private ComPtr<ID3D11DeviceContext> _context;
        private ComPtr<ID3D11Texture2D> _texture;
        private nint _sharedHandle;
        private string _sharedHandleName = string.Empty;

        private readonly int _width;
        private readonly int _height;

        private DispatcherTimer? _timer;
        private byte[]? _pixels;
        private int _frame;

        public nint DevicePtr => _devicePtr;
        public nint ContextPtr => _contextPtr;
        public event Action<string, int, int>? FrameArrived;

        public TestPatternSource(int width = 512, int height = 512)
        {
            _width = width;
            _height = height;
        }

        /// <summary>Starts rendering the test pattern and streaming frames via <see cref="FrameArrived"/>.</summary>
        public void Start()
        {
            CreateDeviceAndTexture();
            DrawPattern();
            PushFrame();

            // A slowly moving marker makes it obvious in VR whether the
            // panel is actually updating (vs. a frozen/stale texture).
            _timer = new DispatcherTimer(DispatcherPriority.Render)
            {
                Interval = TimeSpan.FromMilliseconds(200)
            };
            _timer.Tick += (_, _) =>
            {
                _frame++;
                DrawPattern();
                PushFrame();
            };
            _timer.Start();
        }

        private unsafe void PushFrame()
        {
            if (_pixels is null)
            {
                return;
            }

            fixed (byte* p = _pixels)
            {
                _context.UpdateSubresource(new ComPtr<ID3D11Resource>((ID3D11Resource*)_texture.Handle), 0, null, p, (uint)(_width * 4), 0);
            }

            // The shared texture is read cross-process (by the native OpenXR
            // layer) via the NT handle. Without an explicit Flush, the GPU
            // write above can still be sitting in this device's deferred
            // command queue when the other process opens the resource and
            // copies it, resulting in a stale/black texture even though the
            // copy itself reports success.
            _context.Flush();

            FrameArrived?.Invoke(_sharedHandleName, _width, _height);
        }

        /// <summary>
        /// Fills the buffer with four colored quadrants (red/green/blue/yellow),
        /// a white crosshair through the center, and a moving marker dot so
        /// motion/updates are visible, all in BGRA byte order.
        /// </summary>
        private void DrawPattern()
        {
            _pixels ??= new byte[_width * _height * 4];
            var stride = _width * 4;

            for (var y = 0; y < _height; y++)
            {
                var left = y < _height / 2;
                for (var x = 0; x < _width; x++)
                {
                    var top = x < _width / 2;
                    byte b, g, r;
                    if (left && top) { b = 0; g = 0; r = 255; }        // top-left: red
                    else if (left && !top) { b = 0; g = 255; r = 0; }  // top-right: green
                    else if (!left && top) { b = 255; g = 0; r = 0; }  // bottom-left: blue
                    else { b = 0; g = 255; r = 255; }                  // bottom-right: yellow

                    var isCrosshair = Math.Abs(x - _width / 2) < 3 || Math.Abs(y - _height / 2) < 3;
                    var isBorder = x < 6 || y < 6 || x >= _width - 6 || y >= _height - 6;
                    if (isCrosshair || isBorder)
                    {
                        b = g = r = 255;
                    }

                    var offset = y * stride + x * 4;
                    _pixels[offset + 0] = b;
                    _pixels[offset + 1] = g;
                    _pixels[offset + 2] = r;
                    _pixels[offset + 3] = 255;
                }
            }

            // Moving marker: a small black square that sweeps left-to-right
            // across the top edge, one step per tick, so it's obvious the
            // texture is actually being refreshed frame to frame.
            var markerSize = 20;
            var travel = Math.Max(1, _width - markerSize);
            var markerX = _frame % travel;
            for (var y = 10; y < 10 + markerSize && y < _height; y++)
            {
                for (var x = markerX; x < markerX + markerSize && x < _width; x++)
                {
                    var offset = y * stride + x * 4;
                    _pixels[offset + 0] = 0;
                    _pixels[offset + 1] = 0;
                    _pixels[offset + 2] = 0;
                    _pixels[offset + 3] = 255;
                }
            }
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
                // Cross-process NT shared handle, same as WebDashboardSource,
                // so the native OpenXR layer running inside the host process
                // (e.g. iRacing.exe) can open the same GPU resource.
                MiscFlags = (uint)(ResourceMiscFlag.Shared | ResourceMiscFlag.SharedNthandle)
            };

            ComPtr<ID3D11Texture2D> texture = default;
            _device.CreateTexture2D(in textureDesc, null, ref texture).ThrowOnError();
            _texture = texture;

            CreateSharedHandle();
        }

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
            _timer?.Stop();
            _timer = null;

            DisposeTexture();

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
    }
}
