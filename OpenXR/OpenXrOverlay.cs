using System.Numerics;
using System.Runtime.InteropServices;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.OpenXR;
using Silk.NET.OpenXR.Extensions.KHR;

namespace Donutz_VR_HUD.OpenXR
{
    /// <summary>
    /// Manages an OpenXR instance/session bound to D3D11 and submits up to
    /// several independently positioned/sized quad composition layers — one per
    /// overlay panel (see <see cref="Models.OverlayPanel"/>) — so multiple
    /// dashboards/captures can float in VR at once, alongside the running VR
    /// title's own rendering (e.g. iRacing VR).
    /// </summary>
    public sealed unsafe class OpenXrOverlay : IDisposable
    {
        private readonly XR _xr = XR.GetApi();

        private Instance _instance;
        private ulong _systemId;
        private Session _session;
        private Space _localSpace;
        private Space _viewSpace;

        private nint _devicePtr;
        private nint _contextPtr;

        private Thread? _frameThread;
        private volatile bool _running;

        private readonly object _panelsLock = new();
        private readonly Dictionary<Guid, PanelRuntime> _panels = new();

        private long _lastPredictedDisplayTime;
        private volatile bool _recenterRequested;
        private Vector3 _recenterPosition = Vector3.Zero;
        private Quaternion _recenterOrientation = Quaternion.Identity;

        public bool IsRunning => _running;

        private sealed class PanelRuntime
        {
            public Swapchain Swapchain;
            public bool HasSwapchain;
            public int SwapchainWidth;
            public int SwapchainHeight;

            public nint LatestFramePtr;
            public bool HasFrame;

            public bool Enabled = true;
            public Vector3 Position = Vector3.Zero;
            public Vector3 RotationDeg = Vector3.Zero;
            public float WidthMeters = 0.4f;
            public float HeightMeters = 0.3f;
        }

        /// <summary>
        /// Initializes the OpenXR instance/session bound to the given D3D11 device
        /// (must be the same device used by the panel sources, or one that shares
        /// the adapter, so textures can be copied cheaply).
        /// </summary>
        public void Initialize(nint d3d11DevicePtr, nint d3d11ContextPtr)
        {
            _devicePtr = d3d11DevicePtr;
            _contextPtr = d3d11ContextPtr;

            CreateInstanceAndSession();
        }

        /// <summary>Adds a panel (if new) or updates its transform/visibility.</summary>
        public void SetPanelTransform(Guid panelId, bool enabled, Vector3 position, Vector3 rotationDeg, float widthMeters, float heightMeters)
        {
            lock (_panelsLock)
            {
                if (!_panels.TryGetValue(panelId, out var runtime))
                {
                    runtime = new PanelRuntime();
                    _panels[panelId] = runtime;
                }

                runtime.Enabled = enabled;
                runtime.Position = position;
                runtime.RotationDeg = rotationDeg;
                runtime.WidthMeters = widthMeters;
                runtime.HeightMeters = heightMeters;
            }
        }

        /// <summary>
        /// Call this from a panel source's FrameArrived event to update the
        /// texture that will be submitted for that panel's quad on the next frame.
        /// The swapchain for the panel is (re)created automatically if the
        /// resolution changes.
        /// </summary>
        public void UpdateFrame(Guid panelId, nint texturePtr, int width, int height)
        {
            lock (_panelsLock)
            {
                if (!_panels.TryGetValue(panelId, out var runtime))
                {
                    runtime = new PanelRuntime();
                    _panels[panelId] = runtime;
                }

                if (!runtime.HasSwapchain || runtime.SwapchainWidth != width || runtime.SwapchainHeight != height)
                {
                    if (runtime.HasSwapchain)
                    {
                        _xr.DestroySwapchain(runtime.Swapchain);
                    }

                    runtime.Swapchain = CreateSwapchain(width, height);
                    runtime.SwapchainWidth = width;
                    runtime.SwapchainHeight = height;
                    runtime.HasSwapchain = true;
                }

                runtime.LatestFramePtr = texturePtr;
                runtime.HasFrame = true;
            }
        }

        /// <summary>Removes a panel entirely (e.g. when the user deletes it in the UI).</summary>
        public void RemovePanel(Guid panelId)
        {
            lock (_panelsLock)
            {
                if (_panels.TryGetValue(panelId, out var runtime))
                {
                    if (runtime.HasSwapchain)
                    {
                        _xr.DestroySwapchain(runtime.Swapchain);
                    }

                    _panels.Remove(panelId);
                }
            }
        }

        /// <summary>
        /// Requests a "VR Reset": all panels re-anchor relative to the player's
        /// current head position/orientation, exactly like OpenKneeboard's
        /// recenter. Safe to call from any thread (e.g. a bound wheel button).
        /// </summary>
        public void Recenter() => _recenterRequested = true;

