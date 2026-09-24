#include "ControllerInput.h"
#include "HookedFunctions.h"
#include "Logging.h"

#include <cmath>
#include <cstring>
#include <mutex>

namespace ControllerInput
{
	namespace
	{
		constexpr float kThumbstickDeadzone = 0.5f;
		// Panels are re-armed for a new nudge pulse only once the stick
		// returns below this (lower than the trigger deadzone) magnitude,
		// so holding the stick over doesn't repeat every single frame.
		constexpr float kThumbstickReleaseThreshold = 0.3f;

		std::mutex g_mutex;

		XrInstance g_instance = XR_NULL_HANDLE;
		PFN_xrGetInstanceProcAddr g_getInstanceProcAddr = nullptr;

		PFN_xrCreateActionSet xrCreateActionSet_ = nullptr;
		PFN_xrDestroyActionSet xrDestroyActionSet_ = nullptr;
		PFN_xrCreateAction xrCreateAction_ = nullptr;
		PFN_xrStringToPath xrStringToPath_ = nullptr;
		PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings_ = nullptr;
		PFN_xrCreateActionSpace xrCreateActionSpace_ = nullptr;
		PFN_xrDestroySpace xrDestroySpace_ = nullptr;
		PFN_xrSyncActions xrSyncActions_ = nullptr;
		PFN_xrGetActionStatePose xrGetActionStatePose_ = nullptr;
		PFN_xrGetActionStateBoolean xrGetActionStateBoolean_ = nullptr;
		PFN_xrGetActionStateVector2f xrGetActionStateVector2f_ = nullptr;
		PFN_xrLocateSpace xrLocateSpace_ = nullptr;

		XrActionSet g_actionSet = XR_NULL_HANDLE;
		XrAction g_poseAction = XR_NULL_HANDLE;       // Hand-indexed subaction, grip pose (used for grabbing).
		XrAction g_aimPoseAction = XR_NULL_HANDLE;    // Hand-indexed subaction, aim pose (used for the laser/pointer direction).
		XrAction g_triggerAction = XR_NULL_HANDLE;    // Hand-indexed subaction, boolean (grab).
		XrAction g_thumbstickAction = XR_NULL_HANDLE; // Hand-indexed subaction, vector2f (nudge).
		XrPath g_leftHandPath = XR_NULL_PATH;
		XrPath g_rightHandPath = XR_NULL_PATH;

		bool g_actionsCreated = false;
		bool g_attached = false;

		XrSession g_session = XR_NULL_HANDLE;
		XrSpace g_localSpace = XR_NULL_HANDLE;
		XrSpace g_leftHandSpace = XR_NULL_HANDLE;
		XrSpace g_rightHandSpace = XR_NULL_HANDLE;
		XrSpace g_leftAimSpace = XR_NULL_HANDLE;
		XrSpace g_rightAimSpace = XR_NULL_HANDLE;

		GrabState g_grabState[2]; // indexed by Hand
		// True while the thumbstick on a given hand is past kThumbstickDeadzone
		// and hasn't yet produced a nudge pulse for the current push.
		bool g_thumbstickArmed[2] = { true, true };

		std::vector<uint8_t> g_pendingNudgeActions; // NudgeAction values raised this frame.

		ControllerVisual g_visuals[2]; // indexed by Hand, guarded by g_mutex.

		constexpr VisualColor kColorIdle{ 0.2f, 0.6f, 1.0f, 0.55f };    // blue: tracked, nothing nearby.
		constexpr VisualColor kColorHover{ 1.0f, 0.85f, 0.1f, 0.75f };  // yellow: panel within grab range.
		constexpr VisualColor kColorGrabbed{ 0.15f, 1.0f, 0.25f, 0.9f }; // green: panel currently grabbed.
		constexpr VisualColor kColorGrabHighlight{ 1.0f, 0.85f, 0.1f, 0.9f }; // yellow frame around the grabbed panel.
		constexpr float kLaserLengthMeters = 1.5f;

		XrPath MakePath(const char* pathString)
		{
			if (xrStringToPath_ == nullptr || g_instance == XR_NULL_HANDLE)
			{
				return XR_NULL_PATH;
			}
			XrPath path = XR_NULL_PATH;
			xrStringToPath_(g_instance, pathString, &path);
			return path;
		}

