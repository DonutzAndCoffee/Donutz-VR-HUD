using GameReaderCommon;
using SimHub.Plugins;
using SimHub.Plugins.DataPlugins.RGBDriver;
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using System.Windows.Controls;
using System.Windows.Threading;

namespace Donutz.SimHubLedBridge
{
    [PluginName("Donutz VR HUD LEDs")]
    [PluginAuthor("DonutzAndCoffee")]
    [PluginDescription("Sends RPM LED colors and vertical vehicle acceleration to Donutz VR HUD.")]
    public sealed class LedBridgePlugin : IPlugin, IDataPlugin, IWPFSettings
    {
        private RGBLedsDriver _driver;
        private UdpClient _udp;
        private DispatcherTimer _timer;
        private bool _sendErrorReported;
        private volatile float _heaveG;
        private volatile bool _telemetryActive;
        private long _lastTelemetryUpdateTimestamp;
        private static Type _cachedGameDataType;
        private static PropertyInfo _gameNewDataProperty;
        private static PropertyInfo _heaveProperty;
        public PluginManager PluginManager { get; set; }

        public void Init(PluginManager pluginManager)
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                _driver = new RGBLedsDriver("DonutzVrHudLeds", false, null, false);
                _driver.DefaultLedResult = new LedResult(_driver.Settings.HSLPreference);
                _driver.DefaultLedResult.Clear(256);
                _driver.SetEnabledCallback(() => true);
                _udp = new UdpClient();
                _udp.Connect(IPAddress.Loopback, 26741);
                _timer = new DispatcherTimer(DispatcherPriority.Background)
                {
                    Interval = TimeSpan.FromMilliseconds(33)
                };
                _timer.Tick += SendColors;
                _timer.Start();
            });
            SimHub.Logging.Current.Info("Donutz VR HUD LED bridge started on localhost UDP port 26741.");
        }

        public void DataUpdate(PluginManager pluginManager, ref GameData data)
        {
            _driver.UpdateData(ref data, pluginManager);
            var heaveG = 0f;
            var telemetryActive = data.GameRunning && !data.GamePaused && !data.GameInMenu &&
                TryReadHeave(data, out heaveG);
            _heaveG = telemetryActive ? heaveG : 0f;
            _telemetryActive = telemetryActive;
            Interlocked.Exchange(ref _lastTelemetryUpdateTimestamp, Stopwatch.GetTimestamp());
        }

        private static bool TryReadHeave(GameData data, out float heaveG)
        {
            heaveG = 0f;
            var dataType = data.GetType();
            if (dataType != _cachedGameDataType)
            {
                var newDataProperty = dataType.GetProperty("GameNewData");
                var statusDataType = newDataProperty == null ? null : newDataProperty.PropertyType;
                _gameNewDataProperty = newDataProperty;
                _heaveProperty = statusDataType == null ? null : statusDataType.GetProperty("AccelerationHeave");
                _cachedGameDataType = dataType;
            }

            var newData = _gameNewDataProperty == null ? null : _gameNewDataProperty.GetValue(data, null);
            var rawValue = _heaveProperty == null ? null : _heaveProperty.GetValue(newData, null);
            if (rawValue is double acceleration && !double.IsNaN(acceleration) && !double.IsInfinity(acceleration))
            {
                heaveG = (float)Math.Max(-3d, Math.Min(3d, acceleration));
                return true;
            }

            return false;
        }

        public Control GetWPFSettingsControl(PluginManager pluginManager) => _driver.SettingsControl;

        private void SendColors(object sender, EventArgs e)
        {
            var colors = _driver.GetResult(null);
            var packet = LedPacket.Encode(colors);
            var updateTimestamp = Interlocked.Read(ref _lastTelemetryUpdateTimestamp);
            var telemetryIsFresh = updateTimestamp != 0 &&
                (Stopwatch.GetTimestamp() - updateTimestamp) / (double)Stopwatch.Frequency < 0.5;
            var motionPacket = LedPacket.EncodeMotionPacket(
                telemetryIsFresh ? _heaveG : 0f,
                telemetryIsFresh && _telemetryActive);
            try
            {
                _udp.Send(packet, packet.Length);
                _udp.Send(motionPacket, motionPacket.Length);
                _sendErrorReported = false;
            }
            catch (SocketException ex)
            {
                if (!_sendErrorReported)
                {
                    SimHub.Logging.Current.Warn("Donutz VR HUD telemetry send failed: " + ex.Message);
                    _sendErrorReported = true;
                }
            }
        }

        public void End(PluginManager pluginManager)
        {
            System.Windows.Application.Current.Dispatcher.Invoke(() =>
            {
                _timer.Stop();
                _timer.Tick -= SendColors;
                _driver.SaveSettings();
                _driver.Dispose();
                _udp.Dispose();
            });
        }
    }
}
