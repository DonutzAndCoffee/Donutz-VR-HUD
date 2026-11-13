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
- `HousingRenderer.cpp/.h`: Drei auswählbare Methoden mit unveränderten
  bisherigen Einstellungswerten:
  - **Mesh (recommended), Wert 1, Standard:** bisheriges Panel-Space-Mesh
    mit augenspezifischen Off-Axis-Bildern des Gehäuses. Das Dashboard bleibt
    ein separater Quad in Originalauflösung, unabhängig vom Render-Scale.
    Profile und LEDs bleiben erhalten. Die bekannte Begrenzung auf eine
    flache Compositor-Bildfläche bleibt bestehen (keine echte Seitenansicht).
  - **Stereo 3D (experimental), Wert 2:** räumlicher Renderpfad, nur nach
    expliziter Auswahl. Gehäuse und Dashboard werden gemeinsam gerendert und
    auf einem kopfzugewandten Quad gezeigt; das Dashboard wird dabei einmal
    neu abgetastet und kann etwas weicher wirken als im Mesh-Modus.
  - **Quads (fallback), Wert 0:** einfaches Ersatzgehäuse; weiterhin explizit
    wählbar und als Rückfall bei nicht verfügbarem Mesh verwendbar.
  Neue Panels und Einstellungen ohne Modus verwenden Mesh. Gespeicherte
  Quads-/Mesh-Auswahlen bleiben erhalten; Wert 1 aktiviert nicht experimentell.
  LEDs schalten nur Quads auf Mesh um und überschreiben keine experimentelle
  Auswahl. Alle Modi lassen sich pro Panel mischen und dauerhaft speichern.

  Nur im experimentellen Modus werden Gehäuse und texturiertes
  Dashboard gemeinsam pro Auge mit gemeinsamem Tiefenpuffer gerendert.
  Dadurch bleiben Seiten und Rückseite auch bei 90°/180° Drehung sichtbar.
  Das Ergebnis wird nicht als Projektionslayer eingereicht, sondern pro
  Panel als zwei augenspezifische Quads (links/rechts) auf einer gemeinsamen
  quadratischen Fläche durch die Gehäusemitte, die dem Kopf zugewandt ist
  (`BuildHousingBillboard`). Grund: Quads setzt der Compositor bei jedem
  Refresh mit der aktuellen Kopfpose in allen sechs Freiheitsgraden neu;
  einen Projektionslayer führt er ohne nutzbare Tiefe nur rotatorisch nach,
  wodurch ein Gehäuse in Armlänge bei Kopfbewegung zitterte. Der
  verbleibende Parallaxenfehler betrifft nur noch den Abstand der Geometrie
  zur Quad-Fläche (wenige Zentimeter). Augenpositionen stammen aus
  `xrLocateViews` zur Anzeigezeit des Frames im Raum des Panels (LOCAL oder
  VIEW), nicht aus dem älteren Projektionslayer des Spiels; bei
  VIEW-Head-Locking sind sie damit konstant. Mehrere Stereo-Gehäuse teilen
  keinen Tiefenpuffer mehr und werden von fern nach nah zusammengesetzt.
  Ist der Kopf zu nah am Gehäuse, bleibt das Dashboard als Quad sichtbar.
  Panel-Texturen werden vor Freigabe ihres Swapchain-Bildes in eigene
  Shader-Ressourcen mit Mipmaps kopiert und anisotrop gefiltert, damit
  feine Details bei Verkleinerung oder schrägem Blick nicht flimmern. Dabei
  sind auch typeless BGRA/RGBA-Texturen der Runtime zulässig.
  Content-Bounds verschieben nur das Gehäuse,
  nicht die UVs des vollständigen Dashboards. Im experimentellen Modus bestimmt der Render-Scale
  die Auflösung von Gehäuse **und Dashboard** relativ zur Pixeldichte des
  Dashboards (maximal 4096 Pixel pro Achse); 100 % empfiehlt sich für lesbare Anzeigen.
  Die Bildgröße hängt nur vom Gehäuse ab, nicht von der Kopfposition.
  Wenn ein Gehäuse nicht gerendert werden kann, bleiben die betroffenen
  Dashboards als Quads sichtbar und der Fehler wird protokolliert.
  Vor `xrEndFrame` wird das über `xrGetSystemProperties` ermittelte
  `maxLayerCount` eingehalten: Spiel-Layer, Dashboard-Inhalte und Controller
  haben Vorrang vor Visor und dekorativen Quad-Gehäusen. Ein Stereo-Gehäuse
  belegt zwei Layer; reicht das Budget nicht, behält das entferntere Panel
  nur sein Dashboard als Quad. Ausgelassene
  Overlays werden protokolliert. Es gibt keinen zweiten `xrEndFrame`-Aufruf
  nach einem Fehler, da damit derselbe Frame zweimal beendet würde.
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
Layer-Budget, Texturbesitz vor/nach Release, Fehler-Fallback,
Swapchain-Wiederverwendung und Wiederherstellung des D3D-Zustands.
Zusätzlich werden der stabile Mesh-Pfad mit separatem Dashboard in
Originalauflösung (auch bei 50 % Gehäuse-Scale), Profilwechsel, gemischte
stabile/experimentelle Panels und Moduswechsel geprüft.
Der Test ersetzt keine Stabilitätsprüfung mit der tatsächlichen VR-Runtime.

Aus einer x64 Native Tools PowerShell im Verzeichnis `NativeLayer`
(`OPENXR_SDK_INCLUDE` zeigt auf das SDK-`include`-Verzeichnis):

```powershell
$testOut = Join-Path $env:TEMP 'DonutzHousingTests'
New-Item -ItemType Directory -Force $testOut | Out-Null
cl /nologo /EHsc /std:c++20 /DNOMINMAX "/I$env:OPENXR_SDK_INCLUDE" Tests\HousingStereoTests.cpp HousingRenderer.cpp "/Fo$testOut\\" "/Fe$testOut\HousingStereoTests.exe" /link d3d11.lib d3dcompiler.lib dxgi.lib
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
