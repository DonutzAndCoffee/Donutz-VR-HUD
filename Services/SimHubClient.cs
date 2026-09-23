using System.Net.Http;
using System.Text.Json;

namespace Donutz_VR_HUD.Services
{
    /// <summary>
    /// Client for reading the currently active game/car from a locally
    /// running SimHub instance (via its built-in REST API, enabled together
    /// with the "Web api"/dashboard HTTP server on port 8888), to make
    /// per-game/per-car profile auto-loading easier to set up (instead of
    /// having to type the game process name and car name manually).
    ///
    /// Verified against a running SimHub install: SimHub's web server
    /// exposes <c>GET /api/GetGamedata</c>, returning a JSON document with
    /// (among others) top-level <c>GameName</c>/<c>GameRunning</c> fields
    /// and, while a session is active, a nested <c>NewData.CarModel</c>
    /// field (backed by <c>GameReaderCommon.StatusDataBase.CarModel</c>).
    /// If SimHub isn't running or isn't reachable, all members here fail
    /// quietly and return null -- the game/car can always be entered
    /// manually as a reliable fallback (see Profile.GameProcessName /
    /// Profile.CarModel).
    /// </summary>
    public static class SimHubClient
    {
        private static readonly HttpClient Client = new() { Timeout = TimeSpan.FromMilliseconds(750) };

        // SimHub's built-in web server (used for URL dashboards and its
        // REST API) defaults to this port; see Models/OverlayPanel.cs's
        // default dashboard URL.
        private const string BaseUrl = "http://127.0.0.1:8888";

        /// <summary>Snapshot of the game/car SimHub currently reports.</summary>
        public sealed record SimHubGameState(string? GameName, bool GameRunning, string? CarModel);

        /// <summary>
        /// Tries to read the current game name and car model from SimHub in
        /// a single request. Returns null if SimHub isn't running or isn't
        /// reachable.
        /// </summary>
        public static async Task<SimHubGameState?> TryGetGameStateAsync(CancellationToken cancellationToken = default)
        {
            try
            {
                using var response = await Client.GetAsync($"{BaseUrl}/api/GetGamedata", cancellationToken).ConfigureAwait(false);
                if (!response.IsSuccessStatusCode)
                {
                    return null;
                }

                var content = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
                if (string.IsNullOrWhiteSpace(content))
                {
                    return null;
                }

                using var document = JsonDocument.Parse(content);
                var root = document.RootElement;

                string? gameName = root.TryGetProperty("GameName", out var gameNameElement) && gameNameElement.ValueKind == JsonValueKind.String
                    ? gameNameElement.GetString()
                    : null;

                var gameRunning = root.TryGetProperty("GameRunning", out var gameRunningElement) &&
                                   gameRunningElement.ValueKind == JsonValueKind.True;

                string? carModel = null;
                if (root.TryGetProperty("NewData", out var newDataElement) && newDataElement.ValueKind == JsonValueKind.Object &&
                    newDataElement.TryGetProperty("CarModel", out var carModelElement) && carModelElement.ValueKind == JsonValueKind.String)
                {
                    carModel = carModelElement.GetString();
                }

                return new SimHubGameState(gameName, gameRunning, carModel);
            }
            catch
            {
                // SimHub not running, web server not enabled, network
                // hiccup, etc. -- not fatal, just means auto-detection isn't
                // available right now.
                return null;
            }
        }

        /// <summary>
        /// Tries to read just the current car's name from SimHub. Returns
        /// null if SimHub isn't running/reachable or no car is currently
        /// loaded (e.g. main menu, no active session).
        /// </summary>
        public static async Task<string?> TryGetCurrentCarNameAsync(CancellationToken cancellationToken = default)
        {
            var state = await TryGetGameStateAsync(cancellationToken).ConfigureAwait(false);
            return string.IsNullOrWhiteSpace(state?.CarModel) ? null : state.CarModel;
        }
    }
}
