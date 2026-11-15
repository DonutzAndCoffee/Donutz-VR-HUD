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
- `HousingRenderer.cpp/.h`: Zwei auswählbare Methoden; experimentelle Diagnose,
  Legacy-Quads, die kopfzugewandten Stereo-Quads und der Projektionsmodus bleiben im
  IPC/native code erhalten, werden aber nicht in der UI angeboten:
  - **Stereo 3D (recommended), Wert 4, Standard:** rendert Gehäuse und Dashboard
    pro Auge gemeinsam auf Compositor-Quads, die in der Display-Ebene liegen (erst
    jenseits von 60° Blickwinkel wird die Ebene zum Kopf gekippt). Inhalte in der
    Ebene, also das Dashboard, sind unabhängig von Fehlern der Augenposition
    geometrisch exakt; nur die Gehäusetiefe bleibt eine Näherung. Im Headset
    (iRacing, SteamVR) war das Dashboard damit deutlich weniger verzerrt als mit Wert 2;
    die Tiefendarstellung des Gehäuses ist noch nicht einwandfrei.
  - **Mesh, Wert 1:** bisheriges Panel-Space-Mesh als augenspezifische Bilder;
    das Dashboard bleibt separat und scharf. Es ist eine stabile Alternative,
    bildet das Gehäuse aber als flache Compositor-Ebene ab.
  - **Stereo 3D (experimental), Wert 5 (nicht auswählbar):** Arbeitsstand zur Gehäusetiefe. Derzeit wie
    Wert 4 plus Tiefenprüfung oberhalb des Gehäuses. Links: Für einen Punkt 100 mm vor
    der Display-Ebene wird in jedes Augenbild ein 30-mm-Quadrat gezeichnet (rot = links,
    grün = rechts), dort wo der Sehstrahl des Auges die Bildebene schneidet; ein weißes
    10-mm-Compositor-Quad liegt an der echten 3D-Position des Punkts. Sitzt Weiß für
    jedes Auge mittig im farbigen Quadrat, spricht das für eine Übereinstimmung der
    Projektion an dieser Testposition. Rechts: ein zweites weißes Quad 100 mm hinter
    der Ebene, ohne Farbmarke. Im Headset-Test (iRacing, SteamVR) war jedes Augen-Quad
    nur für das zugehörige Auge sichtbar; zuletzt waren beide weißen Referenzen sichtbar.
    Bei 107 % SteamVR-World-Scale war die Tiefendarstellung verzerrt; bei 100 % war sie
    laut Nutzer korrekt. Der genaue Skalierungsmechanismus wurde nicht nachgewiesen.
    OpenXR stellt keinen allgemein nutzbaren World-Scale-Wert zur Verfügung; eine
    runtime-unabhängige Korrektur ist nicht implementiert. Andere Runtimes, einschließlich
    Pimax Play, wurden hierzu nicht getestet.
  - **Kopfzugewandte Stereo-Quads, Wert 2 (nicht auswählbar):** wie Wert 4, aber die
    Ebene geht durch die Gehäusemitte und ist zum Kopf gedreht; das Dashboard wirkte
    damit leicht perspektivisch verzerrt.
  - **Projektion, Wert 3 (nicht auswählbar):** echte per-eye Projektion des 3D-Gehäuses
    als OpenXR-Projektionslayer; das Dashboard bleibt als separates Quad in
    Originalauflösung. Bei verfügbarer `XR_KHR_composition_layer_depth` wird pro
    Auge D32-Tiefe mitgereicht. Im Test mit iRacing unter SteamVR saß das Gehäuse
    sichtbar versetzt zum Dashboard und zitterte bei Kopfbewegung. Ursache ungeklärt:
    eingereichte und Runtime-Augenposen stimmen laut Log auf unter 1 mm überein, und
    der Wechsel der Posenquelle änderte nichts.
  - **Legacy Quads, Wert 0:** einfacher Ersatzpfad; nicht in der UI auswählbar.
  Neue Panels verwenden Wert 4. Gespeicherte Quads-Einstellungen werden zu Mesh,
  gespeicherte Werte 2, 3 und 5 zu Wert 4 migriert. Die auswählbaren Modi können pro
  Panel gemischt werden.

  Im Modus 3 wird die Housing-Geometrie gegen die aktuellen Augenposen
  und FOVs der Runtime (`xrLocateViews`, wie in den Quad-Modi) gerendert, nicht gegen die
  vom Spiel eingereichten Posen; vom Spiel stammt nur die Renderauflösung. Das Dashboard bleibt in
  einer eigenen OpenXR-Quad-Ebene. Falls die Projektion nicht erstellt werden kann,
  bleibt das Dashboard sichtbar und der Fehler wird protokolliert. Edit-Hilfslayer
  (Shortcut-HUD und Auswahlrahmen) erhalten bei knappem Layerbudget Vorrang vor den
  Housing-Projektionslayern.
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
    den Attach-Call der Host-App (z. B. iRacing) einzuschleusen. Der Layer
    fängt auch `xrSuggestInteractionProfileBindings` ab und ergänzt seine
    Bindings, da spätere Vorschläge der Host-App die bisherigen Bindings
    für dasselbe Interaction Profile sonst vollständig ersetzen.
  - Auch `xrSyncActions` wird abgefangen: Host- und Layer-ActionSets bleiben
    gemeinsam aktiv. Ein separater Sync nur des Layers würde die Host-Sets
    deaktivieren; ein Host-Sync ohne Layer würde die Controller deaktivieren.
  - Interaction-Profile-Bindings werden für Valve Index, HTC Vive,
    Oculus/Meta Touch, Windows-Mixed-Reality-Controller sowie
    `khr/simple_controller` als Fallback vorgeschlagen. Beim Simple-Profil
    werden nur Posen und Select gebunden, da kein Thumbstick vorhanden ist.
    Der Log zeigt Bearbeitungsmodus, aktive Posen, Tracking-Flags und Profile.
  - Grab-Erkennung erfolgt über einen Strahltest von der Aim-Pose gegen die
    Panel-Rechtecke; während gehalten wird die Panel-Transform direkt in
    `IpcServer`s Panel-Tabelle
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
    /`FillSolidColorTexture`). Nur im Bearbeitungsmodus sichtbar. Farbe zeigt
    den Zustand: Blau = getrackt, kein Panel getroffen; Gelb = ein Panel wird
    anvisiert; Grün = ein Panel wird gerade gehalten (der Strahl kürzt
    sich dabei automatisch bis zum gehaltenen Panel).

