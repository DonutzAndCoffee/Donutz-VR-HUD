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
- Optional per-panel **3D housing**: a bezel/case that adapts to the panel size (configurable bezel width, depth, color, roughness, metallic and corner size). It is lit only by the sim's own rendered image, so it automatically gets dark at night or in tunnels.
- **VR controller interaction** (experimental): grab-and-move panels with the grip button and fine-nudge them with the thumbstick, using standard OpenXR actions — no SteamVR-specific SDK required, so it works with any OpenXR runtime.
- **Profiles** for saving/restoring different panel layouts per game or session.
- **SimHub** integration for telemetry data and optional per-housing LED effects on all four edges.
- Automatic detection of the active game/process.
- English UI.
- Dark mode support (WPF-UI).

<img width="946" height="953" alt="image" src="https://github.com/user-attachments/assets/a9ae346d-7974-41b3-be7d-5a1b32d3ea4b" />

<img width="2560" height="1440" alt="Screenshot 2026-09-27 06-24-59" src="https://github.com/user-attachments/assets/6bccecb7-72d9-4066-8698-42a4f3346338" />



## SimHub RPM LEDs

Enable **3D housing**, then **SimHub RPM LEDs** on a panel. LEDs can be enabled on each housing edge (top defaults to 16 LEDs). Mesh rendering is used while enabled; disabling LEDs restores the selected housing render mode. Existing profiles keep LEDs disabled.

The HUD does not calculate RPM effects itself. The bundled bridge uses SimHub's LED editor for effects and profiles:

1. In the HUD, open **SimHub LED bridge setup and status** in a panel's LED settings, or open global settings. If SimHub is not found automatically, choose its installation folder.
2. Close SimHub completely and click **Install / update bridge**. If Windows denies access, accept the administrator restart and click the button again. Only the bridge DLL is copied; no SimHub assemblies are replaced.
3. Start SimHub, enable **Donutz VR HUD LEDs** in Plugins, and configure its LED effects. The setup shows both the installed DLL version and whether valid LED data is currently arriving; an installed DLL alone does not mean the plugin is enabled. Data reception can be checked without connecting the overlay or starting a game.
4. In the HUD, enable the housing and desired edges, then connect the overlay. Start with **Start LED = 1**, **LED count = 16**.

Each edge selects a contiguous range from a shared **256-LED color stream**. LED count (1-64), start channel (1-based), and order are saved per edge; width and brightness are shared by the housing. Settings are saved with the panel/profile. Ranges stop at channel 256. Multiple edges or panels can mirror a range or use separate ranges.

For developers and release packaging, first build `NativeLayer\NativeLayer.vcxproj` in Release x64 and `SimHubBridge\Donutz.SimHubLedBridge.csproj` in Release (the bridge requires the .NET Framework 4.8 targeting pack and an installed SimHub). For a non-default SimHub installation, pass `-p:SimHubDirectory="C:\Your\SimHub"`. Publish the HUD afterwards: it includes the native layer and the bridge in `SimHubBridge\Donutz.SimHubLedBridge.dll` next to the executable. Publishing fails if either component was not built; ordinary development builds without the bridge show a missing-package status instead.

The bridge sends full snapshots at approximately 30 Hz over **UDP 127.0.0.1:26741**; the HUD binds only to loopback. Missing/short effects clear unused channels. With no valid update for **2 seconds**, the native renderer switches emission off (also after SimHub or the HUD crashes), leaving the dark reflective lenses visible. Receiver/bind errors and malformed packets are reported in the HUD app log; sender errors appear in SimHub's log. Only one HUD instance can receive on this port.

Protocol v1: ASCII `DLED`, version byte `1`, reserved byte `0`, little-endian `uint16` color count (0-256), then exactly that many RGB byte triples. Bridge snapshots contain 256 colors; alpha is composited against black. Static strip configuration and dynamic colors use separate IPC messages, so animated effects do not rebuild housing meshes.

**Compatibility:** the bridge compiles against the installed SimHub LED engine (`RGBLedsDriver`), which is not part of SimHub's stable public plugin SDK. It was build-checked against the locally installed engine; future SimHub updates may require rebuilding/adapting the bridge. End-to-end testing in SimHub and an OpenXR headset is still required. Material presets/textures are not included.

## Repository layout

| Path | Description |
|---|---|
| `Donutz VR HUD.csproj` | WPF configuration UI and overlay frame sources (.NET 10) |
| `NativeLayer/` | Native C++ OpenXR API layer project (see [`NativeLayer/README.md`](NativeLayer/README.md) for build details) |
| `Shared/OverlayIpcContract.cs` | Shared IPC message contract between the WPF app and the native layer |
| `SimHubBridge/` | Optional .NET Framework 4.8 SimHub LED editor bridge (built separately) |
| `Sources/` | Overlay frame source implementations (web, image, HUD text, window capture, etc.) |
| `Settings/` | Persisted app settings, profiles, and panel configuration |
| `OpenXR/` | Managed-side OpenXR layer registration and IPC client |
| `OpenXR-SDK/` | Vendored Khronos OpenXR SDK (headers used by the native layer) |

