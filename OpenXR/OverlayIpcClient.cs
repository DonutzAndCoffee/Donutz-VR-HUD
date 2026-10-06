using System.IO;
using System.IO.Pipes;
using Donutz_VR_HUD.Shared;

namespace Donutz_VR_HUD.OpenXR
{
    /// <summary>
    /// Client-side counterpart to <c>NativeLayer/IpcServer.cpp</c>: connects to
    /// the named pipe exposed by the native OpenXR API layer once it has been
    /// loaded inside the target VR application's process (e.g. iRacing.exe),
    /// and streams panel transform/frame/recenter messages to it.
    ///
    /// This replaces the old approach of starting our own OpenXR session
    /// (<see cref="OpenXrOverlay"/>), which caused SteamVR to terminate the
    /// host application's session. Instead, the actual OpenXR session and
    /// frame loop stay owned by the host app; this client only pushes data
    /// for the native layer to compose as additional quad layers.
    /// </summary>
    public sealed class OverlayIpcClient : IDisposable
    {
        private const string PipeName = "DonutzVrHudOverlay";

        /// <summary>
        /// Path to the native layer's diagnostic log file (see
        /// NativeLayer/Logging.cpp). Useful to check whether the DLL was
        /// actually loaded/negotiated by the host application at all, as
        /// opposed to just failing to accept a pipe connection.
        ///
        /// This lives under %ProgramData% rather than %TEMP%, because
        /// %TEMP% resolves per-user and can point somewhere different when
        /// the host application (e.g. iRacing) runs elevated or as another
        /// user/session than the one running this app.
        /// </summary>
        public static string NativeLayerLogPath =>
            Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                "DonutzVrHud",
                "DonutzVrHudLayer.log");

