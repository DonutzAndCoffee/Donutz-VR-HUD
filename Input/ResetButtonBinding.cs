using SharpDX.DirectInput;

namespace Donutz_VR_HUD.Input
{
    /// <summary>A DirectInput device the user can pick a button from (e.g. a racing wheel).</summary>
    public sealed record InputDeviceInfo(Guid InstanceGuid, string Name);

    /// <summary>
    /// Lets the user assign any button on any connected DirectInput device (e.g. a
    /// steering wheel) as the "VR Reset" (recenter) trigger, and polls it on a
    /// background thread so it works even while the app window isn't focused.
    ///
    /// Nobody memorizes device GUIDs or button indices, so binding is done via
    /// "learn mode": all connected devices are polled simultaneously and the
    /// very first button press detected anywhere is auto-assigned.
    /// </summary>
    public sealed class ResetButtonBinding : IDisposable
    {
        private readonly DirectInput _directInput = new();
        private Joystick? _joystick;
        private Thread? _pollThread;
        private volatile bool _running;
        private int _buttonIndex = -1;
        private bool _wasPressed;

        private Thread? _learnThread;
        private volatile bool _learning;

        /// <summary>Raised on the poll thread whenever the bound button transitions from released to pressed.</summary>
        public event Action? ButtonPressed;

        /// <summary>Raised on a background thread once learn mode detects and binds a button press.</summary>
        public event Action<InputDeviceInfo, int>? Learned;

        public string? BoundDeviceName { get; private set; }
        public Guid BoundDeviceGuid { get; private set; }
        public int BoundButtonIndex { get; private set; } = -1;
        public bool IsLearning => _learning;

        public static IReadOnlyList<InputDeviceInfo> GetAvailableDevices()
        {
            using var directInput = new DirectInput();
            var devices = new List<InputDeviceInfo>();

            foreach (var device in directInput.GetDevices(DeviceClass.GameControl, DeviceEnumerationFlags.AttachedOnly))
            {
                devices.Add(new InputDeviceInfo(device.InstanceGuid, device.InstanceName));
            }

            return devices;
        }

        /// <summary>
        /// Starts "learn mode": polls every connected DirectInput device at once and
        /// automatically binds to the first button pressed on any of them. Call
        /// <see cref="CancelLearning"/> to abort without binding.
        /// </summary>
        public void StartLearning()
        {
            CancelLearning();
            Stop();

            _learning = true;
            _learnThread = new Thread(LearnLoop) { IsBackground = true, Name = "VR Reset Button Learn" };
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
            var devices = GetAvailableDevices();
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
                                Bind(info.InstanceGuid, pressedIndex, info.Name);
                                Learned?.Invoke(info, pressedIndex);
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

        /// <summary>Binds to the given device and starts polling the given button index (0-based).</summary>
        public void Bind(Guid deviceInstanceGuid, int buttonIndex, string? deviceName = null)
        {
            Stop();

            _joystick = new Joystick(_directInput, deviceInstanceGuid);
            _joystick.Acquire();
            _buttonIndex = buttonIndex;
            _wasPressed = false;

            BoundDeviceName = deviceName;
            BoundDeviceGuid = deviceInstanceGuid;
            BoundButtonIndex = buttonIndex;

            _running = true;
            _pollThread = new Thread(PollLoop) { IsBackground = true, Name = "VR Reset Button Poll" };
            _pollThread.Start();
        }

        public void Stop()
        {
            _running = false;
            _pollThread?.Join(TimeSpan.FromSeconds(1));
            _pollThread = null;

            _joystick?.Unacquire();
            _joystick?.Dispose();
            _joystick = null;
        }

        private void PollLoop()
        {
            while (_running && _joystick is not null)
            {
                try
                {
                    _joystick.Poll();
                    var state = _joystick.GetCurrentState();
                    var isPressed = _buttonIndex >= 0 && _buttonIndex < state.Buttons.Length && state.Buttons[_buttonIndex];

                    if (isPressed && !_wasPressed)
                    {
                        ButtonPressed?.Invoke();
                    }

                    _wasPressed = isPressed;
                }
                catch
                {
                    // Device may have been unplugged; keep trying until Stop() is called.
                }

                Thread.Sleep(16);
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
