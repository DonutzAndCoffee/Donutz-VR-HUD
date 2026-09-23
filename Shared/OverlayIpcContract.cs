using System.Runtime.InteropServices;

namespace Donutz_VR_HUD.Shared
{
    /// <summary>
    /// Shared message layout for the named-pipe IPC between the WPF
    /// configuration app (client) and the native OpenXR API layer
    /// (<c>NativeLayer/IpcServer.cpp</c>, server, hosted inside the target
    /// VR application's process, e.g. iRacing.exe).
    ///
    /// This mirrors the OpenKneeboard-style architecture: the layer owns the
    /// real OpenXR session/frame loop of the host application and simply
    /// receives panel transform + frame updates from this app to compose as
    /// additional XrCompositionLayerQuad entries in xrEndFrame.
    ///
    /// Wire format: [MessageType:byte][PayloadLength:int32][Payload bytes].
    /// Multi-byte fields are little-endian (native x64).
    /// </summary>
    public enum OverlayIpcMessageType : byte
    {
        /// <summary>Client -> server: create/update a panel's transform and visibility.</summary>
        SetPanelTransform = 1,

        /// <summary>Client -> server: a new frame's D3D11 shared texture handle is available for a panel.</summary>
        UpdateFrame = 2,

        /// <summary>Client -> server: remove a panel entirely.</summary>
        RemovePanel = 3,

        /// <summary>Client -> server: recenter all panels relative to the current head pose.</summary>
        Recenter = 4,

        /// <summary>
        /// Server -> client: a VR controller grab ended, reporting the
        /// panel's final position/rotation so the app can update its
        /// <c>OverlayPanel</c> model and persist it. See
        /// NativeLayer/ControllerInput.cpp.
        /// </summary>
        PanelTransformUpdated = 100,

        /// <summary>
        /// Server -> client: a VR controller thumbstick nudge was detected;
        /// the payload's value mirrors
        /// <see cref="Donutz_VR_HUD.Input.NudgeAction"/>'s ordering. See
        /// NativeLayer/ControllerInput.cpp.
        /// </summary>
        ControllerNudgeAction = 101,

        /// <summary>
        /// Server -> client: a VR controller just grabbed a panel (trigger
        /// pressed while pointing at it). Used by the app to auto-activate
        /// edit mode. See NativeLayer/ControllerInput.cpp.
        /// </summary>
        ControllerGrabStarted = 102,
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SetPanelTransformMessage
    {
        public Guid PanelId;
        [MarshalAs(UnmanagedType.I1)] public bool Enabled;
        public float PositionX, PositionY, PositionZ;
        public float RotationDegX, RotationDegY, RotationDegZ;
        public float WidthMeters;
        public float HeightMeters;

        /// <summary>
        /// If true, the panel follows the headset (VIEW space) instead of
        /// staying fixed relative to the driver's cockpit-anchored position
        /// (LOCAL space, the default).
        /// </summary>
        [MarshalAs(UnmanagedType.I1)] public bool HeadLocked;

        /// <summary>
        /// If true, the panel's quad is composited fully opaque (alpha
        /// channel ignored), so transparent source pixels show up as solid
        /// black instead of letting the scene behind the panel show through.
        /// </summary>
        [MarshalAs(UnmanagedType.I1)] public bool OpaqueBackground;
    }

    /// <summary>
    /// Maximum number of UTF-16 characters (including the terminating null)
    /// that fit in <see cref="UpdateFrameMessage.SharedHandleName"/>.
    /// </summary>
    public static class OverlayIpcContractConstants
    {
        public const int SharedHandleNameLength = 128;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Unicode)]
    public unsafe struct UpdateFrameMessage
    {
        public Guid PanelId;
        public int Width;
        public int Height;

        /// <summary>
        /// Name passed to IDXGIResource1::CreateSharedHandle (and read back
        /// via ID3D11Device1::OpenSharedResourceByName by the native layer).
        /// A raw NT handle value is only valid within the process that
        /// created it, so a *named* shared handle is required for this to
        /// work across process boundaries (this app -> host VR app, e.g.
        /// iRacing.exe). Null-terminated, fixed-size to keep the wire format
        /// simple.
        /// </summary>
        public fixed char SharedHandleName[OverlayIpcContractConstants.SharedHandleNameLength];
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct RemovePanelMessage
    {
        public Guid PanelId;
    }

    // Recenter has no payload.

    /// <summary>Server -> client. See <see cref="OverlayIpcMessageType.PanelTransformUpdated"/>.</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct PanelTransformUpdatedMessage
    {
        public Guid PanelId;
        public float PositionX, PositionY, PositionZ;
        public float RotationDegX, RotationDegY, RotationDegZ;
    }

    /// <summary>Server -> client. See <see cref="OverlayIpcMessageType.ControllerNudgeAction"/>.</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct ControllerNudgeActionMessage
    {
        public byte NudgeAction;
    }

    /// <summary>Server -> client. See <see cref="OverlayIpcMessageType.ControllerGrabStarted"/>.</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct ControllerGrabStartedMessage
    {
        public Guid PanelId;
    }
}
