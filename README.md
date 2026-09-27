# Donutz VR HUD

A configurable **VR HUD / overlay panel system for OpenXR-based sim racing**, built to inject 2D overlay panels (web dashboards, images, telemetry text, panel highlights, etc.) directly into the OpenXR session of games such as **iRacing** — similar in spirit to OpenKneeboard.

The project consists of two cooperating components:

- **WPF configuration app** (`Donutz VR HUD.csproj`, .NET 10 / `net10.0-windows`) — lets you create, position, and configure overlay panels, and acts as the frame source (WebView2 web dashboards, window capture, images, HUD text, etc.).
- **Native OpenXR API layer** (`NativeLayer/`, C++) — a native DLL that hooks into the target application's OpenXR session (`xrCreateSession`, `xrEndFrame`, controller input, etc.) and renders the panels as OpenXR quad layers, since API layers can only be implemented as native code.

The two components communicate via a local **named pipe IPC channel** (panel transforms + D3D11 shared texture handles), defined in [`Shared/OverlayIpcContract.cs`](Shared/OverlayIpcContract.cs).

> **Note:** Parts of this project were developed with the assistance of GitHub Copilot (AI-assisted coding).

## Features

- Multiple independently configurable overlay **panels**, each with its own position, rotation, size, and source type:
  - Web dashboards (via WebView2/CEF)
  - Captured application windows
  - Static images
  - HUD text
  - Panel highlight overlays
  - Test pattern (for calibration)
- Per-panel **head-locked** or **cockpit-fixed** placement, with optional opaque or alpha-blended background.
- **VR controller interaction**: grab-and-move panels with the grip button and fine-nudge them with the thumbstick, using standard OpenXR actions — no SteamVR-specific SDK required, so it works with any OpenXR runtime.
- **Profiles** for saving/restoring different panel layouts per game or session.
- **SimHub** integration for telemetry data.
- Automatic detection of the active game/process.
- English UI.
- Dark mode support (WPF-UI).

## Repository layout

| Path | Description |
|---|---|
| `Donutz VR HUD.csproj` | WPF configuration UI and overlay frame sources (.NET 10) |
| `NativeLayer/` | Native C++ OpenXR API layer project (see [`NativeLayer/README.md`](NativeLayer/README.md) for build details) |
| `Shared/OverlayIpcContract.cs` | Shared IPC message contract between the WPF app and the native layer |
| `Sources/` | Overlay frame source implementations (web, image, HUD text, window capture, etc.) |
| `Settings/` | Persisted app settings, profiles, and panel configuration |
| `OpenXR/` | Managed-side OpenXR layer registration and IPC client |
| `OpenXR-SDK/` | Vendored Khronos OpenXR SDK (headers used by the native layer) |

## Getting started

### Prerequisites

- Visual Studio 2026 with:
  - **.NET desktop development** workload (for the WPF app, targets .NET 10)
  - **Desktop development with C++** workload (for the native OpenXR layer)
- A VR headset/runtime supporting OpenXR (e.g. SteamVR).
- iRacing (or another OpenXR-based sim) for in-game testing.

### Build

1. Open `Donutz VR HUD.slnx` in Visual Studio.
2. Build the `Donutz VR HUD` (WPF) project — this is the configuration UI.
3. Build `NativeLayer\NativeLayer.vcxproj` for `x64` (Debug or Release, matching your WPF build configuration) — this produces the OpenXR API layer DLL and its manifest.
4. Register the native layer as an implicit OpenXR API layer using [`NativeLayer/register-dev-layer.ps1`](NativeLayer/register-dev-layer.ps1), which points the registry at the generated `XR_APILAYER_DONUTZ_vrhud.json` manifest.

> **Note:** the native layer's manifest path must match the build configuration (Debug vs. Release) currently registered in the registry, otherwise the game will load a stale DLL.

### Run

1. Launch the `Donutz VR HUD` WPF app to configure your overlay panels (source, position, size, etc.) and save a profile.
2. Start iRacing (or another OpenXR title) with the layer registered — the native layer will pick up panel updates from the WPF app over the IPC channel and render them as quad layers in the VR session.

## Third-party components & licenses

This project builds on several open-source components, each under its own license. If you distribute binaries built from this repository (e.g. installers, releases), make sure to include attribution/license texts for at least the following:

### Managed (NuGet) dependencies

| Package | License |
|---|---|
| [CefSharp.OffScreen.NETCore](https://github.com/cefsharp/CefSharp) (Chromium Embedded Framework) | BSD-3-Clause (CefSharp) / BSD (Chromium/CEF) |
| [Silk.NET.\*](https://github.com/dotnet/Silk.NET) (OpenXR, Direct3D11, DXGI, Core, OpenXR.Extensions.KHR) | MIT |
| [OpenXR.Loader](https://www.nuget.org/packages/OpenXR.Loader) | Apache-2.0 |
| [SharpDX.DirectInput](https://github.com/sharpdx/SharpDX) | MIT |
| [WPF-UI](https://github.com/lepoco/wpfui) | MIT |

### Vendored native components (`OpenXR-SDK/`)

The `OpenXR-SDK/` directory is a vendored copy of the [Khronos OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK), which itself bundles third-party sources. See `OpenXR-SDK/LICENSE` and `OpenXR-SDK/LICENSES/` for the full set (Apache-2.0, MIT, BSD-3-Clause, Khronos Free Use License, Unlicense, etc.), including:

- OpenXR loader/headers — Apache-2.0 / MIT (Khronos Group)
- [jsoncpp](https://github.com/open-source-parsers/jsoncpp) (`OpenXR-SDK/src/external/jsoncpp/`) — MIT (public-domain-equivalent)
- [jnipp](https://github.com/mitchdowd/jnipp) (`OpenXR-SDK/src/external/jnipp/`) — MIT
- [android-jni-wrappers](OpenXR-SDK/src/external/android-jni-wrappers/) — Apache-2.0
- [sanitizers-cmake](https://github.com/arsenm/sanitizers-cmake) (`OpenXR-SDK/src/external/sanitizers-cmake/`) — MIT

### Recommendation

Since several of these licenses (BSD-3-Clause, MIT, Apache-2.0) require reproducing the copyright/license notice when redistributing binaries, consider adding a `THIRD-PARTY-NOTICES.txt` (or similar) to release artifacts that consolidates the notices above, and/or generating one automatically via `dotnet-project-licenses` or a NuGet license-report tool as part of the release process.

## License

The Donutz VR HUD source code (WPF app and `NativeLayer/`) is licensed under **[CC BY-NC 4.0](LICENSE)** (Attribution – NonCommercial): you're free to use, modify, and share it with credit to the original author, but **commercial use is not permitted**. Vendored/dependency components retain their own licenses as listed above.
