using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;

namespace Donutz_VR_HUD.Services
{
    public sealed class SimHubLedReceiver : IDisposable
    {
        public const int Port = 26741;
        public const int MaxLeds = 256;
        private readonly UdpClient _udp;
        private readonly CancellationTokenSource _stop = new();
        private readonly Action<byte[]> _onColors;
        private readonly Action<string> _log;
        private int _disposed;
        private long _lastPacketTimestamp;
        private byte[] _latestColors = [];

        internal byte[] LatestColors => IsReceiving ? Volatile.Read(ref _latestColors) : [];

        public bool IsReceiving
        {
            get
            {
                var timestamp = Interlocked.Read(ref _lastPacketTimestamp);
                return timestamp != 0 && System.Diagnostics.Stopwatch.GetElapsedTime(timestamp) < TimeSpan.FromSeconds(2);
            }
        }

        public SimHubLedReceiver(Action<byte[]> onColors, Action<string> log, int port = Port)
        {
            _onColors = onColors;
            _log = log;
            _udp = new UdpClient(AddressFamily.InterNetwork);
            try
            {
                _udp.ExclusiveAddressUse = true;
                _udp.Client.Bind(new IPEndPoint(IPAddress.Loopback, port));
                LocalPort = ((IPEndPoint)_udp.Client.LocalEndPoint!).Port;
                Completion = ReceiveAsync();
            }
            catch
            {
                _udp.Dispose();
                _stop.Dispose();
                throw;
            }
        }

        public int LocalPort { get; }
        public Task Completion { get; }

        public static byte[] ParsePacket(ReadOnlySpan<byte> packet)
        {
            if (packet.Length < 8 || !packet[..4].SequenceEqual("DLED"u8) || packet[4] != 1 || packet[5] != 0)
            {
                throw new FormatException("Invalid LED packet header or protocol version.");
            }
            var count = BinaryPrimitives.ReadUInt16LittleEndian(packet[6..8]);
            if (count > MaxLeds || packet.Length != 8 + count * 3)
            {
                throw new FormatException("Invalid LED packet color count or payload length.");
            }
            return packet[8..].ToArray();
        }

        private async Task ReceiveAsync()
        {
            var invalidPacketReported = false;
            try
            {
                while (!_stop.IsCancellationRequested)
                {
                    var received = await _udp.ReceiveAsync(_stop.Token).ConfigureAwait(false);
                    try
                    {
                        var colors = ParsePacket(received.Buffer);
                        invalidPacketReported = false;
                        Volatile.Write(ref _latestColors, colors);
                        Interlocked.Exchange(ref _lastPacketTimestamp, System.Diagnostics.Stopwatch.GetTimestamp());
                        _onColors(colors);
                    }
                    catch (FormatException ex)
                    {
                        if (!invalidPacketReported)
                        {
                            _log($"SimHub LED packet rejected: {ex.Message}");
                            invalidPacketReported = true;
                        }
                    }
                }
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
            catch (ObjectDisposedException) when (_stop.IsCancellationRequested) { }
            catch (SocketException) when (_stop.IsCancellationRequested) { }
            catch (SocketException ex)
            {
                _log($"SimHub LED receiver failed: {ex.Message}");
            }
            finally
            {
                _udp.Dispose();
            }
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            _stop.Cancel();
            _udp.Dispose();
            _ = Completion.ContinueWith(_ => _stop.Dispose(), CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }
}
