using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Donutz_VR_HUD.Models;
using Donutz_VR_HUD.OpenXR;
using Donutz_VR_HUD.Sources;

namespace Donutz_VR_HUD
{
    public sealed class PanelPreviewControl : FrameworkElement
    {
        public static readonly DependencyProperty SimulateLedsProperty = DependencyProperty.Register(
            nameof(SimulateLeds), typeof(bool), typeof(PanelPreviewControl), new PropertyMetadata(false));
        public static readonly DependencyProperty StatusProperty = DependencyProperty.Register(
            nameof(Status), typeof(string), typeof(PanelPreviewControl), new PropertyMetadata(string.Empty));

        private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(100) };
        private readonly long _animationStart = Stopwatch.GetTimestamp();
        private OverlayPanel? _panel;
        private PanelPreviewSource? _source;
        private BitmapSource? _frame;
        private BitmapSource? _housingFrame;
        private byte[] _ledColors = [];
        private bool _isLoaded;
        private bool _sourceDirty = true;
        private bool _starting;
        private long _sourceChanged;
        private string _contentStatus = string.Empty;

        public bool SimulateLeds
        {
            get => (bool)GetValue(SimulateLedsProperty);
            set => SetValue(SimulateLedsProperty, value);
        }
        public string Status
        {
            get => (string)GetValue(StatusProperty);
            private set => SetValue(StatusProperty, value);
        }

        public PanelPreviewControl()
        {
            DataContextChanged += (_, _) => UpdatePanel();
            Loaded += (_, _) =>
            {
                _isLoaded = true;
                UpdatePanel();
                UpdateActivity();
            };
            Unloaded += (_, _) =>
            {
                _isLoaded = false;
                UpdatePanel();
                UpdateActivity();
            };
            IsVisibleChanged += (_, _) => UpdateActivity();
            _timer.Tick += async (_, _) => await RefreshAsync();
            SnapsToDevicePixels = true;
        }

        private void UpdatePanel()
        {
            if (_panel is not null) _panel.PropertyChanged -= OnPanelPropertyChanged;
            StopSource();
            _panel = _isLoaded ? DataContext as OverlayPanel : null;
            if (_panel is not null) _panel.PropertyChanged += OnPanelPropertyChanged;
            _sourceDirty = true;
            InvalidateVisual();
        }

        private void UpdateActivity()
        {
            if (_isLoaded && IsVisible)
            {
                _timer.Start();
            }
            else
            {
                _timer.Stop();
                StopSource();
                _sourceDirty = true;
            }
        }

        private void StopSource()
        {
            _source?.Dispose();
            _source = null;
            _frame = null;
            _housingFrame = null;
        }

