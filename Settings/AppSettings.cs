using Donutz_VR_HUD.Models;

namespace Donutz_VR_HUD.Settings
{
    /// <summary>Serializable snapshot of a single <see cref="OverlayPanel"/>.</summary>
    public sealed class PanelSettings
    {
        public string Name { get; set; } = "Panel";
        public bool IsEnabled { get; set; } = true;
        public PanelSourceType SourceType { get; set; } = PanelSourceType.WebDashboard;
        public long WindowHandle { get; set; }
        public string WindowTitle { get; set; } = string.Empty;
        public string Url { get; set; } = string.Empty;

        public float PositionX { get; set; }
        public float PositionY { get; set; }
        public float PositionZ { get; set; } = -1.0f;

        public float PitchDeg { get; set; }
        public float YawDeg { get; set; }
        public float RollDeg { get; set; }

        public float WidthMeters { get; set; } = 0.4f;
        public float HeightMeters { get; set; } = 0.3f;

        /// <summary>Defaults to false (cockpit-fixed) for backward compatibility with older saved settings.</summary>
        public bool HeadLocked { get; set; }

        /// <summary>Defaults to false (alpha-blended/transparent) for backward compatibility with older saved settings.</summary>
        public bool OpaqueBackground { get; set; }

        /// <summary>If true, the panel's position/rotation/scale controls are locked in the UI to prevent accidental edits.</summary>
        public bool IsLocked { get; set; }

        public string ImagePath { get; set; } = string.Empty;
        public string VisorColor { get; set; } = "#000000";
        public float VisorOpacity { get; set; } = 0.68f;
        public string VisorEndColor { get; set; } = "#1A1A26";
        public float VisorEndOpacity { get; set; } = 0.2f;
        public float VisorCoverage { get; set; } = 0.58f;
        public float VisorCurve { get; set; }
        public float VisorScratches { get; set; } = 0.03f;
        public float VisorDirt { get; set; }
        public float VisorBubbles { get; set; } = 0.05f;
        public int VisorSeed { get; set; } = 1;
        public float VisorOffsetX { get; set; }
        public float VisorOffsetY { get; set; } = -0.10f;
        public float VisorOffsetZ { get; set; } = 0.24f;
        public float VisorSize { get; set; } = 2.85f;
        public string VisorText { get; set; } = "DONUTZ";
        public string VisorTextColor { get; set; } = "#FFFFFF";
        public float VisorTextOpacity { get; set; } = 0.09f;
        public float VisorTextSize { get; set; } = 1f;
        public float VisorTextBlur { get; set; } = 0.32f;
        public float VisorOpenAngle { get; set; } = 59f;
        public float VisorEdgeCurve { get; set; } = 0.18f;
        public string VisorTextFont { get; set; } = "Bahnschrift";
        public bool VisorTextBold { get; set; } = true;
        public bool VisorTextItalic { get; set; } = true;

        public bool HousingEnabled { get; set; }
        public float HousingBezelWidthMm { get; set; } = 12f;
        public float HousingDepthMm { get; set; } = 25f;
        public string HousingColor { get; set; } = "#202022";
        public float HousingRoughness { get; set; } = 0.6f;
        public float HousingMetallic { get; set; }
        public float HousingCornerRadiusMm { get; set; } = 4f;
        public HousingRenderMode HousingRenderMode { get; set; } = HousingRenderMode.Quads;
        public float HousingRenderScale { get; set; } = 1f;
        public string HousingProfileName { get; set; } = HousingProfile.DefaultName;