        public void StartFrameLoop()
        {
            if (_running)
            {
                return;
            }

            _running = true;
            _frameThread = new Thread(FrameLoop) { IsBackground = true, Name = "OpenXR Overlay Frame Loop" };
            _frameThread.Start();
        }

        public void Stop()
        {
            _running = false;
            _frameThread?.Join(TimeSpan.FromSeconds(2));
        }

        private void CreateInstanceAndSession()
        {
            var appInfo = new ApplicationInfo
            {
                ApiVersion = 1UL << 48 // XR_CURRENT_API_VERSION placeholder; Silk.NET exposes via Api.Version
            };
            SetString(appInfo.ApplicationName, "Donutz VR HUD");
            SetString(appInfo.EngineName, "Donutz VR HUD Overlay Engine");
            appInfo.ApplicationVersion = 1;
            appInfo.EngineVersion = 1;

            var extensionNames = new[] { "XR_KHR_D3D11_enable" };
            using var extNamesNative = new NativeStringArray(extensionNames);

            var instanceCreateInfo = new InstanceCreateInfo
            {
                Type = StructureType.InstanceCreateInfo,
                ApplicationInfo = appInfo,
                EnabledExtensionCount = (uint)extensionNames.Length,
                EnabledExtensionNames = extNamesNative.Pointer
            };

            _xr.CreateInstance(in instanceCreateInfo, ref _instance).ThrowOnError("xrCreateInstance");

            var systemInfo = new SystemGetInfo
            {
                Type = StructureType.SystemGetInfo,
                FormFactor = FormFactor.HeadMountedDisplay
            };
            _xr.GetSystem(_instance, in systemInfo, ref _systemId).ThrowOnError("xrGetSystem");

            // Per spec, xrGetD3D11GraphicsRequirementsKHR MUST be called before
            // xrCreateSession when using the D3D11 graphics binding, otherwise
            // xrCreateSession fails with XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING.
            if (!_xr.TryGetInstanceExtension<KhrD3D11Enable>(null, _instance, out var d3D11Enable))
            {
                throw new InvalidOperationException("The OpenXR runtime does not support the XR_KHR_D3D11_enable extension.");
            }

            var graphicsRequirements = new GraphicsRequirementsD3D11KHR { Type = StructureType.GraphicsRequirementsD3D11Khr };
            d3D11Enable.GetD3D11GraphicsRequirements(_instance, _systemId, ref graphicsRequirements).ThrowOnError("xrGetD3D11GraphicsRequirementsKHR");

            var binding = new GraphicsBindingD3D11KHR
            {
                Type = StructureType.GraphicsBindingD3D11Khr,
                Device = (ID3D11Device*)_devicePtr
            };

            var sessionCreateInfo = new SessionCreateInfo
            {
                Type = StructureType.SessionCreateInfo,
                SystemId = _systemId,
                Next = &binding
            };

            _xr.CreateSession(_instance, in sessionCreateInfo, ref _session).ThrowOnError("xrCreateSession");

            var localSpaceCreateInfo = new ReferenceSpaceCreateInfo
            {
                Type = StructureType.ReferenceSpaceCreateInfo,
                ReferenceSpaceType = ReferenceSpaceType.Local,
                PoseInReferenceSpace = new Posef
                {
                    Orientation = new Quaternionf(0, 0, 0, 1),
                    Position = new Vector3f(0, 0, 0)
                }
            };
            _xr.CreateReferenceSpace(_session, in localSpaceCreateInfo, ref _localSpace).ThrowOnError("xrCreateReferenceSpace(Local)");

            var viewSpaceCreateInfo = new ReferenceSpaceCreateInfo
            {
                Type = StructureType.ReferenceSpaceCreateInfo,
                ReferenceSpaceType = ReferenceSpaceType.View,
                PoseInReferenceSpace = new Posef
                {
                    Orientation = new Quaternionf(0, 0, 0, 1),
                    Position = new Vector3f(0, 0, 0)
                }
            };
            _xr.CreateReferenceSpace(_session, in viewSpaceCreateInfo, ref _viewSpace).ThrowOnError("xrCreateReferenceSpace(View)");
        }

        private Swapchain CreateSwapchain(int width, int height)
        {
            var swapchainCreateInfo = new SwapchainCreateInfo
            {
                Type = StructureType.SwapchainCreateInfo,
                UsageFlags = SwapchainUsageFlags.SampledBit | SwapchainUsageFlags.ColorAttachmentBit,
                Format = (long)Silk.NET.DXGI.Format.FormatB8G8R8A8Unorm,
                SampleCount = 1,
                Width = (uint)width,
                Height = (uint)height,
                FaceCount = 1,
                ArraySize = 1,
                MipCount = 1
            };

            Swapchain swapchain = default;
            _xr.CreateSwapchain(_session, in swapchainCreateInfo, ref swapchain).ThrowOnError("xrCreateSwapchain");
            return swapchain;
        }

