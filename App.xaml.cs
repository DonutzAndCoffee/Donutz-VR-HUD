using System.Configuration;
using System.Data;
using System.Windows;
using CefSharp;
using CefSharp.OffScreen;

namespace Donutz_VR_HUD
{
    /// <summary>
    /// Interaction logic for App.xaml
    /// </summary>
    public partial class App : Application
    {
        /// <summary>
        /// Initializes the single, process-wide CEF (Chromium Embedded
        /// Framework) instance used by all <see cref="Sources.WebDashboardSource"/>
        /// panels, exactly like OpenKneeboard's ChromiumApp/ChromiumWorker:
        /// CEF can only be initialized once per process and must be shut
        /// down cleanly before the process exits, so this is owned centrally
        /// here rather than per-panel.
        /// </summary>
        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            Wpf.Ui.Appearance.ApplicationThemeManager.Apply(Wpf.Ui.Appearance.ApplicationTheme.Dark);

            var settings = new CefSettings
            {
                // Off-screen rendering: no native browser window is created;
                // all painting happens via the IRenderHandler passed to each
                // ChromiumWebBrowser, delivering frames directly as GPU
                // shared textures (or raw BGRA buffers as a fallback) rather
                // than depending on a real, DWM-composited window like the
                // previous WebView2-based approach did.
                WindowlessRenderingEnabled = true,
                MultiThreadedMessageLoop = true,
            };
            settings.CefCommandLineArgs["enable-gpu"] = "1";
            settings.CefCommandLineArgs["disable-gpu-shader-disk-cache"] = "1";
            settings.CefCommandLineArgs["off-screen-rendering-enabled"] = "1";

            Cef.Initialize(settings, performDependencyCheck: false, browserProcessHandler: null);
        }

        protected override void OnExit(ExitEventArgs e)
        {
            Cef.Shutdown();
            base.OnExit(e);
        }
    }

}
