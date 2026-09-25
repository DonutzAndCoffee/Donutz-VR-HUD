using System.Collections.Generic;
using System.ComponentModel;
using System.Windows.Data;

namespace Donutz_VR_HUD
{
    /// <summary>Supported UI languages.</summary>
    public enum AppLanguage
    {
        German,
        English
    }

    /// <summary>
    /// Small runtime localization helper (no .resx involved) used to
    /// translate the desktop GUI and the in-headset VR overlay HUD shown
    /// while adjusting ("Justierung") panels between German and English.
    /// Exposes a singleton <see cref="Instance"/> implementing
    /// <see cref="INotifyPropertyChanged"/> so XAML can bind directly to
    /// translated strings via its string indexer (e.g.
    /// <c>{Binding Source={x:Static local:Localization.Instance}, Path=[Some.Key]}</c>)
    /// and have them refresh automatically when the language changes.
    /// The currently selected language is persisted via
    /// <see cref="Settings.AppSettings.Language"/>.
    /// </summary>
    public sealed class Localization : INotifyPropertyChanged
    {
        public static readonly Localization Instance = new();

        /// <summary>Raised whenever the language changes, for code-behind that needs to react imperatively (in addition to XAML bindings, which update automatically).</summary>
        public static event System.Action? LanguageChanged;

        public event PropertyChangedEventHandler? PropertyChanged;

        private AppLanguage _language = AppLanguage.German;

        /// <summary>The instance-level current language. Prefer the static <see cref="CurrentLanguage"/> for code-behind use.</summary>
        public AppLanguage Language
        {
            get => _language;
            set
            {
                if (_language == value)
                {
                    return;
                }

                _language = value;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(Binding.IndexerName));
                LanguageChanged?.Invoke();
            }
        }

        /// <summary>Indexer used by XAML bindings (Path=[key]) to look up a translated string that auto-refreshes on language change.</summary>
        public string this[string key] => T(key);

        public static AppLanguage CurrentLanguage
        {
            get => Instance.Language;
            set => Instance.Language = value;
        }

        public static string ToSettingsValue(AppLanguage language) => language == AppLanguage.English ? "en" : "de";

        public static AppLanguage FromSettingsValue(string? value) =>
            string.Equals(value, "en", System.StringComparison.OrdinalIgnoreCase) ? AppLanguage.English : AppLanguage.German;

