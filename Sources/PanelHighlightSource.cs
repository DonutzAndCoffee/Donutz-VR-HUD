using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.DXGI;
using InteropUnmanagedType = System.Runtime.InteropServices.UnmanagedType;

namespace Donutz_VR_HUD.Sources
{
    /// <summary>
    /// Renders a colored picture-frame (a border with a fully transparent
    /// center) into a shared D3D11 texture. Used as its own head-locked-off
    /// overlay quad that is positioned/sized to match whichever panel is
    /// currently selected for keyboard edit-mode editing, so the highlight
    /// is visible inside the VR headset too — green while unlocked, red
    /// while locked. Mirrors the desktop app's colored panel-list border.
    /// Modeled closely on <see cref="HudTextSource"/>/<see cref="TestPatternSource"/>.
    /// </summary>
    public sealed class PanelHighlightSource : IOverlaySource
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

        private nint _devicePtr;
        private nint _contextPtr;
        private ComPtr<ID3D11Device> _device;
        private ComPtr<ID3D11DeviceContext> _context;
        private ComPtr<ID3D11Texture2D> _texture;
        private nint _sharedHandle;
        private string _sharedHandleName = string.Empty;

        private readonly int _width;
        private readonly int _height;

        private byte[]? _pixels;
        private Color _color = Color.LimeGreen;

        public nint DevicePtr => _devicePtr;
        public nint ContextPtr => _contextPtr;
        public event Action<string, int, int>? FrameArrived;

        public PanelHighlightSource(int width = 512, int height = 512)
        {
            _width = width;
            _height = height;
        }

        /// <summary>Creates the shared texture and renders an initial (unlocked/green) frame.</summary>
        public void Start()
        {
            CreateDeviceAndTexture();
            Render();
        }

        /// <summary>Switches the frame color (green = unlocked, red = locked) and re-renders if it changed.</summary>
        public void SetLocked(bool isLocked)
        {
            var newColor = isLocked ? Color.Red : Color.LimeGreen;
            if (newColor == _color)
            {
                return;
            }

            _color = newColor;
            Render();
        }

        private unsafe void Render()
        {
            DrawFrame();
            PushFrame();
        }

        private void DrawFrame()
        {
            _pixels ??= new byte[_width * _height * 4];

            using var bitmap = new Bitmap(_width, _height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bitmap))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);

                //var thickness = Math.Max(6, _width / 24);
                var thickness = 2;
                using var pen = new Pen(_color, thickness);
                var half = thickness / 2f;
                g.DrawRectangle(pen, half, half, _width - thickness, _height - thickness);
            }

            var data = bitmap.LockBits(new Rectangle(0, 0, _width, _height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try
            {
                Marshal.Copy(data.Scan0, _pixels!, 0, _pixels!.Length);
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
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

            // See TestPatternSource for why an explicit Flush is required:
            // without it, the GPU write can still be queued when the native
            // layer (running in a different process) opens/copies the
            // shared texture, resulting in a stale/black frame.
            _context.Flush();

            FrameArrived?.Invoke(_sharedHandleName, _width, _height);
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
