namespace Donutz_VR_HUD.Settings
{
    /// <summary>
    /// A named, persistable set of panel configurations (and optionally a
    /// VR reset button binding), which can optionally be tied to a specific
    /// simulation (by process name) and/or a specific car (by name/model),
    /// so it can be found and applied automatically once that
    /// simulation/car is detected as active.
    /// </summary>
    public sealed class Profile
    {
        public Guid Id { get; set; } = Guid.NewGuid();

        public string Name { get; set; } = "Profil";

        /// <summary>
        /// Optional: the process name (without ".exe") of the simulation
        /// this profile applies to, e.g. "iRacingSim64DX11". If empty, the
        /// profile is not eligible for game-based auto-loading.
        /// </summary>
        public string? GameProcessName { get; set; }

        /// <summary>
        /// Optional: the car name/model this profile applies to (e.g. as
        /// reported by SimHub or typed in manually). Matching is a
        /// case-insensitive substring/equality check. If empty, the profile
        /// is not car-specific (it can still be game-specific or fully
        /// generic).
        /// </summary>
        public string? CarModel { get; set; }

        public List<PanelSettings> Panels { get; set; } = new();

        /// <summary>Legacy single-binding slot, kept only for reading old profile files. Use <see cref="ResetBindings"/> instead.</summary>
        public ResetBindingSettings? ResetBinding { get; set; }

        /// <summary>All "VR Reset" button bindings (multiple wheel/controller buttons can trigger a recenter).</summary>
        public List<ResetBindingSettings> ResetBindings { get; set; } = new();

        /// <summary>
        /// If true, this profile is used as the catch-all fallback for
        /// automatic loading when no other profile's game/car criteria
        /// match the currently detected simulation/car. At most one
        /// profile should have this set (enforced when saving/updating
        /// profiles in the UI).
        /// </summary>
        public bool IsDefault { get; set; }

        /// <summary>A profile is eligible for automatic loading if it names a game and/or a car.</summary>
        public bool HasAutoLoadCriteria => !string.IsNullOrWhiteSpace(GameProcessName) || !string.IsNullOrWhiteSpace(CarModel);
    }
}
