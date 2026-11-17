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
        private long _lastMotionPacketTimestamp;
        private int _motionTelemetryActive;
        private string? _motionError;
        private byte[] _latestColors = [];

        public string? MotionError => Volatile.Read(ref _motionError);
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
        public event Action<float, bool>? MotionReceived;

        public bool IsReceivingMotion
        {
            get
            {
                var timestamp = Interlocked.Read(ref _lastMotionPacketTimestamp);
                return timestamp != 0 && System.Diagnostics.Stopwatch.GetElapsedTime(timestamp) < TimeSpan.FromSeconds(2);
            }
        }

        public bool HasActiveMotionTelemetry => IsReceivingMotion && Volatile.Read(ref _motionTelemetryActive) != 0;

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

        public static (float HeaveG, bool IsActive) ParseMotionPacket(ReadOnlySpan<byte> packet)
        {
            if (packet.Length == 14 && packet[..4].SequenceEqual("DMOT"u8) && packet[4] == 1)
            {
                throw new FormatException("The running SimHub bridge sends legacy cornering/braking data, not vertical acceleration. Close SimHub, choose Install / update bridge in this HUD, then restart SimHub.");
            }
            // Accept the initial 10-byte heave format, which still used version 1.
            if (packet.Length != 10 || !packet[..4].SequenceEqual("DMOT"u8) || packet[4] is not (1 or 2) || packet[5] > 1)
            {
                throw new FormatException($"Unsupported motion packet (length {packet.Length}). Close SimHub, install the bridge bundled with this HUD, then restart SimHub.");
            }

            var heaveG = BinaryPrimitives.ReadSingleLittleEndian(packet[6..10]);
            if (!float.IsFinite(heaveG))
            {
                throw new FormatException("Motion packet contains non-finite vertical acceleration values.");
            }
            return (heaveG, packet[5] != 0);
        }

        private async Task ReceiveAsync()
        {
            var invalidLedPacketReported = false;
            try
            {
                while (!_stop.IsCancellationRequested)
                {
                    var received = await _udp.ReceiveAsync(_stop.Token).ConfigureAwait(false);
                    var isMotionPacket = received.Buffer.AsSpan().StartsWith("DMOT"u8);
                    try
                    {
                        if (isMotionPacket)
                        {
                            var motion = ParseMotionPacket(received.Buffer);
                            Volatile.Write(ref _motionError, null);
                            Volatile.Write(ref _motionTelemetryActive, motion.IsActive ? 1 : 0);
                            Interlocked.Exchange(ref _lastMotionPacketTimestamp, System.Diagnostics.Stopwatch.GetTimestamp());
                            MotionReceived?.Invoke(motion.IsActive ? motion.HeaveG : 0f, motion.IsActive);
                            continue;
                        }

                        var colors = ParsePacket(received.Buffer);
                        invalidLedPacketReported = false;
                        Volatile.Write(ref _latestColors, colors);
                        Interlocked.Exchange(ref _lastPacketTimestamp, System.Diagnostics.Stopwatch.GetTimestamp());
                        _onColors(colors);
                    }
                    catch (FormatException ex)
                    {
                        if (isMotionPacket)
                        {
                            if (MotionError != ex.Message)
                                _log($"SimHub motion packet rejected: {ex.Message}");
                            Volatile.Write(ref _motionError, ex.Message);
                            Volatile.Write(ref _motionTelemetryActive, 0);
                            Interlocked.Exchange(ref _lastMotionPacketTimestamp, 0);
                        }
                        else if (!invalidLedPacketReported)
                        {
                            _log($"SimHub LED packet rejected: {ex.Message}");
                            invalidLedPacketReported = true;
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
