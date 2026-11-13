using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Donutz_VR_HUD.Capture;
using Donutz_VR_HUD.Models;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;

namespace Donutz_VR_HUD.Sources
{
    internal sealed class PanelPreviewSource : IDisposable
    {
        [DllImport("d3d11.dll")]
        private static extern int D3D11CreateDevice(nint adapter, int driverType, nint software,
            uint flags, nint featureLevels, uint featureLevelCount, uint sdkVersion,
            out nint device, out int featureLevel, out nint context);

        private sealed record Frame(string HandleName, int Width, int Height);

        private IOverlaySource? _source;
        private Frame? _latestFrame;
        private Frame? _displayedFrame;
        private ComPtr<ID3D11Device> _device;
        private ComPtr<ID3D11Device1> _device1;
        private ComPtr<ID3D11DeviceContext> _context;
        private ComPtr<ID3D11Texture2D> _sharedTexture;
        private ComPtr<ID3D11Texture2D> _stagingTexture;
        private string? _handleName;
        private bool _disposed;
        private PixelFormat _pixelFormat = PixelFormats.Pbgra32;

        public Int32Rect ContentBounds { get; private set; }

        public async Task StartAsync(OverlayPanel panel)
        {
            switch (panel.SourceType)
            {
                case PanelSourceType.WebDashboard:
                    if (!Uri.TryCreate(panel.Url, UriKind.Absolute, out var url) ||
                        (url.Scheme != Uri.UriSchemeHttp && url.Scheme != Uri.UriSchemeHttps))
                    {
                        throw new ArgumentException(Localization.T("Panel.Preview.InvalidUrl"));
                    }
                    var web = new WebDashboardSource();
                    Subscribe(web);
                    var aspect = panel.WidthMeters / panel.HeightMeters;
                    if (!float.IsFinite(aspect) || aspect <= 0) aspect = 1;
                    var width = aspect >= 1 ? 1024 : Math.Max(1, (int)Math.Round(1024 * aspect));
                    var height = aspect >= 1 ? Math.Max(1, (int)Math.Round(1024 / aspect)) : 1024;
                    await web.StartAsync(panel.Url, width, height);
                    break;
                case PanelSourceType.TestPattern:
                    var test = new TestPatternSource();
                    Subscribe(test);
                    test.Start();
                    break;
                case PanelSourceType.Image:
                    if (string.IsNullOrWhiteSpace(panel.ImagePath))
                    {
                        throw new FileNotFoundException(Localization.T("Panel.Preview.SelectImage"));
                    }
                    _pixelFormat = PixelFormats.Bgra32;
                    var image = new ImageOverlaySource();
                    Subscribe(image);
                    image.Start(panel.ImagePath);
                    break;
                case PanelSourceType.WindowCapture:
                    if (panel.WindowHandle == 0)
                    {
                        throw new ArgumentException(Localization.T("Panel.Preview.SelectWindow"));
                    }
                    var capture = new WindowCapture();
                    Subscribe(capture);
                    capture.Start(panel.WindowHandle);
                    break;
                default:
                    throw new NotSupportedException("Helmet visors do not have a desktop preview.");
            }
        }

        private void Subscribe(IOverlaySource source)
        {
            _source = source;
            source.FrameArrived += OnFrameArrived;
        }

        private void OnFrameArrived(string handleName, int width, int height) =>
            Volatile.Write(ref _latestFrame, new Frame(handleName, width, height));

        public unsafe BitmapSource? ReadFrame()
        {
            var frame = Volatile.Read(ref _latestFrame);
            if (_disposed || frame is null || ReferenceEquals(frame, _displayedFrame)) return null;

            // Use a separate device/context: source rendering can run on CEF or capture threads.
            if (_device.Handle is null)
            {
                D3D11CreateDevice(0, 1, 0, 0x20, 0, 0, 7,
                    out var device, out _, out var context).ThrowOnError();
                _device = new ComPtr<ID3D11Device>((ID3D11Device*)device);
                _context = new ComPtr<ID3D11DeviceContext>((ID3D11DeviceContext*)context);
                _device1 = _device.QueryInterface<ID3D11Device1>();
            }

            if (_handleName != frame.HandleName)
            {
                if (_stagingTexture.Handle is not null) _stagingTexture.Dispose();
                if (_sharedTexture.Handle is not null) _sharedTexture.Dispose();
                _stagingTexture = default;
                _sharedTexture = default;
                var iid = ID3D11Texture2D.Guid;
                fixed (char* name = frame.HandleName)
                {
                    _device1.OpenSharedResourceByName(name, 0x80000000, &iid,
                        (void**)_sharedTexture.GetAddressOf()).ThrowOnError();
                }
                Texture2DDesc description;
                _sharedTexture.GetDesc(&description);
                description.Usage = Usage.Staging;
                description.BindFlags = 0;
                description.CPUAccessFlags = (uint)CpuAccessFlag.Read;
                description.MiscFlags = 0;
                _device.CreateTexture2D(in description, null, ref _stagingTexture).ThrowOnError();
                _handleName = frame.HandleName;
            }

            var staging = new ComPtr<ID3D11Resource>((ID3D11Resource*)_stagingTexture.Handle);
            _context.CopyResource(staging, new ComPtr<ID3D11Resource>((ID3D11Resource*)_sharedTexture.Handle));
            MappedSubresource mapped;
            _context.Map(staging, 0, Map.Read, 0, &mapped).ThrowOnError();
            try
            {
                ContentBounds = FindContentBounds((byte*)mapped.PData, frame.Width, frame.Height, mapped.RowPitch);
                var bitmap = BitmapSource.Create(frame.Width, frame.Height, 96, 96, _pixelFormat, null,
                    (nint)mapped.PData, checked((int)mapped.RowPitch * frame.Height), checked((int)mapped.RowPitch));
                bitmap.Freeze();
                _displayedFrame = frame;
                return bitmap;
            }
            finally
            {
                _context.Unmap(staging, 0);
            }
        }

        private static unsafe Int32Rect FindContentBounds(byte* pixels, int width, int height, uint rowPitch)
        {
            var minX = width;
            var minY = height;
            var maxX = -1;
            var maxY = -1;
            for (var y = 0; y < height; y++)
            {
                var row = pixels + (nuint)y * rowPitch;
                for (var x = 0; x < width; x++)
                {
                    // Match NativeLayer/ContentBounds.cpp, including its alpha threshold.
                    if (row[x * 4 + 3] <= 16) continue;
                    minX = Math.Min(minX, x);
                    minY = Math.Min(minY, y);
                    maxX = Math.Max(maxX, x);
                    maxY = Math.Max(maxY, y);
                }
            }
            return maxX >= minX && maxY >= minY
                ? new Int32Rect(minX, minY, maxX - minX + 1, maxY - minY + 1)
                : new Int32Rect(0, 0, width, height);
        }

        public unsafe void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (_source is not null)
            {
                _source.FrameArrived -= OnFrameArrived;
                _source.Dispose();
                _source = null;
            }
            if (_stagingTexture.Handle is not null) _stagingTexture.Dispose();
            if (_sharedTexture.Handle is not null) _sharedTexture.Dispose();
            if (_device1.Handle is not null) _device1.Dispose();
            if (_context.Handle is not null) _context.Dispose();
            if (_device.Handle is not null) _device.Dispose();
        }
    }
}