        public static PanelSettings FromPanel(OverlayPanel panel) => new()
        {
            ImagePath = panel.ImagePath,
            VisorColor = panel.VisorColor,
            VisorOpacity = panel.VisorOpacity,
            VisorEndColor = panel.VisorEndColor,
            VisorEndOpacity = panel.VisorEndOpacity,
            VisorCoverage = panel.VisorCoverage,
            VisorCurve = panel.VisorCurve,
            VisorScratches = panel.VisorScratches,
            VisorDirt = panel.VisorDirt,
            VisorBubbles = panel.VisorBubbles,
            VisorSeed = panel.VisorSeed,
            VisorOffsetX = panel.VisorOffsetX,
            VisorOffsetY = panel.VisorOffsetY,
            VisorOffsetZ = panel.VisorOffsetZ,
            VisorSize = panel.VisorSize,
            VisorText = panel.VisorText,
            VisorTextColor = panel.VisorTextColor,
            VisorTextOpacity = panel.VisorTextOpacity,
            VisorTextSize = panel.VisorTextSize,
            VisorTextBlur = panel.VisorTextBlur,
            VisorOpenAngle = panel.VisorOpenAngle,
            VisorEdgeCurve = panel.VisorEdgeCurve,
            VisorTextFont = panel.VisorTextFont,
            VisorTextBold = panel.VisorTextBold,
            VisorTextItalic = panel.VisorTextItalic,
            HousingEnabled = panel.HousingEnabled,
            HousingBezelWidthMm = panel.HousingBezelWidthMm,
            HousingDepthMm = panel.HousingDepthMm,
            HousingColor = panel.HousingColor,
            HousingRoughness = panel.HousingRoughness,
            HousingMetallic = panel.HousingMetallic,
            HousingCornerRadiusMm = panel.HousingCornerRadiusMm,
            HousingRenderMode = panel.HousingRenderMode,
            HousingRenderScale = panel.HousingRenderScale,
            HousingProfileName = panel.HousingProfileName,
            Name = panel.Name,
            IsEnabled = panel.IsEnabled,
            SourceType = panel.SourceType,
            WindowHandle = panel.WindowHandle.ToInt64(),
            WindowTitle = panel.WindowTitle,
            Url = panel.Url,
            PositionX = panel.PositionX,
            PositionY = panel.PositionY,
            PositionZ = panel.PositionZ,
            PitchDeg = panel.PitchDeg,
            YawDeg = panel.YawDeg,
            RollDeg = panel.RollDeg,
            WidthMeters = panel.WidthMeters,
            HeightMeters = panel.HeightMeters,
            HeadLocked = panel.HeadLocked,
            OpaqueBackground = panel.OpaqueBackground,
            IsLocked = panel.IsLocked
        };

        public OverlayPanel ToPanel()
        {
            var panel = new OverlayPanel
            {
                Name = Name,
                IsEnabled = IsEnabled,
                SourceType = SourceType,
                WindowHandle = new IntPtr(WindowHandle),
                WindowTitle = WindowTitle,
                Url = Url,
                PositionX = PositionX,
                PositionY = PositionY,
                PositionZ = PositionZ,
                PitchDeg = PitchDeg,
                YawDeg = YawDeg,
                RollDeg = RollDeg,
                WidthMeters = WidthMeters,
                HeightMeters = HeightMeters,
                HeadLocked = HeadLocked,
                OpaqueBackground = OpaqueBackground,
                IsLocked = IsLocked,
                ImagePath = ImagePath ?? string.Empty,
                VisorColor = VisorColor ?? "#1A1A26",
                VisorOpacity = VisorOpacity,
                VisorEndColor = VisorEndColor ?? "#1A1A26",
                VisorEndOpacity = VisorEndOpacity,
                VisorCoverage = VisorCoverage,
                VisorCurve = VisorCurve,
                VisorScratches = VisorScratches,
                VisorDirt = VisorDirt,
                VisorBubbles = VisorBubbles,
                VisorSeed = VisorSeed,
                VisorOffsetX = VisorOffsetX,
                VisorOffsetY = VisorOffsetY,
                VisorOffsetZ = VisorOffsetZ,
                VisorSize = VisorSize > 0 ? VisorSize : 1f,
                VisorText = VisorText ?? string.Empty,
                VisorTextColor = VisorTextColor ?? "#FFFFFF",
                VisorTextOpacity = VisorTextOpacity,
                VisorTextSize = VisorTextSize,
                VisorTextBlur = VisorTextBlur,
                VisorOpenAngle = VisorOpenAngle,
                VisorEdgeCurve = VisorEdgeCurve,
                VisorTextFont = VisorTextFont ?? "Arial",
                VisorTextBold = VisorTextBold,
                VisorTextItalic = VisorTextItalic,
                HousingEnabled = HousingEnabled,
                HousingBezelWidthMm = HousingBezelWidthMm > 0 ? HousingBezelWidthMm : 12f,
                HousingDepthMm = HousingDepthMm > 0 ? HousingDepthMm : 25f,
                HousingColor = HousingColor ?? "#202022",
                HousingRoughness = HousingRoughness,
                HousingMetallic = HousingMetallic,
                HousingCornerRadiusMm = HousingCornerRadiusMm >= 0 ? HousingCornerRadiusMm : 4f,
                HousingRenderMode = HousingRenderMode,
                HousingRenderScale = HousingRenderScale > 0 ? HousingRenderScale : 1f,
                HousingProfileName = HousingProfileName ?? HousingProfile.DefaultName
            };

            return panel;
        }
    }