        private void OnPanelPropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName is nameof(OverlayPanel.SourceType) or nameof(OverlayPanel.Url) or
                nameof(OverlayPanel.ImagePath) or nameof(OverlayPanel.WindowHandle) or
                nameof(OverlayPanel.WidthMeters) or nameof(OverlayPanel.HeightMeters))
            {
                _sourceDirty = true;
                _sourceChanged = Stopwatch.GetTimestamp();
            }
            InvalidateVisual();
        }

        private async Task RefreshAsync()
        {
            if (_starting || _panel is null) return;
            try
            {
                if (_sourceDirty && Stopwatch.GetElapsedTime(_sourceChanged).TotalMilliseconds >= 350)
                {
                    StopSource();
                    _sourceDirty = false;
                    if (_panel.SourceType != PanelSourceType.Visor)
                    {
                        _contentStatus = Localization.T("Panel.Preview.Loading");
                        var source = new PanelPreviewSource();
                        _source = source;
                        _starting = true;
                        try
                        {
                            await source.StartAsync(_panel);
                        }
                        finally
                        {
                            _starting = false;
                        }
                    }
                }
                var frame = _source?.ReadFrame();
                if (frame is not null)
                {
                    _frame = frame;
                    var bounds = _source!.ContentBounds;
                    _housingFrame = bounds == new Int32Rect(0, 0, frame.PixelWidth, frame.PixelHeight)
                        ? frame : new CroppedBitmap(frame, bounds);
                    _housingFrame.Freeze();
                    _contentStatus = Localization.T("Panel.Preview.Live");
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or
                ArgumentException or InvalidOperationException or ExternalException or NotSupportedException)
            {
                StopSource();
                _contentStatus = string.Format(Localization.T("Panel.Preview.Error"), ex.Message);
                OverlayIpcClient.AppendAppLogEntry($"Panel preview failed for '{_panel.Name}': {ex}");
            }

            var mainWindow = Window.GetWindow(this) as MainWindow;
            _ledColors = mainWindow?.PreviewLedColors ?? [];
            var ledStatus = SimulateLeds
                ? Localization.T("Panel.Preview.Simulating")
                : mainWindow?.PreviewLedStatus ?? Localization.T("Panel.Preview.NoLeds");
            Status = $"{_contentStatus} {ledStatus}";
            InvalidateVisual();
        }

        protected override void OnRender(DrawingContext drawingContext)
        {
            base.OnRender(drawingContext);

            drawingContext.DrawRoundedRectangle(
                new SolidColorBrush(Color.FromRgb(22, 25, 30)), null,
                new Rect(0, 0, ActualWidth, ActualHeight), 5, 5);

            if (_panel is null || ActualWidth < 40 || ActualHeight < 40)
            {
                return;
            }

            const double margin = 14;
            var panelWidth = Math.Max(0.001, _panel.WidthMeters);
            var panelHeight = Math.Max(0.001, _panel.HeightMeters);
            var displayFrame = _panel.HousingEnabled ? _housingFrame : _frame;
            if (_panel.HousingEnabled && _frame is not null && displayFrame is not null)
            {
                panelWidth *= (double)displayFrame.PixelWidth / _frame.PixelWidth;
                panelHeight *= (double)displayFrame.PixelHeight / _frame.PixelHeight;
            }
            var bezelMeters = _panel.HousingEnabled ? Math.Clamp(_panel.HousingBezelWidthMm, 1, 100) / 1000.0 : 0;
            var scale = Math.Min(
                (ActualWidth - margin * 2) / (panelWidth + bezelMeters * 2),
                (ActualHeight - margin * 2) / (panelHeight + bezelMeters * 2));
            if (!double.IsFinite(scale) || scale <= 0)
            {
                return;
            }

            var screenWidth = panelWidth * scale;
            var screenHeight = panelHeight * scale;
            var bezel = bezelMeters * scale;
            var totalWidth = screenWidth + bezel * 2;
            var totalHeight = screenHeight + bezel * 2;
            var left = (ActualWidth - totalWidth) / 2;
            var top = (ActualHeight - totalHeight) / 2;
            var outer = new Rect(left, top, totalWidth, totalHeight);
            var screen = new Rect(left + bezel, top + bezel, screenWidth, screenHeight);
            var cornerRadius = _panel.HousingEnabled
                ? Math.Clamp(_panel.HousingCornerRadiusMm, 0, 50) * scale / 1000.0
                : 2;

            if (_panel.HousingEnabled)
            {
                var housingColor = ParseColor(_panel.HousingColor, Color.FromRgb(32, 32, 34));
                drawingContext.DrawRoundedRectangle(
                    new SolidColorBrush(housingColor),
                    new Pen(new SolidColorBrush(Color.FromRgb(94, 98, 105)), 1),
                    outer, cornerRadius, cornerRadius);
            }

            drawingContext.DrawRectangle(Brushes.Black, null, screen);
            if (!_panel.OpaqueBackground)
            {
                var checker = new DrawingGroup();
                using (var context = checker.Open())
                {
                    context.DrawRectangle(Brushes.DimGray, null, new Rect(0, 0, 12, 12));
                    context.DrawRectangle(Brushes.Gray, null, new Rect(0, 0, 6, 6));
                    context.DrawRectangle(Brushes.Gray, null, new Rect(6, 6, 6, 6));
                }
                var brush = new DrawingBrush(checker)
                {
                    TileMode = TileMode.Tile, Viewport = new Rect(0, 0, 12, 12),
                    ViewportUnits = BrushMappingMode.Absolute, Stretch = Stretch.None
                };
                drawingContext.DrawRectangle(brush, null, screen);
            }
            if (displayFrame is not null) drawingContext.DrawImage(displayFrame, screen);

            if (_panel.HousingEnabled && _panel.HousingLedsEnabled)
            {
                DrawEdge(drawingContext, screen, scale, 0, _panel.HousingLedTopEnabled, _panel.HousingLedCount, _panel.HousingLedStart, _panel.HousingLedReverse);
                DrawEdge(drawingContext, screen, scale, 1, _panel.HousingLedBottomEnabled, _panel.HousingLedBottomCount, _panel.HousingLedBottomStart, _panel.HousingLedBottomReverse);
                DrawEdge(drawingContext, screen, scale, 2, _panel.HousingLedLeftEnabled, _panel.HousingLedLeftCount, _panel.HousingLedLeftStart, _panel.HousingLedLeftReverse);
                DrawEdge(drawingContext, screen, scale, 3, _panel.HousingLedRightEnabled, _panel.HousingLedRightCount, _panel.HousingLedRightStart, _panel.HousingLedRightReverse);
            }
        }

        private void DrawEdge(DrawingContext drawingContext, Rect screen, double scale, int edge,
            bool enabled, int count, int start, bool reverse)
        {
            count = Math.Min(count, 257 - start);
            if (!enabled || count <= 0)
            {
                return;
            }

            var horizontal = edge < 2;
            var length = horizontal ? screen.Width : screen.Height;
            var pitch = length * _panel!.HousingLedWidth / count;
            var bezelPixels = Math.Clamp(_panel.HousingBezelWidthMm, 1, 100) * scale / 1000.0;
            var fixedCoordinate = (horizontal ? screen.Height : screen.Width) / 2 + bezelPixels * 0.4;
            var radius = Math.Max(0.3, Math.Min(bezelPixels * 0.18, pitch * 0.32));
            var middleX = screen.X + screen.Width / 2;
            var middleY = screen.Y + screen.Height / 2;

            for (var i = 0; i < count; i++)
            {
                var ledIndex = Math.Max(0, start - 1) + (reverse ? count - 1 - i : i);
                var along = (i - (count - 1) / 2.0) * pitch;
                var center = horizontal
                    ? new Point(middleX + along, middleY + (edge == 0 ? -1 : 1) * fixedCoordinate)
                    : new Point(middleX + (edge == 2 ? -1 : 1) * fixedCoordinate, middleY - along);
                var color = GetLedColor(ledIndex, _panel.HousingLedBrightness);
                drawingContext.DrawEllipse(new SolidColorBrush(Color.FromArgb(60, color.R, color.G, color.B)), null,
                    center, radius * 1.8, radius * 1.8);
                drawingContext.DrawEllipse(new SolidColorBrush(color),
                    new Pen(new SolidColorBrush(Color.FromRgb(218, 223, 229)), 0.7), center, radius, radius);
            }
        }

        private Color GetLedColor(int index, float brightness)
        {
            Color color;
            if (SimulateLeds)
            {
                var seconds = Stopwatch.GetElapsedTime(_animationStart).TotalSeconds;
                var litCount = (int)((Math.Sin(seconds * 1.8) + 1) * 8);
                var channel = index % 16;
                color = channel >= litCount ? Colors.Black
                    : channel < 8 ? Colors.Lime : channel < 12 ? Colors.Yellow : Colors.Red;
            }
            else
            {
                var offset = index * 3;
                color = offset >= 0 && offset + 2 < _ledColors.Length
                    ? Color.FromRgb(_ledColors[offset], _ledColors[offset + 1], _ledColors[offset + 2])
                    : Colors.Black;
            }

            static byte ApplyBrightness(byte component, float brightness)
            {
                var srgb = component / 255.0;
                var linear = srgb <= 0.04045 ? srgb / 12.92 : Math.Pow((srgb + 0.055) / 1.055, 2.4);
                linear *= brightness;
                return (byte)Math.Clamp(Math.Round(255 * (linear <= 0.0031308
                    ? linear * 12.92 : 1.055 * Math.Pow(linear, 1 / 2.4) - 0.055)), 0, 255);
            }
            return Color.FromRgb(ApplyBrightness(color.R, brightness),
                ApplyBrightness(color.G, brightness), ApplyBrightness(color.B, brightness));
        }

        private static Color ParseColor(string? value, Color fallback)
        {
            try
            {
                return value is not null
                    ? (Color)ColorConverter.ConvertFromString(value)
                    : fallback;
            }
            catch (FormatException)
            {
                return fallback;
            }
        }
    }
}
