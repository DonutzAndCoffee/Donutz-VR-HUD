using System.IO;
using System.Net.Http;
using System.Text.Json;

namespace Donutz_VR_HUD.Services
{
    internal static class ReleaseUpdateChecker
    {
        public const string ReleasesUrl = "https://github.com/DonutzAndCoffee/Donutz-VR-HUD/releases/latest";
        private const string LatestReleaseApi = "https://api.github.com/repos/DonutzAndCoffee/Donutz-VR-HUD/releases/latest";
        private static readonly HttpClient Client = CreateClient();

        private static HttpClient CreateClient()
        {
            var client = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("Donutz-VR-HUD");
            client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            return client;
        }

        public static Task<Version> GetLatestVersionAsync(CancellationToken cancellationToken) =>
            GetLatestVersionAsync(Client, cancellationToken);

        internal static async Task<Version> GetLatestVersionAsync(HttpClient client, CancellationToken cancellationToken)
        {
            using var response = await client.GetAsync(LatestReleaseApi, cancellationToken);
            response.EnsureSuccessStatusCode();
            await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
            using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
            if (document.RootElement.ValueKind != JsonValueKind.Object ||
                !document.RootElement.TryGetProperty("tag_name", out var tagElement) ||
                tagElement.ValueKind != JsonValueKind.String)
            {
                throw new InvalidDataException("The GitHub release response has no tag name.");
            }

            var tag = tagElement.GetString();
            if (tag is null || !tag.StartsWith('v') ||
                !Version.TryParse(tag[1..], out var version) || version.Build < 0 || version.Revision >= 0)
            {
                throw new InvalidDataException($"Unsupported GitHub release tag: {tag}");
            }

            return version;
        }
    }
}
