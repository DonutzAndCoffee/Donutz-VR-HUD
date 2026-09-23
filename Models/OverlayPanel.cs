using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace Donutz_VR_HUD.Models
{
    public enum PanelSourceType
    {
        WindowCapture,
        WebDashboard,
        TestPattern,
        Image,
        Visor
    }

    /// <summary>
    /// Configuration for a single overlay panel: what it shows (a captured
    /// window or a web dashboard URL) and where/how it is placed in 3D space
    /// relative to the player (position, rotation, size).
    /// </summary>
    public sealed class OverlayPanel : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        private bool Set<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
        {
            if (Equals(field, value))
            {
                return false;
            }

            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
            return true;
        }

        public Guid Id { get; } = Guid.NewGuid();

        private string _name = "Panel";
        public string Name { get => _name; set => Set(ref _name, value); }

        private bool _isEnabled = true;
        public bool IsEnabled { get => _isEnabled; set => Set(ref _isEnabled, value); }

        private PanelSourceType _sourceType = PanelSourceType.WebDashboard;
        public PanelSourceType SourceType { get => _sourceType; set => Set(ref _sourceType, value); }

        /// <summary>Handle of the desktop window to capture (SourceType == WindowCapture).</summary>
        private IntPtr _windowHandle;
        public IntPtr WindowHandle { get => _windowHandle; set => Set(ref _windowHandle, value); }

        /// <summary>Human readable window title, shown in the UI once selected.</summary>
        private string _windowTitle = string.Empty;
        public string WindowTitle { get => _windowTitle; set => Set(ref _windowTitle, value); }

        /// <summary>URL to render (SourceType == WebDashboard).</summary>
        private string _url = "http://localhost:8888/simhubdash";
        public string Url { get => _url; set => Set(ref _url, value); }

        /// <summary>Path to an image file (PNG, JPG, BMP, ...) to display (SourceType == Image).</summary>
        private string _imagePath = string.Empty;
        public string ImagePath { get => _imagePath; set => Set(ref _imagePath, value); }

        /// <summary>Tint color ("#RRGGBB") at the visor's top/start edge (SourceType == Visor).</summary>
        private string _visorColor = "#1A1A26";
        public string VisorColor { get => _visorColor; set => Set(ref _visorColor, value); }

        /// <summary>Tint strength (0..1) at the visor's top/start edge.</summary>
        private float _visorOpacity = 0.65f;
        public float VisorOpacity { get => _visorOpacity; set => Set(ref _visorOpacity, value); }

        /// <summary>Tint color ("#RRGGBB") at the visor's bottom/end (curved) edge.</summary>
        private string _visorEndColor = "#1A1A26";
        public string VisorEndColor { get => _visorEndColor; set => Set(ref _visorEndColor, value); }

        /// <summary>Tint strength (0..1) at the visor's bottom/end (curved) edge. Defaults to 0 (fully transparent).</summary>
        private float _visorEndOpacity;
        public float VisorEndOpacity { get => _visorEndOpacity; set => Set(ref _visorEndOpacity, value); }

        /// <summary>Fraction (0..1) of the panel's height covered by the tinted band at its flattest (edges).</summary>
        private float _visorCoverage = 0.35f;
        public float VisorCoverage { get => _visorCoverage; set => Set(ref _visorCoverage, value); }

        /// <summary>Fraction (0..1) controlling how strongly the band's lower edge domes/dips toward the middle.</summary>
        private float _visorCurve = 0.18f;
        public float VisorCurve { get => _visorCurve; set => Set(ref _visorCurve, value); }

        // Position in meters, relative to the player (OpenXR local space).
        private float _positionX;
        public float PositionX { get => _positionX; set => Set(ref _positionX, value); }

        private float _positionY;
        public float PositionY { get => _positionY; set => Set(ref _positionY, value); }

        private float _positionZ = -1.0f;
        public float PositionZ { get => _positionZ; set => Set(ref _positionZ, value); }

        // Rotation in degrees.
        private float _pitchDeg;
        public float PitchDeg { get => _pitchDeg; set => Set(ref _pitchDeg, value); }

        private float _yawDeg;
        public float YawDeg { get => _yawDeg; set => Set(ref _yawDeg, value); }

        private float _rollDeg;
        public float RollDeg { get => _rollDeg; set => Set(ref _rollDeg, value); }

        /// <summary>
        /// If true, the panel follows the headset like a head-locked HUD
        /// (the pre-cockpit-anchor behavior). If false (default), the panel
        /// stays fixed in the world relative to the driver's seated
        /// position/orientation, like a real cockpit dashboard element.
        /// </summary>
        private bool _headLocked;
        public bool HeadLocked { get => _headLocked; set => Set(ref _headLocked, value); }

        /// <summary>
        /// If true, the panel's quad is composited fully opaque (its
        /// texture's alpha channel is ignored), so transparent areas of the
        /// source (e.g. a dashboard with a transparent background) show up
        /// as solid black instead of letting the scene behind the panel show
        /// through. If false (default), alpha-blending is used.
        /// </summary>
        private bool _opaqueBackground;
        public bool OpaqueBackground { get => _opaqueBackground; set => Set(ref _opaqueBackground, value); }

        /// <summary>
        /// If true, this panel's position/rotation/scale controls are
        /// disabled in the UI to prevent accidentally moving the wrong
        /// panel. The panel keeps rendering normally; only editing is
        /// blocked.
        /// </summary>
        private bool _isLocked;
        public bool IsLocked { get => _isLocked; set => Set(ref _isLocked, value); }

        // Size in meters.
        private float _widthMeters = 0.4f;
        public float WidthMeters
        {
            get => _widthMeters;
            set
            {
                if (Set(ref _widthMeters, value))
                {
                    OnPropertyChanged(nameof(Scale));
                }
            }
        }

        private float _heightMeters = 0.3f;
        public float HeightMeters
        {
            get => _heightMeters;
            set
            {
                if (Set(ref _heightMeters, value))
                {
                    OnPropertyChanged(nameof(Scale));
                }
            }
        }

        /// <summary>
        /// Aspect ratio (width / height) captured the first time the panel's
        /// size is set, used so that <see cref="Scale"/> can resize the
        /// panel uniformly without distorting it.
        /// </summary>
        private float? _aspectRatio;

        /// <summary>
        /// Single-slider size control: setting this scales both
        /// <see cref="WidthMeters"/> and <see cref="HeightMeters"/>
        /// proportionally, preserving the panel's aspect ratio.
        /// </summary>
        public float Scale
        {
            get => _heightMeters;
            set
            {
                if (value <= 0)
                {
                    return;
                }

                _aspectRatio ??= _heightMeters > 0 ? _widthMeters / _heightMeters : 4f / 3f;

                var newHeight = value;
                var newWidth = newHeight * _aspectRatio.Value;

                var changed = false;
                if (!Equals(_widthMeters, newWidth))
                {
                    _widthMeters = newWidth;
                    changed = true;
                }
                if (!Equals(_heightMeters, newHeight))
                {
                    _heightMeters = newHeight;
                    changed = true;
                }

                if (changed)
                {
                    PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(WidthMeters)));
                    PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HeightMeters)));
                    PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Scale)));
                }
            }
        }

        private void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