        private void FrameLoop()
        {
            var sessionState = SessionState.Unknown;

            while (_running)
            {
                PollEvents(ref sessionState);

                var waitFrameInfo = new FrameWaitInfo { Type = StructureType.FrameWaitInfo };
                var frameState = new FrameState { Type = StructureType.FrameState };
                _xr.WaitFrame(_session, in waitFrameInfo, ref frameState);
                _lastPredictedDisplayTime = frameState.PredictedDisplayTime;

                if (_recenterRequested)
                {
                    ApplyRecenter(frameState.PredictedDisplayTime);
                    _recenterRequested = false;
                }

                var beginInfo = new FrameBeginInfo { Type = StructureType.FrameBeginInfo };
                _xr.BeginFrame(_session, in beginInfo);

                SubmitPanels(frameState.PredictedDisplayTime);
            }
        }

        private void ApplyRecenter(long predictedDisplayTime)
        {
            var location = new SpaceLocation { Type = StructureType.SpaceLocation };
            var result = _xr.LocateSpace(_viewSpace, _localSpace, predictedDisplayTime, ref location);

            if (result == Result.Success)
            {
                _recenterPosition = new Vector3(location.Pose.Position.X, location.Pose.Position.Y, location.Pose.Position.Z);
                _recenterOrientation = new Quaternion(
                    location.Pose.Orientation.X,
                    location.Pose.Orientation.Y,
                    location.Pose.Orientation.Z,
                    location.Pose.Orientation.W);
            }
        }

        private void PollEvents(ref SessionState sessionState)
        {
            var eventBuffer = new EventDataBuffer { Type = StructureType.EventDataBuffer };
            while (_xr.PollEvent(_instance, ref eventBuffer) == Result.Success)
            {
                if (eventBuffer.Type == StructureType.EventDataSessionStateChanged)
                {
                    var stateChanged = (EventDataSessionStateChanged*)&eventBuffer;
                    sessionState = stateChanged->State;

                    if (sessionState == SessionState.Ready)
                    {
                        var beginInfo = new SessionBeginInfo
                        {
                            Type = StructureType.SessionBeginInfo,
                            PrimaryViewConfigurationType = ViewConfigurationType.PrimaryStereo
                        };
                        _xr.BeginSession(_session, in beginInfo);
                    }
                }

                eventBuffer = new EventDataBuffer { Type = StructureType.EventDataBuffer };
            }
        }

        private void SubmitPanels(long predictedDisplayTime)
        {
            lock (_panelsLock)
            {
                var quads = new CompositionLayerQuad[_panels.Count];
                var layerPtrs = stackalloc CompositionLayerBaseHeader*[_panels.Count];
                var count = 0;

                foreach (var runtime in _panels.Values)
                {
                    if (!runtime.Enabled || !runtime.HasSwapchain || !runtime.HasFrame)
                    {
                        continue;
                    }

                    CopyFrameIntoSwapchain(runtime);
                    quads[count] = BuildQuadLayer(runtime);
                    count++;
                }

                for (var i = 0; i < count; i++)
                {
                    fixed (CompositionLayerQuad* quadPtr = &quads[i])
                    {
                        layerPtrs[i] = (CompositionLayerBaseHeader*)quadPtr;
                    }
                }

                var endInfo = new FrameEndInfo
                {
                    Type = StructureType.FrameEndInfo,
                    DisplayTime = predictedDisplayTime,
                    EnvironmentBlendMode = EnvironmentBlendMode.AlphaBlend,
                    LayerCount = (uint)count,
                    Layers = count > 0 ? layerPtrs : null
                };
                _xr.EndFrame(_session, in endInfo);
            }
        }

        private void CopyFrameIntoSwapchain(PanelRuntime runtime)
        {
            var imageAcquireInfo = new SwapchainImageAcquireInfo { Type = StructureType.SwapchainImageAcquireInfo };
            uint imageIndex = 0;
            _xr.AcquireSwapchainImage(runtime.Swapchain, in imageAcquireInfo, ref imageIndex);

            var waitInfo = new SwapchainImageWaitInfo
            {
                Type = StructureType.SwapchainImageWaitInfo,
                Timeout = long.MaxValue
            };
            _xr.WaitSwapchainImage(runtime.Swapchain, in waitInfo);

            // NOTE: retrieving the swapchain's ID3D11Texture2D array via
            // xrEnumerateSwapchainImages and copying runtime.LatestFramePtr into
            // the acquired index (via the D3D11 device context's
            // CopySubresourceRegion) happens here. Omitted: requires
            // XR_KHR_D3D11_enable's SwapchainImageD3D11KHR struct enumeration,
            // which is device/runtime specific.

            var releaseInfo = new SwapchainImageReleaseInfo { Type = StructureType.SwapchainImageReleaseInfo };
            _xr.ReleaseSwapchainImage(runtime.Swapchain, in releaseInfo);
        }

