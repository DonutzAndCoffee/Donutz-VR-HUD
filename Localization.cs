using System.Collections.Generic;

namespace Donutz_VR_HUD
{
    /// <summary>
    /// Small runtime string table (no .resx involved) used to provide the
    /// desktop GUI and the in-headset VR overlay HUD shown while adjusting
    /// ("edit mode") panels with their English text. Exposes a singleton
    /// <see cref="Instance"/> with a string indexer so XAML can bind
    /// directly to strings via
    /// <c>{Binding Source={x:Static local:Localization.Instance}, Path=[Some.Key]}</c>.
    /// </summary>
    public sealed class Localization
    {
        public static readonly Localization Instance = new();

        /// <summary>Indexer used by XAML bindings (Path=[key]) to look up text.</summary>
        public string this[string key] => T(key);

        private static readonly Dictionary<string, string> Strings = new()
        {
            // About dialog
            ["About.Title"] = "About Donutz VR HUD",
            ["About.Close"] = "Close",
            ["About.Copyright"] = "Copyright (c) 2025 DonutzAndCoffee",
            ["About.LicenseHeader"] = "License",
            ["About.LicenseBody"] =
                "This program is licensed under Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0). Use, modification and redistribution are permitted with attribution to the original author; commercial use is not permitted.",
            ["About.ThirdPartyHeader"] = "Third-party components",
            ["About.ThirdPartyBody"] =
                "This program uses the following open-source components under their respective licenses:",
            ["About.Button"] = "About",
            ["About.Motto"] = "Donutz: Proof that sim racing can be delicious.",

            // Edit mode (desktop HUD + VR overlay adjustment HUD)
            ["EditMode.Title"] = "Edit Mode",
            ["EditMode.PanelLabel"] = "Panel: ",
            ["EditMode.NoPanelSelected"] = "(no panel selected)",
            ["EditMode.LockedSuffix"] = "locked",
            ["EditMode.ModeLabel"] = "Mode: ",
            ["EditMode.SpeedLabel"] = "   Speed: ",
            ["EditMode.Mode.Move"] = "Move",
            ["EditMode.Mode.Rotate"] = "Rotate",
            ["EditMode.Mode.Scale"] = "Scale",
            ["EditMode.Speed.Fine"] = "fine",
            ["EditMode.Speed.Coarse"] = "coarse",
            ["EditMode.Speed.Normal"] = "normal",
            ["EditMode.Values.Pitch"] = "Pitch",
            ["EditMode.Values.Yaw"] = "Yaw",
            ["EditMode.Values.Roll"] = "Roll",
            ["EditMode.Values.Scale"] = "Scale",
            ["EditMode.Legend.Move"] = "W/A/S/D: forward/back \u00B7 left/right",
            ["EditMode.Legend.RollUpDown"] = "Q/E: down/up or roll",
            ["EditMode.Legend.Tab"] = "Tab: Move \u27F7 Rotate",
            ["EditMode.Legend.SwitchPanel"] = "C: switch panel (Shift+C backwards)",
            ["EditMode.Legend.Lock"] = "L: lock/unlock panel",
            ["EditMode.Legend.SpeedKeys"] = "Shift: fine \u00B7 Alt: coarse",
            ["EditMode.Legend.Esc"] = "Esc: exit edit mode",
            ["EditMode.ToggleCheckBox"] = "Edit Mode",
            ["EditMode.Vr.Title"] = "EDIT MODE",
            ["EditMode.Vr.PanelLine"] = "Panel: {0}",
            ["EditMode.Vr.ModeSpeedLine"] = "Mode: {0}   Speed: {1}",
            ["EditMode.Vr.Legend"] =
                "W/A/S/D: move   Q/E: up/down or roll\n" +
                "Tab: Move/Rotate/Scale   C: switch panel\n" +
                "L: lock/unlock   Shift: fine   Alt: coarse   Esc: exit",

            // Profile section
            ["Profile.Header"] = "Profiles:",
            ["Profile.Load"] = "Load",
            ["Profile.SaveAs"] = "Save as\u2026",
            ["Profile.Update"] = "Update",
            ["Profile.Delete"] = "Delete",
            ["Profile.Game"] = "Game (process name):",
            ["Profile.Car"] = "Car:",
            ["Profile.TakeCarFromSimHub"] = "Get from SimHub",
            ["Profile.AutoLoad"] = "Automatically load profiles based on game/car",
            ["Profile.AutoSaveOnExit"] = "Automatically save the profile for the current car when the sim exits",
            ["Profile.IsDefault"] = "Use as default profile (fallback if nothing matches)",

            // Panel list header
            ["Panels.Add"] = "+ Add panel",
            ["Panels.Header"] = "Overlay panels (max. 5):",
            ["Overlay.Status.Stopped"] = "Overlay stopped (click to start)",
            ["Overlay.Status.Active"] = "Overlay active (click to stop)",
            ["Overlay.Status.Waiting"] = "Overlay waiting for connection…",
            ["Overlay.Status.Error"] = "Overlay problem (click to restart)",
            ["Overlay.Status.Connected"] = "Overlay connected with {0} panel(s).",
            ["Overlay.Status.StoppedMessage"] = "Overlay stopped.",

            // Panel item template
            ["Panel.Remove"] = "Remove",
            ["Panel.Lock"] = "Lock",
            ["Panel.Active"] = "Active",
            ["Panel.Source.Window"] = "Window",
            ["Panel.Source.WebDashboard"] = "Web dashboard (URL)",
            ["Panel.Source.TestPattern"] = "Test pattern",
            ["Panel.Source.Image"] = "Image (PNG/JPG/...)",
            ["Panel.Source.Visor"] = "Visor (gradient)",
            ["Panel.Browse"] = "Browse...",
            ["Panel.Visor.Start"] = "Start:",
            ["Panel.Visor.End"] = "End:",
            ["Panel.Visor.Opacity"] = "Opacity:",
            ["Panel.Visor.Height"] = "Height:",
            ["Panel.Visor.Curve"] = "Curve:",
            ["Panel.Visor.ResetSize"] = "Reset size",
            ["Panel.Visor.ResetSizeTooltip"] =
                "Resets to a small, head-locked default size/position so the whole visor is visible.",
            ["Panel.Position.X"] = "X:",
            ["Panel.Position.Y"] = "Y:",
            ["Panel.Position.Z"] = "Z:",
            ["Panel.Rotation.Pitch"] = "Pitch:",
            ["Panel.Rotation.Yaw"] = "Yaw:",
            ["Panel.Rotation.Roll"] = "Roll:",
            ["Panel.Scale"] = "Scale:",
            ["Panel.SizeMeters"] = "Width\u00D7height (m):",
            ["Panel.HeadLocked"] = "Head-locked (follows head movement instead of staying fixed in the cockpit)",
            ["Panel.OpaqueBackground"] = "Black instead of transparency",

            // Global settings expander
            ["Global.Header"] = "Global settings (fine-tuning, VR reset, OpenXR layer)",
            ["Global.VrReset.Header"] = "VR reset (recenter) \u2014 e.g. assign to a wheel button:",
            ["Global.VrReset.NoButton"] = "No button assigned.",
            ["Global.VrReset.Learn"] = "Assign button\u2026",
            ["Global.VrReset.ManualReset"] = "Reset VR now",
            ["Global.Nudge.Header"] = "Controller fine-tuning (usable in headset):",
            ["Global.Nudge.ActivePanel"] = "Active panel:",
            ["Global.Nudge.StepSize"] = "Step size:",
            ["Global.Nudge.Clear"] = "Clear",
            ["Global.Nudge.Assign"] = "Assign\u2026",
            ["Global.Layer.Header"] = "OpenXR API layer:",
            ["Global.Layer.CheckingStatus"] = "Checking status\u2026",
            ["Global.Layer.Register"] = "Register layer",
            ["Global.Layer.Unregister"] = "Remove layer",

            // Status/footer
            ["Status.Ready"] = "Ready.",

            // Native log expander
            ["NativeLog.Header"] = "Native layer log",
            ["NativeLog.Refresh"] = "Refresh",
            ["NativeLog.VerboseLogging"] = "Verbose logging (debug)",
            ["NativeLog.NoFileFound"] = "(no log file found yet)",

            // Input dialog
            ["InputDialog.Ok"] = "OK",
            ["InputDialog.Cancel"] = "Cancel",
        };

        /// <summary>Looks up the text for the given key, falling back to the key itself if missing.</summary>
        public static string T(string key)
        {
            return Strings.TryGetValue(key, out var value) ? value : key;
        }
    }
}