## Installation (prebuilt release)

1. Install the [.NET 10 Desktop Runtime (x64)](https://dotnet.microsoft.com/download/dotnet/10.0) if not already present.
2. Download `DonutzVRHUD-v1.6.0-win-x64.zip` from the [Releases](https://github.com/DonutzAndCoffee/Donutz-VR-HUD/releases) page and extract it to a permanent folder (the OpenXR layer is registered from this location).
3. Start `Donutz VR HUD.exe` and use the layer registration button in the app to register the OpenXR API layer (requires administrator rights).
4. Start your OpenXR sim (e.g. iRacing) in VR.

When upgrading, close the HUD and your VR application, extract the complete new package, and register the layer from the new location. The v1.6 app and native layer use an updated LED IPC format and must be updated together. Follow the SimHub LED setup above to install or update the bridge.

## Changelog

### v1.6.0
- Optional SimHub-driven LEDs on the top, bottom, left and right housing edges, with separate channel ranges and order per edge and per panel.
- SimHub bridge bundled with the app, with installation, update and live-data status in the UI. SimHub LED effects and car-specific panel profiles remain independently configurable.
- OpenXR layer registration moved to the top of global settings; controller fine-tuning is collapsed by default.
- LED data goes dark after two seconds without updates. The bridge uses SimHub's internal LED engine; compatibility may change with SimHub updates.

### v1.5.0
- New optional per-panel 3D housing: bezel/case around the panel with configurable bezel width, depth, color, roughness, metallic and corner size (mm). The display sits 1 mm recessed behind the bezel; the panel content stays fully sharp.
- Housing lighting follows the sim's own rendered image (gets darker at night or in tunnels).
- Housing render mode per panel: "Quads" (lightweight) or "Mesh" (true 3D mesh rendered per eye into one projection layer, with optional depth submission). Mesh mode supports profiles (Flat, Rounded, Chamfered; custom profiles in `%LocalAppData%\Donutz VR HUD\housing-profiles.json`) and a render scale of 50–100 %.
- Housing layers are dropped automatically if the runtime's layer limit would be exceeded; panels themselves stay visible.
- Robustness: if the VR runtime rejects a frame because of too many layers, the HUD falls back to the sim's own layers instead of freezing the headset.

### v1.4.0
- Helmet visor can now be opened/closed with a short animation (wheel button or "Toggle visor"); when open, the raised lower glass edge stays visible at the top of the view.
- More realistic visor: polished lower glass edge with light reflection, padded helmet eyeport framing the view, slight fog at the lower edge.
- New visor settings: open angle (default 58°) and edge curve.
- Visor configuration reorganized into groups (Glass, Sun strip, Wear & dirt, Fit, Open visor, Strip lettering) with value read-outs and detailed tooltips for every option; panel position/size fields are hidden for helmet visors.

### v1.3.0
- Web dashboard URL field now offers a dropdown of locally installed SimHub dashboards (read from SimHub's DashTemplates folder), so URLs no longer need to be copied manually.

### v1.2.0
- Added native helmet visor rendering (procedural visor drawn by the OpenXR layer as a projection layer).
- Added visor controls, presets and helmet library persistence in the app.
- Cycle visors with the VR controller thumbstick in edit mode.
- Updated edit-mode labels and localization.

### v1.1.0
- Fixed overlay reconnect after restarting the app or the game, and panel interactivity after reconnect.
- Improved panel grab handling and hit cursor visuals for VR controller interaction.
- Added close-to-tray window behavior.
- Documented OpenComposite setup for OpenVR-only sims (Assetto Corsa, AMS2, ...).
- Localized overlay status messages.

### v1.0.0
- Initial release.

## Getting started (building from source)

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

### Using OpenVR-only sims (Assetto Corsa, AMS2, …) via OpenComposite

Some sims (e.g. the original **Assetto Corsa** or **Automobilista 2**) only support OpenVR/SteamVR. Because Donutz VR HUD is an OpenXR API layer, it isn't loaded by these games directly. Use [OpenComposite](https://gitlab.com/znixian/OpenOVR) to translate OpenVR to OpenXR, and the layer then works as usual:

1. Install OpenComposite, either system-wide with the **OpenComposite Switcher** ("Switch to OpenComposite") or per game by replacing `openvr_api.dll` in the game folder.
2. Make sure your headset's OpenXR runtime (Meta/Oculus, WMR, Pimax, Virtual Desktop, SteamVR, …) is set as the active OpenXR runtime.
3. Start the Donutz VR HUD app, then launch the game in VR mode.
4. If the panels don't show up, check `%ProgramData%\DonutzVrHud\DonutzVrHudLayer.log` to see whether the layer was loaded.

Telemetry and auto-profile game detection for these sims work through SimHub. For process-based auto-loading, set the profile's game process name, e.g.:

| Sim | Process name |
|---|---|
| iRacing | `iRacingSim64DX11` |
| Assetto Corsa | `acs` |
| Automobilista 2 | `AMS2AVX` (or `AMS2`) |

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
