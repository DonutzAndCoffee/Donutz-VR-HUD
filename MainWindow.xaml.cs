using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Donutz_VR_HUD.Capture;
using Donutz_VR_HUD.Input;
using Donutz_VR_HUD.Models;
using Donutz_VR_HUD.OpenXR;
using Donutz_VR_HUD.Services;
using Donutz_VR_HUD.Settings;
using Donutz_VR_HUD.Sources;
using Donutz_VR_HUD.Views;

namespace Donutz_VR_HUD
{
    /// <summary>Whether keyboard edit-mode currently moves the panel in space or rotates it.</summary>
    public enum PanelEditAxisMode
    {
        Move,
        Rotate,
        Scale
    }

    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        private const int MaxPanels = 5;

        private readonly Dictionary<Guid, IOverlaySource> _panelSources = new();
        private readonly ResetButtonBinding _resetButtonBinding = new();
        private readonly PanelNudgeController _panelNudgeController = new();
        private readonly DispatcherTimer _nativeLogTimer = new() { Interval = TimeSpan.FromSeconds(1) };
        private readonly DispatcherTimer _autoProfileTimer = new() { Interval = TimeSpan.FromSeconds(3) };
        private long _nativeLogLastLength = -1;
        private OverlayIpcClient? _ipcClient;
        private bool _isLoadingSettings;
        private bool _isApplyingProfile;
        private bool _autoLoadProfiles;
        private bool _autoSaveProfileOnSimExit;
        private bool _wasSimRunning;
        private bool _closeToTray;
        private bool _isExiting;
        private System.Windows.Forms.NotifyIcon? _trayIcon;
        private OverlayStatus _overlayStatus = OverlayStatus.Stopped;

        /// <summary>
        /// Visual/logical state shown by the single combined overlay
        /// status/action button (replaces the former separate "Overlay
        /// starten"/"Overlay stoppen" buttons): gray = stopped, yellow =
        /// waiting/connecting, green = active, red = an error occurred.
        /// </summary>
        private enum OverlayStatus
        {
            Stopped,
            Waiting,
            Active,
            Error
        }
        private Guid? _lastAutoLoadedProfileId;
        private static readonly float[] NudgeStepSizes = { 0.001f, 0.01f, 0.05f };
        private int _nudgeStepIndex = 0;

        // Keyboard-driven "edit mode": lets the user nudge the active,
        // unlocked panel with WASD/QE while the app window has focus,
        // switch between moving and rotating with Tab, and hold Shift/Alt
        // for finer/coarser steps, with a live on-screen HUD showing the
        // current panel, mode, step size and key legend.
        private readonly DispatcherTimer _editModeTimer = new() { Interval = TimeSpan.FromMilliseconds(33) };
        private readonly HashSet<Key> _editModePressedKeys = new();
        private bool _isEditModeActive;
        private PanelEditAxisMode _editAxisMode = PanelEditAxisMode.Move;

        // The keyboard edit-mode HUD is also shown as an actual head-locked
        // VR overlay panel (not just the desktop window's HUD border), so
        // it's visible while wearing the headset. It's tracked separately
        // from the user-managed Panels collection since it's not something
        // the user configures/saves as part of a profile.
        private static readonly Guid EditModeHudPanelId = Guid.Parse("6f2f6b3a-6e63-4b0a-9a63-1f7f5c9d8a11");
        private HudTextSource? _editModeHudSource;

        // Mirrors the colored panel-list border (red = locked, green =
        // unlocked) as an actual VR overlay quad, placed directly in front
        // of/around the currently active edit-mode panel, so the highlight
        // is visible inside the headset and not just in the desktop UI.
        private static readonly Guid EditModeHighlightPanelId = Guid.Parse("9d6a2e3c-6a8a-4a2d-9e1c-2a5b7c9d0f22");
        private Sources.PanelHighlightSource? _editModeHighlightSource;

        public ObservableCollection<OverlayPanel> Panels { get; } = new();
        public ObservableCollection<CapturableWindow> AvailableWindows { get; } = new();
        public ObservableCollection<Profile> Profiles { get; } = new();
        public ObservableCollection<string> RunningProcessNames { get; } = new();
        public ObservableCollection<NudgeActionRow> NudgeActionRows { get; } = new();
        public ObservableCollection<ResetBindingRow> ResetBindingRows { get; } = new();

        public MainWindow()
        {
            InitializeComponent();
            //Wpf.Ui.Appearance.ApplicationThemeManager.Apply(this);
            // ApplicationThemeManager.Apply(...) is supposed to also flip the
            // window's title bar to dark via DWM, but that didn't actually
            // happen in testing (likely a WPF-UI/Windows-build quirk), so the
            // DWM "immersive dark mode" attribute is now set directly via
            // P/Invoke instead, once the native window handle exists
            // (SourceInitialized, same timing reasoning as before).
            SourceInitialized += (_, _) => ApplyImmersiveDarkTitleBar(this);

            DataContext = this;
            PanelsItemsControl.ItemsSource = Panels;
            ProfilesComboBox.ItemsSource = Profiles;

            Title = $"Donutz VR HUD (Build {GetBuildTimestamp():yyyy-MM-dd HH:mm:ss})";

            RefreshWindowList();
            RefreshRunningProcessNames();
            InitializeNudgeActionRows();

            _resetButtonBinding.ButtonPressed += () => Dispatcher.Invoke(() => _ipcClient?.Recenter());
            _resetButtonBinding.Learned += (device, buttonIndex) => Dispatcher.Invoke(() =>
            {
                RefreshResetBindingRows();
                LearnResetButton.IsEnabled = true;
                LearnResetButton.Content = "Assign button…";
                SaveSettings();
            });

            _panelNudgeController.ActionHeld += action => Dispatcher.Invoke(() => ApplyNudgeAction(action));
            _panelNudgeController.Learned += (action, device, buttonIndex) => Dispatcher.Invoke(() =>
            {
                var row = NudgeActionRows.FirstOrDefault(r => r.Action == action);
                if (row is not null)
                {
                    row.StatusText = $"\"{device.Name}\", button {buttonIndex}";
                }

                NudgeStatusText.Text = $"{GetNudgeActionLabel(action)} assigned: \"{device.Name}\", button {buttonIndex}.";
                SaveSettings();
            });

            LoadSettings();
            ApplyLocalization();
            LoadProfiles();
            RefreshLayerStatus();
            LoadVerboseLoggingState();

            _nativeLogTimer.Tick += (_, _) => RefreshNativeLog();
            _nativeLogTimer.Start();
            RefreshNativeLog();

            _autoProfileTimer.Tick += async (_, _) => await AutoProfileTimer_TickAsync();
            _autoProfileTimer.Start();

            _editModeTimer.Tick += (_, _) => EditModeTimer_Tick();
            PreviewKeyDown += MainWindow_PreviewKeyDown;
            PreviewKeyUp += MainWindow_PreviewKeyUp;
        }

        /// <summary>
        /// Applies the static edit-mode HUD labels/legend in the desktop GUI (and
        /// refreshes the dynamic parts, including the VR overlay HUD shown
        /// during panel adjustment).
        /// </summary>
        private void ApplyLocalization()
        {
            EditModeToggleCheckBox.Content = Localization.T("EditMode.ToggleCheckBox");

            EditModeHudTitleText.Text = Localization.T("EditMode.Title");
            EditModeHudPanelLabelText.Text = Localization.T("EditMode.PanelLabel");
            EditModeHudModeLabelText.Text = Localization.T("EditMode.ModeLabel");
            EditModeHudSpeedLabelText.Text = Localization.T("EditMode.SpeedLabel");
            EditModeHudLegendMoveText.Text = Localization.T("EditMode.Legend.Move");
            EditModeHudLegendRollText.Text = Localization.T("EditMode.Legend.RollUpDown");
            EditModeHudLegendTabText.Text = Localization.T("EditMode.Legend.Tab");
            EditModeHudLegendSwitchText.Text = Localization.T("EditMode.Legend.SwitchPanel");
            EditModeHudLegendLockText.Text = Localization.T("EditMode.Legend.Lock");
            EditModeHudLegendSpeedKeysText.Text = Localization.T("EditMode.Legend.SpeedKeys");
            EditModeHudLegendEscText.Text = Localization.T("EditMode.Legend.Esc");

            UpdateEditModeHud();
        }

        private void RefreshNativeLogButton_Click(object sender, RoutedEventArgs e)
        {
            // Force a re-read even if the file size didn't change, in case
            // the user just wants to confirm the file still doesn't exist.
            _nativeLogLastLength = -1;
            RefreshNativeLog();
        }

        /// <summary>
        /// Reads the native layer's current log level from
        /// <see cref="OverlayIpcClient.NativeLayerLogLevelPath"/> (defaults
        /// to Info/unchecked if the file doesn't exist yet) and reflects it
        /// in <c>VerboseLoggingCheckBox</c> without triggering the
        /// Checked/Unchecked handler (which would immediately rewrite the
        /// file).
        /// </summary>
        private void LoadVerboseLoggingState()
        {
            bool isDebug = false;
            try
            {
                if (File.Exists(OverlayIpcClient.NativeLayerLogLevelPath))
                {
                    isDebug = File.ReadAllText(OverlayIpcClient.NativeLayerLogLevelPath).Trim()
                        .Equals("Debug", StringComparison.OrdinalIgnoreCase);
                }
            }
            catch (IOException)
            {
            }

            VerboseLoggingCheckBox.Checked -= VerboseLoggingCheckBox_Changed;
            VerboseLoggingCheckBox.Unchecked -= VerboseLoggingCheckBox_Changed;
            VerboseLoggingCheckBox.IsChecked = isDebug;
            VerboseLoggingCheckBox.Checked += VerboseLoggingCheckBox_Changed;
            VerboseLoggingCheckBox.Unchecked += VerboseLoggingCheckBox_Changed;
        }

