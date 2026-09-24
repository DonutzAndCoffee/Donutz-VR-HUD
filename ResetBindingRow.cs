using System.ComponentModel;

namespace Donutz_VR_HUD
{
    /// <summary>UI row for a single "VR Reset" button binding, shown in the global settings section.</summary>
    public sealed class ResetBindingRow : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        /// <summary>Position of this binding within the underlying binding list, used to remove it.</summary>
        public int Index { get; set; }

        public string DisplayText { get; }

        public ResetBindingRow(int index, string? deviceName, int buttonIndex)
        {
            Index = index;
            DisplayText = $"\"{deviceName}\", Taste {buttonIndex}";
        }
    }
}