		void SuggestBindingsForProfile(const char* profilePath, const char* poseSubPath, const char* triggerSubPath, const char* thumbstickSubPath)
		{
			if (xrSuggestInteractionProfileBindings_ == nullptr)
			{
				return;
			}

			XrPath profile = MakePath(profilePath);
			if (profile == XR_NULL_PATH)
			{
				return;
			}

			std::vector<XrActionSuggestedBinding> bindings;

			auto addBinding = [&](XrAction action, XrPath handPath, const char* subPath)
			{
				std::string full = handPath == g_leftHandPath
					? std::string("/user/hand/left") + subPath
					: std::string("/user/hand/right") + subPath;
				XrPath bindingPath = MakePath(full.c_str());
				if (bindingPath != XR_NULL_PATH)
				{
					bindings.push_back(XrActionSuggestedBinding{ action, bindingPath });
				}
			};

			addBinding(g_poseAction, g_leftHandPath, poseSubPath);
			addBinding(g_poseAction, g_rightHandPath, poseSubPath);
			addBinding(g_aimPoseAction, g_leftHandPath, "/input/aim/pose");
			addBinding(g_aimPoseAction, g_rightHandPath, "/input/aim/pose");
			addBinding(g_triggerAction, g_leftHandPath, triggerSubPath);
			addBinding(g_triggerAction, g_rightHandPath, triggerSubPath);
			addBinding(g_thumbstickAction, g_leftHandPath, thumbstickSubPath);
			addBinding(g_thumbstickAction, g_rightHandPath, thumbstickSubPath);

			XrInteractionProfileSuggestedBinding suggestedBinding{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
			suggestedBinding.interactionProfile = profile;
			suggestedBinding.suggestedBindings = bindings.data();
			suggestedBinding.countSuggestedBindings = static_cast<uint32_t>(bindings.size());

			const XrResult result = xrSuggestInteractionProfileBindings_(g_instance, &suggestedBinding);
			if (XR_FAILED(result))
			{
				Logging::Log("ControllerInput: xrSuggestInteractionProfileBindings failed for '" + std::string(profilePath) + "', XrResult=" + std::to_string(result));
			}
		}

		XrQuaternionf QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b)
		{
			return XrQuaternionf{
				a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
				a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
				a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
				a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
			};
		}

		XrQuaternionf QuatConjugate(const XrQuaternionf& q)
		{
			return XrQuaternionf{ -q.x, -q.y, -q.z, q.w };
		}

		XrVector3f QuatRotateVector(const XrQuaternionf& q, const XrVector3f& v)
		{
			const XrVector3f qv{ q.x, q.y, q.z };
			const float qw = q.w;
			const XrVector3f t{
				2.0f * (qv.y * v.z - qv.z * v.y),
				2.0f * (qv.z * v.x - qv.x * v.z),
				2.0f * (qv.x * v.y - qv.y * v.x)
			};
			return XrVector3f{
				v.x + qw * t.x + (qv.y * t.z - qv.z * t.y),
				v.y + qw * t.y + (qv.z * t.x - qv.x * t.z),
				v.z + qw * t.z + (qv.x * t.y - qv.y * t.x)
			};
		}

		XrVector3f VectorAdd(const XrVector3f& a, const XrVector3f& b)
		{
			return XrVector3f{ a.x + b.x, a.y + b.y, a.z + b.z };
		}

		XrVector3f VectorSub(const XrVector3f& a, const XrVector3f& b)
		{
			return XrVector3f{ a.x - b.x, a.y - b.y, a.z - b.z };
		}

		float VectorLength(const XrVector3f& v)
		{
			return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		}

		// Builds a pose pointing forward (-Z, OpenXR's grip "forward") from
		// origin, offset by `forwardMeters` and translated so the quad's
		// *center* sits at half that distance (XrCompositionLayerQuad is
		// centered on its pose), with the quad's local Y axis aligned along
		// the ray so a tall, thin quad reads as a beam.
		XrPosef ForwardBeamPose(const XrPosef& gripPose, float forwardMeters)
		{
			// Rotate the quad so its local +Y (the direction a quad's height
			// extends in) points down -Z of the grip, i.e. rotate -90
			// degrees around local X.
			constexpr float kHalfAngle = -0.7853981634f; // -45 deg in radians (half-angle for the quaternion).
			const XrQuaternionf tiltDown{ sinf(kHalfAngle), 0.0f, 0.0f, cosf(kHalfAngle) };

			XrPosef beam{};
			beam.orientation = QuatMultiply(gripPose.orientation, tiltDown);
			const XrVector3f forwardOffset = QuatRotateVector(gripPose.orientation, XrVector3f{ 0.0f, 0.0f, -forwardMeters * 0.5f });
			beam.position = VectorAdd(gripPose.position, forwardOffset);
			return beam;
		}

