using System.Configuration;
using System.Data;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
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
        private const string SingleInstanceMutexName = @"Local\DonutzVRHUD_SingleInstance";
        private static Mutex? s_singleInstanceMutex;
        private bool _cefInitialized;

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        [DllImport("user32.dll")]
        private static extern bool IsIconic(IntPtr hWnd);

        private const int SW_RESTORE = 9;

        private static void ActivateExistingInstance()
        {
            try
            {
                using var current = Process.GetCurrentProcess();
                foreach (var p in Process.GetProcessesByName(current.ProcessName))
                {
                    using (p)
                    {
                        if (p.Id == current.Id) continue;
                        var hWnd = p.MainWindowHandle;
                        if (hWnd == IntPtr.Zero) continue;
                        if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
                        SetForegroundWindow(hWnd);
                        break;
                    }
                }
            }
            catch
            {
            }
        }

        protected override void OnStartup(StartupEventArgs e)
        {
            s_singleInstanceMutex = new Mutex(true, SingleInstanceMutexName, out bool createdNew);
            if (!createdNew)
            {
                s_singleInstanceMutex.Dispose();
                s_singleInstanceMutex = null;
                ActivateExistingInstance();
                Shutdown(0);
                return;
            }

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
            _cefInitialized = true;
        }

        protected override void OnExit(ExitEventArgs e)
        {
            if (_cefInitialized)
                Cef.Shutdown();
            if (s_singleInstanceMutex != null)
            {
                s_singleInstanceMutex.ReleaseMutex();
                s_singleInstanceMutex.Dispose();
                s_singleInstanceMutex = null;
            }
            base.OnExit(e);
        }
    }

}
