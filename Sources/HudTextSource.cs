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
    /// Renders a block of text (e.g. the keyboard edit-mode status/legend)
    /// into a shared D3D11 texture via GDI+, so it can be shown as a normal
    /// head-locked overlay panel inside the VR headset — mirroring what the
    /// desktop app's own edit-mode HUD border shows, but actually visible
    /// while wearing the headset. Modeled closely on <see cref="TestPatternSource"/>.
    /// </summary>
    public sealed class HudTextSource : IOverlaySource
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
        private string _text = string.Empty;
        private readonly object _textLock = new();

        public nint DevicePtr => _devicePtr;
        public nint ContextPtr => _contextPtr;
        public event Action<string, int, int>? FrameArrived;

        public HudTextSource(int width = 1024, int height = 512)
        {
            _width = width;
            _height = height;
        }

        /// <summary>Creates the shared texture and renders an initial (empty) frame.</summary>
        public void Start()
        {
            CreateDeviceAndTexture();
            Render();
        }

        /// <summary>Re-renders the HUD with new text and pushes the resulting frame.</summary>
        public void UpdateText(string text)
        {
            lock (_textLock)
            {
                _text = text;
            }

            Render();
        }

        private unsafe void Render()
        {
            DrawText();
            PushFrame();
        }

        private void DrawText()
        {
            _pixels ??= new byte[_width * _height * 4];

            string text;
            lock (_textLock)
            {
                text = _text;
            }

            using var bitmap = new Bitmap(_width, _height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bitmap))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
                g.Clear(Color.FromArgb(200, 20, 20, 20));

                using var border = new Pen(Color.Orange, 4);
                g.DrawRectangle(border, 2, 2, _width - 4, _height - 4);

                using var font = new Font("Consolas", 22, System.Drawing.FontStyle.Bold);
                using var brush = new SolidBrush(Color.White);
                var rect = new RectangleF(24, 16, _width - 48, _height - 32);
                g.DrawString(text, font, brush, rect);
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
