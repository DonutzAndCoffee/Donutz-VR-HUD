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
    /// Renders a procedural "helmet visor" gradient into a shared D3D11
    /// texture: a domed (curved) tinted band across the top of the panel
    /// that fades from a tint color to fully transparent, similar to
    /// looking up at the inside of a curved visor/sun-shade. Below the
    /// curve the panel is fully transparent, so it can be layered as a
    /// head-locked overlay on top of the rest of the scene without hiding
    /// it. Modeled closely on <see cref="PanelHighlightSource"/>/
    /// <see cref="TestPatternSource"/>.
    /// </summary>
    public sealed class VisorOverlaySource : IOverlaySource
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

        private Color _startColor = Color.FromArgb(0x20, 0x20, 0x30);
        private float _startOpacity = 0.65f;
        private Color _endColor = Color.FromArgb(0x20, 0x20, 0x30);
        private float _endOpacity = 0f;
        private float _coverage = 0.35f;
        private float _curve = 0.18f;

        public nint DevicePtr => _devicePtr;
        public nint ContextPtr => _contextPtr;
        public event Action<string, int, int>? FrameArrived;

        public VisorOverlaySource(int width = 1024, int height = 1024)
        {
            _width = width;
            _height = height;
        }

        /// <summary>
        /// Creates the shared texture and renders the initial visor
        /// gradient, from <paramref name="startColorHex"/>/<paramref name="startOpacity"/>
        /// at the top edge to <paramref name="endColorHex"/>/<paramref name="endOpacity"/>
        /// at the band's lower (curved) edge. Colors are "#RRGGBB" (or
        /// "RRGGBB") strings; opacities/<paramref name="coverage"/>/
        /// <paramref name="curve"/> are 0..1 fractions (how much of the
        /// panel's height the band covers, and how strongly the band's
        /// lower edge dips/domes in the middle).
        /// </summary>
        public void Start(string startColorHex, float startOpacity, string endColorHex, float endOpacity, float coverage, float curve)
        {
            ApplyParameters(startColorHex, startOpacity, endColorHex, endOpacity, coverage, curve);
            CreateDeviceAndTexture();
            Render();
        }

        /// <summary>Updates the visor's look and re-renders.</summary>
        public void UpdateParameters(string startColorHex, float startOpacity, string endColorHex, float endOpacity, float coverage, float curve)
        {
            ApplyParameters(startColorHex, startOpacity, endColorHex, endOpacity, coverage, curve);
            Render();
        }

        private void ApplyParameters(string startColorHex, float startOpacity, string endColorHex, float endOpacity, float coverage, float curve)
        {
            _startColor = TryParseColor(startColorHex) ?? _startColor;
            _startOpacity = Math.Clamp(startOpacity, 0f, 1f);
            _endColor = TryParseColor(endColorHex) ?? _endColor;
            _endOpacity = Math.Clamp(endOpacity, 0f, 1f);
            _coverage = Math.Clamp(coverage, 0f, 1f);
            _curve = Math.Clamp(curve, 0f, 1f);
        }

        private static Color? TryParseColor(string? hex)
        {
            if (string.IsNullOrWhiteSpace(hex))
            {
                return null;
            }

            try
            {
                return ColorTranslator.FromHtml(hex.StartsWith('#') ? hex : "#" + hex);
            }
            catch
            {
                return null;
            }
        }

        private void Render()
        {
            DrawVisor();
            PushFrame();
        }

        private void DrawVisor()
        {
            _pixels ??= new byte[_width * _height * 4];

            using var bitmap = new Bitmap(_width, _height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bitmap))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);

                // Flat top edge of the tint band; the lower boundary is a
                // Bezier curve dipping down toward the middle, mimicking the
                // dome/curvature of a real visor. Below that curve the panel
                // stays fully transparent so it doesn't block the view.
                var edgeY = _height * (1f - _coverage);
                var dip = _height * _curve;

                var leftEdge = new PointF(0, edgeY);
                var rightEdge = new PointF(_width, edgeY);
                var ctrl1 = new PointF(_width * 0.25f, edgeY + dip * 1.4f);
                var ctrl2 = new PointF(_width * 0.75f, edgeY + dip * 1.4f);

                using var path = new GraphicsPath();
                path.AddLine(0, 0, _width, 0);
                path.AddLine(_width, 0, rightEdge.X, rightEdge.Y);
                path.AddBezier(rightEdge, ctrl2, ctrl1, leftEdge);
                path.CloseFigure();

                var maxY = Math.Max(edgeY + dip * 1.4f, 1f);
                using var brush = new LinearGradientBrush(
                    new PointF(0, 0),
                    new PointF(0, maxY),
                    Color.FromArgb((int)(_startOpacity * 255), _startColor),
                    Color.FromArgb((int)(_endOpacity * 255), _endColor));

                g.FillPath(brush, path);
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
