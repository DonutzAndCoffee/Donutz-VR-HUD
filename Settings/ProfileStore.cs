using System.IO;
using System.Text.Json;

namespace Donutz_VR_HUD.Settings
{
    /// <summary>Loads/saves the list of user-defined <see cref="Profile"/>s as JSON in the user's local app data folder.</summary>
    public static class ProfileStore
    {
        private static readonly string SettingsDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Donutz VR HUD");

        private static readonly string ProfilesFilePath = Path.Combine(SettingsDirectory, "profiles.json");

        private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

        public static List<Profile> Load()
        {
            try
            {
                if (!File.Exists(ProfilesFilePath))
                {
                    return new List<Profile>();
                }

                var json = File.ReadAllText(ProfilesFilePath);
                return JsonSerializer.Deserialize<List<Profile>>(json, JsonOptions) ?? new List<Profile>();
            }
            catch
            {
                // Corrupt or unreadable profiles file: fall back to an empty list rather than crashing on startup.
                return new List<Profile>();
            }
        }

        public static void Save(List<Profile> profiles)
        {
            try
            {
                Directory.CreateDirectory(SettingsDirectory);
                var json = JsonSerializer.Serialize(profiles, JsonOptions);
                File.WriteAllText(ProfilesFilePath, json);
            }
            catch
            {
                // Best-effort: failing to persist profiles should not crash the app.
            }
        }
    }
}
