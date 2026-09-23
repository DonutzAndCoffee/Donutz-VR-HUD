# Donutz VR HUD – OpenXR API Layer (native)

Dieses Verzeichnis enthält das **native C++ API-Layer-Projekt**, das die Overlays
analog zu OpenKneeboard direkt in die OpenXR-Session der Ziel-Anwendung (z. B.
iRacing) injiziert, statt eine eigene Session zu eröffnen.

## Warum nativ?

Der OpenXR-Loader ruft API Layers ausschließlich über eine feste **native
C-ABI-Exportfunktion** (`xrNegotiateLoaderApiLayerInterface`) auf. Diese
Schnittstelle kann nicht direkt von einer verwalteten .NET-Assembly bedient
werden – daher muss der eigentliche Layer als natives C++ DLL-Projekt gebaut
werden. Die bestehende WPF-App (`Donutz VR HUD.csproj`) bleibt als
Konfigurations-UI und Frame-Quelle (WebView2/Fenster-Capture) bestehen und
kommuniziert per IPC (Named Pipe + D3D11 Shared Texture Handles) mit dieser
DLL, sobald sie im Prozess des VR-Spiels geladen ist.

## Build-Voraussetzungen (aktuell NICHT erfüllt)

Dieses Projekt konnte in der aktuellen Entwicklungsumgebung **nicht gebaut
oder verifiziert werden**, weil keine C++-Toolchain installiert ist (kein
`cl.exe`, kein `VC\Tools\MSVC`, kein `vswhere.exe`). Um dieses Projekt zu
bauen:

1. In Visual Studio 2026 die Workload **„Desktopentwicklung mit C++“**
   installieren (Visual Studio Installer → Workloads).
2. Die OpenXR-SDK-Header (`openxr.h`, `openxr_platform.h`,
   `loader_interfaces.h`) werden benötigt. Empfohlen: NuGet-Paket
   `OpenXR.Headers` (falls vorhanden) oder Klonen von
   `https://github.com/KhronosGroup/OpenXR-SDK` und Referenzieren von
   `include/`. Aktuell referenziert dieses Projekt die Header über
   `$(OpenXrSdkInclude)`, das in `NativeLayer.vcxproj.user` (nicht
   eingecheckt) oder als Umgebungsvariable gesetzt werden muss.
3. `NativeLayer.vcxproj` zur Solution hinzufügen (`Donutz VR HUD.slnx`) und
   als x64/Release bauen.

## Aktueller Implementierungsstand

- `ApiLayerInterface.cpp/.h`: Implementiert `xrNegotiateLoaderApiLayerInterface`
  und die Dispatch-Table-Verkettung für `xrGetInstanceProcAddr`,
  `xrCreateApiLayerInstance`.
- `HookedFunctions.cpp/.h`: Enthält die gehookten Funktionen
  (`xrCreateSession`, `xrEndFrame`, `xrDestroySession`) mit TODO-Markierungen
  für die eigentliche Quad-Layer-Injection und D3D11-Texturübernahme.
- `IpcServer.cpp/.h`: Named-Pipe-Server-Grundgerüst, das Panel-Transform- und
  Frame-Update-Nachrichten von der WPF-App entgegennimmt (siehe
  `Shared/OverlayIpcContract.cs` für das gemeinsame Nachrichtenformat).
- `XR_APILAYER_DONUTZ_vrhud.json`: Loader-Manifest, das den Layer unter
  `HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit` registrierbar macht
  (siehe `register-dev-layer.ps1`).