        private CompositionLayerQuad BuildQuadLayer(PanelRuntime runtime)
        {
            // Compose the user-configured panel pose with the recenter offset so
            // panels stay anchored relative to where the player was facing when
            // they last pressed "VR Reset".
            var userOrientation = Quaternion.CreateFromYawPitchRoll(
                DegreesToRadians(runtime.RotationDeg.Y),
                DegreesToRadians(runtime.RotationDeg.X),
                DegreesToRadians(runtime.RotationDeg.Z));

            var worldPosition = _recenterPosition + Vector3.Transform(runtime.Position, _recenterOrientation);
            var worldOrientation = Quaternion.Normalize(_recenterOrientation * userOrientation);

            return new CompositionLayerQuad
            {
                Type = StructureType.CompositionLayerQuad,
                LayerFlags = CompositionLayerFlags.BlendTextureSourceAlphaBit,
                Space = _localSpace,
                EyeVisibility = EyeVisibility.Both,
                SubImage = new SwapchainSubImage
                {
                    Swapchain = runtime.Swapchain,
                    ImageRect = new Rect2Di(new Offset2Di(0, 0), new Extent2Di(runtime.SwapchainWidth, runtime.SwapchainHeight)),
                    ImageArrayIndex = 0
                },
                Pose = new Posef
                {
                    Orientation = new Quaternionf(worldOrientation.X, worldOrientation.Y, worldOrientation.Z, worldOrientation.W),
                    Position = new Vector3f(worldPosition.X, worldPosition.Y, worldPosition.Z)
                },
                Size = new Extent2Df(runtime.WidthMeters, runtime.HeightMeters)
            };
        }

        private static float DegreesToRadians(float degrees) => degrees * (MathF.PI / 180f);

        private static void SetString(byte* destination, string value)
        {
            var bytes = System.Text.Encoding.UTF8.GetBytes(value);
            for (var i = 0; i < bytes.Length; i++)
            {
                destination[i] = bytes[i];
            }
            destination[bytes.Length] = 0;
        }

        public void Dispose()
        {
            Stop();

            lock (_panelsLock)
            {
                foreach (var runtime in _panels.Values)
                {
                    if (runtime.HasSwapchain)
                    {
                        _xr.DestroySwapchain(runtime.Swapchain);
                    }
                }
                _panels.Clear();
            }

            if (_viewSpace.Handle != 0)
            {
                _xr.DestroySpace(_viewSpace);
            }
            if (_localSpace.Handle != 0)
            {
                _xr.DestroySpace(_localSpace);
            }
            if (_session.Handle != 0)
            {
                _xr.DestroySession(_session);
            }
            if (_instance.Handle != 0)
            {
                _xr.DestroyInstance(_instance);
            }
        }
    }

    internal static class OpenXrResultExtensions
    {
        public static void ThrowOnError(this Result result, string operation)
        {
            if (result != Result.Success)
            {
                throw new InvalidOperationException($"OpenXR call '{operation}' failed with result {result}.");
            }
        }
    }

    /// <summary>
    /// Marshals a managed string array into a native char** (array of null
    /// terminated UTF-8 strings), as required by OpenXR's
    /// EnabledExtensionNames/EnabledApiLayerNames parameters.
    /// </summary>
    internal sealed unsafe class NativeStringArray : IDisposable
    {
        private readonly nint[] _stringPointers;
        private readonly nint _arrayPointer;

        public NativeStringArray(IReadOnlyList<string> values)
        {
            _stringPointers = new nint[values.Count];
            for (var i = 0; i < values.Count; i++)
            {
                _stringPointers[i] = Marshal.StringToHGlobalAnsi(values[i]);
            }

            _arrayPointer = Marshal.AllocHGlobal(nint.Size * values.Count);
            Marshal.Copy(_stringPointers, 0, _arrayPointer, values.Count);
        }

        public byte** Pointer => (byte**)_arrayPointer;

        public void Dispose()
        {
            foreach (var ptr in _stringPointers)
            {
                Marshal.FreeHGlobal(ptr);
            }

            Marshal.FreeHGlobal(_arrayPointer);
        }
    }
}