		// Maximum range for the laser/aim-based hit test below. Kept in
		// sync with the visually rendered laser length (kLaserLengthMeters)
		// so a panel can only be highlighted/grabbed while the visible
		// laser beam can actually reach it - a longer range here than what
		// is drawn made panels light up well before the beam visually
		// touched them.
		constexpr float kMaxAimDistanceMeters = 1.5f;

		// True laser/ray hit-test against a panel's rectangular surface.
		// XrCompositionLayerQuad convention: the quad lies in the local
		// XY-plane and faces -Z, with size.width along local X and
		// size.height along local Y. This replaces the previous
		// "is the controller merely close to the panel's center" distance
		// check, so a panel is only grabbed/hovered when the aim ray
		// actually intersects its rectangle, matching what the rendered
		// laser visually points at.
		bool RayHitsPanel(const XrVector3f& rayOrigin, const XrVector3f& rayDir, const XrPosef& panelWorld, float widthMeters, float heightMeters, float* outDistance)
		{
			const XrVector3f normal = QuatRotateVector(panelWorld.orientation, XrVector3f{ 0.0f, 0.0f, 1.0f });
			const float denom = normal.x * rayDir.x + normal.y * rayDir.y + normal.z * rayDir.z;
			// Require the ray to approach the panel's front face (denom
			// clearly negative, i.e. ray direction opposes the panel
			// normal) rather than merely "not parallel" - otherwise a ray
			// passing through the panel's back (e.g. the user pointing
			// past/behind it from the wrong side) would still count as a
			// hit, which read as the highlight "catching" the panel too
			// easily/early.
			constexpr float kMinFacingDot = -0.05f;
			if (denom > kMinFacingDot)
			{
				return false; // Parallel to, or approaching from behind, the panel's front face.
			}

			const XrVector3f toPlane = VectorSub(panelWorld.position, rayOrigin);
			const float t = (toPlane.x * normal.x + toPlane.y * normal.y + toPlane.z * normal.z) / denom;
			if (t <= 0.0f || t > kMaxAimDistanceMeters)
			{
				return false; // Panel is behind the controller, or too far away.
			}

			const XrVector3f hitPoint = VectorAdd(rayOrigin, XrVector3f{ rayDir.x * t, rayDir.y * t, rayDir.z * t });
			const XrQuaternionf inverseOrientation = QuatConjugate(panelWorld.orientation);
			const XrVector3f localHit = QuatRotateVector(inverseOrientation, VectorSub(hitPoint, panelWorld.position));
			if (fabsf(localHit.x) > widthMeters * 0.5f || fabsf(localHit.y) > heightMeters * 0.5f)
			{
				return false; // Hit point falls outside the panel's rectangle.
			}

			*outDistance = t;
			return true;
		}

		// Converts a panel's authored (position, rotationDeg) into a
		// world-space (local-space) pose, composed with the current cockpit
		// anchor, mirroring the math in HookedFunctions.cpp's
		// Hook_xrEndFrame quad construction (kept in sync manually - see
		// that function's comments for the composition order).
		XrPosef PanelWorldPose(const IpcServer::PanelState& panel)
		{
			XrPosef offsetPose{};
			offsetPose.position = { panel.positionX, panel.positionY, panel.positionZ };

			const float radX = panel.rotationDegX * 0.0174532924f * 0.5f;
			const float radY = panel.rotationDegY * 0.0174532924f * 0.5f;
			const float radZ = panel.rotationDegZ * 0.0174532924f * 0.5f;
			const float cx = cosf(radX), sx = sinf(radX);
			const float cy = cosf(radY), sy = sinf(radY);
			const float cz = cosf(radZ), sz = sinf(radZ);
			offsetPose.orientation.w = cx * cy * cz + sx * sy * sz;
			offsetPose.orientation.x = sx * cy * cz - cx * sy * sz;
			offsetPose.orientation.y = cx * sy * cz + sx * cy * sz;
			offsetPose.orientation.z = cx * cy * sz - sx * sy * cz;

			if (panel.headLocked)
			{
				// Head-locked panels aren't grabbable in world space via
				// this simple distance check (their true world position
				// depends on the live head pose, which we intentionally
				// don't re-derive here); treat as far away so they're
				// skipped by grab detection.
				return XrPosef{ { 0, 0, 0, 1 }, { 1e6f, 1e6f, 1e6f } };
			}

			bool hasAnchor = false;
			XrPosef anchor = HookedFunctions::GetAnchorPose(&hasAnchor);
			if (!hasAnchor)
			{
				return offsetPose;
			}

			XrPosef world{};
			world.orientation = QuatMultiply(anchor.orientation, offsetPose.orientation);
			world.position = VectorAdd(anchor.position, QuatRotateVector(anchor.orientation, offsetPose.position));
			return world;
		}

