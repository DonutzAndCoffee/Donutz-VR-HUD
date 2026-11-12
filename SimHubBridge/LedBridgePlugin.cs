using GameReaderCommon;
using SimHub.Plugins;
using SimHub.Plugins.DataPlugins.RGBDriver;
using System;
using System.Net;
using System.Net.Sockets;
using System.Windows.Controls;
using System.Windows.Threading;

namespace Donutz.SimHubLedBridge
{
    [PluginName("Donutz VR HUD LEDs")]
    [PluginAuthor("DonutzAndCoffee")]
    [PluginDescription("Sends LED editor colors to the local Donutz VR HUD RPM strip.")]
    public sealed class LedBridgePlugin : IPlugin, IDataPlugin, IWPFSettings
    {
        private RGBLedsDriver _driver;
        private UdpClient _udp;
        private DispatcherTimer _timer;
        private bool _sendErrorReported;
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

        public void DataUpdate(PluginManager pluginManager, ref GameData data) =>
            _driver.UpdateData(ref data, pluginManager);

        public Control GetWPFSettingsControl(PluginManager pluginManager) => _driver.SettingsControl;

        private void SendColors(object sender, EventArgs e)
        {
            var colors = _driver.GetResult(null);
            var packet = LedPacket.Encode(colors);
            try
            {
                _udp.Send(packet, packet.Length);
                _sendErrorReported = false;
            }
            catch (SocketException ex)
            {
                if (!_sendErrorReported)
                {
                    SimHub.Logging.Current.Warn("Donutz VR HUD LED send failed: " + ex.Message);
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