    /// <summary>Serializable snapshot of the "VR Reset" button binding.</summary>
    public sealed class ResetBindingSettings
    {
        public Guid DeviceInstanceGuid { get; set; }
        public string DeviceName { get; set; } = string.Empty;
        public int ButtonIndex { get; set; } = -1;
    }

    /// <summary>Serializable snapshot of a single panel-nudge action binding.</summary>
    public sealed class NudgeBindingSettings
    {
        public Guid DeviceInstanceGuid { get; set; }
        public string DeviceName { get; set; } = string.Empty;
        public int ButtonIndex { get; set; } = -1;
    }

    /// <summary>Top-level application settings persisted between sessions.</summary>
    public sealed class AppSettings
    {
        public List<PanelSettings> Panels { get; set; } = new();

        /// <summary>Legacy single-binding slot, kept only for reading old settings files. Use <see cref="ResetBindings"/> instead.</summary>
        public ResetBindingSettings? ResetBinding { get; set; }

        /// <summary>All "VR Reset" button bindings (multiple wheel/controller buttons can trigger a recenter).</summary>
        public List<ResetBindingSettings> ResetBindings { get; set; } = new();

        /// <summary>Wheel/button box buttons that open/close (toggle) the helmet visor.</summary>
        public List<ResetBindingSettings> VisorToggleBindings { get; set; } = new();

        /// <summary>Bindings for controller-driven panel fine-tuning ("nudge"), keyed by <see cref="Models.PanelSourceType"/>-agnostic action name.</summary>
        public Dictionary<string, NudgeBindingSettings> NudgeBindings { get; set; } = new();

        /// <summary>
        /// If true, the app automatically loads the best-matching saved
        /// profile based on the currently running simulation process and
        /// (if available via SimHub) the active car.
        /// </summary>
        public bool AutoLoadProfiles { get; set; }

        /// <summary>
        /// If true, the app automatically saves the current panel
        /// configuration back into the profile for the active car as soon
        /// as the simulation is detected as no longer running (i.e. the
        /// user quit/closed it), so manual clicking of "Aktualisieren" is
        /// not required.
        /// </summary>
        public bool AutoSaveProfileOnSimExit { get; set; }

        /// <summary>
        /// If true, closing the main window (X button) minimizes the app to
        /// the system tray instead of exiting the process. The app can then
        /// be restored via the tray icon or fully exited via its context menu.
        /// </summary>
        public bool CloseToTray { get; set; }

        /// <summary>
        /// Legacy UI language setting ("de" or "en") kept only for backward
        /// compatibility with older settings files. The app is English-only
        /// now, so this value is no longer read or written.
        /// </summary>
        public string Language { get; set; } = "en";
    }
}