        /// <summary>
        /// Path to the native layer's log level config file (see
        /// NativeLayer/Logging.cpp CurrentLogLevel()). Contains either
        /// "Info" or "Debug" (default: Info when the file is missing).
        /// This lives next to <see cref="NativeLayerLogPath"/> so the
        /// managed app can toggle verbose logging without needing to set
        /// an environment variable for the host application (e.g.
        /// iRacing.exe), which this app does not launch itself.
        /// </summary>
        public static string NativeLayerLogLevelPath =>
            Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                "DonutzVrHud",
                "LogLevel.txt");

        /// <summary>
        /// Appends a managed-side diagnostic line (e.g. "new game session
        /// detected: AssettoCorsa") to the same log file the native layer
        /// writes to (see NativeLayer/Logging.cpp), tagged with "[App]" so
        /// it's visually distinguishable from native entries. This lets the
        /// user (and us, when debugging pipe-connection timing issues like
        /// the one seen with Assetto Corsa/LMU) correlate exactly when a
        /// game/session was detected against when the native layer
        /// negotiated/created its OpenXR instance and IPC pipe, all in one
        /// place (the "Native layer log" view in the UI).
        ///
        /// Uses FileShare.ReadWrite/Append so this doesn't fight the native
        /// layer, which may have the file open concurrently; failures (e.g.
        /// a momentary sharing violation) are swallowed since this is
        /// diagnostic-only and must never affect app behavior.
        /// </summary>
        public static void AppendAppLogEntry(string message)
        {
            try
            {
                var path = NativeLayerLogPath;
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                using var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                using var writer = new StreamWriter(stream);
                writer.WriteLine($"[{DateTime.Now:HH:mm:ss.fff}] [App] {message}");
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }

        private NamedPipeClientStream? _pipe;
        private readonly object _writeLock = new();
        private Task? _readLoopTask;
        private CancellationTokenSource? _readCts;

        /// <summary>
        /// Raised when the native layer reports the final transform of a
        /// panel after a VR controller grab ended (see
        /// NativeLayer/ControllerInput.cpp). Raised on a background
        /// thread; subscribers must marshal to the UI thread as needed.
        /// </summary>
        public event Action<Guid, float, float, float, float, float, float>? PanelTransformUpdated;

        /// <summary>
        /// Raised when the native layer detects a VR controller thumbstick
        /// nudge past its deadzone. The value mirrors
        /// <see cref="Donutz_VR_HUD.Input.NudgeAction"/>'s ordering. Raised
        /// on a background thread; subscribers must marshal to the UI
        /// thread as needed.
        /// </summary>
        public event Action<Donutz_VR_HUD.Input.NudgeAction>? ControllerNudgeActionReceived;

        /// <summary>
        /// Raised when the native layer reports that a VR controller just
        /// grabbed a panel (trigger pressed while pointing at it). Used to
        /// auto-activate edit mode. Raised on a background thread;
        /// subscribers must marshal to the UI thread as needed.
        /// </summary>
        public event Action<Guid>? ControllerGrabStarted;

        /// <summary>True once <see cref="ConnectAsync"/> has established a connection.</summary>
        public bool IsConnected => _pipe?.IsConnected == true;

        /// <summary>Message describing why the last <see cref="ConnectAsync"/> call failed, if any.</summary>
        public string? LastError { get; private set; }

        /// <summary>
        /// Attempts to connect to the native layer's pipe within the given
        /// timeout. Returns false if the layer isn't loaded (e.g. the host VR
        /// application isn't running yet), so callers can show a clear error
        /// instead of silently falling back to a standalone OpenXR session.
        /// </summary>
        public async Task<bool> ConnectAsync(int timeoutMs = 5000)
        {
            LastError = null;
            var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(timeoutMs).ConfigureAwait(false);
                _pipe = pipe;
                StartReadLoop(pipe);
                return true;
            }
            catch (TimeoutException)
            {
                LastError = $"Timed out after {timeoutMs} ms â€“ pipe \"{PipeName}\" was not found. " +
                    "Either the native layer was not loaded into the VR application, or it has not started an OpenXR session yet.";
                pipe.Dispose();
                return false;
            }
            catch (Exception ex)
            {
                LastError = ex.Message;
                pipe.Dispose();
                return false;
            }
        }

        private void StartReadLoop(NamedPipeClientStream pipe)
        {
            var cts = new CancellationTokenSource();
            _readCts = cts;
            _readLoopTask = Task.Run(() => ReadLoopAsync(pipe, cts.Token));
        }

        /// <summary>
        /// Reads server->client messages (see
        /// <see cref="OverlayIpcMessageType.PanelTransformUpdated"/> and
        /// <see cref="OverlayIpcMessageType.ControllerNudgeAction"/>) sent
        /// by the native layer once VR controller input affects a panel.
        /// Runs until the pipe disconnects or <see cref="Dispose"/> cancels
        /// the token.
        ///
        /// Uses asynchronous, cancellable reads (the pipe is opened with
        /// <see cref="PipeOptions.Asynchronous"/>) rather than a
        /// synchronous <c>Read</c> on a background thread: closing a named
        /// pipe handle while another thread is blocked in a synchronous
        /// <c>ReadFile</c> on it can hang the thread that calls
        /// <c>Dispose</c> (observed as the whole WPF app freezing shortly
        /// after iRacing started and this client reconnected/disconnected).
        /// </summary>
        private async Task ReadLoopAsync(NamedPipeClientStream pipe, CancellationToken cancellationToken)
        {
            try
            {
                var header = new byte[5];
                while (!cancellationToken.IsCancellationRequested && pipe.IsConnected)
                {
                    if (!await ReadExactAsync(pipe, header, cancellationToken).ConfigureAwait(false))
                    {
                        break;
                    }

                    var type = (OverlayIpcMessageType)header[0];
                    var length = BitConverter.ToInt32(header, 1);
                    if (length < 0 || length > 16 * 1024 * 1024)
                    {
                        break;
                    }

                    var payload = length > 0 ? new byte[length] : Array.Empty<byte>();
                    if (length > 0 && !await ReadExactAsync(pipe, payload, cancellationToken).ConfigureAwait(false))
                    {
                        break;
                    }

                    HandleServerMessage(type, payload);
                }
            }
            catch (IOException)
            {
                // Pipe broke (host app/layer terminated); IsConnected will
                // reflect this on the next send/read attempt.
            }
            catch (ObjectDisposedException)
            {
                // Client is being disposed; exit quietly.
            }
            catch (OperationCanceledException)
            {
                // Dispose() requested a shutdown; exit quietly.
            }
        }

        private static async Task<bool> ReadExactAsync(NamedPipeClientStream pipe, byte[] buffer, CancellationToken cancellationToken)
        {
            var totalRead = 0;
            while (totalRead < buffer.Length)
            {
                var read = await pipe.ReadAsync(buffer.AsMemory(totalRead), cancellationToken).ConfigureAwait(false);
                if (read <= 0)
                {
                    return false;
                }
                totalRead += read;
            }
            return true;
        }

        private void HandleServerMessage(OverlayIpcMessageType type, byte[] payload)
        {
            switch (type)
            {
                case OverlayIpcMessageType.PanelTransformUpdated:
                    if (payload.Length >= System.Runtime.InteropServices.Marshal.SizeOf<PanelTransformUpdatedMessage>())
                    {
                        var message = BytesToStruct<PanelTransformUpdatedMessage>(payload);
                        PanelTransformUpdated?.Invoke(
                            message.PanelId,
                            message.PositionX, message.PositionY, message.PositionZ,
                            message.RotationDegX, message.RotationDegY, message.RotationDegZ);
                    }
                    break;
                case OverlayIpcMessageType.ControllerNudgeAction:
                    if (payload.Length >= System.Runtime.InteropServices.Marshal.SizeOf<ControllerNudgeActionMessage>())
                    {
                        var message = BytesToStruct<ControllerNudgeActionMessage>(payload);
                        if (Enum.IsDefined(typeof(Donutz_VR_HUD.Input.NudgeAction), (int)message.NudgeAction))
                        {
                            ControllerNudgeActionReceived?.Invoke((Donutz_VR_HUD.Input.NudgeAction)message.NudgeAction);
                        }
                    }
                    break;
                case OverlayIpcMessageType.ControllerGrabStarted:
                    if (payload.Length >= System.Runtime.InteropServices.Marshal.SizeOf<ControllerGrabStartedMessage>())
                    {
                        var message = BytesToStruct<ControllerGrabStartedMessage>(payload);
                        ControllerGrabStarted?.Invoke(message.PanelId);
                    }
                    break;
            }
        }

        private static T BytesToStruct<T>(byte[] bytes) where T : struct
        {
            var handle = System.Runtime.InteropServices.GCHandle.Alloc(bytes, System.Runtime.InteropServices.GCHandleType.Pinned);
            try
            {
                return System.Runtime.InteropServices.Marshal.PtrToStructure<T>(handle.AddrOfPinnedObject());
            }
            finally
            {
                handle.Free();
            }
        }

        public void SetPanelTransform(Guid panelId, bool enabled, float posX, float posY, float posZ, float rotX, float rotY, float rotZ, float widthMeters, float heightMeters, bool headLocked, bool opaqueBackground, bool nonInteractive = false)
        {
            var message = new SetPanelTransformMessage
            {
                PanelId = panelId,
                Enabled = enabled,
                PositionX = posX,
                PositionY = posY,
                PositionZ = posZ,
                RotationDegX = rotX,
                RotationDegY = rotY,
                RotationDegZ = rotZ,
                WidthMeters = widthMeters,
                HeightMeters = heightMeters,
                HeadLocked = headLocked,
                OpaqueBackground = opaqueBackground,
                NonInteractive = nonInteractive,
            };
            Send(OverlayIpcMessageType.SetPanelTransform, StructToBytes(message));
        }

        public unsafe void UpdateFrame(Guid panelId, string sharedHandleName, int width, int height)
        {
            if (sharedHandleName.Length >= OverlayIpcContractConstants.SharedHandleNameLength)
            {
                throw new ArgumentException($"Shared handle name too long (max {OverlayIpcContractConstants.SharedHandleNameLength - 1} chars).", nameof(sharedHandleName));
            }

            var message = new UpdateFrameMessage
            {
                PanelId = panelId,
                Width = width,
                Height = height,
            };

            var span = new Span<char>(message.SharedHandleName, OverlayIpcContractConstants.SharedHandleNameLength);
            span.Clear();
            sharedHandleName.AsSpan().CopyTo(span);

            Send(OverlayIpcMessageType.UpdateFrame, StructToBytes(message));
        }

        public void RemovePanel(Guid panelId)
        {
            var message = new RemovePanelMessage { PanelId = panelId };
            Send(OverlayIpcMessageType.RemovePanel, StructToBytes(message));
        }

        public void Recenter()
        {
            Send(OverlayIpcMessageType.Recenter, Array.Empty<byte>());
        }

        /// <summary>
        /// Tells the native layer whether the app's edit mode is currently
        /// active. VR controllers only grab/nudge panels while this is
        /// true (see NativeLayer/ControllerInput.cpp).
        /// </summary>
        public void SetEditModeActive(bool active)
        {
            var message = new SetEditModeActiveMessage { Active = active };
            Send(OverlayIpcMessageType.SetEditModeActive, StructToBytes(message));
        }

        private void Send(OverlayIpcMessageType type, byte[] payload)
        {
            var pipe = _pipe;
            if (pipe is null || !pipe.IsConnected)
            {
                return;
            }

            lock (_writeLock)
            {
                try
                {
                    Span<byte> header = stackalloc byte[5];
                    header[0] = (byte)type;
                    BitConverter.TryWriteBytes(header[1..], payload.Length);
                    pipe.Write(header);
                    if (payload.Length > 0)
                    {
                        pipe.Write(payload, 0, payload.Length);
                    }
                    pipe.Flush();
                }
                catch (IOException)
                {
                    // Pipe broke (host app/layer terminated); caller can
                    // detect via IsConnected on the next send attempt.
                }
            }
        }

        private static byte[] StructToBytes<T>(T value) where T : struct
        {
            var size = System.Runtime.InteropServices.Marshal.SizeOf<T>();
            var buffer = new byte[size];
            var handle = System.Runtime.InteropServices.GCHandle.Alloc(buffer, System.Runtime.InteropServices.GCHandleType.Pinned);
            try
            {
                System.Runtime.InteropServices.Marshal.StructureToPtr(value, handle.AddrOfPinnedObject(), false);
            }
            finally
            {
                handle.Free();
            }
            return buffer;
        }

        public void Dispose()
        {
            _readCts?.Cancel();
            try
            {
                _readLoopTask?.Wait(TimeSpan.FromSeconds(2));
            }
            catch (AggregateException)
            {
                // Read loop observed the cancellation via an exception;
                // nothing more to do.
            }
            _pipe?.Dispose();
            _pipe = null;
            _readCts?.Dispose();
            _readCts = null;
            _readLoopTask = null;
        }
    }
}