		// Inverse of PanelWorldPose: given a desired world-space pose,
		// recovers the (position, rotationDeg) authored values relative to
		// the current cockpit anchor.
		void WorldPoseToPanelFields(const XrPosef& world, float* posX, float* posY, float* posZ, float* rotDegX, float* rotDegY, float* rotDegZ)
		{
			bool hasAnchor = false;
			XrPosef anchor = HookedFunctions::GetAnchorPose(&hasAnchor);

			XrVector3f localPos = world.position;
			XrQuaternionf localOrientation = world.orientation;
			if (hasAnchor)
			{
				const XrQuaternionf anchorInverse = QuatConjugate(anchor.orientation);
				localPos = QuatRotateVector(anchorInverse, VectorSub(world.position, anchor.position));
				localOrientation = QuatMultiply(anchorInverse, world.orientation);
			}

			*posX = localPos.x;
			*posY = localPos.y;
			*posZ = localPos.z;

			// Quaternion -> Euler (matches the ZYX composition order used in
			// PanelWorldPose/Hook_xrEndFrame).
			const float w = localOrientation.w, x = localOrientation.x, y = localOrientation.y, z = localOrientation.z;
			const float sinPitch = 2.0f * (w * y - z * x);
			const float pitch = fabsf(sinPitch) >= 1.0f ? copysignf(3.14159265f / 2.0f, sinPitch) : asinf(sinPitch);
			const float yaw = atan2f(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
			const float roll = atan2f(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y));

			constexpr float kRadToDeg = 57.29577951f;
			*rotDegX = roll * kRadToDeg;
			*rotDegY = pitch * kRadToDeg;
			*rotDegZ = yaw * kRadToDeg;
		}

