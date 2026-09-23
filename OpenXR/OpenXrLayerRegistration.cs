using System.IO;
using Microsoft.Win32;

namespace Donutz_VR_HUD.OpenXR
{
    /// <summary>
    /// Registers/unregisters the Donutz VR HUD native OpenXR API layer
    /// (see ../NativeLayer/README.md), equivalent to running
    /// NativeLayer/register-dev-layer.ps1, but directly from the app UI so
    /// the user does not need a separate script/terminal step.
    ///
    /// Registered under HKEY_LOCAL_MACHINE, matching every other implicit
    /// OpenXR API layer observed working alongside iRacing on this machine
    /// (OpenKneeboard, XRFrameTools, OpenXR-Toolkit, etc. are all under
    /// HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit). A per-user
    /// (HKCU) registration was tried first, but iRacing's OpenXR loader
    /// never negotiated the layer with it — only HKLM registration is
    /// reliably picked up. Writing to HKLM requires the app to run
    /// elevated (as Administrator).
    /// </summary>
    public static class OpenXrLayerRegistration
    {
        private const string RegistryKeyPath = @"Software\Khronos\OpenXR\1\ApiLayers\Implicit";
        private const string ManifestFileName = "XR_APILAYER_DONUTZ_vrhud.json";

        /// <summary>
        /// Full path to the built layer manifest next to this app's own
        /// executable (NativeLayer's build output is expected to be copied
        /// or deployed alongside Donutz VR HUD.exe; see NativeLayer/README.md).
        /// </summary>
        public static string ManifestPath =>
            Path.Combine(AppContext.BaseDirectory, ManifestFileName);

        public static bool IsManifestPresent => File.Exists(ManifestPath);

        public static bool IsRegistered()
        {
            using var hklmKey = Registry.LocalMachine.OpenSubKey(RegistryKeyPath, writable: false);
            if (hklmKey?.GetValue(ManifestPath) is not null)
            {
                return true;
            }

            // Older versions of this app registered under HKCU; still
            // report as "registered" so the UI reflects reality, even
            // though iRacing's loader does not honor it.
            using var hkcuKey = Registry.CurrentUser.OpenSubKey(RegistryKeyPath, writable: false);
            return hkcuKey?.GetValue(ManifestPath) is not null;
        }

        public static void Register()
        {
            if (!IsManifestPresent)
            {
                throw new FileNotFoundException(
                    $"Layer-Manifest nicht gefunden: '{ManifestPath}'. Das native Layer-Projekt (NativeLayer/) muss zuerst gebaut und dessen Ausgabe (DLL + JSON) neben '{Path.GetFileName(Environment.ProcessPath) ?? "Donutz VR HUD.exe"}' bereitgestellt werden.",
                    ManifestPath);
            }

            try
            {
                using var key = Registry.LocalMachine.CreateSubKey(RegistryKeyPath, writable: true);
                key.SetValue(ManifestPath, 0, RegistryValueKind.DWord);
            }
            catch (UnauthorizedAccessException ex)
            {
                throw new UnauthorizedAccessException(
                    "Schreibzugriff auf HKEY_LOCAL_MACHINE verweigert. Bitte 'Donutz VR HUD' als Administrator starten, um den OpenXR API Layer zu registrieren " +
                    "(iRacings OpenXR-Loader berücksichtigt nur systemweite HKLM-Registrierungen, nicht HKCU).",
                    ex);
            }

            // Clean up any stale HKCU registration from earlier versions so
            // there is only ever one source of truth.
            using var hkcuKey = Registry.CurrentUser.OpenSubKey(RegistryKeyPath, writable: true);
            hkcuKey?.DeleteValue(ManifestPath, throwOnMissingValue: false);
        }

        public static void Unregister()
        {
            try
            {
                using var hklmKey = Registry.LocalMachine.OpenSubKey(RegistryKeyPath, writable: true);
                hklmKey?.DeleteValue(ManifestPath, throwOnMissingValue: false);
            }
            catch (UnauthorizedAccessException ex)
            {
                throw new UnauthorizedAccessException(
                    "Schreibzugriff auf HKEY_LOCAL_MACHINE verweigert. Bitte 'Donutz VR HUD' als Administrator starten, um den OpenXR API Layer zu entfernen.",
                    ex);
            }

            using var hkcuKey = Registry.CurrentUser.OpenSubKey(RegistryKeyPath, writable: true);
            hkcuKey?.DeleteValue(ManifestPath, throwOnMissingValue: false);
        }
    }
}

