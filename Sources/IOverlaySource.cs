namespace Donutz_VR_HUD.Sources
{
    /// <summary>
    /// Common contract for anything that can feed the OpenXR overlay
    /// (<see cref="OpenXR.OpenXrOverlay"/>) with a live D3D11 texture stream —
    /// e.g. a captured desktop window (<see cref="Capture.WindowCapture"/>) or a
    /// web-based dashboard rendered via CEF/CefSharp off-screen rendering
    /// (<see cref="WebDashboardSource"/>).
    /// </summary>
    public interface IOverlaySource : IDisposable
    {
        /// <summary>Native ID3D11Device pointer backing this source.</summary>
        nint DevicePtr { get; }

        /// <summary>Native ID3D11DeviceContext pointer backing this source.</summary>
        nint ContextPtr { get; }

        /// <summary>
        /// Raised whenever a new frame is available. The string is the name
        /// of the cross-process NT shared handle (see
        /// IDXGIResource1::CreateSharedHandle) backing the frame's D3D11
        /// texture; a raw handle value would only be valid within this
        /// process, so the native OpenXR layer (running inside a different
        /// process, e.g. iRacing.exe) opens the texture by name instead via
        /// ID3D11Device1::OpenSharedResourceByName.
        /// </summary>
        event Action<string, int, int>? FrameArrived;
    }
}
