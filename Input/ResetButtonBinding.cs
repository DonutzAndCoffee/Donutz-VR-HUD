using SharpDX.DirectInput;

namespace Donutz_VR_HUD.Input
{
    /// <summary>A DirectInput device the user can pick a button from (e.g. a racing wheel).</summary>
    public sealed record InputDeviceInfo(Guid InstanceGuid, string Name);

    /// <summary>A single button binding, bound to a specific device/button.</summary>
    public sealed record ResetButtonEntry(Guid DeviceInstanceGuid, string? DeviceName, int ButtonIndex);

    /// <summary>
    /// Lets the user assign one or more buttons on one or more connected DirectInput
    /// devices (e.g. a steering wheel) as "VR Reset" (recenter) triggers, and polls
    /// them on a background thread so it works even while the app window isn't
    /// focused.
    ///
    /// Nobody memorizes device GUIDs or button indices, so binding is done via
    /// "learn mode": all connected devices are polled simultaneously and the
    /// very first button press detected anywhere is auto-assigned (added to the
    /// existing set of bindings, not replacing it).
    ///
    /// If a bound device isn't currently attached (e.g. the wheel is powered
    /// off), that binding is simply skipped instead of throwing/crashing the app;
    /// it keeps being retried in the background in case the device shows up later.
    /// </summary>
    public sealed class ResetButtonBinding : IDisposable
    {
        private sealed class BoundEntry
        {
            public required Guid DeviceInstanceGuid;
            public string? DeviceName;
            public int ButtonIndex;
            public Joystick? Joystick;
            public bool WasPressed;
        }

        private readonly DirectInput _directInput = new();
        private readonly List<BoundEntry> _entries = new();
        private readonly object _entriesLock = new();
        private Thread? _pollThread;
        private volatile bool _running;

        private Thread? _learnThread;
        private volatile bool _learning;

        /// <summary>Raised on the poll thread whenever any bound button transitions from released to pressed.</summary>
        public event Action? ButtonPressed;

        /// <summary>Raised on a background thread once learn mode detects and binds a new button press.</summary>
        public event Action<InputDeviceInfo, int>? Learned;

        public bool IsLearning => _learning;

        /// <summary>Snapshot of all currently bound entries.</summary>
        public IReadOnlyList<ResetButtonEntry> Bindings
        {
            get
            {
                lock (_entriesLock)
                {
                    return _entries
                        .Select(e => new ResetButtonEntry(e.DeviceInstanceGuid, e.DeviceName, e.ButtonIndex))
                        .ToList();
                }
            }
        }

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
        /// automatically adds a binding for the first button pressed on any of them
        /// (existing bindings are kept). Call <see cref="CancelLearning"/> to abort
        /// without binding.
        /// </summary>
        public void StartLearning()
        {
            CancelLearning();

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
                        // Device may not be a joystick-compatible object, or may not be
                        // currently attached; skip it.
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
                                AddBinding(info.InstanceGuid, pressedIndex, info.Name);
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
                    try
                    {
                        stick.Unacquire();
                        stick.Dispose();
                    }
                    catch
                    {
                        // Ignore errors tearing down a possibly-unplugged device.
                    }
                }
            }
        }

        /// <summary>
        /// Adds an additional binding for the given device/button (0-based), keeping
        /// any bindings already present. If the device isn't currently attached, the
        /// binding is still recorded but simply won't fire until it appears.
        /// </summary>
        public void AddBinding(Guid deviceInstanceGuid, int buttonIndex, string? deviceName = null)
        {
            var entry = new BoundEntry
            {
                DeviceInstanceGuid = deviceInstanceGuid,
                DeviceName = deviceName,
                ButtonIndex = buttonIndex,
                Joystick = TryCreateJoystick(deviceInstanceGuid),
                WasPressed = false
            };

            lock (_entriesLock)
            {
                _entries.Add(entry);
            }

            EnsurePolling();
        }

        /// <summary>Removes the binding at the given index (as returned by <see cref="Bindings"/>).</summary>
        public void RemoveBinding(int index)
        {
            BoundEntry? removed = null;
            lock (_entriesLock)
            {
                if (index >= 0 && index < _entries.Count)
                {
                    removed = _entries[index];
                    _entries.RemoveAt(index);
                }
            }

            if (removed?.Joystick is { } joystick)
            {
                try
                {
                    joystick.Unacquire();
                    joystick.Dispose();
                }
                catch
                {
                    // Ignore errors tearing down a possibly-unplugged device.
                }
            }
        }

        /// <summary>Removes all bindings.</summary>
        public void ClearBindings()
        {
            List<BoundEntry> removed;
            lock (_entriesLock)
            {
                removed = new List<BoundEntry>(_entries);
                _entries.Clear();
            }

            foreach (var entry in removed)
            {
                if (entry.Joystick is { } joystick)
                {
                    try
                    {
                        joystick.Unacquire();
                        joystick.Dispose();
                    }
                    catch
                    {
                        // Ignore errors tearing down a possibly-unplugged device.
                    }
                }
            }
        }

        private static Joystick? TryCreateJoystick(Guid deviceInstanceGuid)
        {
            try
            {
                var directInput = new DirectInput();
                var joystick = new Joystick(directInput, deviceInstanceGuid);
                joystick.Acquire();
                return joystick;
            }
            catch
            {
                // Device isn't currently attached (e.g. the wheel is powered off) or
                // couldn't be acquired; skip it instead of crashing the app. It will
                // be retried lazily by the poll loop.
                return null;
            }
        }

        private void EnsurePolling()
        {
            if (_running)
            {
                return;
            }

            _running = true;
            _pollThread = new Thread(PollLoop) { IsBackground = true, Name = "VR Reset Button Poll" };
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
            while (_running)
            {
                List<BoundEntry> entriesSnapshot;
                lock (_entriesLock)
                {
                    entriesSnapshot = new List<BoundEntry>(_entries);
                }

                foreach (var entry in entriesSnapshot)
                {
                    try
                    {
                        // Lazily (re)acquire a device that wasn't attached when the
                        // binding was added/loaded, e.g. a wheel powered on later.
                        entry.Joystick ??= TryCreateJoystick(entry.DeviceInstanceGuid);
                        if (entry.Joystick is null)
                        {
                            continue;
                        }

                        entry.Joystick.Poll();
                        var state = entry.Joystick.GetCurrentState();
                        var isPressed = entry.ButtonIndex >= 0 && entry.ButtonIndex < state.Buttons.Length && state.Buttons[entry.ButtonIndex];

                        if (isPressed && !entry.WasPressed)
                        {
                            ButtonPressed?.Invoke();
                        }

                        entry.WasPressed = isPressed;
                    }
                    catch
                    {
                        // Device may have been unplugged; drop the handle so it gets
                        // re-acquired lazily, and keep polling the other bindings.
                        try
                        {
                            entry.Joystick?.Dispose();
                        }
                        catch
                        {
                            // Ignore.
                        }

                        entry.Joystick = null;
                    }
                }

                Thread.Sleep(16);
            }
        }

        public void Dispose()
        {
            CancelLearning();
            Stop();
            ClearBindings();
            _directInput.Dispose();
        }
    }
}
