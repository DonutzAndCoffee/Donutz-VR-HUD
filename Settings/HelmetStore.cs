using System.IO;
using System.Text.Json;
using Donutz_VR_HUD.Models;

namespace Donutz_VR_HUD.Settings
{
    /// <summary>A named helmet/visor look in the shared helmet library.</summary>
    public sealed class HelmetPreset
    {
        public string Name { get; set; } = "Helmet";
        public PanelSettings Visor { get; set; } = new();

        public override string ToString() => Name;
    }

    /// <summary>Loads/saves the shared helmet library (helmets.json), independent of car profiles.</summary>
    public static class HelmetStore
    {
        private static readonly string FilePath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Donutz VR HUD",
            "helmets.json");

        private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

        public static List<HelmetPreset> Load()
        {
            try
            {
                if (!File.Exists(FilePath))
                {
                    return new List<HelmetPreset>();
                }

                return JsonSerializer.Deserialize<List<HelmetPreset>>(File.ReadAllText(FilePath), JsonOptions) ?? new List<HelmetPreset>();
            }
            catch
            {
                return new List<HelmetPreset>();
            }
        }

        public static void Save(IEnumerable<HelmetPreset> helmets)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
                File.WriteAllText(FilePath, JsonSerializer.Serialize(helmets.ToList(), JsonOptions));
            }
            catch
            {
                // Best-effort persistence.
            }
        }

        /// <summary>Copies only the visor look (not position, name or enabled state) onto <paramref name="panel"/>.</summary>
        public static void ApplyTo(PanelSettings source, OverlayPanel panel)
        {
            panel.VisorColor = source.VisorColor;
            panel.VisorOpacity = source.VisorOpacity;
            panel.VisorEndColor = source.VisorEndColor;
            panel.VisorEndOpacity = source.VisorEndOpacity;
            panel.VisorCoverage = source.VisorCoverage;
            panel.VisorCurve = source.VisorCurve;
            panel.VisorScratches = source.VisorScratches;
            panel.VisorDirt = source.VisorDirt;
            panel.VisorBubbles = source.VisorBubbles;
            panel.VisorSeed = source.VisorSeed;
            panel.VisorOffsetX = source.VisorOffsetX;
            panel.VisorOffsetY = source.VisorOffsetY;
            panel.VisorOffsetZ = source.VisorOffsetZ;
            panel.VisorSize = source.VisorSize > 0 ? source.VisorSize : 1f;
            panel.VisorText = source.VisorText ?? string.Empty;
            panel.VisorTextColor = source.VisorTextColor ?? "#FFFFFF";
            panel.VisorTextOpacity = source.VisorTextOpacity;
            panel.VisorTextSize = source.VisorTextSize;
            panel.VisorTextBlur = source.VisorTextBlur;
            panel.VisorTextFont = source.VisorTextFont ?? "Arial";
            panel.VisorTextBold = source.VisorTextBold;
            panel.VisorTextItalic = source.VisorTextItalic;
        }
    }
}
