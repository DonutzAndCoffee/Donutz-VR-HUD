namespace Donutz_VR_HUD.Models
{
    /// <summary>Placement of a logo decal on the front face of the housing.</summary>
    public enum HousingLogoPosition : byte
    {
        None,
        TopCenter,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    }

    /// <summary>How a panel housing is rendered.</summary>
    public enum HousingRenderMode : byte
    {
        /// <summary>Flat quads around the panel; retained for legacy profiles and fallback.</summary>
        Quads = 0,

        /// <summary>Panel-space mesh with a separate full-resolution dashboard.</summary>
        Mesh = 1,

        /// <summary>Per-eye housing and dashboard on head-facing quads; superseded by <see cref="DisplayPlaneStereo"/>, retained in code but not selectable.</summary>
        HeadFacingStereo = 2,

        /// <summary>True per-eye projection of the 3D housing; retained in code but not selectable (misplaced and jittery on SteamVR).</summary>
        Projection = 3,

        /// <summary>Recommended: per-eye housing and dashboard on quads lying in the display plane, so the dashboard is geometrically exact.</summary>
        DisplayPlaneStereo = 4,

        /// <summary>Retained diagnostic mode, not selectable: <see cref="DisplayPlaneStereo"/> plus a depth check marker.</summary>
        Experimental = 5,
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

        public static IReadOnlyList<HousingRenderMode> RenderModes { get; } = new[]
        {
            HousingRenderMode.DisplayPlaneStereo,
            HousingRenderMode.Mesh,
        };

        public static IReadOnlyList<HousingLogoPosition> LogoPositions { get; } = Enum.GetValues<HousingLogoPosition>();

        public static HousingProfile Find(string? name) =>
            BuiltIn.FirstOrDefault(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase)) ?? BuiltIn[0];
    }
}