## Stereo-Gehäuse-Regressionstest

`Tests\HousingStereoTests.cpp` nutzt den echten D3D11-WARP-Renderer mit
Debug-Layer und einen simulierten OpenXR-Compositor. Geprüft werden
90°/180°-Ansichten, canted Augen, Kopfbewegung, LOCAL/VIEW,
zugeschnittener Inhalt, pixelgenaue Display-Zuordnung, unabhängige
Strahlvergleiche der Stereo-Quads aus den aktuellen (nicht den vom Spiel
eingereichten) Augenposen, Ausrichtung zum Kopf, Reihenfolge fern nach nah,
Layer-Budget einschließlich des Edit-Shortcut-HUDs bei knappem Layerbudget,
Texturbesitz vor/nach Release, Fehler-Fallback, Swapchain-Wiederverwendung und
Wiederherstellung des D3D-Zustands. Für den nicht auswählbaren Projektionsmodus prüft
der Test außerdem die optionale OpenXR-Tiefenebene inklusive D32-Ausgabe der
Gehäusegeometrie; ohne Extension wird weiterhin ohne Depth gerendert. Zusätzlich werden der stabile Mesh-Pfad mit separatem Dashboard in
Originalauflösung (auch bei 50 % Gehäuse-Scale), Profilwechsel, gemischte
Mesh-/Stereo-Panels und Moduswechsel geprüft.
Der Test ersetzt keine Stabilitätsprüfung mit der tatsächlichen VR-Runtime.

Aus einer x64 Native Tools PowerShell im Verzeichnis `NativeLayer`
(`OPENXR_SDK_INCLUDE` zeigt auf das SDK-`include`-Verzeichnis):

```powershell
$testOut = Join-Path $env:TEMP 'DonutzHousingTests'
New-Item -ItemType Directory -Force $testOut | Out-Null
cl /nologo /EHsc /std:c++20 /DNOMINMAX "/I$env:OPENXR_SDK_INCLUDE" Tests\HousingStereoTests.cpp HousingRenderer.cpp "/Fo$testOut\\" "/Fe$testOut\HousingStereoTests.exe" /link d3d11.lib d3dcompiler.lib dxgi.lib windowscodecs.lib ole32.lib uuid.lib
if ($LASTEXITCODE -eq 0) { & "$testOut\HousingStereoTests.exe" }
```

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
