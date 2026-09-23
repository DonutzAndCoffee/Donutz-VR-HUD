using SharpDX.DirectInput;

namespace Donutz_VR_HUD.Input
{
    /// <summary>
    /// The set of actions that can be bound to a controller/wheel button for
    /// hands-on, in-headset fine-tuning of the currently selected (and
    /// unlocked) overlay panel's position/rotation, plus panel selection and
    /// step-size control.
    /// </summary>
    public enum NudgeAction
    {
        MoveXNeg,
        MoveXPos,
        MoveYNeg,
        MoveYPos,
        MoveZNeg,
        MoveZPos,
        PitchNeg,
        PitchPos,
        YawNeg,
        YawPos,
        RollNeg,
        RollPos,
        NextPanel,
        PrevPanel,
        ToggleStepSize,
        ScaleUp,
        ScaleDown
    }

    public sealed record NudgeBinding(Guid DeviceInstanceGuid, string DeviceName, int ButtonIndex);

    /// <summary>
    /// Lets the user bind any number of <see cref="NudgeAction"/>s to buttons
    /// on any connected DirectInput device (typically a steering wheel), and
    /// polls all bound devices on a background thread so that panels can be
    /// nudged into place while wearing the headset, without needing to look
    /// at the desktop app. While a bound button is held down, the
    /// corresponding action is raised repeatedly (once per poll tick) so
    /// holding the button feels like a continuous adjustment.
    /// </summary>
    public sealed class PanelNudgeController : IDisposable
    {
        private readonly DirectInput _directInput = new();
        private readonly Dictionary<NudgeAction, NudgeBinding> _bindings = new();

        private Thread? _pollThread;
        private volatile bool _running;

        private Thread? _learnThread;
        private volatile bool _learning;
        private NudgeAction _learningAction;

        /// <summary>Raised on the poll thread, once per tick, for every action whose bound button is currently held down.</summary>
        public event Action<NudgeAction>? ActionHeld;

        /// <summary>Raised on a background thread once learn mode detects and binds a button press for the requested action.</summary>
        public event Action<NudgeAction, InputDeviceInfo, int>? Learned;

        public bool IsLearning => _learning;

        public IReadOnlyDictionary<NudgeAction, NudgeBinding> Bindings => _bindings;

        /// <summary>Starts "learn mode" for a single action: the first button pressed on any connected device is bound to it.</summary>
        public void StartLearning(NudgeAction action)
        {
            CancelLearning();

            _learningAction = action;
            _learning = true;
            _learnThread = new Thread(LearnLoop) { IsBackground = true, Name = $"Panel Nudge Learn ({action})" };
            _learnThread.Start();
        }

        public void CancelLearning()
        {
            _learning = false;
            _learnThread?.Join(TimeSpan.FromSeconds(1));
            _learnThread = null;
        }

        private void LearnLoop()
        {
            var action = _learningAction;
            var devices = ResetButtonBinding.GetAvailableDevices();
            var joysticks = new List<(InputDeviceInfo Info, Joystick Stick)>();

            try
            {
                foreach (var device in devices)
                {
                    try
                    {
                        var stick = new Joystick(_directInput, device.InstanceGuid);
                        stick.Acquire();
                        joysticks.Add((device, stick));
                    }
                    catch
                    {
                        // Device may not be a joystick-compatible object; skip it.
                    }
                }

                while (_learning)
                {
                    foreach (var (info, stick) in joysticks)
                    {
                        try
                        {
                            stick.Poll();
                            var state = stick.GetCurrentState();
                            var pressedIndex = Array.IndexOf(state.Buttons, true);
                            if (pressedIndex >= 0)
                            {
                                _learning = false;
                                Bind(action, info.InstanceGuid, pressedIndex, info.Name);
                                Learned?.Invoke(action, info, pressedIndex);
                                return;
                            }
                        }
                        catch
                        {
                            // Device may have been unplugged; ignore and keep scanning others.
                        }
                    }

                    Thread.Sleep(16);
                }
            }
            finally
            {
                foreach (var (_, stick) in joysticks)
                {
                    stick.Unacquire();
                    stick.Dispose();
                }
            }
        }

        /// <summary>Binds the given action to a device/button, replacing any previous binding for that action.</summary>
        public void Bind(NudgeAction action, Guid deviceInstanceGuid, int buttonIndex, string? deviceName = null)
        {
            _bindings[action] = new NudgeBinding(deviceInstanceGuid, deviceName ?? string.Empty, buttonIndex);
            RestartPolling();
        }

        public void Unbind(NudgeAction action)
        {
            _bindings.Remove(action);
            RestartPolling();
        }

        public void ClearAllBindings()
        {
            _bindings.Clear();
            RestartPolling();
        }

        private void RestartPolling()
        {
            Stop();

            if (_bindings.Count == 0)
            {
                return;
            }

            _running = true;
            _pollThread = new Thread(PollLoop) { IsBackground = true, Name = "Panel Nudge Poll" };
            _pollThread.Start();
        }

        public void Stop()
        {
            _running = false;
            _pollThread?.Join(TimeSpan.FromSeconds(1));
            _pollThread = null;
        }

        private void PollLoop()
        {
            var bindingsByDevice = _bindings
                .GroupBy(kvp => kvp.Value.DeviceInstanceGuid)
                .ToDictionary(g => g.Key, g => g.Select(kvp => (Action: kvp.Key, kvp.Value.ButtonIndex)).ToList());

            var sticks = new Dictionary<Guid, Joystick>();

            try
            {
                foreach (var deviceGuid in bindingsByDevice.Keys)
                {
                    try
                    {
                        var stick = new Joystick(_directInput, deviceGuid);
                        stick.Acquire();
                        sticks[deviceGuid] = stick;
                    }
                    catch
                    {
                        // Device may have been unplugged since binding; skip it.
                    }
                }

                while (_running)
                {
                    foreach (var (deviceGuid, actions) in bindingsByDevice)
                    {
                        if (!sticks.TryGetValue(deviceGuid, out var stick))
                        {
                            continue;
                        }

                        try
                        {
                            stick.Poll();
                            var state = stick.GetCurrentState();

                            foreach (var (action, buttonIndex) in actions)
                            {
                                if (buttonIndex >= 0 && buttonIndex < state.Buttons.Length && state.Buttons[buttonIndex])
                                {
                                    ActionHeld?.Invoke(action);
                                }
                            }
                        }
                        catch
                        {
                            // Device may have been unplugged; keep trying until Stop() is called.
                        }
                    }

                    Thread.Sleep(33);
                }
            }
            finally
            {
                foreach (var stick in sticks.Values)
                {
                    stick.Unacquire();
                    stick.Dispose();
                }
            }
        }

        public void Dispose()
        {
            CancelLearning();
            Stop();
            _directInput.Dispose();
        }
    }
}