		void UpdateHand(Hand hand, XrSession session, XrSpace localSpace, XrTime time)
		{
			const int handIndex = hand == Hand::Left ? 0 : 1;
			XrSpace handSpace = hand == Hand::Left ? g_leftHandSpace : g_rightHandSpace;
			XrPath handPath = hand == Hand::Left ? g_leftHandPath : g_rightHandPath;
			if (handSpace == XR_NULL_HANDLE)
			{
				return;
			}

			XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
			getInfo.subactionPath = handPath;

			XrActionStateBoolean triggerState{ XR_TYPE_ACTION_STATE_BOOLEAN };
			getInfo.action = g_triggerAction;
			if (xrGetActionStateBoolean_ != nullptr)
			{
				xrGetActionStateBoolean_(session, &getInfo, &triggerState);
			}

			XrActionStateVector2f thumbstickState{ XR_TYPE_ACTION_STATE_VECTOR2F };
			getInfo.action = g_thumbstickAction;
			if (xrGetActionStateVector2f_ != nullptr)
			{
				xrGetActionStateVector2f_(session, &getInfo, &thumbstickState);
			}

			XrSpaceLocation handLocation{ XR_TYPE_SPACE_LOCATION };
			bool handPoseValid = false;
			if (xrLocateSpace_ != nullptr)
			{
				if (XR_SUCCEEDED(xrLocateSpace_(handSpace, localSpace, time, &handLocation)))
				{
					constexpr XrSpaceLocationFlags kRequiredFlags =
						XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
					handPoseValid = (handLocation.locationFlags & kRequiredFlags) == kRequiredFlags;
				}
			}

			// Aim pose: points in the direction the user naturally aims the
			// controller (unlike the grip pose, which is oriented for
			// holding, not pointing). Used for the laser/hover distance
			// check below; grabbing still uses the grip pose since that
			// feels more natural once a panel is actually held.
			XrSpace aimSpace = hand == Hand::Left ? g_leftAimSpace : g_rightAimSpace;
			XrSpaceLocation aimLocation{ XR_TYPE_SPACE_LOCATION };
			bool aimPoseValid = false;
			if (xrLocateSpace_ != nullptr && aimSpace != XR_NULL_HANDLE)
			{
				if (XR_SUCCEEDED(xrLocateSpace_(aimSpace, localSpace, time, &aimLocation)))
				{
					constexpr XrSpaceLocationFlags kRequiredFlags =
						XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
					aimPoseValid = (aimLocation.locationFlags & kRequiredFlags) == kRequiredFlags;
				}
			}
			if (!aimPoseValid)
			{
				// Fall back to the grip pose if the aim pose isn't available
				// (e.g. khr/simple_controller doesn't expose /input/aim/pose).
				aimLocation.pose = handLocation.pose;
				aimPoseValid = handPoseValid;
			}

			// Forward direction of the aim pose (-Z in OpenXR convention),
			// used as the laser/ray direction for the hit tests below.
			const XrVector3f aimForward = QuatRotateVector(aimLocation.pose.orientation, XrVector3f{ 0.0f, 0.0f, -1.0f });

			GrabState& grab = g_grabState[handIndex];

			// --- Grab (trigger) handling ---
			if (triggerState.isActive && triggerState.currentState && handPoseValid)
			{
				if (!grab.active)
				{
					// Just pressed: find the panel actually hit by the aim
					// ray (i.e. the laser is really pointed at it), picking
					// the closest one along the ray if several overlap.
					const auto panels = IpcServer::GetPanelsSnapshot();
					float bestDistance = kMaxAimDistanceMeters;
					bool found = false;
					IpcServer::PanelState bestPanel{};

					for (const auto& panel : panels)
					{
						if (!panel.enabled || panel.headLocked)
						{
							continue;
						}
						XrPosef panelWorld = PanelWorldPose(panel);
						float hitDistance = 0.0f;
						if (RayHitsPanel(aimLocation.pose.position, aimForward, panelWorld, panel.widthMeters, panel.heightMeters, &hitDistance)
							&& hitDistance < bestDistance)
						{
							bestDistance = hitDistance;
							bestPanel = panel;
							found = true;
						}
					}

					if (found)
					{
						XrPosef panelWorld = PanelWorldPose(bestPanel);
						const XrQuaternionf handInverse = QuatConjugate(handLocation.pose.orientation);
						grab.active = true;
						grab.panelId = bestPanel.panelId;
						grab.offsetPosition = QuatRotateVector(handInverse, VectorSub(panelWorld.position, handLocation.pose.position));
						grab.offsetOrientation = QuatMultiply(handInverse, panelWorld.orientation);

						// Tell the managed app a grab just started so it can
						// auto-activate edit mode; otherwise the app appears
						// to "hang" in VR while the user is moving a panel
						// without having manually toggled edit mode first.
						IpcServer::QueueControllerGrabStarted(bestPanel.panelId);
					}
				}
				else
				{
					// Held: recompute the panel's world pose from the
					// current hand pose + captured offset, and push it
					// directly into IpcServer's panel table so it's
					// reflected immediately in Hook_xrEndFrame.
					XrPosef newWorld{};
					newWorld.position = VectorAdd(handLocation.pose.position, QuatRotateVector(handLocation.pose.orientation, grab.offsetPosition));
					newWorld.orientation = QuatMultiply(handLocation.pose.orientation, grab.offsetOrientation);

					float posX, posY, posZ, rotX, rotY, rotZ;
					WorldPoseToPanelFields(newWorld, &posX, &posY, &posZ, &rotX, &rotY, &rotZ);
					IpcServer::ApplyControllerTransform(grab.panelId, posX, posY, posZ, rotX, rotY, rotZ);
				}
			}
			else if (grab.active)
			{
				// Released: report the final transform back to the managed
				// app so it can update its OverlayPanel model / persist it.
				const auto panels = IpcServer::GetPanelsSnapshot();
				for (const auto& panel : panels)
				{
					if (std::memcmp(panel.panelId.bytes, grab.panelId.bytes, sizeof(panel.panelId.bytes)) == 0)
					{
						IpcServer::QueuePanelTransformUpdated(panel);
						break;
					}
				}
				grab.active = false;
			}

			// --- Thumbstick zoom/scale handling ---
			// Panel movement is now done by grabbing with the trigger, so
			// the thumbstick is repurposed entirely for zoom: pushing it
			// up/down scales the (grabbed, or otherwise active) panel up
			// or down instead of nudging its position.
			if (thumbstickState.isActive)
			{
				const float y = thumbstickState.currentState.y;
				const float magnitude = fabsf(y);
				if (magnitude < kThumbstickReleaseThreshold)
				{
					g_thumbstickArmed[handIndex] = true;
				}
				else if (magnitude >= kThumbstickDeadzone && g_thumbstickArmed[handIndex])
				{
					g_thumbstickArmed[handIndex] = false;

					const NudgeAction action = y > 0 ? NudgeAction::ScaleUp : NudgeAction::ScaleDown;
					g_pendingNudgeActions.push_back(static_cast<uint8_t>(action));
				}
			}

			// --- Visualization (marker + laser) ---
			ControllerVisual visual{};
			visual.visible = handPoseValid;

			{
				visual.markerPose.position = handLocation.pose.position;
				visual.markerPose.orientation = handLocation.pose.orientation;

				VisualColor color = kColorIdle;
				float beamLength = kLaserLengthMeters;

				if (grab.active)
				{
					color = kColorGrabbed;
					// Shorten the beam to the grabbed panel's distance so it
					// visually "holds" the panel instead of poking through it,
					// and draw a yellow highlight frame around the grabbed
					// panel itself (mirrors the desktop edit-mode highlight)
					// so it's obvious in the headset which panel is being
					// moved.
					const auto panels = IpcServer::GetPanelsSnapshot();
					for (const auto& panel : panels)
					{
						if (std::memcmp(panel.panelId.bytes, grab.panelId.bytes, sizeof(panel.panelId.bytes)) == 0)
						{
							XrPosef panelWorld = PanelWorldPose(panel);
							beamLength = VectorLength(VectorSub(panelWorld.position, aimLocation.pose.position));

							constexpr float kHighlightMargin = 1.08f;
							visual.grabHighlightVisible = true;
							visual.grabHighlightPose = panelWorld;
							visual.grabHighlightWidthMeters = panel.widthMeters * kHighlightMargin;
							visual.grabHighlightHeightMeters = panel.heightMeters * kHighlightMargin;
							break;
						}
					}
				}
				else
				{
					// Not grabbing: use the same laser/ray hit-test as the
					// trigger-press logic above, so the highlight only
					// appears on the panel the laser is actually pointed
					// at (picking the closest hit if the ray passes
					// through multiple panels), not merely whichever
					// panel happens to be near the controller.
					const auto panels = IpcServer::GetPanelsSnapshot();
					float bestHitDistance = kMaxAimDistanceMeters;
					bool foundHover = false;
					XrPosef bestPanelWorld{};
					float bestWidthMeters = 0.0f;
					float bestHeightMeters = 0.0f;

					for (const auto& panel : panels)
					{
						if (!panel.enabled || panel.headLocked)
						{
							continue;
						}
						XrPosef panelWorld = PanelWorldPose(panel);
						float hitDistance = 0.0f;
						if (RayHitsPanel(aimLocation.pose.position, aimForward, panelWorld, panel.widthMeters, panel.heightMeters, &hitDistance)
							&& hitDistance < bestHitDistance)
						{
							bestHitDistance = hitDistance;
							bestPanelWorld = panelWorld;
							bestWidthMeters = panel.widthMeters;
							bestHeightMeters = panel.heightMeters;
							foundHover = true;
						}
					}

					if (foundHover)
					{
						color = kColorHover;
						beamLength = bestHitDistance;

						constexpr float kHighlightMargin = 1.08f;
						visual.grabHighlightVisible = true;
						visual.grabHighlightPose = bestPanelWorld;
						visual.grabHighlightWidthMeters = bestWidthMeters * kHighlightMargin;
						visual.grabHighlightHeightMeters = bestHeightMeters * kHighlightMargin;
					}
				}

				visual.markerColor = color;
				visual.laserColor = color;
				visual.laserLengthMeters = beamLength;
				visual.laserPose = ForwardBeamPose(aimLocation.pose, beamLength);
			}

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_visuals[handIndex] = visual;
			}
		}
	}

	void OnInstanceCreated(XrInstance instance, PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		g_instance = instance;
		g_getInstanceProcAddr = nextGetInstanceProcAddr;

		auto load = [&](const char* name, auto& fn)
		{
			nextGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn));
		};

		load("xrCreateActionSet", xrCreateActionSet_);
		load("xrDestroyActionSet", xrDestroyActionSet_);
		load("xrCreateAction", xrCreateAction_);
		load("xrStringToPath", xrStringToPath_);
		load("xrSuggestInteractionProfileBindings", xrSuggestInteractionProfileBindings_);
		load("xrCreateActionSpace", xrCreateActionSpace_);
		load("xrDestroySpace", xrDestroySpace_);
		load("xrSyncActions", xrSyncActions_);
		load("xrGetActionStatePose", xrGetActionStatePose_);
		load("xrGetActionStateBoolean", xrGetActionStateBoolean_);
		load("xrGetActionStateVector2f", xrGetActionStateVector2f_);
		load("xrLocateSpace", xrLocateSpace_);

		if (xrCreateActionSet_ == nullptr || xrCreateAction_ == nullptr || xrStringToPath_ == nullptr)
		{
			Logging::Log("ControllerInput: required OpenXR input functions unavailable, VR controller panel input disabled.");
			return;
		}

		g_leftHandPath = MakePath("/user/hand/left");
		g_rightHandPath = MakePath("/user/hand/right");

		XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
		std::strncpy(actionSetInfo.actionSetName, "donutzvrhud_panel_control", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(actionSetInfo.localizedActionSetName, "Donutz VR HUD Panel Control", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		actionSetInfo.priority = 0;

		if (XR_FAILED(xrCreateActionSet_(instance, &actionSetInfo, &g_actionSet)))
		{
			Logging::Log("ControllerInput: xrCreateActionSet failed.");
			return;
		}

		XrPath subactionPaths[] = { g_leftHandPath, g_rightHandPath };

		auto createAction = [&](const char* name, const char* localized, XrActionType type, XrAction& outAction)
		{
			XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
			std::strncpy(actionInfo.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
			std::strncpy(actionInfo.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
			actionInfo.actionType = type;
			actionInfo.countSubactionPaths = 2;
			actionInfo.subactionPaths = subactionPaths;
			return XR_SUCCEEDED(xrCreateAction_(g_actionSet, &actionInfo, &outAction));
		};

		bool ok = true;
		ok &= createAction("panel_hand_pose", "Panel Hand Pose", XR_ACTION_TYPE_POSE_INPUT, g_poseAction);
		ok &= createAction("panel_aim_pose", "Panel Aim Pose", XR_ACTION_TYPE_POSE_INPUT, g_aimPoseAction);
		ok &= createAction("panel_grab_trigger", "Panel Grab Trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, g_triggerAction);
		ok &= createAction("panel_nudge_thumbstick", "Panel Nudge Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, g_thumbstickAction);

		if (!ok)
		{
			Logging::Log("ControllerInput: xrCreateAction failed for one or more panel control actions.");
			return;
		}

		// Cover the common SteamVR-exposed interaction profiles; SteamVR
		// remaps whatever physical controller is connected onto one of
		// these depending on driver, so this list is sufficient for
		// Index/Vive/Touch/WMR controllers used via SteamVR as well as
		// other OpenXR runtimes exposing the same standard profiles.
		SuggestBindingsForProfile("/interaction_profiles/valve/index_controller", "/input/grip/pose", "/input/trigger/value", "/input/thumbstick");
		SuggestBindingsForProfile("/interaction_profiles/htc/vive_controller", "/input/grip/pose", "/input/trigger/value", "/input/trackpad");
		SuggestBindingsForProfile("/interaction_profiles/oculus/touch_controller", "/input/grip/pose", "/input/trigger/value", "/input/thumbstick");
		SuggestBindingsForProfile("/interaction_profiles/microsoft/motion_controller", "/input/grip/pose", "/input/trigger/value", "/input/thumbstick");
		SuggestBindingsForProfile("/interaction_profiles/khr/simple_controller", "/input/grip/pose", "/input/select/click", "/input/select/click");

		g_actionsCreated = true;
		Logging::Log("ControllerInput: action set and bindings created successfully.");
	}

	XrResult OnAttachSessionActionSets(
		PFN_xrAttachSessionActionSets next,
		XrSession session,
		const XrSessionActionSetsAttachInfo* attachInfo)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (!g_actionsCreated || attachInfo == nullptr)
		{
			return next(session, attachInfo);
		}

		// Merge the app's requested action sets with our own, since only
		// one xrAttachSessionActionSets call is allowed per session.
		std::vector<XrActionSet> merged(attachInfo->actionSets, attachInfo->actionSets + attachInfo->countActionSets);
		merged.push_back(g_actionSet);

		XrSessionActionSetsAttachInfo mergedInfo = *attachInfo;
		mergedInfo.actionSets = merged.data();
		mergedInfo.countActionSets = static_cast<uint32_t>(merged.size());

		const XrResult result = next(session, &mergedInfo);
		g_attached = XR_SUCCEEDED(result);
		if (!g_attached)
		{
			Logging::Log("ControllerInput: merged xrAttachSessionActionSets failed, XrResult=" + std::to_string(result) + ". VR controller panel input disabled for this session.");
		}
		return result;
	}

	void OnSessionCreated(XrSession session, XrSpace localSpace)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_session = session;
		g_localSpace = localSpace;

		if (!g_actionsCreated || xrCreateActionSpace_ == nullptr)
		{
			return;
		}

		auto createSpace = [&](XrAction action, XrPath handPath, XrSpace& outSpace)
		{
			XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
			spaceInfo.action = action;
			spaceInfo.subactionPath = handPath;
			spaceInfo.poseInActionSpace.orientation = { 0, 0, 0, 1 };
			if (XR_FAILED(xrCreateActionSpace_(session, &spaceInfo, &outSpace)))
			{
				Logging::Log("ControllerInput: xrCreateActionSpace failed for a hand.");
			}
		};

		createSpace(g_poseAction, g_leftHandPath, g_leftHandSpace);
		createSpace(g_poseAction, g_rightHandPath, g_rightHandSpace);
		createSpace(g_aimPoseAction, g_leftHandPath, g_leftAimSpace);
		createSpace(g_aimPoseAction, g_rightHandPath, g_rightAimSpace);
	}

	void OnSessionDestroyed()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (xrDestroySpace_ != nullptr)
		{
			if (g_leftHandSpace != XR_NULL_HANDLE) { xrDestroySpace_(g_leftHandSpace); }
			if (g_rightHandSpace != XR_NULL_HANDLE) { xrDestroySpace_(g_rightHandSpace); }
			if (g_leftAimSpace != XR_NULL_HANDLE) { xrDestroySpace_(g_leftAimSpace); }
			if (g_rightAimSpace != XR_NULL_HANDLE) { xrDestroySpace_(g_rightAimSpace); }
		}
		g_leftHandSpace = XR_NULL_HANDLE;
		g_rightHandSpace = XR_NULL_HANDLE;
		g_leftAimSpace = XR_NULL_HANDLE;
		g_rightAimSpace = XR_NULL_HANDLE;
		g_session = XR_NULL_HANDLE;
		g_attached = false;
		g_grabState[0] = GrabState{};
		g_grabState[1] = GrabState{};
	}

	void Update(XrSession session, XrSpace localSpace, XrTime predictedDisplayTime)
	{
		PFN_xrSyncActions syncActions;
		bool attached;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			syncActions = xrSyncActions_;
			attached = g_attached && g_actionsCreated;
		}

		if (!attached || syncActions == nullptr)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_visuals[0] = ControllerVisual{};
			g_visuals[1] = ControllerVisual{};
			return;
		}

		// VR controllers must stay inert (no grabbing/nudging panels)
		// unless the user has explicitly enabled Edit Mode in the GUI;
		// see IpcServer::IsEditModeActive / MainWindow.xaml.cs
		// EditModeToggle_Changed.
		if (!IpcServer::IsEditModeActive())
		{
			g_grabState[0] = GrabState{};
			g_grabState[1] = GrabState{};
			std::lock_guard<std::mutex> lock(g_mutex);
			g_visuals[0] = ControllerVisual{};
			g_visuals[1] = ControllerVisual{};
			return;
		}

		XrActiveActionSet activeSet{ g_actionSet, XR_NULL_PATH };
		XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
		syncInfo.countActiveActionSets = 1;
		syncInfo.activeActionSets = &activeSet;

		if (XR_FAILED(syncActions(session, &syncInfo)))
		{
			return;
		}

		g_pendingNudgeActions.clear();
		UpdateHand(Hand::Left, session, localSpace, predictedDisplayTime);
		UpdateHand(Hand::Right, session, localSpace, predictedDisplayTime);

		for (uint8_t action : g_pendingNudgeActions)
		{
			IpcServer::QueueControllerNudgeAction(action);
		}
	}

	void GetVisuals(ControllerVisual outVisuals[2])
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		outVisuals[0] = g_visuals[0];
		outVisuals[1] = g_visuals[1];
	}
}
