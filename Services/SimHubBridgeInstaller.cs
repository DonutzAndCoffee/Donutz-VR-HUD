using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using Microsoft.Win32;

namespace Donutz_VR_HUD.Services
{
    public static class SimHubBridgeInstaller
    {
        public const string FileName = "Donutz.SimHubLedBridge.dll";
        public static string BundledPath => Path.Combine(AppContext.BaseDirectory, "SimHubBridge", FileName);

        public static string? FindInstallDirectory()
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\SimHub");
            var directory = key?.GetValue("InstallDirectory") as string;
            if (!string.IsNullOrWhiteSpace(directory) && IsSimHubDirectory(directory)) return directory;
            var defaultDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "SimHub");
            return IsSimHubDirectory(defaultDirectory) ? defaultDirectory : null;
        }

        public static bool IsSimHubDirectory(string directory) =>
            File.Exists(Path.Combine(directory, "SimHubWPF.exe")) && File.Exists(Path.Combine(directory, "SimHub.Plugins.dll"));

        public static bool IsSimHubRunning()
        {
            var processes = Process.GetProcessesByName("SimHubWPF");
            try { return processes.Length != 0; }
            finally { foreach (var process in processes) process.Dispose(); }
        }

        public static bool IsBundledVersionInstalled(string directory)
        {
            using var bundled = File.OpenRead(BundledPath);
            using var installed = File.OpenRead(Path.Combine(directory, FileName));
            return CryptographicOperations.FixedTimeEquals(SHA256.HashData(bundled), SHA256.HashData(installed));
        }

        public static void Install(string directory) => Install(directory, IsSimHubRunning());

        internal static void Install(string directory, bool simHubRunning)
        {
            if (!IsSimHubDirectory(directory)) throw new InvalidOperationException("The selected folder is not a SimHub installation.");
            if (simHubRunning) throw new InvalidOperationException("Close SimHub completely before installing the bridge.");
            if (!File.Exists(BundledPath)) throw new FileNotFoundException("The bundled bridge is missing. Rebuild or reinstall Donutz VR HUD with the SimHub bridge included.", BundledPath);
            File.Copy(BundledPath, Path.Combine(directory, FileName), true);
        }
    }
}
