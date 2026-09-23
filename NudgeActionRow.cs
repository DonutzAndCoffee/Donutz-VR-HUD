using System.ComponentModel;
using Donutz_VR_HUD.Input;

namespace Donutz_VR_HUD
{
    /// <summary>UI row for a single controller-nudge action binding, shown in the fine-tuning section.</summary>
    public sealed class NudgeActionRow : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;

        public NudgeAction Action { get; }
        public string Label { get; }

        private string _statusText = "Kein Knopf zugewiesen.";
        public string StatusText
        {
            get => _statusText;
            set
            {
                if (_statusText == value)
                {
                    return;
                }

                _statusText = value;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(StatusText)));
            }
        }

        public NudgeActionRow(NudgeAction action, string label)
        {
            Action = action;
            Label = label;
        }
    }
}
