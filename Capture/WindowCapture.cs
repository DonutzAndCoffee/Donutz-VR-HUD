using System.Runtime.InteropServices;
using Donutz_VR_HUD.Sources;
using Silk.NET.Core.Native;
using Silk.NET.Direct3D11;
using Silk.NET.DXGI;
using WinRTIDirect3DDevice = Windows.Graphics.DirectX.Direct3D11.IDirect3DDevice;
using WinRTPixelFormat = Windows.Graphics.DirectX.DirectXPixelFormat;
using Windows.Graphics.Capture;

namespace Donutz_VR_HUD.Capture
{
    /// <summary>
    /// Captures the live contents of a chosen window (e.g. a SimHub dashboard)
    /// into a D3D11 texture using the Windows.Graphics.Capture API, so it can be
    /// fed into the OpenXR quad-layer overlay (see <see cref="OpenXR.OpenXrOverlay"/>).
    /// </summary>
    public sealed unsafe class WindowCapture : IOverlaySource
    {
        // Interop interface implemented by WinRT objects that wrap a native DXGI interface.
        [ComImport]
        [Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IDirect3DDxgiInterfaceAccess
        {
            IntPtr GetInterface([In] ref Guid iid);
        }

        // Factory interop interface used to create a GraphicsCaptureItem from an HWND.
        [ComImport]
        [Guid("3628E81B-3CAC-4C60-BB49-3699567AABB7")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IGraphicsCaptureItemInterop
        {
            IntPtr CreateForWindow([In] IntPtr window, [In] ref Guid iid);
            IntPtr CreateForMonitor([In] IntPtr monitor, [In] ref Guid iid);
        }

        [DllImport("d3d11.dll", EntryPoint = "CreateDirect3D11DeviceFromDXGIDevice", PreserveSig = false)]
        private static extern void CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);

        [DllImport("d3d11.dll")]
        private static extern int D3D11CreateDevice(
            IntPtr pAdapter,
            int driverType,
            IntPtr software,
            uint flags,
            IntPtr pFeatureLevels,
            uint featureLevels,
            uint sdkVersion,
            out IntPtr ppDevice,
            out int pFeatureLevel,
            out IntPtr ppImmediateContext);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr hObject);

        [DllImport("combase.dll", PreserveSig = false)]
        private static extern void RoGetActivationFactory(
            IntPtr activatableClassId,
            [In] ref Guid iid,
            out IntPtr factory);

