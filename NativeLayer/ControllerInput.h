#pragma once

#include "IpcServer.h"

#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <openxr/openxr.h>

#include <optional>

// Adds a second, independent OpenXR action set for the two VR hand
// controllers (grip pose, trigger, thumbstick), so panels can be grabbed and
// moved directly with the controller, or fine-tuned via the thumbstick using
// the same NudgeAction pipeline as DirectInput wheels/gamepads (see
// Input/PanelNudgeController.cs and MainWindow.xaml.cs ApplyNudgeAction).
//
// This is deliberately a *separate* action set from whatever the host
// application (e.g. iRacing.exe) creates for its own driving inputs.
// xrAttachSessionActionSets may only be called once per session, so
// HookedFunctions.cpp hooks it to merge our action set into the app's call
// (see ControllerInput::OnAttachSessionActionSets). All other
// action-related calls (xrCreateActionSet, xrCreateAction,
// xrSuggestInteractionProfileBindings, xrCreateActionSpace, xrSyncActions,
// xrGetActionState*) are safe to call independently and are not hooked.
namespace ControllerInput
{
	enum class Hand
	{
		Left,
		Right,
	};

	// Grab state for a single hand: which panel (if any) is currently held,
	// and the offset (panel pose relative to controller pose) captured at
	// the moment the trigger was pressed, so the panel keeps its
	// grab-relative offset while being moved.
	struct GrabState
	{
		bool active = false;
		IpcServer::PanelIdBytes panelId{};
		XrVector3f offsetPosition{};
		XrQuaternionf offsetOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };
	};

	// Simple RGBA color, 0..1 per channel, used for the procedural
	// marker/laser quads (no CEF/texture involved).
	struct VisualColor
	{
		float r = 1.0f;
		float g = 1.0f;
		float b = 1.0f;
		float a = 1.0f;
	};

	// Per-hand visualization state for the controller marker (small
	// billboard at the grip position) and laser (thin, elongated quad
	// pointing forward from the grip), recomputed every Update() call so
	// HookedFunctions.cpp's Hook_xrEndFrame can render them as additional
	// XrCompositionLayerQuads, just like the HUD panels themselves.
	struct ControllerVisual
	{
		bool visible = false;
		XrPosef markerPose{};
		VisualColor markerColor{};
		float markerSizeMeters = 0.02f;

		XrPosef laserPose{};
		VisualColor laserColor{};
		float laserLengthMeters = 0.0f; // 0 = don't draw the laser quad.
		float laserWidthMeters = 0.004f;

		// Yellow picture-frame outline drawn around the panel currently
		// being grabbed (mirrors the desktop edit-mode highlight look via
		// HookedFunctions::FillFrameTexture), so it's obvious in the
		// headset which panel is being moved. Only visible while a grab is
		// active for this hand.
		bool grabHighlightVisible = false;
		XrPosef grabHighlightPose{};
		float grabHighlightWidthMeters = 0.0f;
		float grabHighlightHeightMeters = 0.0f;

		// Small dot drawn exactly where the aim ray intersects the
		// targeted panel, so the user can see precisely what the laser
		// points at (a thin flat laser quad alone is hard to judge).
		bool hitCursorVisible = false;
		XrPosef hitCursorPose{};
		VisualColor hitCursorColor{ 1.0f, 1.0f, 1.0f, 0.95f };
		float hitCursorSizeMeters = 0.012f;
	};

	// One-shot nudge event to forward to the managed app's existing
	// NudgeAction pipeline (mirrors Input/PanelNudgeController.cs's
	// NudgeAction enum; kept in sync manually since this is a small,
	// stable set - see Shared/OverlayIpcContract.cs for the wire values).
	enum class NudgeAction : uint8_t
	{
		MoveXNeg = 0,
		MoveXPos = 1,
		MoveYNeg = 2,
		MoveYPos = 3,
		MoveZNeg = 4,
		MoveZPos = 5,
		PitchNeg = 6,
		PitchPos = 7,
		YawNeg = 8,
		YawPos = 9,
		RollNeg = 10,
		RollPos = 11,
		NextPanel = 12,
		PrevPanel = 13,
		ToggleStepSize = 14,
		ScaleUp = 15,
		ScaleDown = 16,
	};

	// Creates the action set/actions and suggests interaction profile
	// bindings. Must be called once, after xrCreateInstance succeeds (mirrors
	// HookedFunctions::OnInstanceCreated). Safe to call even if the
	// controller feature ends up unused at runtime.
	void OnInstanceCreated(XrInstance instance, PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr);

	// Called from the hooked xrAttachSessionActionSets (see
	// HookedFunctions.cpp) to merge our own action set into the app's
	// attach call, since only one xrAttachSessionActionSets call is allowed
	// per session. Returns the actual XrResult to propagate to the caller.
	XrResult OnAttachSessionActionSets(
		PFN_xrAttachSessionActionSets next,
		XrSession session,
		const XrSessionActionSetsAttachInfo* attachInfo);

	// Creates the per-hand action spaces once the session/local space are
	// available. Mirrors the timing of Hook_xrCreateSession's own reference
	// space creation.
	void OnSessionCreated(XrSession session, XrSpace localSpace);

	void OnSessionDestroyed();

	// Called once per frame from Hook_xrEndFrame: syncs our action set,
	// updates grab state directly against IpcServer's panel table, and
	// raises nudge actions to be forwarded to the managed app. No-op if the
	// action set was never successfully attached (e.g. host app doesn't use
	// OpenXR input actions in a way we could hook into, or attach failed).
	void Update(XrSession session, XrSpace localSpace, XrTime predictedDisplayTime);

	// Returns the current marker/laser visualization state for both hands
	// (index 0 = left, index 1 = right), computed by the last Update() call.
	// Thread-safe; safe to call even if the controller feature never
	// successfully attached (returns visible=false in that case).
	void GetVisuals(ControllerVisual outVisuals[2]);
}
