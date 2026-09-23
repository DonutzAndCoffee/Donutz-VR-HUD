using System.Globalization;
using System.Windows;
using System.Windows.Data;
using Donutz_VR_HUD.Models;

namespace Donutz_VR_HUD
{
    /// <summary>Binds a RadioButton's IsChecked to whether SourceType matches the given ConverterParameter.</summary>
    public sealed class SourceTypeCheckedConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is PanelSourceType sourceType && parameter is string parameterText &&
                Enum.TryParse<PanelSourceType>(parameterText, out var target))
            {
                return sourceType == target;
            }

            return false;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is bool isChecked && isChecked && parameter is string parameterText &&
                Enum.TryParse<PanelSourceType>(parameterText, out var target))
            {
                return target;
            }

            return Binding.DoNothing;
        }
    }

    /// <summary>Shows an element only when SourceType matches the given ConverterParameter.</summary>
    public sealed class SourceTypeVisibilityConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is PanelSourceType sourceType && parameter is string parameterText &&
                Enum.TryParse<PanelSourceType>(parameterText, out var target))
            {
                return sourceType == target ? Visibility.Visible : Visibility.Collapsed;
            }

            return Visibility.Collapsed;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) =>
            throw new NotSupportedException();
    }

    /// <summary>Inverts a boolean value, e.g. to disable editing controls when a panel is locked.</summary>
    public sealed class InverseBooleanConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture) =>
            value is bool b ? !b : value;

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) =>
            value is bool b ? !b : value;
    }

    /// <summary>
    /// Determines the panel list item border: highlights the currently
    /// selected/edited panel (e.g. via the active-nudge/edit-mode combo box)
    /// in green when unlocked, red when locked. Falls back to gray for
    /// non-active panels.
    /// Values: [0] = the panel this border belongs to, [1] = the currently
    /// active/selected panel, [2] = IsLocked of the panel in [0].
    /// </summary>
    public sealed class ActivePanelBorderBrushConverter : IMultiValueConverter
    {
        public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture)
        {
            var itemPanel = values.Length > 0 ? values[0] : null;
            var activePanel = values.Length > 1 ? values[1] : null;
            var isLocked = values.Length > 2 && values[2] is bool locked && locked;

            if (itemPanel is null || activePanel is null || !ReferenceEquals(itemPanel, activePanel))
            {
                return System.Windows.Media.Brushes.Gray;
            }

            return isLocked ? System.Windows.Media.Brushes.Red : System.Windows.Media.Brushes.LimeGreen;
        }

        public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) =>
            throw new NotSupportedException();
    }

    /// <summary>Converts a "#RRGGBB" hex string (e.g. <see cref="Models.OverlayPanel.VisorColor"/>) to a SolidColorBrush swatch preview.</summary>
    public sealed class HexColorToBrushConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is string hex && !string.IsNullOrWhiteSpace(hex))
            {
                try
                {
                    var color = (System.Windows.Media.Color)System.Windows.Media.ColorConverter.ConvertFromString(
                        hex.StartsWith('#') ? hex : "#" + hex);
                    return new System.Windows.Media.SolidColorBrush(color);
                }
                catch
                {
                    // Fall through to the default brush below.
                }
            }

            return System.Windows.Media.Brushes.Gray;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) =>
            throw new NotSupportedException();
    }
}