        private static readonly Dictionary<string, (string De, string En)> Strings = new()
        {
            ["Language.Label"] = ("Sprache:", "Language:"),

            // About dialog
            ["About.Title"] = ("Über Donutz VR HUD", "About Donutz VR HUD"),
            ["About.Close"] = ("Schließen", "Close"),
            ["About.Copyright"] = (
                "Copyright (c) 2025 DonutzAndCoffee",
                "Copyright (c) 2025 DonutzAndCoffee"),
            ["About.LicenseHeader"] = ("Lizenz", "License"),
            ["About.LicenseBody"] = (
                "Dieses Programm steht unter der Creative-Commons-Lizenz „Namensnennung – Nicht kommerziell 4.0 International“ (CC BY-NC 4.0). Nutzung, Veränderung und Weitergabe sind unter Namensnennung des Urhebers erlaubt; eine kommerzielle Nutzung ist nicht gestattet.",
                "This program is licensed under Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0). Use, modification and redistribution are permitted with attribution to the original author; commercial use is not permitted."),
            ["About.ThirdPartyHeader"] = ("Drittanbieter-Komponenten", "Third-party components"),
            ["About.ThirdPartyBody"] = (
                "Dieses Programm nutzt die folgenden Open-Source-Komponenten unter ihren jeweiligen Lizenzen:",
                "This program uses the following open-source components under their respective licenses:"),
            ["About.Button"] = ("Über", "About"),

            // Edit mode (desktop HUD + VR overlay adjustment HUD)
            ["EditMode.Title"] = ("Editiermodus", "Edit Mode"),
            ["EditMode.PanelLabel"] = ("Panel: ", "Panel: "),
            ["EditMode.NoPanelSelected"] = ("(kein Panel ausgewählt)", "(no panel selected)"),
            ["EditMode.LockedSuffix"] = ("gesperrt", "locked"),
            ["EditMode.ModeLabel"] = ("Modus: ", "Mode: "),
            ["EditMode.SpeedLabel"] = ("   Geschwindigkeit: ", "   Speed: "),
            ["EditMode.Mode.Move"] = ("Verschieben", "Move"),
            ["EditMode.Mode.Rotate"] = ("Ausrichten", "Rotate"),
            ["EditMode.Mode.Scale"] = ("Größe", "Scale"),
            ["EditMode.Speed.Fine"] = ("fein", "fine"),
            ["EditMode.Speed.Coarse"] = ("grob", "coarse"),
            ["EditMode.Speed.Normal"] = ("normal", "normal"),
            ["EditMode.Values.Pitch"] = ("Neigung", "Pitch"),
            ["EditMode.Values.Yaw"] = ("Drehung", "Yaw"),
            ["EditMode.Values.Roll"] = ("Rollen", "Roll"),
            ["EditMode.Values.Scale"] = ("Größe", "Scale"),
            ["EditMode.Legend.Move"] = ("W/A/S/D: vor/zurück · links/rechts", "W/A/S/D: forward/back · left/right"),
            ["EditMode.Legend.RollUpDown"] = ("Q/E: runter/hoch bzw. Rollen", "Q/E: down/up or roll"),
            ["EditMode.Legend.Tab"] = ("Tab: Verschieben ⟷ Ausrichten", "Tab: Move ⟷ Rotate"),
            ["EditMode.Legend.SwitchPanel"] = ("C: Panel wechseln (Shift+C rückwärts)", "C: switch panel (Shift+C backwards)"),
            ["EditMode.Legend.Lock"] = ("L: Panel sperren/entsperren", "L: lock/unlock panel"),
            ["EditMode.Legend.SpeedKeys"] = ("Shift: fein · Alt: grob", "Shift: fine · Alt: coarse"),
            ["EditMode.Legend.Esc"] = ("Esc: Editiermodus beenden", "Esc: exit edit mode"),
            ["EditMode.ToggleCheckBox"] = ("Editiermodus", "Edit Mode"),
            ["EditMode.Vr.Title"] = ("EDITIERMODUS", "EDIT MODE"),
            ["EditMode.Vr.PanelLine"] = ("Panel: {0}", "Panel: {0}"),
            ["EditMode.Vr.ModeSpeedLine"] = ("Modus: {0}   Geschwindigkeit: {1}", "Mode: {0}   Speed: {1}"),
            ["EditMode.Vr.Legend"] = (
                "W/A/S/D: bewegen   Q/E: hoch/runter bzw. rollen\n" +
                "Tab: Verschieben/Ausrichten/Größe   C: Panel wechseln\n" +
                "L: sperren/entsperren   Shift: fein   Alt: grob   Esc: beenden",
                "W/A/S/D: move   Q/E: up/down or roll\n" +
                "Tab: Move/Rotate/Scale   C: switch panel\n" +
                "L: lock/unlock   Shift: fine   Alt: coarse   Esc: exit"),

            // Profile section
            ["Profile.Header"] = ("Profile:", "Profiles:"),
            ["Profile.Load"] = ("Laden", "Load"),
            ["Profile.SaveAs"] = ("Speichern unter…", "Save as…"),
            ["Profile.Update"] = ("Aktualisieren", "Update"),
            ["Profile.Delete"] = ("Löschen", "Delete"),
            ["Profile.Game"] = ("Spiel (Prozessname):", "Game (process name):"),
            ["Profile.Car"] = ("Fahrzeug:", "Car:"),
            ["Profile.TakeCarFromSimHub"] = ("Von SimHub übernehmen", "Get from SimHub"),
            ["Profile.AutoLoad"] = ("Profile automatisch anhand von Spiel/Fahrzeug laden", "Automatically load profiles based on game/car"),
            ["Profile.AutoSaveOnExit"] = ("Profil beim Beenden der Sim automatisch fürs aktuelle Fahrzeug speichern", "Automatically save the profile for the current car when the sim exits"),
            ["Profile.IsDefault"] = ("Als Standard-Profil verwenden (Fallback, wenn nichts passt)", "Use as default profile (fallback if nothing matches)"),

            // Panel list header
            ["Panels.Add"] = ("+ Panel hinzufügen", "+ Add panel"),
            ["Panels.Header"] = ("Overlay-Panels (max. 5):", "Overlay panels (max. 5):"),
            ["Overlay.Status.Stopped"] = ("Overlay gestoppt (klicken zum Starten)", "Overlay stopped (click to start)"),

            // Panel item template
            ["Panel.Remove"] = ("Entfernen", "Remove"),
            ["Panel.Lock"] = ("Sperren", "Lock"),
            ["Panel.Active"] = ("Aktiv", "Active"),
            ["Panel.Source.Window"] = ("Fenster", "Window"),
            ["Panel.Source.WebDashboard"] = ("Web-Dashboard (URL)", "Web dashboard (URL)"),
            ["Panel.Source.TestPattern"] = ("Test-Muster", "Test pattern"),
            ["Panel.Source.Image"] = ("Bild (PNG/JPG/...)", "Image (PNG/JPG/...)"),
            ["Panel.Source.Visor"] = ("Visier (Verlauf)", "Visor (gradient)"),
            ["Panel.Browse"] = ("Durchsuchen...", "Browse..."),
            ["Panel.Visor.Start"] = ("Start:", "Start:"),
            ["Panel.Visor.End"] = ("Ende:", "End:"),
            ["Panel.Visor.Opacity"] = ("Deckkraft:", "Opacity:"),
            ["Panel.Visor.Height"] = ("Höhe:", "Height:"),
            ["Panel.Visor.Curve"] = ("Wölbung:", "Curve:"),
            ["Panel.Visor.ResetSize"] = ("Startgröße", "Reset size"),
            ["Panel.Visor.ResetSizeTooltip"] = (
                "Setzt eine kleine, kopfgebundene Ausgangsgröße/-position, damit das ganze Visier sichtbar ist.",
                "Resets to a small, head-locked default size/position so the whole visor is visible."),
            ["Panel.Position.X"] = ("X:", "X:"),
            ["Panel.Position.Y"] = ("Y:", "Y:"),
            ["Panel.Position.Z"] = ("Z:", "Z:"),
            ["Panel.Rotation.Pitch"] = ("Neigung:", "Pitch:"),
            ["Panel.Rotation.Yaw"] = ("Drehung:", "Yaw:"),
            ["Panel.Rotation.Roll"] = ("Rollen:", "Roll:"),
            ["Panel.Scale"] = ("Größe:", "Scale:"),
            ["Panel.SizeMeters"] = ("Breite×Höhe (m):", "Width×height (m):"),
            ["Panel.HeadLocked"] = ("Kopf-gebunden (folgt der Kopfbewegung statt fest im Cockpit)", "Head-locked (follows head movement instead of staying fixed in the cockpit)"),
            ["Panel.OpaqueBackground"] = ("Schwarz statt Transparenz", "Black instead of transparency"),

            // Global settings expander
            ["Global.Header"] = ("Globale Einstellungen (Feinjustierung, VR Reset, OpenXR Layer)", "Global settings (fine-tuning, VR reset, OpenXR layer)"),
            ["Global.VrReset.Header"] = ("VR Reset (Recenter) — z.B. auf Lenkrad-Taste legen:", "VR reset (recenter) — e.g. assign to a wheel button:"),
            ["Global.VrReset.NoButton"] = ("Kein Knopf zugewiesen.", "No button assigned."),
            ["Global.VrReset.Learn"] = ("Knopf zuweisen…", "Assign button…"),
            ["Global.VrReset.ManualReset"] = ("VR jetzt zurücksetzen", "Reset VR now"),
            ["Global.Nudge.Header"] = ("Controller-Feinjustierung (im Headset nutzbar):", "Controller fine-tuning (usable in headset):"),
            ["Global.Nudge.ActivePanel"] = ("Aktives Panel:", "Active panel:"),
            ["Global.Nudge.StepSize"] = ("Schrittweite:", "Step size:"),
            ["Global.Nudge.Clear"] = ("Löschen", "Clear"),
            ["Global.Nudge.Assign"] = ("Zuweisen…", "Assign…"),
            ["Global.Layer.Header"] = ("OpenXR API Layer:", "OpenXR API layer:"),
            ["Global.Layer.CheckingStatus"] = ("Status wird geprüft…", "Checking status…"),
            ["Global.Layer.Register"] = ("Layer registrieren", "Register layer"),
            ["Global.Layer.Unregister"] = ("Layer entfernen", "Remove layer"),

            // Status/footer
            ["Status.Ready"] = ("Bereit.", "Ready."),

            // Native log expander
            ["NativeLog.Header"] = ("Natives Layer-Log", "Native layer log"),
            ["NativeLog.Refresh"] = ("Aktualisieren", "Refresh"),
            ["NativeLog.VerboseLogging"] = ("Ausführliches Logging (Debug)", "Verbose logging (debug)"),
            ["NativeLog.NoFileFound"] = ("(noch keine Log-Datei gefunden)", "(no log file found yet)"),

            // Input dialog
            ["InputDialog.Ok"] = ("OK", "OK"),
            ["InputDialog.Cancel"] = ("Abbrechen", "Cancel"),
        };

        /// <summary>Looks up the localized text for the given key, falling back to the key itself if missing.</summary>
        public static string T(string key)
        {
            if (!Strings.TryGetValue(key, out var value))
            {
                return key;
            }

            return Instance._language == AppLanguage.English ? value.En : value.De;
        }
    }
}