- `ControllerInput.cpp/.h`: Eigenes OpenXR-ActionSet für die beiden
  VR-Handcontroller (Grip-Pose, Trigger, Thumbstick), damit Panels direkt
  mit der Hand gegriffen/verschoben ("Grab") oder per Thumbstick
  feinjustiert ("Nudge", über dieselbe `NudgeAction`-Pipeline wie
  DirectInput-Lenkräder/Gamepads) werden können. Läuft rein über
  Standard-OpenXR-Actions und ist damit automatisch mit SteamVR (und jeder
  anderen OpenXR-Runtime) kompatibel, ohne SteamVR-spezifisches SDK.
  - Da `xrAttachSessionActionSets` laut Spezifikation nur einmal pro
    Session aufgerufen werden darf, wird diese Funktion zusätzlich
    gehookt (`Hook_xrAttachSessionActionSets`), um das eigene ActionSet in
    den Attach-Call der Host-App (z. B. iRacing) einzuschleusen. Alle
    anderen Action-Funktionen (`xrCreateActionSet`, `xrCreateAction`,
    `xrSuggestInteractionProfileBindings`, `xrCreateActionSpace`,
    `xrSyncActions`, `xrGetActionState*`) werden unabhängig von der Host-App
    aufgerufen und müssen nicht gehookt werden.
  - Interaction-Profile-Bindings werden für Valve Index, HTC Vive,
    Oculus/Meta Touch, Windows-Mixed-Reality-Controller sowie
    `khr/simple_controller` als Fallback vorgeschlagen.
  - Grab-Erkennung erfolgt über einen einfachen Distanzcheck zwischen
    Controller-Pose und Panel-Weltpose (kein Raycasting); während gehalten
    wird die Panel-Transform direkt in `IpcServer`s Panel-Tabelle
    aktualisiert (`IpcServer::ApplyControllerTransform`), sodass
    `Hook_xrEndFrame` die Änderung sofort ohne IPC-Rundlauf übernimmt. Erst
    beim Loslassen wird die finale Transform per neuer Server→Client-
    IPC-Nachricht (`PanelTransformUpdated`) an die WPF-App gemeldet, damit
    sie das zugehörige `OverlayPanel` aktualisiert/persistiert.
  - Thumbstick-Ausschläge über einer Deadzone erzeugen eine
    `ControllerNudgeAction`-Nachricht (Server→Client), die die WPF-App
    genau wie eine DirectInput-Wheel-Taste über `ApplyNudgeAction`
    verarbeitet.
  - Die IPC-Pipe wurde dafür von `PIPE_ACCESS_INBOUND` auf
    `PIPE_ACCESS_DUPLEX` umgestellt (siehe `IpcServer.cpp`).
  - Zur besseren Sichtbarkeit rendert `Hook_xrEndFrame` pro Hand einen
    kleinen farbigen Marker an der Grip-Position sowie einen dünnen
    "Laserpointer"-Strahl nach vorne, jeweils als zusätzliches
    `XrCompositionLayerQuad` mit einer prozedural eingefärbten 4x4-Textur
    (`ControllerInput::GetVisuals`, `HookedFunctions::EnsureVisualSwapchain`
    /`FillSolidColorTexture`). Farbe zeigt den Zustand: Blau = getrackt,
    kein Panel in Reichweite; Gelb = ein Panel liegt innerhalb der
    Grab-Distanz; Grün = ein Panel wird gerade gehalten (der Strahl kürzt
    sich dabei automatisch bis zum gehaltenen Panel).

## Noch offen (nicht in diesem Schritt umgesetzt)

- Echtes Hooking der Dispatch-Tabelle für `xrCreateSwapchain` /
  `xrEnumerateSwapchainImages`, um App- und Overlay-Swapchains sauber
  zusammenzuführen.
- D3D11 Shared-Handle-Übernahme (`ID3D11Device::OpenSharedResource`) inkl.
  Adapter-/LUID-Abgleich zwischen WPF-Prozess und Spielprozess.
- Vollständige `xrEndFrame`-Layer-Zusammenführung (App-Layer + eigene
  `XrCompositionLayerQuad`s, korrekte `layerCount`/Array-Aufbau).
- Codesignatur der DLL (siehe Warnungen im „OpenXR API Layers“-Tool zu
  unsignierten Layern und Anti-Cheat-Kompatibilität).
