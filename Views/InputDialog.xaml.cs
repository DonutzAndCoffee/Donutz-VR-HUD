using System.Windows;

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

        /// <summary>Shows the dialog and returns the entered text, or null if cancelled/closed.</summary>
        public static string? Show(Window owner, string prompt, string title, string initialValue = "")
        {
            var dialog = new InputDialog(prompt, title, initialValue) { Owner = owner };
            return dialog.ShowDialog() == true ? dialog.InputText : null;
        }
    }
}
