namespace Donutz_VR_HUD.Models
{
    /// <summary>How a panel housing is rendered.</summary>
    public enum HousingRenderMode : byte
    {
        /// <summary>Flat quads around the panel.</summary>
        Quads = 0,

        /// <summary>3D mesh in one projection layer.</summary>
        Mesh = 1,
    }

    /// <summary>One cross-section point: inset from the outer edge (mm) and depth in percent (0 = front, -100 = back).</summary>
    public readonly struct HousingProfilePoint
    {
        public HousingProfilePoint(float insetMm, float zPercent)
        {
            InsetMm = insetMm;
            ZPercent = zPercent;
        }

        public float InsetMm { get; }
        public float ZPercent { get; }
    }

    /// <summary>Cross-section profile swept around the panel in mesh mode.</summary>
    public sealed class HousingProfile
    {
        public const string DefaultName = "Rounded";

        public HousingProfile(string name, float frontCornerRadiusMm, float backCornerRadiusMm, int cornerSegments, IReadOnlyList<HousingProfilePoint> points)
        {
            Name = name;
            FrontCornerRadiusMm = frontCornerRadiusMm;
            BackCornerRadiusMm = backCornerRadiusMm;
            CornerSegments = cornerSegments;
            Points = points;
        }

        public string Name { get; }
        public float FrontCornerRadiusMm { get; }
        public float BackCornerRadiusMm { get; }
        public int CornerSegments { get; }
        public IReadOnlyList<HousingProfilePoint> Points { get; }

        public static IReadOnlyList<HousingProfile> BuiltIn { get; } = new[]
        {
            new HousingProfile("Rounded", 8f, 6f, 8, new[]
            {
                new HousingProfilePoint(1.5f, 0f),
                new HousingProfilePoint(0.45f, -4f),
                new HousingProfilePoint(0f, -12f),
                new HousingProfilePoint(0f, -75f),
                new HousingProfilePoint(0.6f, -90f),
                new HousingProfilePoint(2f, -98f),
                new HousingProfilePoint(4f, -100f),
            }),
            new HousingProfile("Flat", 2f, 2f, 4, new[]
            {
                new HousingProfilePoint(0f, 0f),
                new HousingProfilePoint(0f, -100f),
            }),
            new HousingProfile("Chamfered", 4f, 4f, 4, new[]
            {
                new HousingProfilePoint(3f, 0f),
                new HousingProfilePoint(0f, -15f),
                new HousingProfilePoint(0f, -85f),
                new HousingProfilePoint(3f, -100f),
            }),
        };

        public static IReadOnlyList<string> AllNames { get; } = BuiltIn.Select(p => p.Name).ToArray();

        public static IReadOnlyList<HousingRenderMode> RenderModes { get; } = Enum.GetValues<HousingRenderMode>();

        public static HousingProfile Find(string? name) =>
            BuiltIn.FirstOrDefault(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase)) ?? BuiltIn[0];
    }
}