        /// <summary>
        /// Writes the selected log level ("Debug" or "Info") to
        /// <see cref="OverlayIpcClient.NativeLayerLogLevelPath"/>, which the
        /// native layer (see NativeLayer/Logging.cpp CurrentLogLevel())
        /// re-reads on every log call. This is the only way to control the
        /// native layer's verbosity from here, since this app does not
        /// launch the host application (e.g. iRacing.exe) itself and thus
        /// cannot set an environment variable for it.
        /// </summary>
        private void VerboseLoggingCheckBox_Changed(object sender, RoutedEventArgs e)
        {
            try
            {
                var path = OverlayIpcClient.NativeLayerLogLevelPath;
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, VerboseLoggingCheckBox.IsChecked == true ? "Debug" : "Info");
            }
            catch (IOException)
            {
            }
        }

        /// <summary>
        /// Polls the native layer's diagnostic log file (see
        /// NativeLayer/Logging.cpp) and mirrors its content into
        /// <see cref="NativeLogTextBox"/>, so the user can see directly in
        /// the app whether iRacing ever loaded/negotiated the native OpenXR
        /// API layer, without having to open the file manually. The file is
        /// opened with FileShare.ReadWrite since the native layer keeps it
        /// open for appending while a host application is running.
        /// </summary>
        private void RefreshNativeLog()
        {
            var path = OverlayIpcClient.NativeLayerLogPath;
            try
            {
                if (!File.Exists(path))
                {
                    if (_nativeLogLastLength != 0)
                    {
                        NativeLogTextBox.Text = $"(file does not exist: {path})";
                        _nativeLogLastLength = 0;
                    }
                    return;
                }

                var info = new FileInfo(path);
                if (info.Length == _nativeLogLastLength)
                {
                    return;
                }

                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var reader = new StreamReader(stream);
                NativeLogTextBox.Text = reader.ReadToEnd();
                NativeLogTextBox.CaretIndex = NativeLogTextBox.Text.Length;
                NativeLogTextBox.ScrollToEnd();
                _nativeLogLastLength = info.Length;
            }
            catch (IOException)
            {
                // File is momentarily locked by the native layer while it
                // appends a line; just retry on the next timer tick.
            }
        }

        private void RefreshLayerStatus()
        {
            if (!OpenXrLayerRegistration.IsManifestPresent)
            {
                LayerStatusText.Text = $"Layer-Manifest nicht gefunden ({OpenXrLayerRegistration.ManifestPath}). Natives Layer-Projekt (NativeLayer/) muss zuerst gebaut und dessen Ausgabe neben dieser .exe abgelegt werden.";
                RegisterLayerButton.IsEnabled = false;
                UnregisterLayerButton.IsEnabled = false;
                return;
            }

            RegisterLayerButton.IsEnabled = true;

            if (OpenXrLayerRegistration.IsRegistered())
            {
                LayerStatusText.Text = "OpenXR API layer is registered (HKEY_LOCAL_MACHINE). Overlays should appear in supported titles (e.g. iRacing).";
                UnregisterLayerButton.IsEnabled = true;
            }
            else
            {
                LayerStatusText.Text = "OpenXR API layer is not yet registered.";
                UnregisterLayerButton.IsEnabled = false;
            }
        }

        private void RegisterLayerButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                OpenXrLayerRegistration.Register();
                StatusText.Text = "OpenXR API layer registered (HKEY_LOCAL_MACHINE).";
            }
            catch (UnauthorizedAccessException)
            {
                if (MessageBox.Show(
                        "Administrator rights are required to register the OpenXR API layer (HKEY_LOCAL_MACHINE). " +
                        "Do you want to restart the app as administrator now?",
                        "Administrator rights required",
                        MessageBoxButton.YesNo,
                        MessageBoxImage.Question) == MessageBoxResult.Yes)
                {
                    RestartElevated();
                }
                else
                {
                    StatusText.Text = "Layer registration cancelled: administrator rights required.";
                }
            }
            catch (Exception ex)
            {
                StatusText.Text = $"Layer registration failed: {ex.Message}";
            }
            finally
            {
                RefreshLayerStatus();
            }
        }

        private void UnregisterLayerButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                OpenXrLayerRegistration.Unregister();
                StatusText.Text = "OpenXR API layer removed.";
            }
            catch (UnauthorizedAccessException)
            {
                if (MessageBox.Show(
                        "Administrator rights are required to remove the OpenXR API layer (HKEY_LOCAL_MACHINE). " +
                        "Do you want to restart the app as administrator now?",
                        "Administrator rights required",
                        MessageBoxButton.YesNo,
                        MessageBoxImage.Question) == MessageBoxResult.Yes)
                {
                    RestartElevated();
                }
                else
                {
                    StatusText.Text = "Layer removal cancelled: administrator rights required.";
                }
            }
            catch (Exception ex)
            {
                StatusText.Text = $"Layer removal failed: {ex.Message}";
            }
            finally
            {
                RefreshLayerStatus();
            }
        }

        /// <summary>
        /// Relaunches this app elevated (UAC prompt) via the "runas" verb,
        /// then exits the current, non-elevated instance. Needed because
        /// registering the OpenXR API layer under HKEY_LOCAL_MACHINE
        /// requires admin rights on this machine (iRacing's OpenXR loader
        /// does not honor a per-user HKCU registration).
        /// </summary>
        private void RestartElevated()
        {
            try
            {
                var exePath = Environment.ProcessPath;
                if (string.IsNullOrEmpty(exePath))
                {
                    StatusText.Text = "Restart as administrator failed: could not determine path to the .exe.";
                    return;
                }

                var startInfo = new System.Diagnostics.ProcessStartInfo(exePath)
                {
                    UseShellExecute = true,
                    Verb = "runas",
                };
                System.Diagnostics.Process.Start(startInfo);
                Application.Current.Shutdown();
            }
            catch (System.ComponentModel.Win32Exception)
            {
                // UAC prompt was cancelled by the user.
                StatusText.Text = "Restart as administrator cancelled.";
            }
        }

        private void LoadSettings()
        {
            _isLoadingSettings = true;
            try
            {
                var settings = SettingsStore.Load();

                if (settings.Panels.Count == 0)
                {
                    AddPanelButton_Click(this, new RoutedEventArgs());
                }
                else
                {
                    foreach (var panelSettings in settings.Panels.Take(MaxPanels))
                    {
                        var panel = panelSettings.ToPanel();
                        panel.PropertyChanged += Panel_PropertyChanged;
                        Panels.Add(panel);
                        PushTransform(panel);
                    }
                }

                var resetBindings = settings.ResetBindings.Count > 0
                    ? settings.ResetBindings
                    : (settings.ResetBinding is { ButtonIndex: >= 0 } legacyResetBinding ? new List<ResetBindingSettings> { legacyResetBinding } : new List<ResetBindingSettings>());

                _resetButtonBinding.ClearBindings();
                foreach (var resetBinding in resetBindings.Where(b => b.ButtonIndex >= 0))
                {
                    _resetButtonBinding.AddBinding(resetBinding.DeviceInstanceGuid, resetBinding.ButtonIndex, resetBinding.DeviceName);
                }
                RefreshResetBindingRows();

                foreach (var (actionName, binding) in settings.NudgeBindings)
                {
                    if (binding.ButtonIndex < 0 || !Enum.TryParse<NudgeAction>(actionName, out var action))
                    {
                        continue;
                    }

                    _panelNudgeController.Bind(action, binding.DeviceInstanceGuid, binding.ButtonIndex, binding.DeviceName);
                    var row = NudgeActionRows.FirstOrDefault(r => r.Action == action);
                    if (row is not null)
                    {
                        row.StatusText = $"\"{binding.DeviceName}\", button {binding.ButtonIndex}";
                    }
                }
            }
            finally
            {
                _isLoadingSettings = false;
            }
        }

        private void SaveSettings()
        {
            if (_isLoadingSettings)
            {
                return;
            }

            var settings = new AppSettings
            {
                Panels = Panels.Select(PanelSettings.FromPanel).ToList(),
                AutoLoadProfiles = _autoLoadProfiles,
                AutoSaveProfileOnSimExit = _autoSaveProfileOnSimExit,
                CloseToTray = _closeToTray
            };

            settings.ResetBindings = _resetButtonBinding.Bindings
                .Select(b => new ResetBindingSettings
                {
                    DeviceInstanceGuid = b.DeviceInstanceGuid,
                    DeviceName = b.DeviceName ?? string.Empty,
                    ButtonIndex = b.ButtonIndex
                })
                .ToList();

            foreach (var (action, binding) in _panelNudgeController.Bindings)
            {
                settings.NudgeBindings[action.ToString()] = new NudgeBindingSettings
                {
                    DeviceInstanceGuid = binding.DeviceInstanceGuid,
                    DeviceName = binding.DeviceName,
                    ButtonIndex = binding.ButtonIndex
                };
            }

            SettingsStore.Save(settings);
        }

        /// <summary>
        /// Forces the window's native title bar into dark mode via
        /// DWMWA_USE_IMMERSIVE_DARK_MODE. Falls back to the pre-20H1 constant
        /// (19) if the current one is rejected, since older Windows 10
        /// Insider builds used a different attribute id for the same thing.
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

        private void RefreshWindowList()
        {
            AvailableWindows.Clear();
            foreach (var window in WindowEnumerator.GetCapturableWindows())
            {
                AvailableWindows.Add(window);
            }
        }

        private void RefreshRunningProcessNames()
        {
            RunningProcessNames.Clear();
            try
            {
                var names = System.Diagnostics.Process.GetProcesses()
                    .Select(p =>
                    {
                        try { return p.ProcessName; }
                        catch { return null; }
                    })
                    .Where(n => !string.IsNullOrWhiteSpace(n))
                    .Distinct(StringComparer.OrdinalIgnoreCase)
                    .OrderBy(n => n, StringComparer.OrdinalIgnoreCase);

                foreach (var name in names)
                {
                    RunningProcessNames.Add(name!);
                }
            }
            catch
            {
                // Best-effort only; leave the list empty if enumeration fails.
            }
        }

        private void LoadProfiles()
        {
            Profiles.Clear();
            foreach (var profile in ProfileStore.Load())
            {
                Profiles.Add(profile);
            }

            var settings = SettingsStore.Load();
            _autoLoadProfiles = settings.AutoLoadProfiles;
            AutoLoadProfilesCheckBox.IsChecked = _autoLoadProfiles;
            _autoSaveProfileOnSimExit = settings.AutoSaveProfileOnSimExit;
            AutoSaveProfileOnSimExitCheckBox.IsChecked = _autoSaveProfileOnSimExit;
            _closeToTray = settings.CloseToTray;
            CloseToTrayCheckBox.IsChecked = _closeToTray;
        }

        /// <summary>
        /// Builds a <see cref="Profile"/> snapshot of the currently active
        /// panels/reset binding, reusing the same per-panel serialization as
        /// the regular application settings.
        /// </summary>
        private Profile BuildProfileFromCurrentState(string name, Guid? existingId = null)
        {
            var profile = new Profile
            {
                Id = existingId ?? Guid.NewGuid(),
                Name = name,
                GameProcessName = string.IsNullOrWhiteSpace(ProfileGameComboBox.Text) ? null : ProfileGameComboBox.Text.Trim(),
                CarModel = string.IsNullOrWhiteSpace(ProfileCarTextBox.Text) ? null : ProfileCarTextBox.Text.Trim(),
                IsDefault = ProfileIsDefaultCheckBox.IsChecked == true,
                Panels = Panels.Select(PanelSettings.FromPanel).ToList()
            };

            profile.ResetBindings = _resetButtonBinding.Bindings
                .Select(b => new ResetBindingSettings
                {
                    DeviceInstanceGuid = b.DeviceInstanceGuid,
                    DeviceName = b.DeviceName ?? string.Empty,
                    ButtonIndex = b.ButtonIndex
                })
                .ToList();

            return profile;
        }

        private void SaveProfileAsButton_Click(object sender, RoutedEventArgs e)
        {
            // Suggest a sensible default name so an existing profile (e.g.
            // for a different car) can quickly be "cloned" for the
            // currently configured car: prefer the car name, falling back
            // to the name of the profile that was used as a starting point.
            var suggestedName = !string.IsNullOrWhiteSpace(ProfileCarTextBox.Text)
                ? ProfileCarTextBox.Text.Trim()
                : (ProfilesComboBox.SelectedItem as Profile)?.Name ?? string.Empty;

            var name = InputDialog.Show(this, "Name des neuen Profils:", "Profil speichern unter…", suggestedName);
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            if (!RequireGameAndCarOrDefault())
            {
                return;
            }

            var profile = BuildProfileFromCurrentState(name.Trim());
            EnforceSingleDefaultProfile(profile);
            Profiles.Add(profile);
            ProfileStore.Save(Profiles.ToList());
            ProfilesComboBox.SelectedItem = profile;
            ProfileStatusText.Text = $"Profil \"{profile.Name}\" gespeichert.";
        }

        /// <summary>
        /// Since this app is meant to support multiple simulations, a
        /// profile that only names a car (or nothing at all) is ambiguous
        /// -- the same car name can exist in several sims, and without the
        /// game/process name attached, auto-loading/-saving can match the
        /// wrong profile (see the Caterham mix-up between iRacing car
        /// variants). So both "Spiel" and "Fahrzeug" are required before
        /// saving/updating a profile, unless it's explicitly marked as the
        /// catch-all default/fallback profile (which is intentionally
        /// game-/car-agnostic).
        /// </summary>
        private bool RequireGameAndCarOrDefault()
        {
            if (ProfileIsDefaultCheckBox.IsChecked == true)
            {
                return true;
            }

            if (!string.IsNullOrWhiteSpace(ProfileGameComboBox.Text) && !string.IsNullOrWhiteSpace(ProfileCarTextBox.Text))
            {
                return true;
            }

            ProfileStatusText.Text = "Bitte sowohl \"Spiel\" als auch \"Fahrzeug\" angeben (oder als Standard-Profil markieren), " +
                "damit das Profil eindeutig einer Sim/einem Fahrzeug zugeordnet werden kann -- wichtig, sobald mehrere Simulationen genutzt werden.";
            return false;
        }

        /// <summary>
        /// Ensures at most one profile is marked as the default (catch-all
        /// fallback for auto-loading): if <paramref name="newDefault"/> is
        /// marked default, clears the flag on every other profile.
        /// </summary>
        private void EnforceSingleDefaultProfile(Profile newDefault)
        {
            if (!newDefault.IsDefault)
            {
                return;
            }

            foreach (var other in Profiles)
            {
                if (other.Id != newDefault.Id)
                {
                    other.IsDefault = false;
                }
            }
        }

        private void UpdateProfileButton_Click(object sender, RoutedEventArgs e)
        {
            if (ProfilesComboBox.SelectedItem is not Profile selected)
            {
                ProfileStatusText.Text = "Please select a profile first.";
                return;
            }

            if (!RequireGameAndCarOrDefault())
            {
                return;
            }

            var updated = BuildProfileFromCurrentState(selected.Name, selected.Id);
            EnforceSingleDefaultProfile(updated);
            var index = Profiles.IndexOf(selected);
            Profiles[index] = updated;
            ProfileStore.Save(Profiles.ToList());
            ProfilesComboBox.SelectedItem = updated;
            ProfileStatusText.Text = $"Profile \"{updated.Name}\" updated.";
        }

        private void DeleteProfileButton_Click(object sender, RoutedEventArgs e)
        {
            if (ProfilesComboBox.SelectedItem is not Profile selected)
            {
                ProfileStatusText.Text = "Please select a profile first.";
                return;
            }

            Profiles.Remove(selected);
            ProfileStore.Save(Profiles.ToList());
            if (_lastAutoLoadedProfileId == selected.Id)
            {
                _lastAutoLoadedProfileId = null;
            }
            ProfileStatusText.Text = $"Profile \"{selected.Name}\" deleted.";
        }

        private async void LoadProfileButton_Click(object sender, RoutedEventArgs e)
        {
            if (ProfilesComboBox.SelectedItem is not Profile selected)
            {
                ProfileStatusText.Text = "Please select a profile first.";
                return;
            }

            // Any saved profile can be loaded regardless of the car/game it
            // was originally created for: this lets the user pick a similar
            // car's profile as a starting point, adjust the panels and the
            // "Car"/"Game" fields for the current car, and then use
            // "Save as…" to store it as a new, separate profile
            // without overwriting the original.
            await ApplyProfileAsync(selected, isAutomatic: false);
            ProfileStatusText.Text = $"Profile \"{selected.Name}\" loaded. Adjust car/panels if needed and save under a new name.";
        }

        private void ProfilesComboBox_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
        {
            if (ProfilesComboBox.SelectedItem is not Profile selected)
            {
                return;
            }

            ProfileGameComboBox.Text = selected.GameProcessName ?? string.Empty;
            ProfileCarTextBox.Text = selected.CarModel ?? string.Empty;
            ProfileIsDefaultCheckBox.IsChecked = selected.IsDefault;
        }

        private async void TakeCarFromSimHubButton_Click(object sender, RoutedEventArgs e)
        {
            ProfileStatusText.Text = "Asking SimHub for the current car…";
            var state = await SimHubClient.TryGetGameStateAsync();
            if (state is null)
            {
                ProfileStatusText.Text = "SimHub is not reachable. Please check that SimHub is running and its built-in web server (default port 8888) is active.";
                return;
            }

            if (!string.IsNullOrWhiteSpace(state.GameName) && string.IsNullOrWhiteSpace(ProfileGameComboBox.Text))
            {
                ProfileGameComboBox.Text = state.GameName;
            }

            if (string.IsNullOrWhiteSpace(state.CarModel))
            {
                ProfileStatusText.Text = state.GameRunning
                    ? "SimHub is running, but no car is loaded yet (e.g. still in a menu). Please get in a car and try again."
                    : "SimHub is running, but no game is currently active. Please start the simulation and try again.";
                return;
            }

            ProfileCarTextBox.Text = state.CarModel;
            ProfileStatusText.Text = $"Car \"{state.CarModel}\" retrieved from SimHub.";
        }

        private void AutoLoadProfilesCheckBox_Changed(object sender, RoutedEventArgs e)
        {
            _autoLoadProfiles = AutoLoadProfilesCheckBox.IsChecked == true;
            SaveSettings();
        }

        private void AutoSaveProfileOnSimExitCheckBox_Changed(object sender, RoutedEventArgs e)
        {
            _autoSaveProfileOnSimExit = AutoSaveProfileOnSimExitCheckBox.IsChecked == true;
            SaveSettings();
        }

        private void CloseToTrayCheckBox_Changed(object sender, RoutedEventArgs e)
        {
            _closeToTray = CloseToTrayCheckBox.IsChecked == true;
            SaveSettings();
        }

        /// <summary>
        /// Saves the current panel configuration back into the profile
        /// that matches the currently configured car (preferring the one
        /// most recently auto-/manually loaded), mirroring what the
        /// "Aktualisieren" button does. Used to automatically persist
        /// tweaks made while a simulation was running once it's detected
        /// as no longer running, without requiring the user to remember to
        /// click "Aktualisieren" themselves.
        ///
        /// If no existing profile is associated with the current car/game
        /// (e.g. the very first time a given car is driven), a brand-new
        /// profile is created and named after it instead of silently doing
        /// nothing -- otherwise there would never be anything for a future
        /// session to auto-load for that car.
        /// </summary>
        private void AutoSaveCurrentProfileForCar()
        {
            var currentCar = ProfileCarTextBox.Text?.Trim();
            var currentGame = ProfileGameComboBox.Text?.Trim();

            // Match on game+car together, not car alone: the same car name
            // can exist in several supported sims, so matching by car name
            // only risks updating the wrong sim's profile.
            var target = Profiles.FirstOrDefault(p => p.Id == _lastAutoLoadedProfileId)
                ?? Profiles.FirstOrDefault(p =>
                    !string.IsNullOrWhiteSpace(p.CarModel) &&
                    string.Equals(p.CarModel, currentCar, StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(p.GameProcessName ?? string.Empty, currentGame ?? string.Empty, StringComparison.OrdinalIgnoreCase));

            if (target is not null)
            {
                var updated = BuildProfileFromCurrentState(target.Name, target.Id);
                EnforceSingleDefaultProfile(updated);
                var index = Profiles.IndexOf(target);
                Profiles[index] = updated;
                ProfileStore.Save(Profiles.ToList());
                if (ReferenceEquals(ProfilesComboBox.SelectedItem, target))
                {
                    ProfilesComboBox.SelectedItem = updated;
                }

                ProfileStatusText.Text = $"Profil \"{updated.Name}\" automatisch gespeichert (Sim beendet).";
                return;
            }

            // No profile is associated with this car/game yet: create one
            // automatically so the next session with the same car has
            // something to auto-load, instead of silently dropping the
            // tweaks made during this session. Both the game and the car
            // must be known here -- creating a profile for just a car name
            // would be ambiguous once multiple sims are in use.
            if (string.IsNullOrWhiteSpace(currentCar) || string.IsNullOrWhiteSpace(currentGame))
            {
                return;
            }

            var newName = $"{currentGame} - {currentCar}";
            var created = BuildProfileFromCurrentState(newName);
            EnforceSingleDefaultProfile(created);
            Profiles.Add(created);
            ProfileStore.Save(Profiles.ToList());
            ProfilesComboBox.SelectedItem = created;
            _lastAutoLoadedProfileId = created.Id;

            ProfileStatusText.Text = $"Neues Profil \"{created.Name}\" automatisch angelegt (Sim beendet).";
        }

        /// <summary>
        /// Replaces the currently active panels (and, if present, the reset
        /// button binding) with the given profile's saved configuration.
        ///
        /// If the overlay is currently connected, it is fully stopped and
        /// reconnected from scratch (rather than just swapping panels on
        /// the existing pipe): some host applications (e.g. iRacing when
        /// switching cars) recreate their OpenXR session internally, which
        /// can silently invalidate the native layer's side of the
        /// previously connected pipe even though the managed client still
        /// considers itself connected, leaving the overlay blank until the
        /// user manually toggles it off/on. Reconnecting here reproduces
        /// exactly what that manual toggle does.
        /// </summary>
        private async Task ApplyProfileAsync(Profile profile, bool isAutomatic)
        {
            _isApplyingProfile = true;
            try
            {
                var wasOverlayActive = _ipcClient is not null;
                if (wasOverlayActive)
                {
                    StopOverlay();
                }

                foreach (var panel in Panels.ToList())
                {
                    panel.PropertyChanged -= Panel_PropertyChanged;
                }
                Panels.Clear();

                foreach (var panelSettings in profile.Panels.Take(MaxPanels))
                {
                    var panel = panelSettings.ToPanel();
                    panel.PropertyChanged += Panel_PropertyChanged;
                    Panels.Add(panel);
                }

                var profileResetBindings = profile.ResetBindings.Count > 0
                    ? profile.ResetBindings
                    : (profile.ResetBinding is { ButtonIndex: >= 0 } legacyResetBinding ? new List<ResetBindingSettings> { legacyResetBinding } : new List<ResetBindingSettings>());

                _resetButtonBinding.ClearBindings();
                foreach (var resetBinding in profileResetBindings.Where(b => b.ButtonIndex >= 0))
                {
                    _resetButtonBinding.AddBinding(resetBinding.DeviceInstanceGuid, resetBinding.ButtonIndex, resetBinding.DeviceName);
                }
                RefreshResetBindingRows();

                ProfileGameComboBox.Text = profile.GameProcessName ?? string.Empty;
                // The next auto-profile tick will immediately re-sync this
                // field to whatever car SimHub actually reports as active
                // (see AutoProfileTimer_TickAsync), so it's fine to show
                // the profile's own saved value here in the meantime.
                ProfileCarTextBox.Text = profile.CarModel ?? string.Empty;
                ProfileIsDefaultCheckBox.IsChecked = profile.IsDefault;
                ProfilesComboBox.SelectedItem = profile;
                _lastAutoLoadedProfileId = profile.Id;

                ProfileStatusText.Text = isAutomatic
                    ? $"Profil \"{profile.Name}\" automatisch geladen."
                    : $"Profil \"{profile.Name}\" geladen.";

                if (wasOverlayActive)
                {
                    await StartOverlayAsync();
                }
            }
            finally
            {
                _isApplyingProfile = false;
            }

            SaveSettings();
        }

        /// <summary>
        /// Periodically (a) asks SimHub which game/car is currently active
        /// and mirrors that into the profile game/car fields (so the user
        /// never has to click "Get from SimHub" manually) and (b), if
        /// auto-loading is enabled, applies the best-matching profile:
        /// game+car match takes precedence over car-only, then game-only,
        /// then (if configured) a designated default profile as a
        /// catch-all fallback.
        /// </summary>
        private async Task AutoProfileTimer_TickAsync()
        {
            RefreshRunningProcessNames();

            // Ask SimHub regardless of whether auto-loading is enabled, so
            // the game/car fields (and thus "Speichern unter…"/"Aktualisieren")
            // are always kept up to date automatically. Skipped while a
            // profile is actively being applied to avoid racing with
            // ApplyProfile's own field updates.
            SimHubClient.SimHubGameState? liveState = null;
            if (!_isApplyingProfile)
            {
                liveState = await SimHubClient.TryGetGameStateAsync();
                if (liveState is not null)
                {
                    if (!string.IsNullOrWhiteSpace(liveState.GameName))
                    {
                        ProfileGameComboBox.Text = liveState.GameName;
                    }

                    // Always mirror SimHub's live car value, even after a
                    // profile was loaded (manually or automatically) and
                    // overwrote the field with its own saved value: the
                    // field must reflect the car that's actually being
                    // driven right now, otherwise saving/updating a profile
                    // (e.g. via "Speichern unter...") could tag it with a
                    // stale car name left over from a previously loaded
                    // profile, silently mixing up which profile belongs to
                    // which car.
                    if (!string.IsNullOrWhiteSpace(liveState.CarModel))
                    {
                        ProfileCarTextBox.Text = liveState.CarModel;
                    }

                    ProfileStatusText.Text = !string.IsNullOrWhiteSpace(liveState.CarModel)
                        ? $"SimHub detected: {liveState.GameName} / {liveState.CarModel}"
                        : liveState.GameRunning
                            ? $"SimHub detected: {liveState.GameName} (no car loaded)"
                            : "SimHub is running, no game currently active.";
                }
            }

            // Automatically start the overlay as soon as any supported
            // simulation is detected running, so the user doesn't have to
            // remember to click "Overlay starten" every time — either
            // SimHub reports an active game, or one of the process names
            // configured on a saved profile is running. The same
            // running/not-running state is also used below to detect when
            // the simulation has just been closed, so the current profile
            // for the active car can be auto-saved.
            var isSimRunning = liveState?.GameRunning == true;
            string? detectedProcessName = null;
            if (!isSimRunning)
            {
                var knownProcessNames = Profiles
                    .Where(p => !string.IsNullOrWhiteSpace(p.GameProcessName))
                    .Select(p => p.GameProcessName!)
                    .Distinct(StringComparer.OrdinalIgnoreCase)
                    .ToList();
                detectedProcessName = knownProcessNames.Count > 0 ? ActiveGameDetector.GetRunningProcessName(knownProcessNames) : null;
                isSimRunning = detectedProcessName is not null;
            }

            // Detect a new game session starting (wasn't running on the
            // previous tick, is now) and log it alongside the native layer's
            // log so the exact detection time can be correlated with when
            // the native OpenXR layer negotiates/creates its instance and
            // IPC pipe - useful for diagnosing connection-timing issues like
            // the one seen with Assetto Corsa/LMU.
            if (!_wasSimRunning && isSimRunning)
            {
                var detectedGameName = liveState?.GameName ?? detectedProcessName ?? "unknown";
                OverlayIpcClient.AppendAppLogEntry($"New game session detected: {detectedGameName}");
            }

            // The sim was closed, or the native layer's pipe broke (e.g. a
            // new iRacing session/process): drop the stale client so the
            // overlay is reconnected automatically below instead of
            // requiring the user to toggle it manually.
            if (_ipcClient is not null && ((_wasSimRunning && !isSimRunning) || !_ipcClient.IsConnected))
            {
                OverlayIpcClient.AppendAppLogEntry("Overlay connection lost or sim closed; resetting overlay for automatic reconnect.");
                StopOverlay();
            }

            if (_ipcClient is null && Panels.Count > 0 && isSimRunning)
            {
                await StartOverlayAsync();
            }

            // Detect the simulation having just been closed (was running on
            // the previous tick, isn't anymore) and, if enabled, persist the
            // current panel configuration back into the profile for the car
            // that was active, so any tweaks made during the session aren't
            // lost.
            if (_autoSaveProfileOnSimExit && _wasSimRunning && !isSimRunning && !_isApplyingProfile)
            {
                AutoSaveCurrentProfileForCar();
            }
            _wasSimRunning = isSimRunning;

            if (!_autoLoadProfiles || Profiles.Count == 0)
            {
                return;
            }

            var candidateGameNames = Profiles
                .Where(p => !string.IsNullOrWhiteSpace(p.GameProcessName))
                .Select(p => p.GameProcessName!)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();

            var runningProcessGame = ActiveGameDetector.GetRunningProcessName(candidateGameNames);

            // SimHub is the preferred source for both the active game (it
            // reports "GameRunning"/"GameName" for its currently loaded
            // reader, independent of the exact process name) and the active
            // car; falling back to the local process-name match if SimHub
            // isn't reachable or hasn't detected a game.
            var simHubState = await SimHubClient.TryGetGameStateAsync();

            var currentCar = string.IsNullOrWhiteSpace(simHubState?.CarModel) ? null : simHubState.CarModel;

            string? runningGame = runningProcessGame;
            if (simHubState is { GameRunning: true } && !string.IsNullOrWhiteSpace(simHubState.GameName))
            {
                // Prefer whichever configured profile game name matches
                // SimHub's reported game name (substring match, since
                // SimHub's names, e.g. "IRacing", and process names, e.g.
                // "iRacingSim64DX11", commonly differ only in casing/suffix).
                var simHubMatch = candidateGameNames.FirstOrDefault(name =>
                    simHubState.GameName!.Contains(name, StringComparison.OrdinalIgnoreCase) ||
                    name.Contains(simHubState.GameName!, StringComparison.OrdinalIgnoreCase));
                runningGame = simHubMatch ?? runningProcessGame;
            }

            // Prefer a profile matching both game and car, then car-only,
            // then game-only, then a designated default profile as a
            // catch-all so *something* sensible always loads.
            //
            // When several profiles' CarModel values match (e.g. a generic
            // "Caterham" profile alongside variant-specific ones like
            // "Caterham Classic" and "Caterham Superlight R500"), the most
            // specific (longest) CarModel wins instead of whichever happens
            // to come first in the list, so switching between similar car
            // variants reliably picks the matching profile.
            Profile? match = null;
            if (runningGame is not null && !string.IsNullOrWhiteSpace(currentCar))
            {
                match = Profiles
                    .Where(p =>
                        string.Equals(p.GameProcessName, runningGame, StringComparison.OrdinalIgnoreCase) &&
                        !string.IsNullOrWhiteSpace(p.CarModel) &&
                        currentCar.Contains(p.CarModel!, StringComparison.OrdinalIgnoreCase))
                    .OrderByDescending(p => p.CarModel!.Length)
                    .FirstOrDefault();
            }

            if (match is null && !string.IsNullOrWhiteSpace(currentCar))
            {
                match = Profiles
                    .Where(p =>
                        string.IsNullOrWhiteSpace(p.GameProcessName) &&
                        !string.IsNullOrWhiteSpace(p.CarModel) &&
                        currentCar.Contains(p.CarModel!, StringComparison.OrdinalIgnoreCase))
                    .OrderByDescending(p => p.CarModel!.Length)
                    .FirstOrDefault();
            }

            if (match is null && runningGame is not null)
            {
                match = Profiles.FirstOrDefault(p =>
                    string.Equals(p.GameProcessName, runningGame, StringComparison.OrdinalIgnoreCase) &&
                    string.IsNullOrWhiteSpace(p.CarModel));
            }

            if (match is null)
            {
                match = Profiles.FirstOrDefault(p => p.IsDefault);
            }

            if (match is null || match.Id == _lastAutoLoadedProfileId)
            {
                return;
            }

            await ApplyProfileAsync(match, isAutomatic: true);
        }

        private void AddPanelButton_Click(object sender, RoutedEventArgs e)
        {
            if (Panels.Count >= MaxPanels)
            {
                StatusText.Text = $"A maximum of {MaxPanels} panels is possible.";
                return;
            }

            var panel = new OverlayPanel { Name = $"Panel {Panels.Count + 1}" };
            panel.PropertyChanged += Panel_PropertyChanged;
            Panels.Add(panel);
            PushTransform(panel);
            SaveSettings();
        }

        private void RemovePanelButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: OverlayPanel panel })
            {
                return;
            }

            panel.PropertyChanged -= Panel_PropertyChanged;
            Panels.Remove(panel);

            if (_panelSources.TryGetValue(panel.Id, out var source))
            {
                source.Dispose();
                _panelSources.Remove(panel.Id);
            }

            _ipcClient?.RemovePanel(panel.Id);
            SaveSettings();
        }

        private void BrowseImageButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: OverlayPanel panel })
            {
                return;
            }

            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Filter = "Image files|*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tiff|All files|*.*",
                CheckFileExists = true
            };

            if (dialog.ShowDialog(this) == true)
            {
                panel.ImagePath = dialog.FileName;
            }
        }

        /// <summary>
        /// Handles the mouse-friendly +/- step buttons next to each
        /// position/rotation/scale value, as an easier alternative to
        /// dragging the (fiddly) sliders. The button's Tag encodes the
        /// target property name and the step delta as
        /// "PropertyName,Delta" (e.g. "PositionX,0.005"), and is applied to
        /// the <see cref="OverlayPanel"/> that is the button's DataContext.
        /// Holding the button (RepeatButton) repeats the step.
        /// </summary>
        private void NudgeStepButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: string tag, DataContext: OverlayPanel panel } ||
                panel.IsLocked)
            {
                return;
            }

            var parts = tag.Split(',');
            if (parts.Length != 2 || !float.TryParse(parts[1], System.Globalization.CultureInfo.InvariantCulture, out var delta))
            {
                return;
            }

            switch (parts[0])
            {
                case nameof(OverlayPanel.PositionX): panel.PositionX += delta; break;
                case nameof(OverlayPanel.PositionY): panel.PositionY += delta; break;
                case nameof(OverlayPanel.PositionZ): panel.PositionZ += delta; break;
                case nameof(OverlayPanel.PitchDeg): panel.PitchDeg += delta; break;
                case nameof(OverlayPanel.YawDeg): panel.YawDeg += delta; break;
                case nameof(OverlayPanel.RollDeg): panel.RollDeg += delta; break;
                case nameof(OverlayPanel.Scale): panel.Scale = Math.Max(0.01f, panel.Scale + delta); break;
            }
        }

        #region Keyboard edit mode (WASD/QE + Tab + Shift/Alt, with on-screen HUD)

        private void EditModeToggle_Changed(object sender, RoutedEventArgs e)
        {
            _isEditModeActive = EditModeToggleCheckBox.IsChecked == true;
            _ipcClient?.SetEditModeActive(_isEditModeActive);

            if (_isEditModeActive)
            {
                _editModePressedKeys.Clear();
                _editModeTimer.Start();
                EditModeHudBorder.Visibility = Visibility.Visible;
                StartEditModeVrHud();
                UpdateEditModeHud();
                Keyboard.Focus(this);
            }
            else
            {
                _editModeTimer.Stop();
                _editModePressedKeys.Clear();
                EditModeHudBorder.Visibility = Visibility.Collapsed;
                StopEditModeVrHud();
            }
        }

        /// <summary>
        /// Starts a dedicated, head-locked VR overlay panel that mirrors the
        /// desktop edit-mode HUD, so the panel name/mode/values/legend are
        /// visible inside the headset while editing — without this, the
        /// user would have to take off the headset or look at the desktop
        /// window to see what edit mode is currently doing.
        /// </summary>
        private void StartEditModeVrHud()
        {
            if (_ipcClient is null || !_ipcClient.IsConnected)
            {
                return;
            }

            _editModeHudSource ??= new HudTextSource();
            if (_editModeHudSource.DevicePtr == 0)
            {
                _editModeHudSource.Start();
                _editModeHudSource.FrameArrived += (sharedHandleName, width, height) =>
                    _ipcClient?.UpdateFrame(EditModeHudPanelId, sharedHandleName, width, height);
            }

            // Placed slightly below and in front of the player, head-locked,
            // so it stays comfortably in view regardless of which direction
            // the edited panel itself is in. Kept fairly small so it doesn't
            // dominate the view — it's a status/legend readout, not a panel
            // the user needs to read from a distance.
            _ipcClient.SetPanelTransform(
                EditModeHudPanelId,
                enabled: true,
                posX: 0f, posY: -0.15f, posZ: -0.5f,
                rotX: 0f, rotY: 0f, rotZ: 0f,
                widthMeters: 0.3f, heightMeters: 0.15f,
                headLocked: true, opaqueBackground: true, nonInteractive: true);
        }

        private void StopEditModeVrHud()
        {
            _ipcClient?.RemovePanel(EditModeHudPanelId);
            _editModeHudSource?.Dispose();
            _editModeHudSource = null;

            _ipcClient?.RemovePanel(EditModeHighlightPanelId);
            _editModeHighlightSource?.Dispose();
            _editModeHighlightSource = null;
        }

        /// <summary>
        /// Draws/updates a colored frame overlay quad around the currently
        /// active edit-mode panel — red while locked, green while unlocked —
        /// matching that panel's own transform (slightly enlarged) so it
        /// reads as a highlight around it, visible inside the headset.
        /// </summary>
        private void UpdateEditModeVrHighlight(OverlayPanel? panel)
        {
            if (_ipcClient is null || !_ipcClient.IsConnected)
            {
                return;
            }

            if (panel is null)
            {
                _ipcClient.RemovePanel(EditModeHighlightPanelId);
                return;
            }

            _editModeHighlightSource ??= new Sources.PanelHighlightSource();
            if (_editModeHighlightSource.DevicePtr == 0)
            {
                _editModeHighlightSource.Start();
                _editModeHighlightSource.FrameArrived += (sharedHandleName, width, height) =>
                    _ipcClient?.UpdateFrame(EditModeHighlightPanelId, sharedHandleName, width, height);
            }

            _editModeHighlightSource.SetLocked(panel.IsLocked);

            // A small margin around the panel's own size/position so the
            // frame reads as an outline around it rather than covering it.
            const float margin = 1.08f;
            _ipcClient.SetPanelTransform(
                EditModeHighlightPanelId,
                enabled: true,
                posX: panel.PositionX, posY: panel.PositionY, posZ: panel.PositionZ,
                rotX: panel.PitchDeg, rotY: panel.YawDeg, rotZ: panel.RollDeg,
                widthMeters: panel.WidthMeters * margin,
                heightMeters: panel.HeightMeters * margin,
                headLocked: panel.HeadLocked, opaqueBackground: false, nonInteractive: true);
        }

        private void MainWindow_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (!_isEditModeActive)
            {
                return;
            }

            // Don't hijack keystrokes while the user is typing into a text
            // field (e.g. renaming a panel, editing a URL).
            if (Keyboard.FocusedElement is System.Windows.Controls.TextBox or System.Windows.Controls.ComboBox { IsEditable: true })
            {
                return;
            }

            if (e.Key == Key.Escape)
            {
                EditModeToggleCheckBox.IsChecked = false;
                e.Handled = true;
                return;
            }

            if (e.Key == Key.Tab)
            {
                _editAxisMode = _editAxisMode switch
                {
                    PanelEditAxisMode.Move => PanelEditAxisMode.Rotate,
                    PanelEditAxisMode.Rotate => PanelEditAxisMode.Scale,
                    _ => PanelEditAxisMode.Move
                };
                UpdateEditModeHud();
                e.Handled = true;
                return;
            }

            if (e.Key == Key.C)
            {
                CycleActiveEditModePanel(Keyboard.Modifiers.HasFlag(ModifierKeys.Shift) ? -1 : 1);
                e.Handled = true;
                return;
            }

            if (e.Key == Key.L)
            {
                ToggleActiveEditModePanelLock();
                e.Handled = true;
                return;
            }

            if (IsEditModeKey(e.Key))
            {
                _editModePressedKeys.Add(e.Key);
                e.Handled = true;
            }
        }

        private void MainWindow_PreviewKeyUp(object sender, KeyEventArgs e)
        {
            if (!_isEditModeActive)
            {
                return;
            }

            if (IsEditModeKey(e.Key))
            {
                _editModePressedKeys.Remove(e.Key);
                e.Handled = true;
            }
        }

        private static bool IsEditModeKey(Key key) => key is Key.W or Key.A or Key.S or Key.D or Key.Q or Key.E or Key.C or Key.L
            or Key.LeftShift or Key.RightShift or Key.LeftAlt or Key.RightAlt;

        /// <summary>Locks/unlocks the currently active panel, preventing/allowing further edit-mode or UI adjustments.</summary>
        private void ToggleActiveEditModePanelLock()
        {
            if (ActiveNudgePanelComboBox.SelectedItem is not OverlayPanel panel)
            {
                return;
            }

            panel.IsLocked = !panel.IsLocked;
            UpdateEditModeHud();
        }

        /// <summary>Switches the active panel used by edit mode / controller fine-tuning, wrapping around the panel list.</summary>
        private void CycleActiveEditModePanel(int direction)
        {
            if (Panels.Count == 0)
            {
                return;
            }

            var currentIndex = ActiveNudgePanelComboBox.SelectedIndex;
            var nextIndex = currentIndex < 0
                ? 0
                : (currentIndex + direction + Panels.Count) % Panels.Count;

            ActiveNudgePanelComboBox.SelectedIndex = nextIndex;
            UpdateEditModeHud();
        }

        /// <summary>
        /// Applies the currently held WASD/QE keys, at a rate determined by
        /// the poll interval and whether Shift (fine) or Alt (coarse) is
        /// held, to the active, unlocked panel selected in
        /// <see cref="ActiveNudgePanelComboBox"/>.
        /// </summary>
        private void EditModeTimer_Tick()
        {
            if (!_isEditModeActive)
            {
                return;
            }

            if (ActiveNudgePanelComboBox.SelectedItem is not OverlayPanel panel || panel.IsLocked)
            {
                UpdateEditModeHud();
                return;
            }

            var isFine = _editModePressedKeys.Contains(Key.LeftShift) || _editModePressedKeys.Contains(Key.RightShift);
            var isCoarse = _editModePressedKeys.Contains(Key.LeftAlt) || _editModePressedKeys.Contains(Key.RightAlt);

            var positionStep = isFine ? 0.001f : isCoarse ? 0.02f : 0.005f;
            var rotationStep = isFine ? 0.1f : isCoarse ? 2f : 0.5f;

            var moved = false;

            if (_editAxisMode == PanelEditAxisMode.Move)
            {
                if (_editModePressedKeys.Contains(Key.W)) { panel.PositionZ -= positionStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.S)) { panel.PositionZ += positionStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.A)) { panel.PositionX -= positionStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.D)) { panel.PositionX += positionStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.E)) { panel.PositionY += positionStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.Q)) { panel.PositionY -= positionStep; moved = true; }
            }
            else if (_editAxisMode == PanelEditAxisMode.Rotate)
            {
                if (_editModePressedKeys.Contains(Key.W)) { panel.PitchDeg += rotationStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.S)) { panel.PitchDeg -= rotationStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.A)) { panel.YawDeg -= rotationStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.D)) { panel.YawDeg += rotationStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.E)) { panel.RollDeg += rotationStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.Q)) { panel.RollDeg -= rotationStep; moved = true; }
            }
            else
            {
                var scaleStep = isFine ? 0.001f : isCoarse ? 0.02f : 0.005f;
                if (_editModePressedKeys.Contains(Key.W) || _editModePressedKeys.Contains(Key.D)) { panel.Scale += scaleStep; moved = true; }
                if (_editModePressedKeys.Contains(Key.S) || _editModePressedKeys.Contains(Key.A)) { panel.Scale = Math.Max(0.01f, panel.Scale - scaleStep); moved = true; }
            }

            UpdateEditModeHud(isFine, isCoarse);

            _ = moved;
        }

        /// <summary>Refreshes the on-screen HUD with the active panel, current axis mode, step size and live values.</summary>
        private void UpdateEditModeHud(bool isFine = false, bool isCoarse = false)
        {
            if (!_isEditModeActive)
            {
                return;
            }

            var panel = ActiveNudgePanelComboBox.SelectedItem as OverlayPanel;

            EditModeHudPanelNameText.Text = panel is null
                ? Localization.T("EditMode.NoPanelSelected")
                : panel.IsLocked ? $"{panel.Name} ({Localization.T("EditMode.LockedSuffix")})" : panel.Name;

            EditModeHudModeText.Text = _editAxisMode switch
            {
                PanelEditAxisMode.Move => Localization.T("EditMode.Mode.Move"),
                PanelEditAxisMode.Rotate => Localization.T("EditMode.Mode.Rotate"),
                _ => Localization.T("EditMode.Mode.Scale")
            };
            EditModeHudSpeedText.Text = isFine ? Localization.T("EditMode.Speed.Fine") : isCoarse ? Localization.T("EditMode.Speed.Coarse") : Localization.T("EditMode.Speed.Normal");

            EditModeHudValuesText.Text = panel is null
                ? string.Empty
                : $"X {panel.PositionX:0.000}  Y {panel.PositionY:0.000}  Z {panel.PositionZ:0.000}   " +
                  $"{Localization.T("EditMode.Values.Pitch")} {panel.PitchDeg:0.0}°  {Localization.T("EditMode.Values.Yaw")} {panel.YawDeg:0.0}°  {Localization.T("EditMode.Values.Roll")} {panel.RollDeg:0.0}°   " +
                  $"{Localization.T("EditMode.Values.Scale")} {panel.Scale:0.000}";

            _editModeHudSource?.UpdateText(BuildEditModeVrHudText(panel, isFine, isCoarse));
            UpdateEditModeVrHighlight(panel);
        }

        /// <summary>Builds the text shown on the in-headset edit-mode HUD panel.</summary>
        private string BuildEditModeVrHudText(OverlayPanel? panel, bool isFine, bool isCoarse)
        {
            var panelName = panel is null
                ? Localization.T("EditMode.NoPanelSelected")
                : panel.IsLocked ? $"{panel.Name} ({Localization.T("EditMode.LockedSuffix")})" : panel.Name;
            var mode = _editAxisMode switch
            {
                PanelEditAxisMode.Move => Localization.T("EditMode.Mode.Move"),
                PanelEditAxisMode.Rotate => Localization.T("EditMode.Mode.Rotate"),
                _ => Localization.T("EditMode.Mode.Scale")
            };
            var speed = isFine ? Localization.T("EditMode.Speed.Fine") : isCoarse ? Localization.T("EditMode.Speed.Coarse") : Localization.T("EditMode.Speed.Normal");
            var values = panel is null
                ? string.Empty
                : $"X {panel.PositionX:0.000}  Y {panel.PositionY:0.000}  Z {panel.PositionZ:0.000}\n" +
                  $"{Localization.T("EditMode.Values.Pitch")} {panel.PitchDeg:0.0}°  {Localization.T("EditMode.Values.Yaw")} {panel.YawDeg:0.0}°  {Localization.T("EditMode.Values.Roll")} {panel.RollDeg:0.0}°\n" +
                  $"{Localization.T("EditMode.Values.Scale")} {panel.Scale:0.000}";

            var panelLine = string.Format(Localization.T("EditMode.Vr.PanelLine"), panelName);
            var modeSpeedLine = string.Format(Localization.T("EditMode.Vr.ModeSpeedLine"), mode, speed);

            return $"{Localization.T("EditMode.Vr.Title")}\n{panelLine}\n{modeSpeedLine}\n{values}\n\n" +
                   Localization.T("EditMode.Vr.Legend");
        }

        #endregion

        /// <summary>
        /// Gives a (still effectively default) panel a sensible starting
        /// transform for the Visor source: small enough and far enough back
        /// that the whole curved band fits in view, and head-locked so it
        /// behaves like a real helmet visor (always in the same spot
        /// relative to the headset) instead of staying fixed in the world.
        /// Skipped if the panel already has a custom transform, so this
        /// doesn't clobber placement the user already fine-tuned.
        /// </summary>
        private static void ApplyDefaultVisorPlacement(OverlayPanel panel)
        {
            const float defaultWidth = 0.4f;
            const float defaultHeight = 0.3f;
            const float defaultPositionZ = -1.0f;

            var looksUntouched =
                panel.PositionX == 0f && panel.PositionY == 0f && panel.PositionZ == defaultPositionZ &&
                panel.PitchDeg == 0f && panel.YawDeg == 0f && panel.RollDeg == 0f &&
                panel.WidthMeters == defaultWidth && panel.HeightMeters == defaultHeight &&
                !panel.HeadLocked;

            if (!looksUntouched)
            {
                return;
            }

            ResetVisorPlacement(panel);
        }

        /// <summary>Shared by <see cref="ApplyDefaultVisorPlacement"/> and the manual "Reset size" button.</summary>
        private static void ResetVisorPlacement(OverlayPanel panel)
        {
            panel.HeadLocked = true;
            panel.PositionX = 0f;
            panel.PositionY = 0.15f;
            panel.PositionZ = -1.2f;
            panel.PitchDeg = 0f;
            panel.YawDeg = 0f;
            panel.RollDeg = 0f;
            panel.WidthMeters = 0.5f;
            panel.HeightMeters = 0.3f;
        }

        private void ResetVisorSizeButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: OverlayPanel panel })
            {
                return;
            }

            ResetVisorPlacement(panel);
        }

        /// <summary>
        /// Opens the native color picker to edit either the visor's start
        /// (top) or end (bottom) tint color. Which one is determined by the
        /// button's CommandParameter ("Start" or "End"), while Tag carries
        /// the panel as usual.
        /// </summary>
        private void PickVisorColorButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: OverlayPanel panel } element)
            {
                return;
            }

            var isEndColor = string.Equals(element.GetValue(System.Windows.Controls.Primitives.ButtonBase.CommandParameterProperty) as string, "End", StringComparison.Ordinal);
            var currentHex = isEndColor ? panel.VisorEndColor : panel.VisorColor;

            using var dialog = new System.Windows.Forms.ColorDialog
            {
                FullOpen = true,
                AnyColor = true
            };

            try
            {
                var current = System.Drawing.ColorTranslator.FromHtml(
                    currentHex.StartsWith('#') ? currentHex : "#" + currentHex);
                dialog.Color = current;
            }
            catch
            {
                // Keep the dialog's own default if the current value can't be parsed.
            }

            if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
            {
                var c = dialog.Color;
                var hex = $"#{c.R:X2}{c.G:X2}{c.B:X2}";
                if (isEndColor)
                {
                    panel.VisorEndColor = hex;
                }
                else
                {
                    panel.VisorColor = hex;
                }
            }
        }

        private void Panel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (sender is not OverlayPanel panel)
            {
                return;
            }

            switch (e.PropertyName)
            {
                case nameof(OverlayPanel.PositionX):
                case nameof(OverlayPanel.PositionY):
                case nameof(OverlayPanel.PositionZ):
                case nameof(OverlayPanel.PitchDeg):
                case nameof(OverlayPanel.YawDeg):
                case nameof(OverlayPanel.RollDeg):
                case nameof(OverlayPanel.WidthMeters):
                case nameof(OverlayPanel.HeightMeters):
                case nameof(OverlayPanel.IsEnabled):
                case nameof(OverlayPanel.HeadLocked):
                case nameof(OverlayPanel.OpaqueBackground):
                    PushTransform(panel);
                    SaveSettings();
                    break;
                case nameof(OverlayPanel.SourceType):
                    if (panel.SourceType == PanelSourceType.Visor)
                    {
                        // The visor is meant to be a small, head-locked
                        // accent in the corner of the view (not a
                        // full-screen dashboard), so switching to it on an
                        // otherwise still-default/untouched panel starts it
                        // small and pulled back far enough that the whole
                        // curved band is visible at once, instead of
                        // filling (and overflowing) the field of view.
                        ApplyDefaultVisorPlacement(panel);
                    }
                    goto case nameof(OverlayPanel.ImagePath);
                case nameof(OverlayPanel.WindowHandle):
                case nameof(OverlayPanel.Url):
                case nameof(OverlayPanel.ImagePath):
                case nameof(OverlayPanel.VisorColor):
                case nameof(OverlayPanel.VisorOpacity):
                case nameof(OverlayPanel.VisorEndColor):
                case nameof(OverlayPanel.VisorEndOpacity):
                case nameof(OverlayPanel.VisorCoverage):
                case nameof(OverlayPanel.VisorCurve):
                    if (_ipcClient is not null)
                    {
                        _ = RestartPanelSourceAsync(panel);
                    }
                    SaveSettings();
                    break;
                case nameof(OverlayPanel.Name):
                    SaveSettings();
                    break;
            }
        }

        private void PushTransform(OverlayPanel panel)
        {
            _ipcClient?.SetPanelTransform(
                panel.Id,
                panel.IsEnabled,
                panel.PositionX, panel.PositionY, panel.PositionZ,
                panel.PitchDeg, panel.YawDeg, panel.RollDeg,
                panel.WidthMeters,
                panel.HeightMeters,
                panel.HeadLocked,
                panel.OpaqueBackground);
        }

        private async Task RestartPanelSourceAsync(OverlayPanel panel)
        {
            if (_panelSources.TryGetValue(panel.Id, out var existingSource))
            {
                existingSource.Dispose();
                _panelSources.Remove(panel.Id);
            }

            await StartPanelSourceAsync(panel);
        }

        private async Task StartPanelSourceAsync(OverlayPanel panel)
        {
            if (_ipcClient is null)
            {
                return;
            }

            IOverlaySource source;
            if (panel.SourceType == PanelSourceType.TestPattern)
            {
                var testSource = new TestPatternSource();
                testSource.Start();
                source = testSource;
            }
            else if (panel.SourceType == PanelSourceType.WebDashboard)
            {
                if (string.IsNullOrWhiteSpace(panel.Url))
                {
                    return;
                }

                // Render at a resolution matching the panel's physical
                // aspect ratio (WidthMeters/HeightMeters) instead of a fixed
                // square, otherwise the dashboard is stretched to fit the
                // quad and no longer matches the source's real proportions.
                var (renderWidth, renderHeight) = ComputeDashboardRenderResolution(panel);

                var webSource = new WebDashboardSource();
                await webSource.StartAsync(panel.Url, renderWidth, renderHeight);
                source = webSource;
            }
            else if (panel.SourceType == PanelSourceType.Image)
            {
                if (string.IsNullOrWhiteSpace(panel.ImagePath) || !File.Exists(panel.ImagePath))
                {
                    return;
                }

                // Unlike WindowCapture/WebDashboard (which keep pushing
                // frames on an ongoing capture/render loop), the image
                // source renders its single frame synchronously inside
                // Start(). Subscribe to FrameArrived *before* calling Start()
                // here, otherwise that one-and-only frame fires with no
                // listener attached yet and is silently lost, so the native
                // layer never receives a texture and nothing is drawn in VR.
                var imageSource = new ImageOverlaySource();
                imageSource.FrameArrived += (sharedHandleName, width, height) => _ipcClient?.UpdateFrame(panel.Id, sharedHandleName, width, height);
                imageSource.Start(panel.ImagePath);
                source = imageSource;
                _panelSources[panel.Id] = source;
                return;
            }
            else if (panel.SourceType == PanelSourceType.Visor)
            {
                // Same as Image above: Start() renders its single frame
                // synchronously, so FrameArrived must be subscribed first.
                var visorSource = new VisorOverlaySource();
                visorSource.FrameArrived += (sharedHandleName, width, height) => _ipcClient?.UpdateFrame(panel.Id, sharedHandleName, width, height);
                visorSource.Start(panel.VisorColor, panel.VisorOpacity, panel.VisorEndColor, panel.VisorEndOpacity, panel.VisorCoverage, panel.VisorCurve);
                source = visorSource;
                _panelSources[panel.Id] = source;
                return;
            }
            else
            {
                if (panel.WindowHandle == IntPtr.Zero)
                {
                    return;
                }

                var windowSource = new WindowCapture();
                windowSource.Start(panel.WindowHandle);
                source = windowSource;
            }

            source.FrameArrived += (sharedHandleName, width, height) => _ipcClient?.UpdateFrame(panel.Id, sharedHandleName, width, height);
            _panelSources[panel.Id] = source;
        }

        /// <summary>
        /// Picks a CEF off-screen render resolution matching the panel's
        /// physical aspect ratio (<see cref="OverlayPanel.WidthMeters"/> /
        /// <see cref="OverlayPanel.HeightMeters"/>), capping the longer edge
        /// at 1024px so the browser doesn't render an unnecessarily large
        /// (VRAM/perf-costly) surface. Without this, a fixed 1024x1024
        /// render target gets stretched non-uniformly onto the panel's quad
        /// whenever its aspect ratio isn't 1:1, distorting the dashboard.
        /// </summary>
        private static (int Width, int Height) ComputeDashboardRenderResolution(OverlayPanel panel)
        {
            const int maxEdge = 1024;

            var aspect = panel.HeightMeters > 0
                ? panel.WidthMeters / panel.HeightMeters
                : 1f;

            if (aspect <= 0 || float.IsNaN(aspect) || float.IsInfinity(aspect))
            {
                aspect = 1f;
            }

            int width, height;
            if (aspect >= 1f)
            {
                width = maxEdge;
                height = Math.Max(1, (int)Math.Round(maxEdge / aspect));
            }
            else
            {
                height = maxEdge;
                width = Math.Max(1, (int)Math.Round(maxEdge * aspect));
            }

            return (width, height);
        }

        private void LearnResetButton_Click(object sender, RoutedEventArgs e)
        {
            if (_resetButtonBinding.IsLearning)
            {
                _resetButtonBinding.CancelLearning();
                LearnResetButton.Content = "Assign button…";
                ResetBindingStatusText.Text = "Assignment cancelled.";
                return;
            }

            ResetBindingStatusText.Text = "Please press the desired button now…";
            LearnResetButton.Content = "Abbrechen";
            _resetButtonBinding.StartLearning();
        }

        private void RemoveResetBindingButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button { Tag: ResetBindingRow row })
            {
                _resetButtonBinding.RemoveBinding(row.Index);
                RefreshResetBindingRows();
                SaveSettings();
            }
        }

        /// <summary>Rebuilds <see cref="ResetBindingRows"/> from the current <see cref="ResetButtonBinding"/> state.</summary>
        private void RefreshResetBindingRows()
        {
            ResetBindingRows.Clear();
            var bindings = _resetButtonBinding.Bindings;
            for (var i = 0; i < bindings.Count; i++)
            {
                ResetBindingRows.Add(new ResetBindingRow(i, bindings[i].DeviceName, bindings[i].ButtonIndex));
            }

            ResetBindingStatusText.Text = bindings.Count == 0 ? "No button assigned." : string.Empty;
            ResetBindingStatusText.Visibility = bindings.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        private void ManualResetButton_Click(object sender, RoutedEventArgs e)
        {
            _ipcClient?.Recenter();
            StatusText.Text = "VR reset.";
        }

        private static string GetNudgeActionLabel(NudgeAction action) => action switch
        {
            NudgeAction.MoveXNeg => "X -",
            NudgeAction.MoveXPos => "X +",
            NudgeAction.MoveYNeg => "Y -",
            NudgeAction.MoveYPos => "Y +",
            NudgeAction.MoveZNeg => "Z -",
            NudgeAction.MoveZPos => "Z +",
            NudgeAction.PitchNeg => "Pitch -",
            NudgeAction.PitchPos => "Pitch +",
            NudgeAction.YawNeg => "Yaw -",
            NudgeAction.YawPos => "Yaw +",
            NudgeAction.RollNeg => "Roll -",
            NudgeAction.RollPos => "Roll +",
            NudgeAction.NextPanel => "Next panel",
            NudgeAction.PrevPanel => "Previous panel",
            NudgeAction.ToggleStepSize => "Switch step size",
            NudgeAction.ScaleUp => "Zoom +",
            NudgeAction.ScaleDown => "Zoom -",
            _ => action.ToString()
        };

        private void InitializeNudgeActionRows()
        {
            NudgeActionRows.Clear();
            foreach (NudgeAction action in Enum.GetValues<NudgeAction>())
            {
                NudgeActionRows.Add(new NudgeActionRow(action, GetNudgeActionLabel(action)));
            }
        }

        private void LearnNudgeBindingButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: NudgeAction action })
            {
                return;
            }

            if (_panelNudgeController.IsLearning)
            {
                _panelNudgeController.CancelLearning();
                NudgeStatusText.Text = "Assignment cancelled.";
                return;
            }

            NudgeStatusText.Text = $"Please press the button for \"{GetNudgeActionLabel(action)}\" now…";
            _panelNudgeController.StartLearning(action);
        }

        private void ClearNudgeBindingButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement { Tag: NudgeAction action })
            {
                return;
            }

            _panelNudgeController.Unbind(action);
            var row = NudgeActionRows.FirstOrDefault(r => r.Action == action);
            if (row is not null)
            {
                row.StatusText = "No button assigned.";
            }

            SaveSettings();
        }

        /// <summary>
        /// Forwards a VR controller thumbstick nudge (see
        /// NativeLayer/ControllerInput.cpp) to the same ApplyNudgeAction
        /// pipeline used for DirectInput wheel/gamepad bindings. Raised on
        /// the IPC read thread, so marshal to the UI thread.
        /// </summary>
        private void OnControllerNudgeActionReceived(NudgeAction action)
        {
            Dispatcher.Invoke(() => ApplyNudgeAction(action));
        }

        /// <summary>
        /// Auto-activates edit mode as soon as a VR controller grabs a
        /// panel, and selects that panel as the active nudge target.
        /// Without this, moving a panel via the controller while edit mode
        /// is off leaves the app looking "stuck"/unresponsive in VR, since
        /// the edit-mode HUD/highlight and nudge pipeline are normally only
        /// active once the user explicitly toggles edit mode. Raised on the
        /// IPC read thread, so marshal to the UI thread.
        /// </summary>
        private void OnControllerGrabStarted(Guid panelId)
        {
            Dispatcher.Invoke(() =>
            {
                var panel = Panels.FirstOrDefault(p => p.Id == panelId);
                if (panel is null)
                {
                    return;
                }

                // Grabbing a panel in VR is an explicit intent to move it,
                // so unlock it automatically.
                panel.IsLocked = false;

                if (!_isEditModeActive)
                {
                    EditModeToggleCheckBox.IsChecked = true;
                }

                ActiveNudgePanelComboBox.SelectedItem = panel;
                UpdateEditModeHud();
            });
        }

        /// <summary>
        /// Applies the final panel transform
        /// after a VR controller grab ended, so the corresponding
        /// <see cref="OverlayPanel"/> (and, via its bindings, the UI and
        /// eventual profile persistence) reflect the new position. Raised
        /// on the IPC read thread, so marshal to the UI thread.
        /// </summary>
        private void OnControllerPanelTransformUpdated(Guid panelId, float posX, float posY, float posZ, float rotDegX, float rotDegY, float rotDegZ)
        {
            Dispatcher.Invoke(() =>
            {
                var panel = Panels.FirstOrDefault(p => p.Id == panelId);
                if (panel is null || panel.IsLocked)
                {
                    return;
                }

                panel.PositionX = posX;
                panel.PositionY = posY;
                panel.PositionZ = posZ;
                panel.PitchDeg = rotDegX;
                panel.YawDeg = rotDegY;
                panel.RollDeg = rotDegZ;

                // The active profile is re-applied on the next session
                // (auto-load), so persist the moved panel there as well;
                // otherwise the global settings get overwritten by the
                // profile's stale panel positions.
                SaveActiveProfilePanels();
            });
        }

        /// <summary>
        /// Writes the current panel configuration into the currently active
        /// (last loaded) profile, keeping its name/game/car metadata.
        /// </summary>
        private void SaveActiveProfilePanels()
        {
            if (_isApplyingProfile || _lastAutoLoadedProfileId is null)
            {
                return;
            }

            var target = Profiles.FirstOrDefault(p => p.Id == _lastAutoLoadedProfileId);
            if (target is null)
            {
                return;
            }

            target.Panels = Panels.Select(PanelSettings.FromPanel).ToList();
            ProfileStore.Save(Profiles.ToList());
        }

        /// <summary>
        /// Applies a single controller-nudge action to the currently
        /// selected, unlocked panel (or advances the panel selection /
        /// toggles the step size), giving immediate feedback in the headset
        /// without needing to look at the desktop app.
        /// </summary>
        private void ApplyNudgeAction(NudgeAction action)
        {
            if (action == NudgeAction.ToggleStepSize)
            {
                _nudgeStepIndex = (_nudgeStepIndex + 1) % NudgeStepSizes.Length;
                NudgeStepSizeText.Text = _nudgeStepIndex switch
                {
                    0 => "fein",
                    1 => "mittel",
                    _ => "grob"
                };
                return;
            }

            if (action == NudgeAction.NextPanel || action == NudgeAction.PrevPanel)
            {
                if (Panels.Count == 0)
                {
                    return;
                }

                var currentIndex = ActiveNudgePanelComboBox.SelectedItem is OverlayPanel current
                    ? Panels.IndexOf(current)
                    : -1;

                var delta = action == NudgeAction.NextPanel ? 1 : -1;

                // Skip locked panels while cycling, so the user doesn't
                // accidentally land on (and then nudge) a panel they've
                // explicitly locked to protect from edits.
                for (var i = 0; i < Panels.Count; i++)
                {
                    currentIndex = ((currentIndex + delta) % Panels.Count + Panels.Count) % Panels.Count;
                    if (!Panels[currentIndex].IsLocked)
                    {
                        ActiveNudgePanelComboBox.SelectedIndex = currentIndex;
                        return;
                    }
                }

                return;
            }

            if (ActiveNudgePanelComboBox.SelectedItem is not OverlayPanel panel || panel.IsLocked)
            {
                return;
            }

            var positionStep = NudgeStepSizes[_nudgeStepIndex];
            var rotationStep = positionStep * 100f;

            switch (action)
            {
                case NudgeAction.MoveXNeg: panel.PositionX -= positionStep; break;
                case NudgeAction.MoveXPos: panel.PositionX += positionStep; break;
                case NudgeAction.MoveYNeg: panel.PositionY -= positionStep; break;
                case NudgeAction.MoveYPos: panel.PositionY += positionStep; break;
                case NudgeAction.MoveZNeg: panel.PositionZ -= positionStep; break;
                case NudgeAction.MoveZPos: panel.PositionZ += positionStep; break;
                case NudgeAction.PitchNeg: panel.PitchDeg -= rotationStep; break;
                case NudgeAction.PitchPos: panel.PitchDeg += rotationStep; break;
                case NudgeAction.YawNeg: panel.YawDeg -= rotationStep; break;
                case NudgeAction.YawPos: panel.YawDeg += rotationStep; break;
                case NudgeAction.RollNeg: panel.RollDeg -= rotationStep; break;
                case NudgeAction.RollPos: panel.RollDeg += rotationStep; break;
                case NudgeAction.ScaleUp: panel.Scale += positionStep * 10f; break;
                case NudgeAction.ScaleDown: panel.Scale = Math.Max(0.01f, panel.Scale - positionStep * 10f); break;
            }
        }

        private void AboutButton_Click(object sender, RoutedEventArgs e)
        {
            new Views.AboutWindow { Owner = this }.ShowDialog();
        }

        private void SettingsToggleButton_Click(object sender, RoutedEventArgs e)
        {
            GlobalSettingsPopup.IsOpen = !GlobalSettingsPopup.IsOpen;
        }

        private void NativeLogToggleButton_Click(object sender, RoutedEventArgs e)
        {
            NativeLogPopup.IsOpen = !NativeLogPopup.IsOpen;
        }

        private async void OverlayStatusButton_Click(object sender, RoutedEventArgs e)
        {
            if (_ipcClient is not null)
            {
                StopOverlay();
                StatusText.Text = Localization.Instance["Overlay.Status.StoppedMessage"];
            }
            else
            {
                await StartOverlayAsync();
            }
        }

        /// <summary>
        /// Connects to the native OpenXR layer and starts all configured
        /// panels' sources. Used both by the manual overlay status/action
        /// button and by the automatic-start check in
        /// <see cref="AutoProfileTimer_TickAsync"/> once a supported
        /// simulation is detected running.
        /// </summary>
        /// <returns>True if the overlay is now connected (or already was), false if starting failed.</returns>
        private async Task<bool> StartOverlayAsync()
        {
            if (_ipcClient is not null)
            {
                // Already connected; nothing to do.
                return true;
            }

            try
            {
                if (Panels.Count == 0)
                {
                    StatusText.Text = "Please add at least one panel.";
                    SetOverlayStatus(OverlayStatus.Error);
                    return false;
                }

                if (!OpenXrLayerRegistration.IsRegistered())
                {
                    StatusText.Text = "OpenXR API layer is not registered. Please register it first.";
                    SetOverlayStatus(OverlayStatus.Error);
                    return false;
                }

                var ipcClient = new OverlayIpcClient();
                StatusText.Text = "Connecting to native OpenXR layer (e.g. in iRacing)…";
                SetOverlayStatus(OverlayStatus.Waiting);
                if (!await ipcClient.ConnectAsync())
                {
                    ipcClient.Dispose();
                    StatusText.Text = "Connection to native layer failed. Is the VR application (e.g. iRacing) already running in VR with the layer registered? " +
                        $"Native layer diagnostic log: {OverlayIpcClient.NativeLayerLogPath} (if the file does not exist, the DLL was not loaded by iRacing).";
                    SetOverlayStatus(OverlayStatus.Error);
                    return false;
                }

                _ipcClient = ipcClient;
                ipcClient.PanelTransformUpdated += OnControllerPanelTransformUpdated;
                ipcClient.ControllerNudgeActionReceived += OnControllerNudgeActionReceived;
                ipcClient.ControllerGrabStarted += OnControllerGrabStarted;
                ipcClient.SetEditModeActive(_isEditModeActive);

                for (var i = 0; i < Panels.Count; i++)
                {
                    await StartPanelSourceAsync(Panels[i]);
                    PushTransform(Panels[i]);
                }

                if (_isEditModeActive)
                {
                    StartEditModeVrHud();
                }

                StatusText.Text = string.Format(Localization.Instance["Overlay.Status.Connected"], Panels.Count);
                SetOverlayStatus(OverlayStatus.Active);
                return true;
            }
            catch (Exception ex)
            {
                StatusText.Text = $"Error starting the overlay: {ex.Message}";
                StopOverlay();
                SetOverlayStatus(OverlayStatus.Error);
                return false;
            }
        }

        private void StopOverlay()
        {
            if (_isEditModeActive)
            {
                StopEditModeVrHud();
            }

            if (_ipcClient is not null)
            {
                foreach (var panel in Panels)
                {
                    _ipcClient.RemovePanel(panel.Id);
                }
            }
            _ipcClient?.Dispose();
            _ipcClient = null;

            foreach (var source in _panelSources.Values)
            {
                source.Dispose();
            }
            _panelSources.Clear();

            SetOverlayStatus(OverlayStatus.Stopped);
        }

        /// <summary>
        /// Updates the combined overlay status/action button's color,
        /// text and enabled state to reflect the given <see cref="OverlayStatus"/>.
        /// </summary>
        private void SetOverlayStatus(OverlayStatus status)
        {
            _overlayStatus = status;

            (Brush fill, string text) = status switch
            {
                OverlayStatus.Active => (Brushes.LimeGreen, Localization.Instance["Overlay.Status.Active"]),
                OverlayStatus.Waiting => (Brushes.Gold, Localization.Instance["Overlay.Status.Waiting"]),
                OverlayStatus.Error => (Brushes.Red, Localization.Instance["Overlay.Status.Error"]),
                _ => (Brushes.Gray, Localization.Instance["Overlay.Status.Stopped"]),
            };

            OverlayStatusIndicator.Fill = fill;
            OverlayStatusButtonText.Text = text;
            OverlayStatusButton.IsEnabled = status != OverlayStatus.Waiting;
        }

        /// <summary>
        /// Intercepts the window close request (e.g. the X button) and, if
        /// "close to tray" is enabled, hides the window and keeps the app
        /// running in the background (with a tray icon to restore/exit it)
        /// instead of actually shutting down.
        /// </summary>
        protected override void OnClosing(CancelEventArgs e)
        {
            if (_closeToTray && !_isExiting)
            {
                e.Cancel = true;
                Hide();
                ShowTrayIcon();
                return;
            }

            base.OnClosing(e);
        }

        protected override void OnClosed(EventArgs e)
        {
            _nativeLogTimer.Stop();
            SaveSettings();
            StopOverlay();
            _resetButtonBinding.Dispose();
            _panelNudgeController.Dispose();
            _trayIcon?.Dispose();
            _trayIcon = null;
            base.OnClosed(e);
        }

        /// <summary>
        /// Lazily creates (if needed) and shows the tray icon used while the
        /// window is hidden due to "close to tray". Double-clicking it or
        /// using its context menu's "Open" restores the window; "Exit"
        /// performs a real shutdown of the application.
        /// </summary>
        private void ShowTrayIcon()
        {
            if (_trayIcon is null)
            {
                var contextMenu = new System.Windows.Forms.ContextMenuStrip();
                contextMenu.Items.Add("Open", null, (_, _) => RestoreFromTray());
                contextMenu.Items.Add("Exit", null, (_, _) => ExitFromTray());

                var exePath = Assembly.GetExecutingAssembly().Location;
                var icon = string.IsNullOrEmpty(exePath) ? null : System.Drawing.Icon.ExtractAssociatedIcon(exePath);

                _trayIcon = new System.Windows.Forms.NotifyIcon
                {
                    Icon = icon ?? System.Drawing.SystemIcons.Application,
                    Text = "Donutz VR HUD",
                    ContextMenuStrip = contextMenu,
                    Visible = false
                };
                _trayIcon.DoubleClick += (_, _) => RestoreFromTray();
            }

            _trayIcon.Visible = true;
        }

        private void RestoreFromTray()
        {
            if (_trayIcon is not null)
            {
                _trayIcon.Visible = false;
            }

            Show();
            WindowState = WindowState.Normal;
            Activate();
        }

        private void ExitFromTray()
        {
            _isExiting = true;
            if (_trayIcon is not null)
            {
                _trayIcon.Visible = false;
            }

            Close();
        }

        // Returns the last-write timestamp of the running executable, which
        // reflects when it was actually built/linked (unlike an embedded
        // version number that only changes when manually bumped).
        private static DateTime GetBuildTimestamp()
        {
            try
            {
                var exePath = Assembly.GetExecutingAssembly().Location;
                return File.GetLastWriteTime(exePath);
            }
            catch
            {
                return DateTime.MinValue;
            }
        }
    }
}
