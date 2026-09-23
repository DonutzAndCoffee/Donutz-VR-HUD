#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal named-pipe IPC server, hosted inside the target application's
// process (e.g. iRacing.exe) once this layer is loaded. The WPF
// configuration app (Donutz VR HUD.exe) connects to this pipe as a client
// and streams panel transform/visibility updates and frame (D3D11 shared
// texture handle) updates.
//
// Wire format: see ../Shared/OverlayIpcContract.cs for the authoritative,
// shared message layout used by both the native layer and the C# client.
// [MessageType:byte][PayloadLength:int32][Payload bytes], little-endian.
namespace IpcServer
{
	// Must match Shared/OverlayIpcContract.cs's OverlayIpcContractConstants.SharedHandleNameLength.
	constexpr size_t kSharedHandleNameLength = 128;

	// Mirrors Shared/OverlayIpcContract.cs. Layout must stay in sync.
	// Types 1-4 are client->server; types 100+ are server->client (sent
	// over the same duplex pipe once VR controller input produces a panel
	// transform change or a nudge action - see ControllerInput.cpp).
	enum class MessageType : uint8_t
	{
		SetPanelTransform = 1,
		UpdateFrame = 2,
		RemovePanel = 3,
		Recenter = 4,

		// Server -> client.
		PanelTransformUpdated = 100,
		ControllerNudgeAction = 101,
		ControllerGrabStarted = 102,
	};

#pragma pack(push, 1)
	struct PanelIdBytes
	{
		uint8_t bytes[16];
	};

	struct SetPanelTransformPayload
	{
		PanelIdBytes panelId;
		uint8_t enabled;
		float positionX, positionY, positionZ;
		float rotationDegX, rotationDegY, rotationDegZ;
		float widthMeters;
		float heightMeters;
		// Mirrors Shared/OverlayIpcContract.cs's SetPanelTransformMessage.
		// HeadLocked: true = follow the headset (VIEW space) instead of
		// staying fixed relative to the driver's cockpit anchor (LOCAL
		// space, the default).
		uint8_t headLocked;
		// OpaqueBackground: true = composite the quad fully opaque (ignore
		// alpha), so transparent source pixels render as solid black
		// instead of blending with the scene behind the panel.
		uint8_t opaqueBackground;
	};

	struct UpdateFramePayload
	{
		PanelIdBytes panelId;
		int32_t width;
		int32_t height;
		// Null-terminated UTF-16 name passed to
		// IDXGIResource1::CreateSharedHandle by the sender; opened here via
		// ID3D11Device1::OpenSharedResourceByName. A raw handle value would
		// only be valid within the sender's (WPF app's) process, so a named
		// handle is required to cross process boundaries into the host
		// application (e.g. iRacing.exe).
		wchar_t sharedHandleName[kSharedHandleNameLength];
	};

	struct RemovePanelPayload
	{
		PanelIdBytes panelId;
	};

	// Server -> client: final transform after a VR controller grab ends
	// (see ControllerInput.cpp), so the managed app can update its
	// OverlayPanel model and persist the new position/rotation.
	struct PanelTransformUpdatedPayload
	{
		PanelIdBytes panelId;
		float positionX, positionY, positionZ;
		float rotationDegX, rotationDegY, rotationDegZ;
	};

	// Server -> client: a VR controller thumbstick nudge was detected;
	// value mirrors Input/PanelNudgeController.cs's NudgeAction enum
	// ordering (see Shared/OverlayIpcContract.cs).
	struct ControllerNudgeActionPayload
	{
		uint8_t nudgeAction;
	};

	// Server -> client: a VR controller just grabbed a panel (trigger
	// pressed while pointing at it). Used by the managed app to
	// auto-activate edit mode so the panel highlight/HUD is visible and
	// the app doesn't appear to "hang" while the user is moving a panel
	// in the headset without having manually toggled edit mode first.
	struct ControllerGrabStartedPayload
	{
		PanelIdBytes panelId;
	};
#pragma pack(pop)

	// In-memory representation of a single overlay panel, updated by the
	// pipe server as messages arrive and consumed by
	// HookedFunctions::Hook_xrEndFrame to build XrCompositionLayerQuad
	// entries.
	struct PanelState
	{
		PanelIdBytes panelId{};
		bool enabled = false;
		float positionX = 0, positionY = 0, positionZ = -1.0f;
		float rotationDegX = 0, rotationDegY = 0, rotationDegZ = 0;
		float widthMeters = 0.4f;
		float heightMeters = 0.3f;
		bool headLocked = false;
		bool opaqueBackground = false;
		std::wstring sharedHandleName;
		int32_t frameWidth = 0;
		int32_t frameHeight = 0;
		bool hasFrame = false;
	};

	// Starts the background pipe-server thread on first call; subsequent
	// calls are no-ops. Safe to call from any thread.
	void EnsureStarted();

	// Stops the server and releases the pipe handle. Called from the
	// layer's instance/session teardown path.
	void Stop();

	// Called from DllMain(DLL_PROCESS_DETACH). The process is already
	// tearing down at that point, so this must not block (joining the
	// server thread there risks deadlocking on the loader lock); it just
	// detaches the thread so its std::thread destructor doesn't call
	// std::terminate()/abort() for a still-joinable thread.
	void DetachForProcessExit();

	// Returns a snapshot copy of all currently known panels (thread-safe).
	// Used by HookedFunctions.cpp once per frame.
	std::vector<PanelState> GetPanelsSnapshot();

	// Returns true and clears the flag if a Recenter message was received
	// since the last call (thread-safe, consumed once).
	bool ConsumeRecenterRequested();

	// Called every frame from ControllerInput.cpp while a panel is being
	// held by a VR controller grab, to update its transform in-place so
	// Hook_xrEndFrame reflects the change immediately (thread-safe).
	void ApplyControllerTransform(
		const PanelIdBytes& panelId,
		float positionX, float positionY, float positionZ,
		float rotationDegX, float rotationDegY, float rotationDegZ);

	// Queues a PanelTransformUpdated message to be sent to the connected
	// client on the pipe's write side (e.g. once a VR controller grab
	// ends), so the managed app can persist the final transform. No-op if
	// no client is connected. Thread-safe.
	void QueuePanelTransformUpdated(const PanelState& panel);

	// Queues a ControllerNudgeAction message (value mirrors
	// Input/PanelNudgeController.cs's NudgeAction enum) to be sent to the
	// connected client, e.g. from a VR controller thumbstick nudge.
	// Thread-safe.
	void QueueControllerNudgeAction(uint8_t nudgeAction);

	// Queues a ControllerGrabStarted message (see
	// ControllerGrabStartedPayload) to be sent to the connected client,
	// e.g. once a VR controller grab attaches to a panel. Thread-safe.
	void QueueControllerGrabStarted(const PanelIdBytes& panelId);
}
