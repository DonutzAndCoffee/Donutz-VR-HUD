using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace Donutz_VR_HUD.Views
{
    /// <summary>Small, reusable modal text-input dialog (e.g. for naming a new profile).</summary>
    public partial class InputDialog : Window
    {
        public string InputText
        {
            get => InputTextBox.Text;
            set => InputTextBox.Text = value;
        }

        public InputDialog(string prompt, string title, string initialValue = "")
        {
            InitializeComponent();
            SourceInitialized += (_, _) => ApplyImmersiveDarkTitleBar(this);
            Title = title;
            PromptText.Text = prompt;
            InputTextBox.Text = initialValue;
            Loaded += (_, _) =>
            {
                InputTextBox.Focus();
                InputTextBox.SelectAll();
            };
        }

        private void OkButton_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = true;
        }

        private void CancelButton_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = false;
        }

        /// <summary>
        /// Forces the window's native title bar into dark mode via
        /// DWMWA_USE_IMMERSIVE_DARK_MODE (see MainWindow.xaml.cs for the same helper).
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

        /// <summary>Shows the dialog and returns the entered text, or null if cancelled/closed.</summary>
        public static string? Show(Window owner, string prompt, string title, string initialValue = "")
        {
            var dialog = new InputDialog(prompt, title, initialValue) { Owner = owner };
            return dialog.ShowDialog() == true ? dialog.InputText : null;
        }
    }
}