        [DllImport("combase.dll", PreserveSig = false)]
        private static extern void WindowsCreateString(
            [MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string sourceString,
            int length,
            out IntPtr hstring);

        [DllImport("combase.dll")]
        private static extern int WindowsDeleteString(IntPtr hstring);

        // IUnknown::QueryInterface is vtable slot 0 on every COM interface.
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int QueryInterfaceDelegate(IntPtr self, [In] ref Guid iid, out IntPtr ppv);

        // IGraphicsCaptureItemInterop::CreateForWindow is vtable slot 3
        // (0=QueryInterface, 1=AddRef, 2=Release, 3=CreateForWindow).
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int CreateForWindowDelegate(IntPtr self, IntPtr window, [In] ref Guid iid, out IntPtr result);

        private const int D3D_DRIVER_TYPE_HARDWARE = 1;
        private const uint D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
        private const uint D3D11_SDK_VERSION = 7;

        private static readonly Guid IID_IDXGIDevice = new("54ec77fa-1377-44e6-8c32-88fd5f44c84c");
        private static readonly Guid IID_ID3D11Texture2D = new("6f15aaf2-d208-4e89-9ab4-489535d34f9c");
        private static readonly Guid IID_ID3D11Device1 = new("a04bfb29-08ef-43d6-a49c-a9bdbdcbe686");
        private static readonly Guid IID_IDXGIResource1 = new("30961379-4609-4a41-998e-54fe567ee0c1");

        private IntPtr _devicePtr;
        private IntPtr _contextPtr;
        private WinRTIDirect3DDevice? _winrtDevice;

        private GraphicsCaptureItem? _item;
        private Direct3D11CaptureFramePool? _framePool;
        private GraphicsCaptureSession? _session;

        // Stable staging texture with a cross-process shared handle name: the
        // WGC frame texture itself is only valid within this process, so each
        // captured frame is copied into this texture, which the native layer
        // opens by name (see IDXGIResource1::CreateSharedHandle /
        // ID3D11Device1::OpenSharedResourceByName).
        private IntPtr _stagingTexturePtr;
        private string _sharedHandleName = string.Empty;
        private int _stagingWidth;
        private int _stagingHeight;

        /// <summary>Native ID3D11Device pointer backing this capture pipeline.</summary>
        public IntPtr DevicePtr => _devicePtr;

        /// <summary>Native ID3D11DeviceContext pointer backing this capture pipeline.</summary>
        public IntPtr ContextPtr => _contextPtr;

        /// <summary>
        /// Raised whenever a new frame has been captured. The string is the
        /// name of the cross-process shared handle backing the frame.
        /// </summary>
        public event Action<string, int, int>? FrameArrived;

        public void Start(IntPtr hWnd)
        {
            CreateDeviceResources();

            // WinRT.ActivationFactory.Get(...) returns a CsWinRT-managed
            // wrapper object that cannot be cast to a custom [ComImport]
            // interface via a normal C# cast, and Marshal.GetObjectForIUnknown
            // followed by a cast doesn't reliably support QueryInterface for
            // user-defined interop interfaces on modern .NET either. Instead,
            // call QueryInterface and CreateForWindow directly through their
            // known vtable slots (see Microsoft's Win32CaptureSample, which
            // uses the same IGraphicsCaptureItemInterop COM interface).
            const string className = "Windows.Graphics.Capture.GraphicsCaptureItem";
            WindowsCreateString(className, className.Length, out var classNameHString);
            IntPtr factoryPtr;
            try
            {
                var iidIUnknown = new Guid("00000000-0000-0000-C000-000000000046");
                RoGetActivationFactory(classNameHString, ref iidIUnknown, out factoryPtr);
            }
            finally
            {
                WindowsDeleteString(classNameHString);
            }

            try
            {
                var interopGuid = typeof(IGraphicsCaptureItemInterop).GUID;
                var factoryVtable = Marshal.ReadIntPtr(factoryPtr);
                var queryInterface = Marshal.GetDelegateForFunctionPointer<QueryInterfaceDelegate>(
                    Marshal.ReadIntPtr(factoryVtable, 0));

                int hr = queryInterface(factoryPtr, ref interopGuid, out var interopPtr);
                if (hr < 0)
                {
                    Marshal.ThrowExceptionForHR(hr);
                }

                try
                {
                    var interopVtable = Marshal.ReadIntPtr(interopPtr);
                    var createForWindow = Marshal.GetDelegateForFunctionPointer<CreateForWindowDelegate>(
                        Marshal.ReadIntPtr(interopVtable, 3 * IntPtr.Size));

                    var itemGuid = typeof(GraphicsCaptureItem).GUID;
                    hr = createForWindow(interopPtr, hWnd, ref itemGuid, out var itemPtr);
                    if (hr < 0)
                    {
                        Marshal.ThrowExceptionForHR(hr);
                    }

                    _item = GraphicsCaptureItem.FromAbi(itemPtr);
                    Marshal.Release(itemPtr);
                }
                finally
                {
                    Marshal.Release(interopPtr);
                }
            }
            finally
            {
                Marshal.Release(factoryPtr);
            }

            _framePool = Direct3D11CaptureFramePool.Create(
                _winrtDevice!,
                WinRTPixelFormat.B8G8R8A8UIntNormalized,
                2,
                _item.Size);

            _framePool.FrameArrived += OnFrameArrived;

            _session = _framePool.CreateCaptureSession(_item);
            _session.IsCursorCaptureEnabled = false;
            _session.StartCapture();
        }

        private void OnFrameArrived(Direct3D11CaptureFramePool sender, object args)
        {
            using var frame = sender.TryGetNextFrame();
            if (frame is null)
            {
                return;
            }

            var access = (IDirect3DDxgiInterfaceAccess)(object)frame.Surface;
            var texGuid = IID_ID3D11Texture2D;
            var texPtr = access.GetInterface(ref texGuid);
            try
            {
                var width = frame.ContentSize.Width;
                var height = frame.ContentSize.Height;
                EnsureStagingTexture(width, height);

                using var context = new ComPtr<ID3D11DeviceContext>((ID3D11DeviceContext*)_contextPtr);
                context.CopyResource(
                    new ComPtr<ID3D11Resource>((ID3D11Resource*)_stagingTexturePtr),
                    new ComPtr<ID3D11Resource>((ID3D11Resource*)texPtr));

                FrameArrived?.Invoke(_sharedHandleName, width, height);
            }
            finally
            {
                Marshal.Release(texPtr);
            }
        }

        private void EnsureStagingTexture(int width, int height)
        {
            if (_stagingTexturePtr != IntPtr.Zero && _stagingWidth == width && _stagingHeight == height)
            {
                return;
            }

            DisposeStagingTexture();

            var desc = new Texture2DDesc
            {
                Width = (uint)width,
                Height = (uint)height,
                MipLevels = 1,
                ArraySize = 1,
                Format = Format.FormatB8G8R8A8Unorm,
                SampleDesc = new Silk.NET.DXGI.SampleDesc(1, 0),
                Usage = Usage.Default,
                BindFlags = (uint)BindFlag.ShaderResource,
                CPUAccessFlags = 0,
                // Cross-process NT shared handle, opened by name from the
                // native OpenXR layer's process (e.g. iRacing.exe).
                MiscFlags = (uint)(ResourceMiscFlag.Shared | ResourceMiscFlag.SharedNthandle)
            };

            var deviceGuid = typeof(ID3D11Device).GUID;
            Marshal.QueryInterface(_devicePtr, ref deviceGuid, out var rawDevicePtr);
            using var device = new ComPtr<ID3D11Device>((ID3D11Device*)rawDevicePtr);

            ComPtr<ID3D11Texture2D> texture = default;
            device.CreateTexture2D(in desc, null, ref texture).ThrowOnError();
            _stagingTexturePtr = (IntPtr)texture.Handle;
            _stagingWidth = width;
            _stagingHeight = height;

            using var resource = texture.QueryInterface<IDXGIResource1>();
            void* handle = null;
            // IDXGIResource1::CreateSharedHandle's dwAccess parameter expects
            // the DXGI_SHARED_RESOURCE_* flags (READ | WRITE), not a generic
            // Windows access right like GENERIC_ALL. Passing GENERIC_ALL here
            // caused the receiving process's OpenSharedResourceByName call to
            // fail with E_INVALIDARG.
            const uint DxgiSharedResourceRead = 0x80000000;
            const uint DxgiSharedResourceWrite = 0x00000001;
            const uint DxgiSharedResourceReadWrite = DxgiSharedResourceRead | DxgiSharedResourceWrite;
            SecurityAttributes* securityAttributes = null;
            _sharedHandleName = $"Local\\DonutzVrHud_{Guid.NewGuid():N}";
            fixed (char* name = _sharedHandleName)
            {
                resource.CreateSharedHandle(
                    securityAttributes,
                    DxgiSharedResourceReadWrite,
                    name,
                    &handle).ThrowOnError();
            }
        }

        private void DisposeStagingTexture()
        {
            if (_stagingTexturePtr != IntPtr.Zero)
            {
                Marshal.Release(_stagingTexturePtr);
                _stagingTexturePtr = IntPtr.Zero;
            }
        }

        private void CreateDeviceResources()
        {
            var hr = D3D11CreateDevice(
                IntPtr.Zero,
                D3D_DRIVER_TYPE_HARDWARE,
                IntPtr.Zero,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                IntPtr.Zero,
                0,
                D3D11_SDK_VERSION,
                out _devicePtr,
                out _,
                out _contextPtr);

            if (hr < 0)
            {
                Marshal.ThrowExceptionForHR(hr);
            }

            var dxgiDeviceGuid = IID_IDXGIDevice;
            Marshal.QueryInterface(_devicePtr, ref dxgiDeviceGuid, out var dxgiDevicePtr);
            try
            {
                CreateDirect3D11DeviceFromDXGIDevice(dxgiDevicePtr, out var winrtDevicePtr);
                _winrtDevice = WinRT.MarshalInterface<WinRTIDirect3DDevice>.FromAbi(winrtDevicePtr);
                Marshal.Release(winrtDevicePtr);
            }
            finally
            {
                Marshal.Release(dxgiDevicePtr);
            }
        }

        public void Dispose()
        {
            _session?.Dispose();
            if (_framePool is not null)
            {
                _framePool.FrameArrived -= OnFrameArrived;
                _framePool.Dispose();
            }

            DisposeStagingTexture();

            if (_contextPtr != IntPtr.Zero)
            {
                Marshal.Release(_contextPtr);
            }

            if (_devicePtr != IntPtr.Zero)
            {
                Marshal.Release(_devicePtr);
            }
        }
    }
}
