using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Navigation;

namespace Donutz_VR_HUD.Views
{
    /// <summary>
    /// "About" dialog showing app version, copyright, license (CC BY-NC 4.0)
    /// and third-party component attributions, so the license info from
    /// README.md/LICENSE is also reachable directly from the GUI.
    /// </summary>
    public partial class AboutWindow : Window
    {
        public AboutWindow()
        {
            InitializeComponent();
            SourceInitialized += (_, _) => ApplyImmersiveDarkTitleBar(this);

            var version = Assembly.GetExecutingAssembly().GetName().Version;
            VersionText.Text = version is null ? string.Empty : $"Version {version.ToString(3)}";
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e)
        {
            Close();
        }

        private void Hyperlink_RequestNavigate(object sender, RequestNavigateEventArgs e)
        {
            Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
            e.Handled = true;
        }

        /// <summary>
        /// Forces the window's native title bar into dark mode via
        /// DWMWA_USE_IMMERSIVE_DARK_MODE (same helper as MainWindow.xaml.cs/InputDialog.xaml.cs).
        /// </summary>
        private static void ApplyImmersiveDarkTitleBar(Window window)
        {
            var hwnd = new WindowInteropHelper(window).EnsureHandle();
            int useImmersiveDarkMode = 1;
            const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
            const int DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;

            if (DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref useImmersiveDarkMode, sizeof(int)) != 0)
            {
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1, ref useImmersiveDarkMode, sizeof(int));
            }
        }

        [DllImport("dwmapi.dll", PreserveSig = true)]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int pvAttribute, int cbAttribute);
    }
}
