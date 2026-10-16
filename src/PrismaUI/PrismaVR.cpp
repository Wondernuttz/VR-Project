// PrismaVR.cpp — VR Bridge for Prisma UI
// Translates 2D Prisma views into 3D VR overlays with full interaction.
//
// Supported runtime: OpenComposite Unleashed.
// SteamVR headset users should run through OCU with SteamVR as the OpenXR runtime.
// Native SteamVR keyboard/laser fallbacks are intentionally not supported here.

#include "PrismaVR.h"
#include "PrismaVR_Bridge.h"
#include "ModelPreview.h"
#include "VRInputFilters.h"

#include <Windows.h>
#include <commctrl.h>
#include <d3d11.h>
#include <cmath>
#include <map>
#include <string>
#include <chrono>
#include <cstdlib>
#include <mutex>

#pragma comment(lib, "comctl32.lib")

// ============================================================================
// Section 1: Minimal OpenVR type definitions
// We define just enough to call the IVROverlay and IVRSystem interfaces
// without requiring the full openvr.h header in Prisma's build.
// ============================================================================

namespace openvr {

	typedef uint32_t TrackedDeviceIndex_t;
	typedef uint64_t VROverlayHandle_t;

	static constexpr uint32_t k_unMaxTrackedDeviceCount = 64;
	static constexpr TrackedDeviceIndex_t k_unTrackedDeviceIndexInvalid = 0xFFFFFFFF;

	enum ETrackedDeviceClass {
		TrackedDeviceClass_Invalid = 0,
		TrackedDeviceClass_HMD = 1,
		TrackedDeviceClass_Controller = 2,
		TrackedDeviceClass_GenericTracker = 3,
		TrackedDeviceClass_TrackingReference = 4,
	};

	enum ETrackingUniverseOrigin {
		TrackingUniverseSeated = 0,
		TrackingUniverseStanding = 1,
		TrackingUniverseRawAndUncalibrated = 2,
	};

	enum ETrackedControllerRole {
		TrackedControllerRole_Invalid = 0,
		TrackedControllerRole_LeftHand = 1,
		TrackedControllerRole_RightHand = 2,
	};

	enum EVROverlayError {
		VROverlayError_None = 0,
	};

	enum ETextureType {
		TextureType_DirectX = 0,
	};

	enum EColorSpace {
		ColorSpace_Auto = 0,
	};

	enum EVROverlayInputMethod {
		VROverlayInputMethod_None = 0,
		VROverlayInputMethod_Mouse = 1,
	};

	struct HmdMatrix34_t {
		float m[3][4];
	};

	struct HmdVector2_t {
		float v[2];
	};

	struct HmdVector3_t {
		float v[3];
	};

	struct Texture_t {
		void* handle;
		ETextureType eType;
		EColorSpace eColorSpace;
	};

	struct TrackedDevicePose_t {
		HmdMatrix34_t mDeviceToAbsoluteTracking;
		HmdVector3_t vVelocity;
		HmdVector3_t vAngularVelocity;
		uint32_t eTrackingResult;
		bool bPoseIsValid;
		bool bDeviceIsConnected;
	};

	struct VRControllerAxis_t {
		float x;
		float y;
	};

	struct VRControllerState_t {
		uint32_t unPacketNum;
		uint64_t ulButtonPressed;
		uint64_t ulButtonTouched;
		VRControllerAxis_t rAxis[5];
	};

	// Button IDs
	static constexpr int k_EButton_SteamVR_Trigger = 33;
	static constexpr int k_EButton_Grip = 2;
	static constexpr int k_EButton_SteamVR_Touchpad = 32;
	inline uint64_t ButtonMaskFromId(int id) { return 1ULL << id; }

	// Overlay intersection types
	struct VROverlayIntersectionParams_t {
		HmdVector3_t vSource;
		HmdVector3_t vDirection;
		ETrackingUniverseOrigin eOrigin;
	};

	struct VROverlayIntersectionResults_t {
		HmdVector3_t vPoint;
		HmdVector3_t vNormal;
		HmdVector2_t vUVs;
		float fDistance;
	};

	enum EVRMouseButton {
		VRMouseButton_Left = 0x0001,
		VRMouseButton_Right = 0x0002,
		VRMouseButton_Middle = 0x0004,
	};

} // namespace openvr

// ============================================================================
// Vtable call infrastructure
// OpenVR C++ interfaces are obtained via VR_GetGenericInterface. Rather than
// defining fake C++ classes (which crash if even one vtable entry is wrong),
// we call specific vtable slots by index — counted from IVROverlay_026.h.
// On x64 Windows, all calling conventions use the Microsoft x64 ABI.
// ============================================================================

template<typename Ret, typename... Args>
static Ret VCall(void* iface, int slot, Args... args) {
	void** vtable = *reinterpret_cast<void***>(iface);
	using Fn = Ret(*)(void*, Args...);
	return reinterpret_cast<Fn>(vtable[slot])(iface, args...);
}

// Checked overlay call — logs non-zero EVROverlayError returns.
// Use this for all IVROverlay calls that return an error code.
template<typename... Args>
static openvr::EVROverlayError VCallOvl(void* iface, int slot, const char* funcName, Args... args) {
	auto err = VCall<openvr::EVROverlayError>(iface, slot, args...);
	if (err != openvr::VROverlayError_None) {
		logger::warn("PrismaVR: {} failed (error {})", funcName, static_cast<int>(err));
	}
	return err;
}

// IVROverlay_026 vtable slot indices (counted from build/generated/interfaces/IVROverlay_026.h)
namespace OVL_SLOT {
	constexpr int CreateOverlay = 1;                    // (key, name, *handle) → error
	constexpr int DestroyOverlay = 2;                   // (handle) → error
	constexpr int SetOverlayWidthInMeters = 21;         // (handle, width) → error
	constexpr int SetOverlayTransformAbsolute = 32;     // (handle, origin, *matrix) → error
	constexpr int ShowOverlay = 43;                     // (handle) → error
	constexpr int HideOverlay = 44;                     // (handle) → error
	constexpr int SetOverlayInputMethod = 50;           // (handle, method) → error
	constexpr int SetOverlayMouseScale = 52;            // (handle, *vec2) → error
	constexpr int SetOverlaySortOrder = 19;              // (handle, sortOrder) → error
	constexpr int SetOverlayTexture = 60;               // (handle, *texture) → error
}

// IVRSystem_022 vtable slot indices
namespace SYS_SLOT {
	constexpr int GetDeviceToAbsoluteTrackingPose = 11; // (origin, predSec, *poses, count) → void
	constexpr int GetControllerRoleForTrackedDeviceIndex = 18; // (deviceIdx) → ETrackedControllerRole
	constexpr int GetTrackedDeviceClass = 19;              // (deviceIdx) → ETrackedDeviceClass
	constexpr int GetControllerState = 33;                 // (deviceIdx, *state, stateSize) → bool
}

// ============================================================================
// Section 2: Function pointer types for dynamic loading
// ============================================================================

typedef void* (*VR_GetGenericInterface_t)(const char* pchInterfaceVersion, int* peError);

// ============================================================================
// Section 3: Per-view VR overlay state
// ============================================================================

struct VROverlayState {
	openvr::VROverlayHandle_t handle = 0;
	bool textureBound = false;  // has a live texture submitted; gates show/hide during teardown
	Microsoft::WRL::ComPtr<ID3D11Texture2D> submittedTexture;  // retain while OpenVR may sample it

	// 3D position and orientation
	float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
	float fwdX = 0.0f, fwdY = 0.0f, fwdZ = -1.0f; // Facing the player
	float upX = 0.0f, upY = 1.0f, upZ = 0.0f;

	float widthMeters = 0.8f;
	bool worldLocked = false;
	bool modderPositioned = false;

	// Grab state. The offset keeps the exact point under the laser attached to
	// the controller ray instead of snapping the panel center to it.
	bool isGrabbed = false;
	int grabbedByController = -1; // 0=left, 1=right
	float grabDistance = 1.0f;    // Distance along laser ray at grab start
	float grabOffX = 0.0f, grabOffY = 0.0f, grabOffZ = 0.0f;

	// Resize state (both triggers held)
	bool isResizing = false;
	float resizeBaseDistance = 0.0f; // Controller distance when resize started
	float resizeBaseWidth = 0.0f;   // Overlay width when resize started

	// Index in the fan-out arc (for head-following)
	int arcIndex = 0;

	// Texture dimensions (cached for aspect ratio)
	uint32_t texWidth = 0;
	uint32_t texHeight = 0;
};

// ============================================================================
// Section 4: Global state
// ============================================================================

static bool g_vrActive = false;
static HMODULE g_openvrDll = nullptr;
static VR_GetGenericInterface_t g_vrGetInterface = nullptr;

// Raw interface pointers obtained via VR_GetGenericInterface.
// Called through VCall<> with explicit vtable slot indices.
// Works identically on SteamVR (native) and OpenComposite (reimplemented).
static void* g_overlay = nullptr;  // IVROverlay_026
static void* g_system = nullptr;   // IVRSystem_022

static std::map<uint64_t, VROverlayState> g_vrOverlays;
static int g_overlayCounter = 0;

// Controller tracking
struct ControllerInfo {
	bool valid = false;
	float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
	float fwdX = 0.0f, fwdY = 0.0f, fwdZ = -1.0f;
	bool triggerPressed = false;
	bool gripPressed = false;
	bool gripWasPressed = false;
	float triggerValue = 0.0f;
	float thumbstickX = 0.0f;
	float thumbstickY = 0.0f;
	// Trigger timing for tap vs hold detection
	bool triggerWasPressed = false;
	std::chrono::steady_clock::time_point triggerPressTime;
	uint64_t pressedViewId = 0;
	int pressedPixelX = 0;
	int pressedPixelY = 0;
	bool mouseDownActive = false;
	bool scrollCaptureActive = false;
	std::chrono::steady_clock::time_point lastScrollTime{};
	bool scrollClockInitialized = false;
	float scrollRemainderX = 0.0f;
	float scrollRemainderY = 0.0f;
	std::chrono::steady_clock::time_point lastPoseTime;
	bool poseClockInitialized = false;
	PrismaUI::VRInput::AdaptivePoseFilter poseFilter;
};

static ControllerInfo g_controllers[2]; // 0=left, 1=right

// A short hold on ordinary panel chrome moves the panel. A deliberate half-second
// hold inside scrollable content captures that exact pane instead.
static constexpr float PANEL_GRAB_HOLD_THRESHOLD = 0.3f;
static constexpr float SCROLL_CAPTURE_HOLD_THRESHOLD = 0.5f;

// World-locked panels auto-destruct beyond this distance from the player (meters)
static constexpr float AUTO_CLOSE_DISTANCE = 50.0f;

// Teleport detection: if head moves more than this in one frame, assume load screen
static constexpr float TELEPORT_THRESHOLD = 10.0f;

// Previous frame head position for teleport detection
static float g_lastHeadX = 0.0f, g_lastHeadY = 0.0f, g_lastHeadZ = 0.0f;
static bool g_headPosInitialized = false;

// Per-controller intersection state
struct HitInfo {
	bool hitting = false;
	uint64_t hitViewId = 0;
	float hitU = 0.0f, hitV = 0.0f;
	float hitDistance = 0.0f;
};
static HitInfo g_hitInfo[2]; // 0=left, 1=right

// Laser beam overlays (beams from controllers, dots rendered via CSS on the panel)
// Visuals match OCU's menu laser (VRMenuLaser/BeamTexture): tapered tip, soft
// parabolic edges, hot core, warm white idle and electric blue while clicking.
static openvr::VROverlayHandle_t g_laserBeamHandle[2] = {0, 0};
static ID3D11Texture2D* g_laserBeamTex[2] = { nullptr, nullptr }; // [0]=idle, [1]=click
static bool g_laserClickApplied[2] = { false, false }; // which texture each hand shows
static bool g_lasersCreated = false;
static constexpr float LASER_LENGTH = 0.3048f; // 1 foot
static constexpr float LASER_WIDTH = 0.003f;   // 3mm beam width
static constexpr float LASER_SURFACE_GAP = 0.02f;  // stop beam this far before panel (meters)
static constexpr float MIN_BEAM_LENGTH   = 0.01f;  // minimum visible beam length (meters)
static constexpr uint32_t LASER_SORT_ORDER   = 100;  // overlay render priority (on top of panels)
// Overlay quad height = widthMeters * (texH / texW), so the texture aspect must
// equal LASER_LENGTH / LASER_WIDTH for the existing beamWidth scaling to keep
// the quad exactly beamLen long.
static constexpr int BEAM_TEX_W = 32;
static constexpr int BEAM_TEX_H = (int)(BEAM_TEX_W * (LASER_LENGTH / LASER_WIDTH) + 0.5f);

// Overlay positioning defaults
static constexpr float DEFAULT_OVERLAY_DISTANCE = 0.85f;  // meters in front of head
static constexpr float DEFAULT_OVERLAY_Y_OFFSET = 0.1f;   // meters below eye level
static constexpr float DEFAULT_OVERLAY_WIDTH    = 0.8f;    // meters
static constexpr float OVERLAY_ARC_ANGLE_DEG    = 15.0f;   // fan-out degrees per additional view
static constexpr float DEG_TO_RAD = 3.14159f / 180.0f;

// Resize/grab constraints
static constexpr float MIN_OVERLAY_WIDTH     = 0.2f;  // meters — smallest manual resize
static constexpr float MAX_OVERLAY_WIDTH     = 3.0f;  // meters — largest manual resize
static constexpr float API_MIN_OVERLAY_WIDTH = 0.1f;  // meters — API lower bound
static constexpr float API_MAX_OVERLAY_WIDTH = 5.0f;  // meters — API upper bound
static constexpr float MIN_GRAB_DISTANCE     = 0.2f;  // meters — can't push panel into face
static constexpr float MAX_GRAB_DISTANCE     = 5.0f;  // meters — can't push panel to infinity
static constexpr float MIN_RESIZE_DISTANCE   = 0.01f; // meters — prevents division by zero

// Input thresholds
static constexpr float MANIPULATION_THUMBSTICK_DEADZONE = 0.1f;
static constexpr float RESIZE_PUSH_PULL_SPEED  = 0.03f;  // meters/frame at full stick deflection
static constexpr float DEPTH_ADJUST_SPEED      = 0.02f;  // meters/frame for free-hand depth control

// VR input configuration. Values are loaded once from Data/SKSE/Plugins/PrismaUI_VR.ini.
static constexpr const char* VR_INPUT_INI_PATH = ".\\Data\\SKSE\\Plugins\\PrismaUI_VR.ini";

struct VRInputConfig {
	PrismaUI::VRInput::PoseFilterConfig poseFilter;
	float thumbstickDeadzone = 0.18f;
	float scrollPixelsPerSecond = 1000.0f;
};

static VRInputConfig g_vrInputConfig;

static float ReadIniFloat(const char* section, const char* key, float defaultValue)
{
	char defaultText[32]{};
	_snprintf_s(defaultText, sizeof(defaultText), _TRUNCATE, "%.6g", defaultValue);
	char valueText[64]{};
	GetPrivateProfileStringA(section, key, defaultText, valueText,
	                         static_cast<DWORD>(sizeof(valueText)), VR_INPUT_INI_PATH);
	char* end = nullptr;
	const float value = std::strtof(valueText, &end);
	return end != valueText && std::isfinite(value) ? value : defaultValue;
}

static void LoadVRInputConfig()
{
	auto& filter = g_vrInputConfig.poseFilter;
	filter.enabled = GetPrivateProfileIntA("Laser", "bSmoothing", 1, VR_INPUT_INI_PATH) != 0;
	filter.directionMinCutoffHz = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fDirectionMinCutoffHz", 4.0f), 0.5f, 30.0f);
	filter.directionBeta = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fDirectionBeta", 0.35f), 0.0f, 20.0f);
	filter.positionMinCutoffHz = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fPositionMinCutoffHz", 6.0f), 0.5f, 30.0f);
	filter.positionBeta = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fPositionBeta", 12.0f), 0.0f, 30.0f);
	filter.derivativeCutoffHz = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fDerivativeCutoffHz", 1.0f), 0.1f, 10.0f);
	filter.snapAngleDegrees = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fSnapAngleDegrees", 60.0f), 1.0f, 90.0f);
	filter.snapDistanceMeters = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fSnapDistanceMeters", 0.35f), 0.02f, 1.0f);
	filter.maxGapSeconds = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Laser", "fMaxGapSeconds", 0.25f), 0.05f, 1.0f);
	g_vrInputConfig.thumbstickDeadzone = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Scrolling", "fThumbstickDeadzone", 0.18f), 0.05f, 0.80f);
	g_vrInputConfig.scrollPixelsPerSecond = PrismaUI::VRInput::Clamp(
		ReadIniFloat("Scrolling", "fPixelsPerSecond", 1000.0f), 100.0f, 4000.0f);

	logger::info(
		"PrismaVR: input config loaded: smoothing={}, dirCutoff={:.2f}Hz beta={:.2f}, "
		"posCutoff={:.2f}Hz beta={:.2f}, snap={:.0f}deg/{:.2f}m, gap={:.2f}s, "
		"scrollDeadzone={:.2f}, scrollSpeed={:.0f}px/s",
		filter.enabled, filter.directionMinCutoffHz, filter.directionBeta,
		filter.positionMinCutoffHz, filter.positionBeta,
		filter.snapAngleDegrees, filter.snapDistanceMeters, filter.maxGapSeconds,
		g_vrInputConfig.thumbstickDeadzone, g_vrInputConfig.scrollPixelsPerSecond);
}

// CSS cursor dot styling
static constexpr int CURSOR_DOT_SIZE_PX = 12;          // pixel diameter of laser dot on panel
static constexpr int CURSOR_DOT_Z_INDEX = 2147483647;  // max int — always renders on top

// Polling intervals (in frames, not seconds)
static constexpr int TEXT_FOCUS_POLL_FRAMES    = 10;   // ~110ms at 90fps
static constexpr int DEBUG_LOG_INTERVAL_FRAMES = 300;  // ~5 seconds at 60fps

// CSS cursor dots injected into Prisma HTML views (one per hand, rendered ON the panel texture)
static uint64_t g_lastHitViewId[2] = {0, 0};        // per-controller last-hit view for hiding
static int g_lastCursorX[2] = {-1, -1};              // last injected cursor X (skip if unchanged)
static int g_lastCursorY[2] = {-1, -1};              // last injected cursor Y (skip if unchanged)
static bool g_cursorDotCreated[2] = {false, false};   // track if dot element exists in DOM
static bool g_lastDotClicked[2] = {false, false};     // dot showing click (blue) styling

// Control masking via Skyrim's ControlMap (global toggles)
static bool g_movementMasked = false;
static bool g_combatMasked = false;

// VR keyboard: WndProc subclass to intercept WM_OC_CHAR from OpenComposite
static HWND g_gameHwnd = nullptr;
static constexpr UINT_PTR PRISMAVR_SUBCLASS_ID = 0x5052564B; // "PRVK"
static constexpr UINT WM_OC_CHAR = WM_APP + 0x4F45;
static std::atomic<uint64_t> g_keyboardTargetViewId{0}; // view that last received a laser click
static std::atomic<uint64_t> g_keyboardFocusGeneration{0};
static std::mutex g_keyboardFocusStateMutex;
static std::atomic<bool> g_textInputFocused{false}; // true ONLY when an <input>/<textarea>/contenteditable has focus
static std::atomic<bool> g_interactiveDragActive[2]{{false}, {false}}; // per-hand slider/draggable state; scroll panes remain grabbable
static std::atomic<bool> g_scrollTargetCaptured[2]{{false}, {false}}; // exact pane captured at trigger-down
static std::atomic<uint64_t> g_scrollTargetViewId[2]{{0}, {0}};
static std::atomic<bool> g_pointerProbePending[2]{{false}, {false}};
static std::atomic<uint64_t> g_pointerInteractionGeneration[2]{{0}, {0}};

// Active hand for mouse interaction: -1 = none (first click activates), 0 = left, 1 = right.
// Only the active hand sends mouse move/click events to Ultralight.
// Both hands still show cursor dots and can grab panels.
// Switches when the OTHER hand clicks on a panel.
static int g_mouseActiveHand = -1;

// ── Aim pose data from OpenComposite (exposed via window property) ──
// OCU computes aim poses from OpenXR's /input/aim/pose.
struct OCAimPoseData {
	openvr::HmdMatrix34_t matrix[2]; // [0]=left, [1]=right
	bool valid[2];
};
static const OCAimPoseData* g_aimPoses = nullptr; // pointer from OC via window property

static void ClearCapturedScrollTarget(std::shared_ptr<PrismaUI::Core::PrismaView> view, int hand)
{
	if (hand < 0 || hand > 1)
		return;
	g_scrollTargetCaptured[hand].store(false);
	g_scrollTargetViewId[hand].store(0);
	if (!view)
		return;
	PrismaVR_Bridge::RunJavaScript(view,
		"var a=window.__prismaVrScrollTargets;if(a)a[" + std::to_string(hand) + "]=null;");
}

static void QueueCancelledMouseRelease(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	const ControllerInfo& controller)
{
	if (!view || !controller.mouseDownActive)
		return;
	ultralight::MouseEvent cancelMove;
	cancelMove.type = ultralight::MouseEvent::kType_MouseMoved;
	cancelMove.x = -1;
	cancelMove.y = -1;
	cancelMove.button = ultralight::MouseEvent::kButton_None;
	PrismaVR_Bridge::FireMouseEvent(view, cancelMove);

	ultralight::MouseEvent cancelUp;
	cancelUp.type = ultralight::MouseEvent::kType_MouseUp;
	cancelUp.x = -1;
	cancelUp.y = -1;
	cancelUp.button = ultralight::MouseEvent::kButton_Left;
	PrismaVR_Bridge::FireMouseEvent(view, cancelUp);
}

static void ResetPointerState(int hand)
{
	if (hand < 0 || hand > 1)
		return;
	g_pointerInteractionGeneration[hand].fetch_add(1);
	g_pointerProbePending[hand].store(false);
	g_interactiveDragActive[hand].store(false);
	g_scrollTargetCaptured[hand].store(false);
	g_scrollTargetViewId[hand].store(0);
	auto& controller = g_controllers[hand];
	controller.mouseDownActive = false;
	controller.pressedViewId = 0;
	controller.scrollCaptureActive = false;
	controller.scrollClockInitialized = false;
	controller.scrollRemainderX = 0.0f;
	controller.scrollRemainderY = 0.0f;
	if (g_mouseActiveHand == hand)
		g_mouseActiveHand = -1;
}

static void CancelPointerStateForView(uint64_t viewId,
	std::shared_ptr<PrismaUI::Core::PrismaView> view)
{
	auto overlayIt = g_vrOverlays.find(viewId);
	for (int hand = 0; hand < 2; ++hand) {
		auto& controller = g_controllers[hand];
		const bool pressedOnView = controller.pressedViewId == viewId;
		const bool grabbedView = overlayIt != g_vrOverlays.end() && overlayIt->second.isGrabbed &&
		                         overlayIt->second.grabbedByController == hand;
		if (!pressedOnView && !grabbedView)
			continue;

		if (pressedOnView)
			QueueCancelledMouseRelease(view, controller);
		ResetPointerState(hand);
		ClearCapturedScrollTarget(view, hand);

		if (grabbedView) {
			overlayIt->second.isGrabbed = false;
			overlayIt->second.isResizing = false;
			overlayIt->second.grabbedByController = -1;
		}
	}
}

// ============================================================================
// Section 5: VR Runtime Detection & Interface Loading
// ============================================================================

static bool DetectVR()
{
	// SkyrimVR.exe is a separate executable from SkyrimSE.exe
	HMODULE vrExe = GetModuleHandleA("SkyrimVR.exe");
	if (!vrExe) {
		logger::info("PrismaVR: Not running in SkyrimVR.exe, VR disabled.");
		return false;
	}

	// openvr_api.dll should already be loaded by the game or OpenComposite
	g_openvrDll = GetModuleHandleA("openvr_api.dll");
	if (!g_openvrDll) {
		logger::warn("PrismaVR: openvr_api.dll not loaded, VR disabled.");
		return false;
	}

	g_vrGetInterface = (VR_GetGenericInterface_t)GetProcAddress(g_openvrDll, "VR_GetGenericInterface");
	if (!g_vrGetInterface) {
		logger::warn("PrismaVR: VR_GetGenericInterface not found, VR disabled.");
		return false;
	}

	int error = 0;

	// Try IVROverlay v026 first (latest), fall back to v025
	g_overlay = g_vrGetInterface("IVROverlay_026", &error);
	if (!g_overlay || error != 0) {
		g_overlay = g_vrGetInterface("IVROverlay_025", &error);
	}
	if (!g_overlay || error != 0) {
		logger::warn("PrismaVR: Failed to get IVROverlay interface (error {}), VR disabled.", error);
		return false;
	}

	g_system = g_vrGetInterface("IVRSystem_022", &error);
	if (!g_system || error != 0) {
		g_system = g_vrGetInterface("IVRSystem_021", &error);
	}
	if (!g_system || error != 0) {
		logger::warn("PrismaVR: Failed to get IVRSystem interface (error {}), VR disabled.", error);
		g_overlay = nullptr;
		return false;
	}


	logger::info("PrismaVR: VR runtime detected and interfaces loaded.");
	return true;
}

// ============================================================================
// Section 5b: Laser beam overlay creation
// ============================================================================

// Tapered beam pixels, ported from OCU's BeamTexture.h so both lasers render
// identically. V axis runs along the beam: row 0 = tip (at the panel), last
// row = controller end. Constant-width shaft with soft parabolic edges and a
// subtle hot core; the last 30% toward the tip narrows and fades to a point.
// Colours are PREMULTIPLIED: OCU composites overlay quads premultiplied (no
// UNPREMULTIPLIED flag on its layers), matching its own beam pipeline.
static void FillTaperedBeam(std::vector<uint32_t>& px, uint8_t r, uint8_t g, uint8_t b, uint8_t baseAlpha)
{
	px.assign((size_t)BEAM_TEX_W * BEAM_TEX_H, 0);
	constexpr float kTipZone = 0.30f; // fraction of the beam that tapers to the point
	for (int y = 0; y < BEAM_TEX_H; y++) {
		float t = (float)y / (BEAM_TEX_H - 1); // 0 at tip -> 1 at controller
		float tip = (t < kTipZone) ? (t / kTipZone) : 1.0f;
		float half = (0.04f + 0.96f * tip) * (BEAM_TEX_W * 0.5f - 1.0f);
		float lenA = tip * tip; // eased fade, 0 exactly at the tip
		for (int x = 0; x < BEAM_TEX_W; x++) {
			float d = fabsf(x - (BEAM_TEX_W - 1) * 0.5f) / half; // 0 = core, 1 = edge
			if (d >= 1.0f)
				continue;
			float edge = 1.0f - d * d; // parabolic soft edge
			float a = (baseAlpha / 255.0f) * lenA * edge;
			float core = (d < 0.35f) ? (1.0f - d / 0.35f) : 0.0f;
			float cr = (r + (255 - r) * core * 0.8f) / 255.0f;
			float cg = (g + (255 - g) * core * 0.8f) / 255.0f;
			float cb = (b + (255 - b) * core * 0.8f) / 255.0f;
			uint8_t pr = (uint8_t)(cr * a * 255.0f + 0.5f);
			uint8_t pg = (uint8_t)(cg * a * 255.0f + 0.5f);
			uint8_t pb = (uint8_t)(cb * a * 255.0f + 0.5f);
			uint8_t pa = (uint8_t)(a * 255.0f + 0.5f);
			px[(size_t)y * BEAM_TEX_W + x] = pr | (pg << 8) | (pb << 16) | ((uint32_t)pa << 24);
		}
	}
}

static void CreateLaserOverlays()
{
	if (g_lasersCreated) return;

	ID3D11Device* dev = PrismaVR_Bridge::GetD3DDevice();
	if (!dev || !g_overlay) return;

	// Both colour variants up front so a click never stalls on texture upload.
	// Matches OCU: warm white idle, electric blue while the trigger is held.
	std::vector<uint32_t> beamPixels;
	for (int variant = 0; variant < 2; variant++) {
		if (variant)
			FillTaperedBeam(beamPixels, 55, 145, 255, 220); // electric blue click
		else
			FillTaperedBeam(beamPixels, 255, 240, 220, 200); // warm white idle

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = BEAM_TEX_W;
		td.Height = BEAM_TEX_H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc = { 1, 0 };
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA initData = {};
		initData.pSysMem = beamPixels.data();
		initData.SysMemPitch = sizeof(uint32_t) * BEAM_TEX_W;

		HRESULT hr = dev->CreateTexture2D(&td, &initData, &g_laserBeamTex[variant]);
		if (FAILED(hr)) {
			logger::warn("PrismaVR: Failed to create laser beam texture {}", variant);
			if (g_laserBeamTex[0]) { g_laserBeamTex[0]->Release(); g_laserBeamTex[0] = nullptr; }
			return;
		}
	}

	const char* beamKeys[] = { "prisma_laser_beam_L", "prisma_laser_beam_R" };

	int successCount = 0;
	for (int i = 0; i < 2; i++) {
		auto err = VCall<openvr::EVROverlayError>(g_overlay, OVL_SLOT::CreateOverlay,
			beamKeys[i], beamKeys[i], &g_laserBeamHandle[i]);
		if (err != openvr::VROverlayError_None) {
			logger::warn("PrismaVR: Failed to create laser beam overlay {} (error {})", i, (int)err);
			g_laserBeamHandle[i] = 0;
			continue;
		}
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayWidthInMeters, "SetOverlayWidthInMeters(laser)",
			g_laserBeamHandle[i], LASER_WIDTH);
		openvr::Texture_t bt; bt.handle = g_laserBeamTex[0];
		bt.eType = openvr::TextureType_DirectX; bt.eColorSpace = openvr::ColorSpace_Auto;
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayTexture, "SetOverlayTexture(laser)",
			g_laserBeamHandle[i], &bt);
		g_laserClickApplied[i] = false;
		// Render laser ON TOP of panel overlays (default sort order = 0)
		VCallOvl(g_overlay, OVL_SLOT::SetOverlaySortOrder, "SetOverlaySortOrder(laser)",
			g_laserBeamHandle[i], LASER_SORT_ORDER);
		successCount++;
	}

	if (successCount > 0) {
		g_lasersCreated = true;
		logger::info("PrismaVR: Laser beam overlays created ({}/2 hands).", successCount);
	} else {
		logger::warn("PrismaVR: All laser overlay creation failed — will retry next frame.");
	}
}

// ============================================================================
// Section 6: Transform helpers
// ============================================================================

static void BuildOverlayTransform(const VROverlayState& state, openvr::HmdMatrix34_t& out)
{
	// Build a 3x4 row-major transform from position + forward + up
	// Right = cross(up, forward) for right-handed coordinate system

	// Normalize forward
	float fl = sqrtf(state.fwdX * state.fwdX + state.fwdY * state.fwdY + state.fwdZ * state.fwdZ);
	if (fl < 1e-6f) fl = 1.0f;
	float fx = state.fwdX / fl, fy = state.fwdY / fl, fz = state.fwdZ / fl;

	// Normalize up
	float ul = sqrtf(state.upX * state.upX + state.upY * state.upY + state.upZ * state.upZ);
	if (ul < 1e-6f) ul = 1.0f;
	float ux = state.upX / ul, uy = state.upY / ul, uz = state.upZ / ul;

	// Right = up x forward
	float rx = uy * fz - uz * fy;
	float ry = uz * fx - ux * fz;
	float rz = ux * fy - uy * fx;

	// Re-orthogonalize up = forward x right
	ux = fy * rz - fz * ry;
	uy = fz * rx - fx * rz;
	uz = fx * ry - fy * rx;

	// HmdMatrix34_t: row-major, columns = basis vectors
	// Col 0 = right, Col 1 = up, Col 2 = forward, Col 3 = translation
	out.m[0][0] = rx; out.m[0][1] = ux; out.m[0][2] = fx; out.m[0][3] = state.posX;
	out.m[1][0] = ry; out.m[1][1] = uy; out.m[1][2] = fy; out.m[1][3] = state.posY;
	out.m[2][0] = rz; out.m[2][1] = uz; out.m[2][2] = fz; out.m[2][3] = state.posZ;
}

static void GetHeadPose(float& hx, float& hy, float& hz, float& fx, float& fy, float& fz)
{
	openvr::TrackedDevicePose_t poses[openvr::k_unMaxTrackedDeviceCount];
	VCall<void>(g_system, SYS_SLOT::GetDeviceToAbsoluteTrackingPose,
		(int)openvr::TrackingUniverseStanding, 0.0f, poses, openvr::k_unMaxTrackedDeviceCount);

	// Device 0 is always the HMD
	if (poses[0].bPoseIsValid) {
		const auto& m = poses[0].mDeviceToAbsoluteTracking;
		hx = m.m[0][3]; hy = m.m[1][3]; hz = m.m[2][3];
		// Forward = -Z column
		fx = -m.m[0][2]; fy = -m.m[1][2]; fz = -m.m[2][2];
	} else {
		hx = hy = hz = 0.0f;
		fx = 0.0f; fy = 0.0f; fz = -1.0f;
	}
}

// ============================================================================
// Section 7: Default positioning
// ============================================================================

static void PositionNewOverlay(VROverlayState& state, int viewIndex)
{
	float hx, hy, hz, fx, fy, fz;
	GetHeadPose(hx, hy, hz, fx, fy, fz);

	// Normalize forward (horizontal only for default placement)
	float hfLen = sqrtf(fx * fx + fz * fz);
	if (hfLen < 1e-6f) { fx = 0.0f; fz = -1.0f; hfLen = 1.0f; }
	float nfx = fx / hfLen;
	float nfz = fz / hfLen;

	float distance = DEFAULT_OVERLAY_DISTANCE;
	state.posX = hx + nfx * distance;
	state.posY = hy - DEFAULT_OVERLAY_Y_OFFSET;
	state.posZ = hz + nfz * distance;

	// Fan out additional views in an arc (±15° per view)
	if (viewIndex > 0) {
		float angle = ((viewIndex + 1) / 2) * OVERLAY_ARC_ANGLE_DEG * DEG_TO_RAD;
		if (viewIndex % 2 == 0) angle = -angle;

		float cosA = cosf(angle);
		float sinA = sinf(angle);

		// Rotate around Y axis relative to head
		float dx = nfx * distance;
		float dz = nfz * distance;
		float rotX = dx * cosA - dz * sinA;
		float rotZ = dx * sinA + dz * cosA;

		state.posX = hx + rotX;
		state.posZ = hz + rotZ;
	}

	// Face the player (overlay forward points toward the head)
	state.fwdX = hx - state.posX;
	state.fwdY = 0.0f; // Keep overlay vertical
	state.fwdZ = hz - state.posZ;
	state.upX = 0.0f;
	state.upY = 1.0f;
	state.upZ = 0.0f;
}

// ============================================================================
// Section 8: Overlay lifecycle
// ============================================================================

// CreateVROverlay — Creates an OpenVR overlay for a Prisma view and places it in 3D space.
//
// Each Prisma view gets one VR overlay with its D3D11 texture as the content.
// New overlays spawn in front of the player in an arc pattern (first centered,
// additional views fan out ±15° to avoid stacking). The overlay is immediately
// world-locked so it stays where it spawned instead of following the head.
//
// Mouse scale is set to the texture dimensions so OpenVR correctly maps laser
// hit coordinates to pixel coordinates (even though we do our own raycasting,
// this is needed if any runtime-level input falls through).
static void CreateVROverlay(uint64_t viewId, PrismaVR_Bridge::ViewInfo& viewInfo, int viewIndex)
{
	VROverlayState state;
	state.arcIndex = viewIndex;

	auto* view = viewInfo.view.get();
	auto surface = PrismaVR_Bridge::AcquireTextureSnapshot(view);
	state.texWidth = surface.width;
	state.texHeight = surface.height;

	state.widthMeters = DEFAULT_OVERLAY_WIDTH;

	PositionNewOverlay(state, viewIndex);

	// World-lock immediately after initial placement so the overlay stays
	// where it spawned instead of following the player's head every frame.
	// Teleport/cell-change detection in CleanupDistantOverlays() will reset
	// this to false, causing SyncOverlays to re-anchor to the new position.
	state.worldLocked = true;

	// Create OpenVR overlay
	std::string key = "prisma_vr_" + std::to_string(viewId) + "_" + std::to_string(g_overlayCounter++);
	std::string name = "Prisma View " + std::to_string(viewId);

	auto err = VCall<openvr::EVROverlayError>(g_overlay, OVL_SLOT::CreateOverlay,
		key.c_str(), name.c_str(), &state.handle);
	if (err != openvr::VROverlayError_None) {
		logger::warn("PrismaVR: Failed to create overlay for view {} (error {})", viewId, (int)err);
		return;
	}

	// Configure overlay
	VCallOvl(g_overlay, OVL_SLOT::SetOverlayWidthInMeters, "SetOverlayWidthInMeters",
		state.handle, state.widthMeters);

	// Set mouse scale to texture dimensions (needed for mouse event coordinate space)
	if (state.texWidth > 0 && state.texHeight > 0) {
		openvr::HmdVector2_t mouseScale;
		mouseScale.v[0] = (float)state.texWidth;
		mouseScale.v[1] = (float)state.texHeight;
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayMouseScale, "SetOverlayMouseScale",
			state.handle, &mouseScale);
	}

	// Set input method to mouse so events get generated
	VCallOvl(g_overlay, OVL_SLOT::SetOverlayInputMethod, "SetOverlayInputMethod",
		state.handle, (int)openvr::VROverlayInputMethod_Mouse);

	// Set transform
	openvr::HmdMatrix34_t transform;
	BuildOverlayTransform(state, transform);
	VCallOvl(g_overlay, OVL_SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute",
		state.handle, (int)openvr::TrackingUniverseStanding, &transform);

	// Set texture
	ID3D11Texture2D* tex = surface.texture.Get();
	if (tex) {
		openvr::Texture_t vrTex;
		vrTex.handle = tex;
		vrTex.eType = openvr::TextureType_DirectX;
		vrTex.eColorSpace = openvr::ColorSpace_Auto;
		if (VCallOvl(g_overlay, OVL_SLOT::SetOverlayTexture, "SetOverlayTexture",
				state.handle, &vrTex) == openvr::VROverlayError_None) {
			state.submittedTexture = surface.texture;
			state.textureBound = true;
		}
	}

	// Do not enable an empty overlay. SyncOverlays will show it after a later
	// texture submission succeeds.
	if (state.textureBound)
		VCallOvl(g_overlay, OVL_SLOT::ShowOverlay, "ShowOverlay", state.handle);

	g_vrOverlays[viewId] = state;
	logger::info("PrismaVR: Created VR overlay for view {} at ({:.2f}, {:.2f}, {:.2f})",
		viewId, state.posX, state.posY, state.posZ);
}

static void DestroyVROverlay(uint64_t viewId)
{
	auto it = g_vrOverlays.find(viewId);
	if (it == g_vrOverlays.end()) return;

	// A hidden/destroyed last view bypasses ProcessInput for this frame. Cancel
	// any DOM press and invalidate delayed scroll capture while the view object
	// is still reachable, before removing its overlay state.
	CancelPointerStateForView(viewId, PrismaVR_Bridge::FindView(viewId));
	{
		std::lock_guard lock(g_keyboardFocusStateMutex);
		if (g_keyboardTargetViewId.load() == viewId) {
			g_keyboardTargetViewId.store(0);
			g_keyboardFocusGeneration.fetch_add(1);
			g_textInputFocused.store(false);
			if (g_gameHwnd)
				RemovePropW(g_gameHwnd, L"OC_PRISMA_TEXT");
		}
	}

	if (it->second.handle) {
		VCallOvl(g_overlay, OVL_SLOT::HideOverlay, "HideOverlay", it->second.handle);
		VCallOvl(g_overlay, OVL_SLOT::DestroyOverlay, "DestroyOverlay", it->second.handle);
	}

	g_vrOverlays.erase(it);
	for (int hand = 0; hand < 2; hand++) {
		if (g_hitInfo[hand].hitViewId == viewId) {
			g_hitInfo[hand].hitting = false;
			g_hitInfo[hand].hitViewId = 0;
			g_lastHitViewId[hand] = 0;
			g_lastCursorX[hand] = -1;
			g_lastCursorY[hand] = -1;
			g_cursorDotCreated[hand] = false;
			if (g_mouseActiveHand == hand)
				g_mouseActiveHand = -1;
		}
	}
	// VR funnel for panel destruction - clears any previews riding this panel
	PrismaUI::ModelPreview::OnPanelDestroyed(viewId);
	logger::info("PrismaVR: Destroyed VR overlay for view {}", viewId);
}

// SyncOverlays — Keeps VR overlays in sync with Prisma's 2D view lifecycle.
//
// Prisma UI creates/destroys views dynamically (modder calls, user actions).
// Each frame we diff the set of visible Prisma views against our VR overlays:
//   - New views get a VR overlay created (CreateVROverlay)
//   - Existing views get their D3D11 texture re-submitted (Prisma may have redrawn)
//   - Vanished views get their VR overlay destroyed (DestroyVROverlay)
//
// This is the ONLY place overlays are created from view discovery. Other code
// (CleanupDistantOverlays) may destroy overlays, but SyncOverlays will recreate
// them on the next frame if the underlying Prisma view is still alive.
/// Synchronizes native VR overlays for visible views not externally hosted.
static void SyncOverlays()
{
	auto visibleViews = PrismaVR_Bridge::GetVisibleViews();

	// Track which views are currently visible
	std::map<uint64_t, bool> currentViews;
	int newIndex = 0;

	for (auto& viewInfo : visibleViews) {
		uint64_t id = viewInfo.id;
		if (PrismaVR_Bridge::IsExternallyHosted(viewInfo.view.get()))
			continue;
		currentViews[id] = true;

		auto it = g_vrOverlays.find(id);
		if (it == g_vrOverlays.end()) {
			// New view — create overlay
			CreateVROverlay(id, viewInfo, (int)g_vrOverlays.size());
			it = g_vrOverlays.find(id);
		}

		if (it != g_vrOverlays.end()) {
			auto* view = viewInfo.view.get();
			// Retain a synchronized texture snapshot through the compositor call.
			// A tearing-down or rebuilding view returns an empty snapshot.
			auto surface = PrismaVR_Bridge::AcquireTextureSnapshot(view);
			ID3D11Texture2D* tex = surface.texture.Get();

			if (tex && it->second.handle) {
				openvr::Texture_t vrTex;
				vrTex.handle = tex;
				vrTex.eType = openvr::TextureType_DirectX;
				vrTex.eColorSpace = openvr::ColorSpace_Auto;
				const auto setError = VCallOvl(g_overlay, OVL_SLOT::SetOverlayTexture, "SetOverlayTexture",
					it->second.handle, &vrTex);
				if (setError == openvr::VROverlayError_None) {
					it->second.submittedTexture = surface.texture;
				}
				if (setError == openvr::VROverlayError_None && !it->second.textureBound) {
					VCallOvl(g_overlay, OVL_SLOT::ShowOverlay, "ShowOverlay(retex)", it->second.handle);
					it->second.textureBound = true;
				}
			} else if (it->second.handle && it->second.textureBound) {
				if (VCallOvl(g_overlay, OVL_SLOT::HideOverlay, "HideOverlay(teardown)",
						it->second.handle) == openvr::VROverlayError_None) {
					it->second.textureBound = false;
					it->second.submittedTexture.Reset();
				}
			}

			// Update cached dimensions
			uint32_t tw = surface.width;
			uint32_t th = surface.height;
			if (tw > 0) it->second.texWidth = tw;
			if (th > 0) it->second.texHeight = th;
		}

		newIndex++;
	}

	// Remove overlays for views that are no longer visible
	std::vector<uint64_t> toRemove;
	for (auto& [id, state] : g_vrOverlays) {
		if (currentViews.find(id) == currentViews.end()) {
			toRemove.push_back(id);
		}
	}
	for (uint64_t id : toRemove) {
		DestroyVROverlay(id);
	}
}

// ============================================================================
// Section 9: Controller tracking
// ============================================================================

// UpdateControllers — Reads VR controller poses and button states each frame.
//
// Pose sourcing:
//   OCU exposes OpenXR /input/aim/pose through the game window.
//   Raw SteamVR grip-pose laser fallback is intentionally disabled.
//
// Button state is read via GetControllerState() — works identically on both
// OC and SteamVR. We track trigger (click/grab), grip, and thumbstick axes.
// Previous-frame state is saved for edge detection (press/release transitions).
static void UpdateControllers()
{
	openvr::TrackedDevicePose_t poses[openvr::k_unMaxTrackedDeviceCount];
	VCall<void>(g_system, SYS_SLOT::GetDeviceToAbsoluteTrackingPose,
		(int)openvr::TrackingUniverseStanding, 0.0f, poses, openvr::k_unMaxTrackedDeviceCount);

	// Find left and right controllers by querying device class + role
	// This works universally across all headsets (Oculus, Vive, Index, WMR, etc.)
	openvr::TrackedDeviceIndex_t leftIdx = openvr::k_unTrackedDeviceIndexInvalid;
	openvr::TrackedDeviceIndex_t rightIdx = openvr::k_unTrackedDeviceIndexInvalid;

	for (uint32_t i = 1; i < openvr::k_unMaxTrackedDeviceCount; i++) {
		if (!poses[i].bPoseIsValid || !poses[i].bDeviceIsConnected) continue;

		auto devClass = VCall<openvr::ETrackedDeviceClass>(
			g_system, SYS_SLOT::GetTrackedDeviceClass, i);
		if (devClass != openvr::TrackedDeviceClass_Controller) continue;

		auto role = VCall<openvr::ETrackedControllerRole>(
			g_system, SYS_SLOT::GetControllerRoleForTrackedDeviceIndex, i);

		if (role == openvr::TrackedControllerRole_LeftHand && leftIdx == openvr::k_unTrackedDeviceIndexInvalid)
			leftIdx = i;
		else if (role == openvr::TrackedControllerRole_RightHand && rightIdx == openvr::k_unTrackedDeviceIndexInvalid)
			rightIdx = i;

		if (leftIdx != openvr::k_unTrackedDeviceIndexInvalid &&
			rightIdx != openvr::k_unTrackedDeviceIndexInvalid)
			break; // Found both
	}


	// One-time log when controllers are first detected
	static bool s_loggedControllers = false;
	if (!s_loggedControllers && (leftIdx != openvr::k_unTrackedDeviceIndexInvalid ||
	                             rightIdx != openvr::k_unTrackedDeviceIndexInvalid)) {
		logger::info("PrismaVR: Controllers found — left={}, right={}",
			leftIdx != openvr::k_unTrackedDeviceIndexInvalid ? (int)leftIdx : -1,
			rightIdx != openvr::k_unTrackedDeviceIndexInvalid ? (int)rightIdx : -1);
		s_loggedControllers = true;
	}

	for (int hand = 0; hand < 2; hand++) {
		openvr::TrackedDeviceIndex_t idx = (hand == 0) ? leftIdx : rightIdx;
		auto& ctrl = g_controllers[hand];
		auto invalidateTracking = [&ctrl]() {
			ctrl.valid = false;
			ctrl.triggerPressed = false;
			ctrl.gripPressed = false;
			ctrl.triggerValue = 0.0f;
			ctrl.thumbstickX = 0.0f;
			ctrl.thumbstickY = 0.0f;
			ctrl.poseClockInitialized = false;
			ctrl.poseFilter.Reset();
		};

		// Save previous state for edge detection (must happen BEFORE updating)
		ctrl.triggerWasPressed = ctrl.triggerPressed;
		ctrl.gripWasPressed = ctrl.gripPressed;

		if (idx == openvr::k_unTrackedDeviceIndexInvalid || !poses[idx].bPoseIsValid) {
			invalidateTracking();
			continue;
		}

		ctrl.valid = true;

		if (!g_aimPoses || !g_aimPoses->valid[hand]) {
			invalidateTracking();
			continue;
		}

		// --- Pose: position and forward direction ---
		const auto& aim = g_aimPoses->matrix[hand];
		const PrismaUI::VRInput::Vec3 rawPosition{aim.m[0][3], aim.m[1][3], aim.m[2][3]};
		// Aim pose -Z is the pointing direction (OpenXR convention)
		const PrismaUI::VRInput::Vec3 rawDirection{-aim.m[0][2], -aim.m[1][2], -aim.m[2][2]};
		const bool finitePose = std::isfinite(rawPosition.x) && std::isfinite(rawPosition.y) &&
		                        std::isfinite(rawPosition.z) && std::isfinite(rawDirection.x) &&
		                        std::isfinite(rawDirection.y) && std::isfinite(rawDirection.z);
		if (!finitePose || PrismaUI::VRInput::Length(rawDirection) < 1.0e-6f) {
			invalidateTracking();
			continue;
		}

		const auto poseNow = std::chrono::steady_clock::now();
		float poseDeltaSeconds = 0.0f;
		if (ctrl.poseClockInitialized) {
			poseDeltaSeconds = std::chrono::duration<float>(poseNow - ctrl.lastPoseTime).count();
		}
		ctrl.lastPoseTime = poseNow;
		ctrl.poseClockInitialized = true;
		PrismaUI::VRInput::Vec3 filteredPosition{};
		PrismaUI::VRInput::Vec3 filteredDirection{};
		ctrl.poseFilter.Update(rawPosition, rawDirection, poseDeltaSeconds,
		                       g_vrInputConfig.poseFilter, filteredPosition, filteredDirection);
		ctrl.posX = filteredPosition.x;
		ctrl.posY = filteredPosition.y;
		ctrl.posZ = filteredPosition.z;
		ctrl.fwdX = filteredDirection.x;
		ctrl.fwdY = filteredDirection.y;
		ctrl.fwdZ = filteredDirection.z;


		// --- Button state: universal across all headsets ---
		openvr::VRControllerState_t state{};
		bool gotState = VCall<bool>(g_system, SYS_SLOT::GetControllerState,
			idx, &state, (uint32_t)sizeof(openvr::VRControllerState_t));

		if (gotState) {
			ctrl.triggerPressed = (state.ulButtonPressed & openvr::ButtonMaskFromId(openvr::k_EButton_SteamVR_Trigger)) != 0;
			ctrl.gripPressed    = (state.ulButtonPressed & openvr::ButtonMaskFromId(openvr::k_EButton_Grip)) != 0;
			ctrl.triggerValue   = state.rAxis[1].x; // Trigger analog: 0.0 (released) to 1.0 (fully pressed)
			ctrl.thumbstickX    = state.rAxis[0].x; // Thumbstick/touchpad X: -1.0 to 1.0
			ctrl.thumbstickY    = state.rAxis[0].y; // Thumbstick/touchpad Y: -1.0 to 1.0
		} else {
			ctrl.triggerPressed = false;
			ctrl.gripPressed = false;
			ctrl.triggerValue = 0.0f;
			ctrl.thumbstickX = 0.0f;
			ctrl.thumbstickY = 0.0f;
		}
	}
}

// ============================================================================
// Section 10: Ray-plane intersection (our own math fed by OCU aim poses)
// ============================================================================

struct RayHit {
	bool hit = false;
	float u = 0.0f, v = 0.0f;
	float distance = 0.0f;
	float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
};

// RayOverlayIntersection — Pure math: does this controller's laser hit this overlay?
//
// Fires a ray from the controller position along its forward direction and tests
// intersection with the overlay's rectangular plane. Returns UV coordinates (0–1)
// if the ray hits within the overlay bounds, plus the 3D hit point and distance.
//
// This replaces OpenVR's built-in ComputeOverlayIntersection which requires the
// overlay to be visible and has quirks across runtimes. Our own math stays
// stable because OCU supplies calibrated aim poses.
static RayHit RayOverlayIntersection(const ControllerInfo& ctrl, const VROverlayState& overlay)
{
	RayHit result;

	// Get overlay basis vectors from the stored orientation
	float fl = sqrtf(overlay.fwdX * overlay.fwdX + overlay.fwdY * overlay.fwdY + overlay.fwdZ * overlay.fwdZ);
	if (fl < 1e-6f) return result;
	float fx = overlay.fwdX / fl, fy = overlay.fwdY / fl, fz = overlay.fwdZ / fl;

	float ul = sqrtf(overlay.upX * overlay.upX + overlay.upY * overlay.upY + overlay.upZ * overlay.upZ);
	if (ul < 1e-6f) return result;
	float ux = overlay.upX / ul, uy = overlay.upY / ul, uz = overlay.upZ / ul;

	// Right = up x forward
	float rx = uy * fz - uz * fy;
	float ry = uz * fx - ux * fz;
	float rz = ux * fy - uy * fx;

	// Normal = forward (overlay Z axis points toward viewer)
	float nx = fx, ny = fy, nz = fz;

	// Ray-plane intersection
	float denom = ctrl.fwdX * nx + ctrl.fwdY * ny + ctrl.fwdZ * nz;
	if (fabsf(denom) < 1e-6f) return result;

	float dx = overlay.posX - ctrl.posX;
	float dy = overlay.posY - ctrl.posY;
	float dz = overlay.posZ - ctrl.posZ;

	float t = (dx * nx + dy * ny + dz * nz) / denom;
	if (t < 0.0f) return result;

	// Hit point
	result.hitX = ctrl.posX + t * ctrl.fwdX;
	result.hitY = ctrl.posY + t * ctrl.fwdY;
	result.hitZ = ctrl.posZ + t * ctrl.fwdZ;

	// Project into overlay local space
	float lx = (result.hitX - overlay.posX) * rx + (result.hitY - overlay.posY) * ry + (result.hitZ - overlay.posZ) * rz;
	float ly = (result.hitX - overlay.posX) * ux + (result.hitY - overlay.posY) * uy + (result.hitZ - overlay.posZ) * uz;

	// Overlay dimensions
	float width = overlay.widthMeters;
	float aspectRatio = (overlay.texWidth > 0 && overlay.texHeight > 0)
		? (float)overlay.texHeight / (float)overlay.texWidth
		: 1.0f;
	float height = width * aspectRatio;

	// UV coordinates: u = left→right [0,1], v = top→bottom [0,1]
	float u = (lx / width) + 0.5f;
	float v = 0.5f - (ly / height);

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return result;

	result.hit = true;
	result.u = u;
	result.v = v;
	result.distance = t;
	return result;
}

// ============================================================================
// Section 11: Input injection
// ============================================================================

// Forward declaration — defined in Section 14c (after WndProc section)
static void CheckTextInputFocusAsync(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	uint64_t expectedViewId);

// Capture exactly one scroll container under the laser. The reference lives in
// the target view's JS window, so nested scroll panes do not bubble a generic
// wheel event into their parents and separately positioned panels stay isolated.
static void CaptureScrollTargetAsync(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	uint64_t viewId, int hand, int pixelX, int pixelY, uint64_t generation)
{
	if (hand < 0 || hand > 1)
		return;
	if (!view || !view->ultralightView) {
		if (g_pointerInteractionGeneration[hand].load() == generation) {
			g_interactiveDragActive[hand].store(true); // unknown: fail closed
			g_scrollTargetCaptured[hand].store(false);
			g_scrollTargetViewId[hand].store(0);
			g_pointerProbePending[hand].store(false);
		}
		return;
	}
	PrismaUI::Core::ultralightThread.submit([view, viewId, hand, pixelX, pixelY, generation]() {
		if (!view->ultralightView || g_pointerInteractionGeneration[hand].load() != generation) {
			if (g_pointerInteractionGeneration[hand].load() == generation) {
				g_interactiveDragActive[hand].store(true);
				g_scrollTargetCaptured[hand].store(false);
				g_scrollTargetViewId[hand].store(0);
				g_pointerProbePending[hand].store(false);
			}
			return;
		}
		const std::string script =
			"(function(){"
			"var k='__prismaVrScrollTargets';if(!window[k])window[k]=[null,null];window[k][" +
			std::to_string(hand) + "]=null;"
			"var x=" + std::to_string(pixelX) + ",y=" + std::to_string(pixelY) + ";"
			"function directDrag(n,s){var t=n.tagName||'',role=((n.getAttribute&&n.getAttribute('role'))||'').toLowerCase();"
			"if((t==='INPUT'&&((n.getAttribute('type')||'').toLowerCase()==='range'))||role==='slider'||role==='scrollbar')return true;"
			"if(n.getAttribute&&n.getAttribute('draggable')==='true')return true;"
			"return s.cursor==='grab'||s.cursor==='grabbing'||s.cursor==='move'||s.cursor==='col-resize'||"
			"s.cursor==='row-resize'||s.cursor==='ew-resize'||s.cursor==='ns-resize';}"
			"function control(n,s){var t=n.tagName||'';"
			"if(/^(BUTTON|INPUT|SELECT|TEXTAREA|OPTION|LABEL|SUMMARY|AUDIO|VIDEO|IFRAME)$/.test(t)||"
			"(t==='A'&&n.getAttribute('href')!==null)||n.isContentEditable)return true;"
			"var role=((n.getAttribute&&n.getAttribute('role'))||'').toLowerCase();"
			"if(/^(button|slider|scrollbar|textbox|searchbox|combobox|checkbox|radio|link|spinbutton|switch|tab|option|listbox|treeitem|gridcell|menuitem|menuitemcheckbox|menuitemradio)$/.test(role))return true;"
			"if((n.hasAttribute&&n.hasAttribute('onclick'))||typeof n.onclick==='function')return true;"
			"return directDrag(n,s)||s.cursor==='pointer';}"
			"function gutter(n,s,sx,sy){if(!n.getBoundingClientRect||!n.offsetWidth||!n.offsetHeight)return false;"
			"var r=n.getBoundingClientRect(),kx=r.width/n.offsetWidth,ky=r.height/n.offsetHeight;"
			"if(!isFinite(kx)||!isFinite(ky)||kx<=0||ky<=0)return false;"
			"var lx=(x-r.left)/kx,ly=(y-r.top)/ky,cl=n.clientLeft,ct=n.clientTop,"
			"cr=cl+n.clientWidth,cb=ct+n.clientHeight;"
			"return (sy&&ly>=ct&&ly<cb&&(lx<cl||lx>=cr))||"
			"(sx&&lx>=cl&&lx<cr&&(ly<ct||ly>=cb))||"
			"(sx&&sy&&(lx<cl||lx>=cr)&&(ly<ct||ly>=cb));}"
			"var e=document.elementFromPoint(x,y);if(!e)return '1';"
			"var n=e,interactive=false,drag=false;while(n){"
			"var s=window.getComputedStyle(n);"
			"interactive=interactive||control(n,s);drag=drag||directDrag(n,s);"
			"var oy=s.overflowY,ox=s.overflowX;"
			"var sy=(oy==='scroll'||oy==='auto'||oy==='overlay')&&n.scrollHeight>n.clientHeight+1;"
			"var sx=(ox==='scroll'||ox==='auto'||ox==='overlay')&&n.scrollWidth>n.clientWidth+1;"
			"if(sx||sy){if(gutter(n,s,sx,sy)||drag)return '1';window[k][" +
			std::to_string(hand) + "]=n;return '2';}"
			"if(n===document.body||n===document.documentElement)break;n=n.parentElement;"
			"}"
			"var r=document.scrollingElement||document.documentElement;"
			"if(r&&(r.scrollHeight>r.clientHeight+1||r.scrollWidth>r.clientWidth+1)){"
			"var rs=window.getComputedStyle(r),rx=r.scrollWidth>r.clientWidth+1,ry=r.scrollHeight>r.clientHeight+1;"
			"if(gutter(r,rs,rx,ry)||drag)return '1';window[k][" + std::to_string(hand) + "]=r;return '2';}"
			"return interactive?'1':'0';"
			"})()";
		ultralight::String result = view->ultralightView->EvaluateScript(
			ultralight::String(script.c_str()), nullptr, ultralight::String(""));
		bool interactive = true; // An empty/error result must never turn a control into a panel grab.
		bool capturedScrollTarget = false;
		if (!result.empty()) {
			const std::string text(result.utf8().data(), result.utf8().length());
			interactive = text != "0" && text != "2";
			capturedScrollTarget = text == "2";
		}
		if (g_pointerInteractionGeneration[hand].load() == generation) {
			g_interactiveDragActive[hand].store(interactive);
			g_scrollTargetCaptured[hand].store(capturedScrollTarget);
			g_scrollTargetViewId[hand].store(capturedScrollTarget ? viewId : 0);
			g_pointerProbePending[hand].store(false);
		}
	});
}

static void ScrollCapturedTarget(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	int hand, int deltaX, int deltaY)
{
	if (!view || (deltaX == 0 && deltaY == 0) || hand < 0 || hand > 1)
		return;
	PrismaVR_Bridge::RunJavaScript(view,
		"(function(){var a=window.__prismaVrScrollTargets,t=a&&a[" + std::to_string(hand) + "];"
		"if(!t||!(t===document.scrollingElement||document.documentElement.contains(t))){"
		"if(a)a[" + std::to_string(hand) + "]=null;return;}"
		"if(" + std::to_string(deltaX) + ")t.scrollLeft+=" + std::to_string(deltaX) + ";"
		"if(" + std::to_string(deltaY) + ")t.scrollTop+=" + std::to_string(deltaY) + ";})()");
}

static void ScrollCapturedTargetForHand(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	int hand)
{
	if (!view || hand < 0 || hand > 1)
		return;
	auto& controller = g_controllers[hand];
	const auto now = std::chrono::steady_clock::now();
	float deltaSeconds = 0.0f;
	if (controller.scrollClockInitialized) {
		deltaSeconds = std::chrono::duration<float>(now - controller.lastScrollTime).count();
		deltaSeconds = PrismaUI::VRInput::Clamp(deltaSeconds, 0.0f, 0.05f);
	}
	controller.lastScrollTime = now;
	controller.scrollClockInitialized = true;

	const int deltaX = PrismaUI::VRInput::AccumulateScrollDelta(
		controller.thumbstickX, deltaSeconds, g_vrInputConfig.thumbstickDeadzone,
		g_vrInputConfig.scrollPixelsPerSecond, controller.scrollRemainderX);
	const int deltaY = PrismaUI::VRInput::AccumulateScrollDelta(
		-controller.thumbstickY, deltaSeconds, g_vrInputConfig.thumbstickDeadzone,
		g_vrInputConfig.scrollPixelsPerSecond, controller.scrollRemainderY);
	ScrollCapturedTarget(view, hand, deltaX, deltaY);
}

// ProcessInput — Per-frame input processing: raycasting, cursor dots, mouse events.
//
// This is the core interaction loop. For each hand (left=0, right=1):
//
//   1. RAYCAST: Fire a ray from the controller through all visible Prisma overlays.
//      Pick the closest hit (RayOverlayIntersection). Update g_hitInfo[hand].
//
//   2. CSS CURSOR DOT: Inject/update a fixed-position <div> on the panel's HTML
//      at the hit pixel coordinates. One dot per hand (_pvrc0, _pvrc1). Both
//      hands always show dots. Dots are hidden when the laser leaves the panel.
//
//   3. MOUSE EVENTS (active hand only): Only ONE hand drives Ultralight's mouse
//      cursor at a time (g_mouseActiveHand). This prevents two cursors fighting
//      over one DOM. The active hand switches when the OTHER hand clicks a panel.
//      - Trigger press → mouseDown + focus + interactive drag check
//      - Trigger held half a second in a scroll pane → same-hand stick captures it
//      - Trigger held on ordinary panel chrome → enter panel grab mode
//      - Trigger release → mouseUp (completes click or ends interactive drag)
//      - Every frame → mouseMove (hover tracking for CSS :hover, tooltips, etc.)
//
//   4. HOLD ROUTING: A captured scroll pane wins over panel movement. Its exact
//      DOM node keeps the same controller's stick until trigger release. Outside
//      scrollable content, the existing panel-grab behavior remains available.
static void ProcessInput()
{
	auto visibleViews = PrismaVR_Bridge::GetVisibleViews();
	auto findVisibleView = [&visibleViews](uint64_t viewId) -> std::shared_ptr<PrismaUI::Core::PrismaView> {
		for (const auto& view : visibleViews) {
			if (view.id == viewId)
				return view.view;
		}
		return {};
	};
	auto finishPointerInteraction = [&findVisibleView](int hand, bool cancel, bool checkTextFocus) {
		auto& controller = g_controllers[hand];
		const bool hasState = controller.mouseDownActive || controller.pressedViewId != 0 ||
		                      g_pointerProbePending[hand].load() ||
		                      g_interactiveDragActive[hand].load() ||
		                      g_scrollTargetCaptured[hand].load() ||
		                      controller.scrollCaptureActive;
		if (!hasState)
			return;
		auto pressedView = findVisibleView(controller.pressedViewId);
		auto scrollView = findVisibleView(g_scrollTargetViewId[hand].load());
		if (controller.mouseDownActive && pressedView) {
			const int releaseX = cancel ? -1 : controller.pressedPixelX;
			const int releaseY = cancel ? -1 : controller.pressedPixelY;
			if (cancel) {
				ultralight::MouseEvent cancelMove;
				cancelMove.type = ultralight::MouseEvent::kType_MouseMoved;
				cancelMove.x = releaseX;
				cancelMove.y = releaseY;
				cancelMove.button = ultralight::MouseEvent::kButton_None;
				PrismaVR_Bridge::FireMouseEvent(pressedView, cancelMove);
			}
			ultralight::MouseEvent upEvent;
			upEvent.type = ultralight::MouseEvent::kType_MouseUp;
			upEvent.x = releaseX;
			upEvent.y = releaseY;
			upEvent.button = ultralight::MouseEvent::kButton_Left;
			PrismaVR_Bridge::FireMouseEvent(pressedView, upEvent);
			if (checkTextFocus)
				CheckTextInputFocusAsync(pressedView, controller.pressedViewId);
		}
		g_pointerInteractionGeneration[hand].fetch_add(1);
		g_pointerProbePending[hand].store(false);
		g_interactiveDragActive[hand].store(false);
		ClearCapturedScrollTarget(scrollView ? scrollView : pressedView, hand);
		controller.mouseDownActive = false;
		controller.pressedViewId = 0;
		controller.scrollCaptureActive = false;
		controller.scrollClockInitialized = false;
		controller.scrollRemainderX = 0.0f;
		controller.scrollRemainderY = 0.0f;
	};

	for (int hand = 0; hand < 2; hand++) {
		auto& ctrl = g_controllers[hand];
		if (!ctrl.valid) {
			finishPointerInteraction(hand, true, false);
			g_hitInfo[hand].hitting = false;
			if (g_lastHitViewId[hand] != 0) {
				auto oldView = findVisibleView(g_lastHitViewId[hand]);
				if (oldView) {
					const std::string dotId = "_pvrc" + std::to_string(hand);
					PrismaVR_Bridge::RunJavaScript(oldView,
						"var c=document.getElementById('" + dotId + "');if(c)c.style.display='none'");
				}
				g_lastHitViewId[hand] = 0;
				g_lastCursorX[hand] = -1;
				g_lastCursorY[hand] = -1;
				g_cursorDotCreated[hand] = false;
			}
			continue;
		}

		// The DOM probe captured the pane at trigger-down, not wherever the laser
		// happens to drift later. After one deliberate half-second, cancel the pending
		// click and give this hand's own stick exclusively to that pane.
		if (!ctrl.scrollCaptureActive && ctrl.triggerPressed && ctrl.triggerWasPressed &&
		    ctrl.mouseDownActive && ctrl.pressedViewId != 0) {
			const float heldSeconds = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - ctrl.triggerPressTime).count();
			const bool targetMatchesPress = g_scrollTargetCaptured[hand].load() &&
				g_scrollTargetViewId[hand].load() == ctrl.pressedViewId;
			if (PrismaUI::VRInput::ShouldActivateScrollCapture(
			        heldSeconds, targetMatchesPress, g_pointerProbePending[hand].load(),
			        g_interactiveDragActive[hand].load(), SCROLL_CAPTURE_HOLD_THRESHOLD)) {
				auto pressedView = findVisibleView(ctrl.pressedViewId);
				if (pressedView) {
					QueueCancelledMouseRelease(pressedView, ctrl);
					g_pointerInteractionGeneration[hand].fetch_add(1);
					g_pointerProbePending[hand].store(false);
					g_interactiveDragActive[hand].store(false);
					ctrl.mouseDownActive = false;
					ctrl.pressedViewId = 0;
					ctrl.scrollCaptureActive = true;
					ctrl.scrollClockInitialized = false;
					ctrl.scrollRemainderX = 0.0f;
					ctrl.scrollRemainderY = 0.0f;
					logger::info("PrismaVR: hand {} captured scroll pane after {:.2f}s hold",
					             hand, heldSeconds);
				}
			}
		}

		if (ctrl.scrollCaptureActive) {
			const uint64_t targetViewId = g_scrollTargetViewId[hand].load();
			auto targetView = findVisibleView(targetViewId);
			if (!ctrl.triggerPressed || !targetView || !g_scrollTargetCaptured[hand].load()) {
				finishPointerInteraction(hand, true, false);
			} else {
				ScrollCapturedTargetForHand(targetView, hand);
			}
			continue;
		}

		// Find closest overlay hit
		RayHit bestHit;
		uint64_t bestViewId = 0;
		std::shared_ptr<PrismaUI::Core::PrismaView> bestViewPtr;

		for (auto& viewInfo : visibleViews) {
			auto it = g_vrOverlays.find(viewInfo.id);
			if (it == g_vrOverlays.end()) continue;

			RayHit hit = RayOverlayIntersection(ctrl, it->second);
			if (hit.hit && (!bestHit.hit || hit.distance < bestHit.distance)) {
				bestHit = hit;
				bestViewId = viewInfo.id;
				bestViewPtr = viewInfo.view;
			}
		}

		auto& hitState = g_hitInfo[hand];

		if (bestHit.hit && bestViewPtr) {
			hitState.hitting = true;
			hitState.hitViewId = bestViewId;
			hitState.hitU = bestHit.u;
			hitState.hitV = bestHit.v;
			hitState.hitDistance = bestHit.distance;

			auto it = g_vrOverlays.find(bestViewId);
			if (it == g_vrOverlays.end()) continue;
			auto& ovl = it->second;

			// Convert UV to pixel coordinates
			int pixelX = (int)(bestHit.u * (float)ovl.texWidth);
			int pixelY = (int)(bestHit.v * (float)ovl.texHeight);

			// Clamp to texture bounds
			if (pixelX < 0) pixelX = 0;
			if (pixelY < 0) pixelY = 0;
			if (ovl.texWidth > 0 && pixelX >= (int)ovl.texWidth) pixelX = (int)ovl.texWidth - 1;
			if (ovl.texHeight > 0 && pixelY >= (int)ovl.texHeight) pixelY = (int)ovl.texHeight - 1;

			// --- CSS cursor dot: one per hand, created once per view ---
			// One dot per hand (_pvrc0 = left, _pvrc1 = right), both warm white.
			// Only inject the creation JS if we haven't created the dot yet for this view.
			// If the DOM wasn't ready (page navigation), g_cursorDotCreated resets on view switch.
			std::string dotId = "_pvrc" + std::to_string(hand);
			if (!g_cursorDotCreated[hand] || g_lastHitViewId[hand] != bestViewId) {
				const char* bg = "background:rgba(255,250,240,0.95);box-shadow:0 0 8px 3px rgba(255,250,240,0.6),0 0 16px 6px rgba(255,200,150,0.3);";
				PrismaVR_Bridge::RunJavaScript(bestViewPtr,
					"(function(){"
					"if(document.getElementById('" + dotId + "'))return;"
					"if(!document.body)return;"
					"var d=document.createElement('div');"
					"d.id='" + dotId + "';"
					"d.style.cssText='position:fixed;width:" + std::to_string(CURSOR_DOT_SIZE_PX) + "px;height:" + std::to_string(CURSOR_DOT_SIZE_PX) + "px;border-radius:50%;"
					+ std::string(bg) +
					"pointer-events:none;z-index:" + std::to_string(CURSOR_DOT_Z_INDEX) + ";"
					"transform:translate(-50%,-50%);display:none;left:0;top:0;';"
					"document.body.appendChild(d);"
					"})()");
				g_cursorDotCreated[hand] = true;
				g_lastDotClicked[hand] = false; // fresh dot styles warm white
			}

			// Move this hand's dot to hit coordinates — only if position actually changed
			if (pixelX != g_lastCursorX[hand] || pixelY != g_lastCursorY[hand]) {
				PrismaVR_Bridge::RunJavaScript(bestViewPtr,
					"var c=document.getElementById('" + dotId + "');"
					"if(c){c.style.left='" + std::to_string(pixelX) + "px';"
					"c.style.top='" + std::to_string(pixelY) + "px';"
					"c.style.display='block'}");
				g_lastCursorX[hand] = pixelX;
				g_lastCursorY[hand] = pixelY;
			}

			// OCU-style click feedback on the dot: electric blue while the
			// trigger is held, warm white otherwise. Matches the beam swap.
			if (ctrl.triggerPressed != g_lastDotClicked[hand]) {
				const char* dotStyle = ctrl.triggerPressed
					? "c.style.background='rgba(55,145,255,0.95)';"
					  "c.style.boxShadow='0 0 8px 3px rgba(55,145,255,0.6),0 0 16px 6px rgba(90,170,255,0.35)';"
					: "c.style.background='rgba(255,250,240,0.95)';"
					  "c.style.boxShadow='0 0 8px 3px rgba(255,250,240,0.6),0 0 16px 6px rgba(255,200,150,0.3)';";
				PrismaVR_Bridge::RunJavaScript(bestViewPtr,
					"var c=document.getElementById('" + dotId + "');"
					"if(c){" + std::string(dotStyle) + "}");
				g_lastDotClicked[hand] = ctrl.triggerPressed;
			}

			// If we switched views, hide this hand's dot on the old one
			if (g_lastHitViewId[hand] != 0 && g_lastHitViewId[hand] != bestViewId) {
				std::string oldDotId = "_pvrc" + std::to_string(hand);
				for (auto& vi : visibleViews) {
					if (vi.id == g_lastHitViewId[hand]) {
						PrismaVR_Bridge::RunJavaScript(vi.view,
							"var c=document.getElementById('" + oldDotId + "');if(c)c.style.display='none'");
						break;
					}
				}
			}
			g_lastHitViewId[hand] = bestViewId;

			// --- While grabbed or resizing, skip mouse/click events ---
			if (ovl.isGrabbed || ovl.isResizing) {
				// Don't send mouse events while manipulating the overlay. A second
				// hand may still own a press from another panel; cancel it so resize
				// cannot swallow its MouseUp edge and leave the DOM latched.
				if (ovl.grabbedByController != hand)
					finishPointerInteraction(hand, true, false);
			} else {
				// --- Active hand: only one controller drives the Ultralight cursor ---
				// Trigger press on EITHER hand activates that hand for mouse interaction.
				// Both hands show cursor dots, both can grab, but only active hand sends
				// hover/click events to prevent two cursors fighting over one DOM.
				bool isMouseHand = (g_mouseActiveHand == hand);

				// --- Trigger press: activate this hand + start click ---
				if (ctrl.triggerPressed && !ctrl.triggerWasPressed) {
					finishPointerInteraction(hand, true, false);
					g_mouseActiveHand = hand;
					isMouseHand = true;

					ctrl.triggerPressTime = std::chrono::steady_clock::now();
					g_interactiveDragActive[hand].store(false); // Reset — will be set async if needed
					g_scrollTargetCaptured[hand].store(false);
					g_scrollTargetViewId[hand].store(0);
					g_pointerProbePending[hand].store(true);
					const uint64_t interactionGeneration =
						g_pointerInteractionGeneration[hand].fetch_add(1) + 1;

					// Tell Ultralight this view has keyboard focus BEFORE the click,
					// so the DOM processes mouseDown with focus already set (required
					// for <input> clicks to set document.activeElement).
					{
						std::lock_guard lock(g_keyboardFocusStateMutex);
						g_keyboardTargetViewId.store(bestViewId);
						g_keyboardFocusGeneration.fetch_add(1);
					}
					PrismaVR_Bridge::FocusView(bestViewPtr);

					ultralight::MouseEvent downEv;
					downEv.type = ultralight::MouseEvent::kType_MouseDown;
					downEv.x = pixelX;
					downEv.y = pixelY;
					downEv.button = ultralight::MouseEvent::kButton_Left;
					PrismaVR_Bridge::FireMouseEvent(bestViewPtr, downEv);
					ctrl.pressedViewId = bestViewId;
					ctrl.pressedPixelX = pixelX;
					ctrl.pressedPixelY = pixelY;
					ctrl.mouseDownActive = true;

					// Direct-drag controls keep native mouse drag. A selectable row inside
					// a scroll pane still captures that exact pane for a deliberate hold.
					CaptureScrollTargetAsync(bestViewPtr, bestViewId, hand, pixelX, pixelY,
					                         interactionGeneration);
				}

				// --- Mouse move event (only active hand, every frame while pointing) ---
				if (isMouseHand) {
					ultralight::MouseEvent moveEv;
					moveEv.type = ultralight::MouseEvent::kType_MouseMoved;
					moveEv.x = pixelX;
					moveEv.y = pixelY;
					moveEv.button = ultralight::MouseEvent::kButton_None;
					PrismaVR_Bridge::FireMouseEvent(bestViewPtr, moveEv);
					if (ctrl.mouseDownActive && ctrl.pressedViewId == bestViewId) {
						ctrl.pressedPixelX = pixelX;
						ctrl.pressedPixelY = pixelY;
					}
				}

				// --- Trigger held: ordinary panel chrome may enter panel-grab mode ---
				// Scroll panes are handled above and never move the panel.
				if (ctrl.triggerPressed && ctrl.triggerWasPressed && ctrl.mouseDownActive &&
				    ctrl.pressedViewId == bestViewId &&
				    !g_pointerProbePending[hand].load() && !g_interactiveDragActive[hand].load() &&
				    !g_scrollTargetCaptured[hand].load()) {
					auto elapsed = std::chrono::duration<float>(
						std::chrono::steady_clock::now() - ctrl.triggerPressTime).count();

					if (elapsed >= PANEL_GRAB_HOLD_THRESHOLD) {
						// Cancel the pending DOM click before moving the panel.
						auto pressedView = findVisibleView(ctrl.pressedViewId);
						if (ctrl.mouseDownActive && pressedView) {
							ultralight::MouseEvent cancelMove;
							cancelMove.type = ultralight::MouseEvent::kType_MouseMoved;
							cancelMove.x = -1;
							cancelMove.y = -1;
							cancelMove.button = ultralight::MouseEvent::kButton_None;
							PrismaVR_Bridge::FireMouseEvent(pressedView, cancelMove);
							ultralight::MouseEvent upEv;
							upEv.type = ultralight::MouseEvent::kType_MouseUp;
							upEv.x = -1;
							upEv.y = -1;
							upEv.button = ultralight::MouseEvent::kButton_Left;
							PrismaVR_Bridge::FireMouseEvent(pressedView, upEv);
						}
						ctrl.mouseDownActive = false;
						ctrl.pressedViewId = 0;

						ovl.isGrabbed = true;
						ovl.grabbedByController = hand;
						const auto anchor = PrismaUI::VRInput::CaptureGrabAnchor(
							{ovl.posX, ovl.posY, ovl.posZ},
							{ctrl.posX, ctrl.posY, ctrl.posZ},
							{ctrl.fwdX, ctrl.fwdY, ctrl.fwdZ},
							bestHit.distance);
						ovl.grabDistance = anchor.distance;
						ovl.grabOffX = anchor.offset.x;
						ovl.grabOffY = anchor.offset.y;
						ovl.grabOffZ = anchor.offset.z;
					}
				}

				// --- Trigger release: complete a click or direct HTML drag ---
				if (!ctrl.triggerPressed && ctrl.triggerWasPressed) {
					const bool releasedOnPressedView = ctrl.pressedViewId == bestViewId;
					finishPointerInteraction(hand, !releasedOnPressedView, releasedOnPressedView);
				}
			}

			// Outside an active half-second scroll capture, the stick keeps its normal
			// game behavior. A quick trigger tap remains a normal DOM selection.

			// Grip button is NOT used for Prisma UI — it stays free for game interaction.
			// Prisma menus are controlled by hotkeys.

		} else {
			// Laser left the view — hide this hand's CSS dot on whatever we were hitting
			if (g_lastHitViewId[hand] != 0) {
				std::string dotId = "_pvrc" + std::to_string(hand);
				for (auto& vi : visibleViews) {
					if (vi.id == g_lastHitViewId[hand]) {
						PrismaVR_Bridge::RunJavaScript(vi.view,
							"var c=document.getElementById('" + dotId + "');if(c)c.style.display='none'");
						break;
					}
				}
				g_lastHitViewId[hand] = 0;
				g_lastCursorX[hand] = -1;
				g_lastCursorY[hand] = -1;
				g_cursorDotCreated[hand] = false;
			}
			if (!ctrl.triggerPressed && ctrl.triggerWasPressed)
				finishPointerInteraction(hand, true, false);
			hitState.hitting = false;
		}

		// If this hand is grabbing OR resizing a panel, force hit state to stay active.
		// The raycast can miss during grab/resize because the panel hasn't been repositioned
		// yet this frame (ProcessGrabMoveResize runs after ProcessInput).
		for (auto& [id, ovl] : g_vrOverlays) {
			if (ovl.isGrabbed && (ovl.grabbedByController == hand || ovl.isResizing)) {
				hitState.hitting = true;
				hitState.hitViewId = id;
				break;
			}
		}
	}
}

// ============================================================================
// Section 12: Grab, Move & Resize
// ============================================================================

static float ControllerDistance(const ControllerInfo& a, const ControllerInfo& b)
{
	float dx = a.posX - b.posX;
	float dy = a.posY - b.posY;
	float dz = a.posZ - b.posZ;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ProcessGrabMoveResize — Handles panel movement and resizing.
//
// Grab mode is entered by ProcessInput() when ordinary panel chrome is held.
// Once grabbed, the overlay follows the controller's laser ray at a locked distance.
//
// Two-hand resize:
//   While the primary hand holds grab, the second hand's trigger enters resize mode.
//   Panel width scales proportionally to the change in distance between controllers
//   (pinch-to-zoom). Panel position moves to the midpoint between both hands.
//   Both thumbsticks can push/pull the panel depth during resize.
//
// Single-hand grab:
//   Panel follows the aiming ray while preserving the exact point where it was
//   grabbed. The other hand's Y axis adjusts panel depth. Scroll panes never
//   enter this path; they use dedicated same-hand capture in ProcessInput().
//
// On trigger release, the panel world-locks at its current position.
static void ProcessGrabMoveResize()
{
	const auto visibleViews = PrismaVR_Bridge::GetVisibleViews();
	auto findView = [&visibleViews](uint64_t viewId) -> std::shared_ptr<PrismaUI::Core::PrismaView> {
		for (const auto& view : visibleViews) {
			if (view.id == viewId)
				return view.view;
		}
		return {};
	};

	for (auto& [viewId, ovl] : g_vrOverlays) {
		if (!ovl.isGrabbed || ovl.grabbedByController < 0 || ovl.grabbedByController > 1)
			continue;

		int grabHand = ovl.grabbedByController;
		int otherHand = 1 - grabHand;
		auto& ctrl = g_controllers[grabHand];
		auto& otherCtrl = g_controllers[otherHand];

		if (!ctrl.valid) {
			g_pointerInteractionGeneration[grabHand].fetch_add(1);
			g_pointerProbePending[grabHand].store(false);
			g_interactiveDragActive[grabHand].store(false);
			ClearCapturedScrollTarget(findView(viewId), grabHand);
			ovl.isGrabbed = false;
			ovl.isResizing = false;
			ovl.grabbedByController = -1;
			continue;
		}

		// --- Check for resize mode: second trigger enters resize ---
		// Only enter resize if the other hand's laser is actually on the panel.
		bool otherHandOnPanel = g_hitInfo[otherHand].hitting && g_hitInfo[otherHand].hitViewId == viewId;
		if (otherCtrl.valid && otherCtrl.triggerPressed && otherHandOnPanel) {
			if (!ovl.isResizing) {
				// Second trigger just pressed — enter resize mode
				ovl.isResizing = true;
				ovl.resizeBaseDistance = ControllerDistance(ctrl, otherCtrl);
				ovl.resizeBaseWidth = ovl.widthMeters;
				if (ovl.resizeBaseDistance < MIN_RESIZE_DISTANCE)
					ovl.resizeBaseDistance = MIN_RESIZE_DISTANCE;
			}

			// Resize: scale proportional to controller distance change
			float currentDist = ControllerDistance(ctrl, otherCtrl);
			float ratio = currentDist / ovl.resizeBaseDistance;
			ovl.widthMeters = ovl.resizeBaseWidth * ratio;

			if (ovl.widthMeters < MIN_OVERLAY_WIDTH) ovl.widthMeters = MIN_OVERLAY_WIDTH;
			if (ovl.widthMeters > MAX_OVERLAY_WIDTH) ovl.widthMeters = MAX_OVERLAY_WIDTH;

			VCallOvl(g_overlay, OVL_SLOT::SetOverlayWidthInMeters, "SetOverlayWidthInMeters(resize)",
				ovl.handle, ovl.widthMeters);

			// While resizing, overlay position = midpoint between controllers
			ovl.posX = (ctrl.posX + otherCtrl.posX) * 0.5f;
			ovl.posY = (ctrl.posY + otherCtrl.posY) * 0.5f;
			ovl.posZ = (ctrl.posZ + otherCtrl.posZ) * 0.5f;

			// Keep the existing resize push/pull control.
			float stickY = (fabsf(ctrl.thumbstickY) > fabsf(otherCtrl.thumbstickY))
				? ctrl.thumbstickY : otherCtrl.thumbstickY;
			if (fabsf(stickY) > MANIPULATION_THUMBSTICK_DEADZONE) {
				float speed = stickY * RESIZE_PUSH_PULL_SPEED;
				ovl.posX += ovl.fwdX * speed;
				ovl.posY += ovl.fwdY * speed;
				ovl.posZ += ovl.fwdZ * speed;
			}

			// Keep the primary ray anchor current throughout resize. Exiting resize
			// can then continue from this exact center without a one-frame jump.
			const auto resizedAnchor = PrismaUI::VRInput::ReanchorGrabAtCurrentPosition(
				{ovl.posX, ovl.posY, ovl.posZ},
				{ctrl.posX, ctrl.posY, ctrl.posZ},
				{ctrl.fwdX, ctrl.fwdY, ctrl.fwdZ},
				MIN_GRAB_DISTANCE, MAX_GRAB_DISTANCE);
			ovl.grabDistance = resizedAnchor.distance;
			ovl.grabOffX = resizedAnchor.offset.x;
			ovl.grabOffY = resizedAnchor.offset.y;
			ovl.grabOffZ = resizedAnchor.offset.z;

		} else {
			// Second trigger released — exit resize mode
			if (ovl.isResizing) {
				ovl.isResizing = false;
				// Re-anchor at the current resized position. This makes the first
				// single-hand frame continuous instead of snapping back to the ray.
				const auto anchor = PrismaUI::VRInput::ReanchorGrabAtCurrentPosition(
					{ovl.posX, ovl.posY, ovl.posZ},
					{ctrl.posX, ctrl.posY, ctrl.posZ},
					{ctrl.fwdX, ctrl.fwdY, ctrl.fwdZ},
					MIN_GRAB_DISTANCE, MAX_GRAB_DISTANCE);
				ovl.grabDistance = anchor.distance;
				ovl.grabOffX = anchor.offset.x;
				ovl.grabOffY = anchor.offset.y;
				ovl.grabOffZ = anchor.offset.z;
			}

			// The non-grabbing hand keeps panel push/pull behavior.
			if (otherCtrl.valid && fabsf(otherCtrl.thumbstickY) > MANIPULATION_THUMBSTICK_DEADZONE) {
				ovl.grabDistance += otherCtrl.thumbstickY * DEPTH_ADJUST_SPEED;
				if (ovl.grabDistance < MIN_GRAB_DISTANCE) ovl.grabDistance = MIN_GRAB_DISTANCE;
				if (ovl.grabDistance > MAX_GRAB_DISTANCE) ovl.grabDistance = MAX_GRAB_DISTANCE;
			}

			const auto panelCenter = PrismaUI::VRInput::ResolveGrabbedPanelCenter(
				{ctrl.posX, ctrl.posY, ctrl.posZ},
				{ctrl.fwdX, ctrl.fwdY, ctrl.fwdZ},
				{ovl.grabDistance, {ovl.grabOffX, ovl.grabOffY, ovl.grabOffZ}});
			ovl.posX = panelCenter.x;
			ovl.posY = panelCenter.y;
			ovl.posZ = panelCenter.z;
		}

		// --- Grab release: primary trigger released ---
		if (!ctrl.triggerPressed) {
			g_pointerInteractionGeneration[grabHand].fetch_add(1);
			g_pointerProbePending[grabHand].store(false);
			g_interactiveDragActive[grabHand].store(false);
			ClearCapturedScrollTarget(findView(viewId), grabHand);
			ovl.isGrabbed = false;
			ovl.isResizing = false;
			ovl.grabbedByController = -1;
			ovl.worldLocked = true; // Stay in place after grab
		}

		// Update overlay transform
		openvr::HmdMatrix34_t transform;
		BuildOverlayTransform(ovl, transform);
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute(grab)",
			ovl.handle, (int)openvr::TrackingUniverseStanding, &transform);
	}
}

// ============================================================================
// Section 12b: Face all world-locked overlays toward the player
// ============================================================================

static void FaceOverlaysToPlayer()
{
	float hx, hy, hz, hfx, hfy, hfz;
	GetHeadPose(hx, hy, hz, hfx, hfy, hfz);

	for (auto& [viewId, ovl] : g_vrOverlays) {
		// Non-locked overlays are handled by UpdateHeadRelativeOverlays
		// Grabbed overlays shouldn't rotate while being held
		if (!ovl.worldLocked || ovl.isGrabbed) continue;

		float dx = hx - ovl.posX;
		float dz = hz - ovl.posZ;
		float fl = sqrtf(dx * dx + dz * dz);
		if (fl < 1e-6f) continue;

		ovl.fwdX = dx / fl;
		ovl.fwdY = 0.0f;
		ovl.fwdZ = dz / fl;

		openvr::HmdMatrix34_t transform;
		BuildOverlayTransform(ovl, transform);
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute(face)",
			ovl.handle, (int)openvr::TrackingUniverseStanding, &transform);
	}
}

// ============================================================================
// Section 12c: Laser beam positioning & visibility
// Lasers only appear when a controller ray hits an open Prisma UI menu.
// ============================================================================

// UpdateLasers — Positions and shows/hides the laser beam overlays each frame.
//
// Each hand gets a thin beam overlay (1px wide texture stretched to 3mm) that
// extends from the controller toward the panel. The beam:
//   - Only appears when the controller's ray is hitting an open Prisma panel
//   - Hides during grab mode (beam would clip through the panel being held)
//   - Stops 2cm before the panel surface (LASER_SURFACE_GAP) to avoid clipping
//   - Billboards toward the player's head so it's always visible edge-on
//
// The cursor dot is NOT drawn here — it's a CSS element injected into the panel's
// HTML by ProcessInput(). This function only handles the 3D beam geometry.
static void UpdateLasers()
{
	if (!g_lasersCreated || !g_overlay) return;

	float hx, hy, hz, hfx, hfy, hfz;
	GetHeadPose(hx, hy, hz, hfx, hfy, hfz);

	for (int hand = 0; hand < 2; hand++) {
		auto& ctrl = g_controllers[hand];
		auto& hit = g_hitInfo[hand];

		// Show beam when laser is hitting a panel (including during grab)
		if (!ctrl.valid || !hit.hitting) {
			if (g_laserBeamHandle[hand])
				VCallOvl(g_overlay, OVL_SLOT::HideOverlay, "HideOverlay(laser)", g_laserBeamHandle[hand]);
			continue;
		}

		// Beam length: cap at 1 foot, shorter if menu is closer
		// Stop 2cm before the panel surface so the beam doesn't clip into/behind it
		float maxLen = hit.hitDistance - LASER_SURFACE_GAP;
		if (maxLen < MIN_BEAM_LENGTH) maxLen = MIN_BEAM_LENGTH;
		float beamLen = (maxLen < LASER_LENGTH) ? maxLen : LASER_LENGTH;
		if (beamLen < MIN_BEAM_LENGTH) beamLen = MIN_BEAM_LENGTH;

		// Scale width proportionally to maintain aspect ratio (1x100 texture)
		float beamWidth = LASER_WIDTH * (beamLen / LASER_LENGTH);

		// Beam midpoint
		float midX = ctrl.posX + ctrl.fwdX * beamLen * 0.5f;
		float midY = ctrl.posY + ctrl.fwdY * beamLen * 0.5f;
		float midZ = ctrl.posZ + ctrl.fwdZ * beamLen * 0.5f;

		// Build beam transform: Y axis = beam direction, billboarded toward viewer
		float yX = ctrl.fwdX, yY = ctrl.fwdY, yZ = ctrl.fwdZ;

		// Z axis = toward head (billboard)
		float zX = hx - midX, zY = hy - midY, zZ = hz - midZ;
		float zLen = sqrtf(zX * zX + zY * zY + zZ * zZ);
		if (zLen > 1e-6f) { zX /= zLen; zY /= zLen; zZ /= zLen; }
		else { zX = 0; zY = 0; zZ = 1; }

		// X axis = Y cross Z
		float xX = yY * zZ - yZ * zY;
		float xY = yZ * zX - yX * zZ;
		float xZ = yX * zY - yY * zX;
		float xLen = sqrtf(xX * xX + xY * xY + xZ * xZ);
		if (xLen > 1e-6f) { xX /= xLen; xY /= xLen; xZ /= xLen; }

		// Recompute Z for orthogonality: Z = X cross Y
		zX = xY * yZ - xZ * yY;
		zY = xZ * yX - xX * yZ;
		zZ = xX * yY - xY * yX;

		openvr::HmdMatrix34_t bt;
		bt.m[0][0] = xX; bt.m[0][1] = yX; bt.m[0][2] = zX; bt.m[0][3] = midX;
		bt.m[1][0] = xY; bt.m[1][1] = yY; bt.m[1][2] = zY; bt.m[1][3] = midY;
		bt.m[2][0] = xZ; bt.m[2][1] = yZ; bt.m[2][2] = zZ; bt.m[2][3] = midZ;

		VCallOvl(g_overlay, OVL_SLOT::SetOverlayWidthInMeters, "SetOverlayWidthInMeters(beam)",
			g_laserBeamHandle[hand], beamWidth);
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute(beam)",
			g_laserBeamHandle[hand], (int)openvr::TrackingUniverseStanding, &bt);

		// OCU-style click feedback: electric blue while the trigger is held.
		// Both textures exist up front, so this is a cheap handle swap.
		bool clicked = ctrl.triggerPressed;
		if (clicked != g_laserClickApplied[hand] && g_laserBeamTex[clicked ? 1 : 0]) {
			openvr::Texture_t ct; ct.handle = g_laserBeamTex[clicked ? 1 : 0];
			ct.eType = openvr::TextureType_DirectX; ct.eColorSpace = openvr::ColorSpace_Auto;
			VCallOvl(g_overlay, OVL_SLOT::SetOverlayTexture, "SetOverlayTexture(beam-click)",
				g_laserBeamHandle[hand], &ct);
			g_laserClickApplied[hand] = clicked;
		}

		VCallOvl(g_overlay, OVL_SLOT::ShowOverlay, "ShowOverlay(beam)",
			g_laserBeamHandle[hand]);

		// Dot is rendered as CSS element ON the panel (see ProcessInput)
	}
}

// ============================================================================
// Section 13: Head-relative repositioning for non-locked overlays
// ============================================================================

static void UpdateHeadRelativeOverlays()
{
	// Check if any overlay needs head-following
	bool anyNeedsUpdate = false;
	for (auto& [viewId, ovl] : g_vrOverlays) {
		if (!ovl.worldLocked && !ovl.modderPositioned && !ovl.isGrabbed) {
			anyNeedsUpdate = true;
			break;
		}
	}
	if (!anyNeedsUpdate) return;

	// Get head pose once for all overlays that need it
	float hx, hy, hz, fx, fy, fz;
	GetHeadPose(hx, hy, hz, fx, fy, fz);

	for (auto& [viewId, ovl] : g_vrOverlays) {
		if (ovl.worldLocked || ovl.modderPositioned || ovl.isGrabbed)
			continue;

		// Reposition to stay in front of the player at same distance
		PositionNewOverlay(ovl, ovl.arcIndex);

		// Update the VR overlay transform
		openvr::HmdMatrix34_t transform;
		BuildOverlayTransform(ovl, transform);
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute(head)",
			ovl.handle, (int)openvr::TrackingUniverseStanding, &transform);
	}
}

// ============================================================================
// Section 14: Distance cleanup and load screen detection
// ============================================================================
//
// NOTE FOR VR SUPPORT (Wondernutts' note):
//
// Prisma UI's original codebase has no automatic view cleanup on cell change,
// load screens, or distance. Views persist until explicitly destroyed by the
// modder's JS code or at full plugin shutdown. This is fine for flat-screen
// users because their overlays are attached to the 2D screen in plain sight,
// and they close them manually.
//
// VR is fundamentally different. If users can grab a panel, place it in 3D world
// space, and walk away this is unlike screen-attached 2D overlays, these panels stay
// at their absolute world position. Over the course of a play session, a user
// could leave behind dozens of world-locked panels across multiple locations.
// These could register and stack up invisibly (user has long since moved on) and waste 
// both VR overlay resources and per-frame texture submission calls.
//
// Many modders WILL NOT THINK of cleanup calls and this will cause support tickets 
// if left unhandled. Users will report "performance gets worse the longer I play" 
// or "my overlays stopped working" Bad modding practices could erroneously be seen as
// a Prisma UI issue if not properly handled. VR HAS to set such limits anyway. 
//
// (OpenVR has a hard limit on active overlays). The cleanup below handles two
// scenarios:
//
//   1. Cell transition / load screen: detected via head position jumping >10m
//      in a single frame. World-locked panels revert to head-following so they
//      aren't stranded in the previous cell at coordinates that no longer exist.
//
//   2. Distance auto-destroy: world-locked panels >50m from the player get
//      their VR overlay destroyed. SyncOverlays will recreate them as
//      head-following if the underlying Prisma view is still alive.
//
// If you remove this cleanup, make sure you understand or have an alternative 
// strategy for preventing unbounded overlay accumulation in VR sessions..... 
// or a lot of time to explain to hundreds of people why it's a modder issue.
// ============================================================================

static void CleanupDistantOverlays()
{
	float hx, hy, hz, fx, fy, fz;
	GetHeadPose(hx, hy, hz, fx, fy, fz);

	// --- Teleport / load screen detection ---
	if (g_headPosInitialized) {
		float dx = hx - g_lastHeadX;
		float dy = hy - g_lastHeadY;
		float dz = hz - g_lastHeadZ;
		float moved = sqrtf(dx * dx + dy * dy + dz * dz);

		if (moved > TELEPORT_THRESHOLD) {
			// Player teleported (load screen / cell transition)
			// Reset all world-locked panels to head-following
			for (auto& [viewId, ovl] : g_vrOverlays) {
				if (ovl.worldLocked && !ovl.modderPositioned) {
					ovl.worldLocked = false;
					logger::info("PrismaVR: View {} reset to head-following after cell transition", viewId);
				}
			}
		}
	}
	g_lastHeadX = hx;
	g_lastHeadY = hy;
	g_lastHeadZ = hz;
	g_headPosInitialized = true;

	// --- Distance-based auto-destroy ---
	std::vector<uint64_t> toDestroy;
	for (auto& [viewId, ovl] : g_vrOverlays) {
		if (!ovl.worldLocked) continue; // Only applies to world-locked panels

		float dx = ovl.posX - hx;
		float dy = ovl.posY - hy;
		float dz = ovl.posZ - hz;
		float dist = sqrtf(dx * dx + dy * dy + dz * dz);

		if (dist > AUTO_CLOSE_DISTANCE) {
			toDestroy.push_back(viewId);
			logger::info("PrismaVR: View {} auto-closed at {:.1f}m distance", viewId, dist);
		}
	}
	for (uint64_t id : toDestroy) {
		DestroyVROverlay(id);
		// SyncOverlays will recreate it as head-following if the Prisma view still exists
	}
}

// ============================================================================
// Section 14b: Game control masking
// Movement: ControlMap (global) — masked during panel manipulation or scroll capture.
// ============================================================================

// UpdateControlMasking — Freezes movement while manipulating a panel or pane.
//
// Walking while dragging a panel or using its stick-scroll capture would steal
// the same input and be disorienting. Unmask immediately when interaction ends.
static void UpdateControlMasking()
{
	if (g_vrOverlays.empty()) {
		if (g_combatMasked) { PrismaVR_Bridge::UnmaskCombatControls(); g_combatMasked = false; }
		if (g_movementMasked) { PrismaVR_Bridge::UnmaskMovement(); g_movementMasked = false; }
		return;
	}

	// Combat: mask only while a live controller trigger is actively interacting
	// with an open Prisma panel. Hover alone must not disable Skyrim's fighting
	// group; in VR that group owns weapon fire, spell fire, and combat stance.
	auto triggerOnOpenPanel = [](int hand) {
		return g_controllers[hand].scrollCaptureActive ||
		       (g_controllers[hand].valid &&
		       g_controllers[hand].triggerPressed &&
		       g_hitInfo[hand].hitting &&
		       g_vrOverlays.find(g_hitInfo[hand].hitViewId) != g_vrOverlays.end());
	};
	bool anyTriggerOnPanel = triggerOnOpenPanel(0) || triggerOnOpenPanel(1);
	if (anyTriggerOnPanel && !g_combatMasked) {
		logger::info("PrismaVR: masking combat controls (trigger on panel)");
		PrismaVR_Bridge::MaskCombatControls();
		g_combatMasked = true;
	} else if (!anyTriggerOnPanel && g_combatMasked) {
		logger::info("PrismaVR: unmasking combat controls (trigger released/off panel)");
		PrismaVR_Bridge::UnmaskCombatControls();
		g_combatMasked = false;
	}

	// Movement: mask while grabbing or while a same-hand scroll capture owns a stick.
	bool anyManipulating = g_controllers[0].scrollCaptureActive ||
	                       g_controllers[1].scrollCaptureActive;
	for (auto& [id, ovl] : g_vrOverlays) {
		if (ovl.isGrabbed) { anyManipulating = true; break; }
	}
	if (anyManipulating && !g_movementMasked) {
		logger::info("PrismaVR: masking movement controls (panel manipulation)");
		PrismaVR_Bridge::MaskMovement();
		g_movementMasked = true;
	} else if (!anyManipulating && g_movementMasked) {
		logger::info("PrismaVR: unmasking movement controls (panel manipulation ended)");
		PrismaVR_Bridge::UnmaskMovement();
		g_movementMasked = false;
	}
}

// ============================================================================
// Section 14c: VR Keyboard — WndProc subclass for WM_OC_CHAR
// Intercepts OC VR keyboard characters and injects them into Ultralight.
// When a Prisma panel is active, we consume the message so the SKSE plugin
// doesn't also inject it into Scaleform (no double-typing).
// ============================================================================

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lParam) {
	DWORD pid;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
		*reinterpret_cast<HWND*>(lParam) = hwnd;
		return FALSE;
	}
	return TRUE;
}

static std::shared_ptr<PrismaUI::Core::PrismaView> FindKeyboardTargetView()
{
	const uint64_t targetViewId = g_keyboardTargetViewId.load();
	auto views = PrismaVR_Bridge::GetVisibleViews();
	for (auto& vi : views) {
		if (vi.id == targetViewId)
			return vi.view;
	}
	return nullptr;
}

// Async-poll whether the focused HTML element is a text input field.
// Submits a JS eval to the Ultralight thread; result updates g_textInputFocused.
// Called after each laser click and periodically (~every 10 frames) to catch
// Enter/Tab/click-away that blur the field.
static void CheckTextInputFocusAsync(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	uint64_t expectedViewId)
{
	if (!view || !view->ultralightView || g_keyboardTargetViewId.load() != expectedViewId)
		return;
	const uint64_t expectedGeneration = g_keyboardFocusGeneration.load();
	PrismaUI::Core::ultralightThread.submit([view, expectedViewId, expectedGeneration]() {
		if (!view->ultralightView || g_keyboardTargetViewId.load() != expectedViewId ||
		    g_keyboardFocusGeneration.load() != expectedGeneration)
			return;
		ultralight::String result = view->ultralightView->EvaluateScript(
			ultralight::String(
				"(function(){"
				"var a=document.activeElement;"
				"if(!a)return '0';"
				"if(a.tagName==='TEXTAREA')return '1';"
				"if(a.isContentEditable)return '1';"
				"if(a.tagName==='INPUT'){"
				"var t=a.type.toLowerCase();"
				"if(t==='text'||t==='password'||t==='email'||t==='search'||t==='url'"
				"||t==='tel'||t==='number'||t==='')return '1';"
				"}"
				"return '0';"
				"})()"
			), nullptr, ultralight::String(""));
		// EvaluateScript returns the JS result as a String — "1" or "0"
		bool focused = false;
		if (!result.empty()) {
			std::string r(result.utf8().data(), result.utf8().length());
			focused = (r.find('1') != std::string::npos);
		}
		std::lock_guard lock(g_keyboardFocusStateMutex);
		if (g_keyboardTargetViewId.load() != expectedViewId ||
		    g_keyboardFocusGeneration.load() != expectedGeneration)
			return;
		bool prev = g_textInputFocused.exchange(focused);
		if (focused != prev) {
			logger::info("PrismaVR: text input focus changed: {} (raw result: '{}', gameHwnd=0x{:X})",
				focused ? "FOCUSED" : "unfocused",
				result.empty() ? "(empty)" : std::string(result.utf8().data(), result.utf8().length()),
				(uintptr_t)g_gameHwnd);
			// Signal OC's auto-detect: set/remove window property so the VR keyboard
			// opens for Prisma text inputs (OC reads this alongside textEntryCount).
			if (g_gameHwnd) {
				if (focused)
					SetPropW(g_gameHwnd, L"OC_PRISMA_TEXT", (HANDLE)1);
				else
					RemovePropW(g_gameHwnd, L"OC_PRISMA_TEXT");
			} else {
				logger::warn("PrismaVR: g_gameHwnd is NULL — cannot signal OC for VR keyboard!");
			}
			// OCU owns VR keyboard creation through OC_PRISMA_TEXT.
		}
	});
}

// PrismaVR_KBSubclassProc — WndProc subclass that intercepts keyboard input for Prisma panels.
//
// When a Prisma text input (<input>, <textarea>, contenteditable) has focus,
// this subclass captures keyboard messages BEFORE Skyrim processes them.
// Without this, typing in a Prisma text field would also trigger game hotkeys
// (e.g., pressing 'I' opens inventory while typing in a search box).
//
// Two input sources are handled:
//   1. Standard Windows keyboard (WM_KEYDOWN/WM_KEYUP/WM_CHAR) — physical keyboard,
//      system VR keyboard, or any IME. Translated to Ultralight KeyEvents.
//   2. OC VR keyboard (WM_OC_CHAR) — custom message from OpenComposite with UTF-16
//      characters. Each char needs the full RawKeyDown→Char→KeyUp sequence for
//      Chromium/Ultralight to process it into <input> fields.
// Interception is only active when: VR is on, panels are open, a text field has
// DOM focus (g_textInputFocused), and we have a target view. Alt+F4 always passes through.
static LRESULT CALLBACK PrismaVR_KBSubclassProc(
	HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/)
{
	// Only intercept keyboard when VR is active AND a text input field has focus.
	// When no text field is focused, keys pass through as normal game hotkeys.
	const uint64_t keyboardTargetViewId = g_keyboardTargetViewId.load();
	bool canIntercept = g_vrActive && !g_vrOverlays.empty()
	                    && keyboardTargetViewId != 0
	                    && g_textInputFocused.load();

	if (uMsg == WM_OC_CHAR && !canIntercept) {
		logger::warn("PrismaVR: WM_OC_CHAR DROPPED — canIntercept=false (vrActive={}, overlays={}, targetId={}, textFocused={})",
			g_vrActive, !g_vrOverlays.empty(), keyboardTargetViewId, g_textInputFocused.load());
	}

	if (canIntercept) {
		// --- Standard Windows keyboard messages (physical keyboard, system VR keyboard) ---
		if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
			// Let Alt+F4 and other system combos through
			if (wParam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
				return DefSubclassProc(hwnd, uMsg, wParam, lParam);

			auto targetView = FindKeyboardTargetView();
			if (targetView) {
				ultralight::KeyEvent keyEv;
				keyEv.type = ultralight::KeyEvent::kType_RawKeyDown;
				keyEv.virtual_key_code = static_cast<int>(wParam);
				keyEv.native_key_code = static_cast<int>((lParam >> 16) & 0xFF); // scan code
				keyEv.modifiers = 0;
				if (GetKeyState(VK_SHIFT) & 0x8000)   keyEv.modifiers |= 1; // kMod_ShiftKey
				if (GetKeyState(VK_CONTROL) & 0x8000)  keyEv.modifiers |= 2; // kMod_CtrlKey
				if (GetKeyState(VK_MENU) & 0x8000)     keyEv.modifiers |= 4; // kMod_AltKey
				keyEv.is_auto_repeat = (lParam & 0x40000000) != 0;
				PrismaVR_Bridge::FireKeyEvent(targetView, keyEv);
				return 0;
			}
		}

		if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
			auto targetView = FindKeyboardTargetView();
			if (targetView) {
				ultralight::KeyEvent keyEv;
				keyEv.type = ultralight::KeyEvent::kType_KeyUp;
				keyEv.virtual_key_code = static_cast<int>(wParam);
				keyEv.native_key_code = static_cast<int>((lParam >> 16) & 0xFF);
				keyEv.modifiers = 0;
				if (GetKeyState(VK_SHIFT) & 0x8000)   keyEv.modifiers |= 1;
				if (GetKeyState(VK_CONTROL) & 0x8000)  keyEv.modifiers |= 2;
				if (GetKeyState(VK_MENU) & 0x8000)     keyEv.modifiers |= 4;
				keyEv.is_auto_repeat = false;
				PrismaVR_Bridge::FireKeyEvent(targetView, keyEv);
				return 0;
			}
		}

		if (uMsg == WM_CHAR || uMsg == WM_SYSCHAR) {
			wchar_t ch = static_cast<wchar_t>(wParam);
			if (ch >= 0x20 || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == L'\b') {
				auto targetView = FindKeyboardTargetView();
				if (targetView) {
					ultralight::KeyEvent charEv;
					charEv.type = ultralight::KeyEvent::kType_Char;
					charEv.modifiers = 0;
					wchar_t str[2] = { ch, 0 };
					char utf8[8] = { 0 };
					WideCharToMultiByte(CP_UTF8, 0, str, -1, utf8, sizeof(utf8), nullptr, nullptr);
					ultralight::String ul_text(utf8);
					charEv.text = ul_text;
					charEv.unmodified_text = ul_text;
					charEv.virtual_key_code = 0;
					charEv.native_key_code = 0;
					charEv.is_auto_repeat = (lParam & 0x40000000) != 0;
					PrismaVR_Bridge::FireKeyEvent(targetView, charEv);
					return 0;
				}
			}
		}

		// --- OC VR keyboard custom message ---
		if (uMsg == WM_OC_CHAR) {
			wchar_t ch = static_cast<wchar_t>(wParam);
			LPARAM mode = lParam; // 0 = printable char, 1 = control key
			logger::info("PrismaVR: WM_OC_CHAR received — ch={} mode={} targetViewId={}", (int)ch, (int)mode, keyboardTargetViewId);

			auto targetView = FindKeyboardTargetView();
			if (targetView) {
				logger::info("PrismaVR: WM_OC_CHAR — targetView found, dispatching FireKeyEvent");
				if (mode == 0 && (ch >= 0x20 || ch == L'\t')) {
					// Chromium/Ultralight requires: RawKeyDown → Char → KeyUp
					// for text to appear in <input> fields.
					wchar_t str[2] = { ch, 0 };
					char utf8[8] = { 0 };
					WideCharToMultiByte(CP_UTF8, 0, str, -1, utf8, sizeof(utf8), nullptr, nullptr);
					ultralight::String ul_text(utf8);

					ultralight::KeyEvent keyDown;
					keyDown.type = ultralight::KeyEvent::kType_RawKeyDown;
					keyDown.virtual_key_code = static_cast<int>(ch);
					keyDown.native_key_code = static_cast<int>(ch);
					keyDown.modifiers = 0;
					keyDown.is_auto_repeat = false;
					keyDown.text = ul_text;
					keyDown.unmodified_text = ul_text;
					PrismaVR_Bridge::FireKeyEvent(targetView, keyDown);

					ultralight::KeyEvent charEv;
					charEv.type = ultralight::KeyEvent::kType_Char;
					charEv.modifiers = 0;
					charEv.text = ul_text;
					charEv.unmodified_text = ul_text;
					charEv.virtual_key_code = 0;
					charEv.native_key_code = 0;
					charEv.is_auto_repeat = false;
					PrismaVR_Bridge::FireKeyEvent(targetView, charEv);

					ultralight::KeyEvent keyUp;
					keyUp.type = ultralight::KeyEvent::kType_KeyUp;
					keyUp.virtual_key_code = static_cast<int>(ch);
					keyUp.native_key_code = static_cast<int>(ch);
					keyUp.modifiers = 0;
					keyUp.is_auto_repeat = false;
					PrismaVR_Bridge::FireKeyEvent(targetView, keyUp);
				} else if (mode == 1) {
					// Control key (Backspace, Arrow, Delete, Enter, Tab, etc.).
					// Populate key_identifier — required for Chromium to route
					// navigation keys correctly. Without it, Backspace and arrow
					// keys do nothing in <input> fields.
					int vkCode = static_cast<int>(ch);
					ultralight::String keyId;
					ultralight::GetKeyIdentifierFromVirtualKeyCode(vkCode, keyId);

					ultralight::KeyEvent keyDown;
					keyDown.type = ultralight::KeyEvent::kType_RawKeyDown;
					keyDown.virtual_key_code = vkCode;
					keyDown.native_key_code = vkCode;
					keyDown.modifiers = 0;
					keyDown.is_auto_repeat = false;
					keyDown.key_identifier = keyId;
					PrismaVR_Bridge::FireKeyEvent(targetView, keyDown);

					ultralight::KeyEvent keyUp;
					keyUp.type = ultralight::KeyEvent::kType_KeyUp;
					keyUp.virtual_key_code = vkCode;
					keyUp.native_key_code = vkCode;
					keyUp.modifiers = 0;
					keyUp.is_auto_repeat = false;
					keyUp.key_identifier = keyId;
					PrismaVR_Bridge::FireKeyEvent(targetView, keyUp);
				}
				return 0;
			}
		}
	}

	if (uMsg == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, PrismaVR_KBSubclassProc, uIdSubclass);
	}

	return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// ============================================================================
// Section 15: Public API implementation
// ============================================================================

namespace PrismaVR {

	void Initialize()
	{
		LoadVRInputConfig();
		g_vrActive = DetectVR();
		if (g_vrActive) {
			// Game window search is deferred to OnFrame() — at Initialize() time the
			// Skyrim VR window usually isn't visible yet, causing EnumWindows to miss it.
			// g_gameHwnd will be found and the WndProc subclass installed on the first
			// OnFrame() call after the window appears.
			logger::info("PrismaVR: VR mode active — overlay bridge initialized (game window search deferred).");
		}
	}

	// OnFrame — Main per-frame entry point. Called by Prisma's render loop every frame.
	//
	// Execution order matters — each step depends on the previous:
	//   1. Game window search (deferred from Initialize — window isn't ready at plugin load)
	//   2. OC aim pose acquisition (one-time, via window property from OpenComposite)
	//   3. SyncOverlays: create/destroy VR overlays to match Prisma's visible views
	//   4. UpdateControllers: read poses + button states for both hands
	//   5. ProcessInput: raycast, cursor dots, mouse events, grab initiation
	//   6. ProcessGrabMoveResize: move/resize panels that are being held
	//   7. UpdateControlMasking: combat masking (laser on panel) + movement masking (grab)
	//   8. FaceOverlaysToPlayer: rotate world-locked panels to face the head
	//   9. UpdateHeadRelativeOverlays: reposition non-locked panels to follow head
	//  10. CleanupDistantOverlays: detect teleports, destroy far-away panels
	//  11. CreateLaserOverlays: one-time laser beam texture/overlay creation
	//  12. UpdateLasers: position beams from controllers to panels
	//  13. Text focus polling for OCU keyboard routing

	// Lookup passed to ModelPreview::TickVR so it can place each preview on its panel.
	static bool ModelPreviewPanelLookup(uint64_t viewId, PrismaUI::ModelPreview::PanelInfo& out)
	{
		auto it = g_vrOverlays.find(viewId);
		if (it == g_vrOverlays.end()) return false;
		auto& s = it->second;
		out.valid = true;
		out.pos[0] = s.posX; out.pos[1] = s.posY; out.pos[2] = s.posZ;
		out.fwd[0] = s.fwdX; out.fwd[1] = s.fwdY; out.fwd[2] = s.fwdZ;
		out.up[0] = s.upX; out.up[1] = s.upY; out.up[2] = s.upZ;
		out.widthMeters = s.widthMeters;
		out.texWidth = s.texWidth;
		out.texHeight = s.texHeight;
		return true;
	}

	void OnFrame()
	{
		if (!g_vrActive) return;

		// Deferred game window search: at Initialize() time the window isn't visible yet.
		// Retry each frame until found, then install WndProc subclass for keyboard input.
		if (!g_gameHwnd) {
			HWND found = nullptr;
			EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&found));
			if (found) {
				std::lock_guard lock(g_keyboardFocusStateMutex);
				g_gameHwnd = found;
				SetWindowSubclass(g_gameHwnd, PrismaVR_KBSubclassProc, PRISMAVR_SUBCLASS_ID, 0);
				logger::info("PrismaVR: Game window found (deferred), WndProc subclass installed. HWND=0x{:X}",
					(uintptr_t)g_gameHwnd);
			}
		}

		// Read OC aim pose pointer (set once by OpenComposite via window property)
		static bool s_loggedMissingAimPoses = false;
		if (g_gameHwnd && !g_aimPoses) {
			g_aimPoses = reinterpret_cast<const OCAimPoseData*>(GetPropW(g_gameHwnd, L"OC_AIM_POSES"));
			if (g_aimPoses) {
				logger::info("PrismaVR: OC aim pose data found — universal laser pointing active.");
			} else if (!s_loggedMissingAimPoses) {
				logger::warn("PrismaVR: OC aim pose data unavailable; VR laser input requires OpenComposite Unleashed.");
				s_loggedMissingAimPoses = true;
			}
		}


		SyncOverlays();

		// Nothing to do if no Prisma panels are open
		if (g_vrOverlays.empty()) {
			// Hide laser beams that may still be showing from the last frame
			for (int i = 0; i < 2; i++) {
				if (g_laserBeamHandle[i])
					VCallOvl(g_overlay, OVL_SLOT::HideOverlay, "HideOverlay(laser/cleanup)", g_laserBeamHandle[i]);
			}
			// No panels open — unmask movement if it was masked
			if (g_combatMasked) { PrismaVR_Bridge::UnmaskCombatControls(); g_combatMasked = false; }
			if (g_movementMasked) { PrismaVR_Bridge::UnmaskMovement(); g_movementMasked = false; }
			// Clear hit state and active hand so masking doesn't re-trigger on next panel open
			g_hitInfo[0].hitting = false;
			g_hitInfo[1].hitting = false;
			g_mouseActiveHand = -1;
			// No panels open — keyboard goes back to normal hotkey mode
			{
				std::lock_guard lock(g_keyboardFocusStateMutex);
				g_keyboardTargetViewId.store(0);
				g_keyboardFocusGeneration.fetch_add(1);
				g_textInputFocused.store(false);
				if (g_gameHwnd) RemovePropW(g_gameHwnd, L"OC_PRISMA_TEXT");
			}
			// Do NOT call HideKeyboard here — it maps to OC's BaseOverlay::HideKeyboard()
			// which destroys the user's VR keyboard (including shortcut-opened ones).
			return;
		}

		UpdateControllers();
		ProcessInput();
		ProcessGrabMoveResize();
		UpdateControlMasking();
		FaceOverlaysToPlayer();
		UpdateHeadRelativeOverlays();
		CleanupDistantOverlays();
		CreateLaserOverlays();
		UpdateLasers();

		// Drive the 3D model preview overlays (panel transforms are final here)
		if (PrismaUI::ModelPreview::Enabled())
			PrismaUI::ModelPreview::TickVR(g_overlay, &ModelPreviewPanelLookup);

		// Poll text input focus state every ~10 frames (~110ms at 90fps).
		// Catches Enter/Tab/click-away that blur the text field without a new laser click.
		static int textFocusPollFrame = 0;
		const uint64_t keyboardTargetViewId = g_keyboardTargetViewId.load();
		if (++textFocusPollFrame % TEXT_FOCUS_POLL_FRAMES == 0 && keyboardTargetViewId != 0) {
			auto tv = FindKeyboardTargetView();
			if (tv) CheckTextInputFocusAsync(tv, keyboardTargetViewId);
		}

		// OCU owns Prisma text input through OC_PRISMA_TEXT and direct delivery.

		// Debug: periodic status log every ~5 seconds
		static int dbgFrame = 0;
		if (++dbgFrame % DEBUG_LOG_INTERVAL_FRAMES == 0) {
			logger::debug("PrismaVR DBG: overlays={}, ctrl0={} ctrl1={}, hit0={} hit1={}, activeHand={}, moveMask={}, combatMask={}",
				g_vrOverlays.size(),
				g_controllers[0].valid, g_controllers[1].valid,
				g_hitInfo[0].hitting, g_hitInfo[1].hitting,
				g_mouseActiveHand, g_movementMasked, g_combatMasked);
		}
	}

	// Shutdown — Full teardown of the VR bridge. Called when the SKSE plugin unloads.
	//
	// Cleans up in reverse order: control masking → laser overlays → view overlays
	// → WndProc subclass → interface pointers. After this, no VR calls are safe.
	void Shutdown()
	{
		if (!g_vrActive) return;

		// Invalidate async DOM probes before any overlay/view teardown. Cancel
		// active presses while their shared view can still be resolved.
		const uint64_t pressedViewIds[2] = {
			g_controllers[0].pressedViewId,
			g_controllers[1].pressedViewId
		};
		for (uint64_t pressedViewId : pressedViewIds) {
			if (pressedViewId != 0)
				CancelPointerStateForView(pressedViewId, PrismaVR_Bridge::FindView(pressedViewId));
		}
		for (int hand = 0; hand < 2; ++hand)
			ResetPointerState(hand);

		// Unmask all controls on shutdown
		if (g_combatMasked) { PrismaVR_Bridge::UnmaskCombatControls(); g_combatMasked = false; }
		if (g_movementMasked) { PrismaVR_Bridge::UnmaskMovement(); g_movementMasked = false; }

		// Destroy laser beam overlays
		for (int i = 0; i < 2; i++) {
			if (g_laserBeamHandle[i]) {
				VCallOvl(g_overlay, OVL_SLOT::DestroyOverlay, "DestroyOverlay(laser/shutdown)", g_laserBeamHandle[i]);
				g_laserBeamHandle[i] = 0;
			}
		}
		for (int i = 0; i < 2; i++) {
			if (g_laserBeamTex[i]) { g_laserBeamTex[i]->Release(); g_laserBeamTex[i] = nullptr; }
		}
		g_lasersCreated = false;
		g_lastHitViewId[0] = g_lastHitViewId[1] = 0;

		// Destroy all view overlays
		std::vector<uint64_t> ids;
		for (auto& [id, state] : g_vrOverlays) {
			ids.push_back(id);
		}
		for (uint64_t id : ids) {
			DestroyVROverlay(id);
		}
		g_vrOverlays.clear();

		// Remove VR keyboard WndProc subclass and Prisma text property
		{
			std::lock_guard lock(g_keyboardFocusStateMutex);
			if (g_gameHwnd) {
				RemovePropW(g_gameHwnd, L"OC_PRISMA_TEXT");
				RemoveWindowSubclass(g_gameHwnd, PrismaVR_KBSubclassProc, PRISMAVR_SUBCLASS_ID);
				g_gameHwnd = nullptr;
			}
			g_keyboardTargetViewId.store(0);
			g_keyboardFocusGeneration.fetch_add(1);
			g_textInputFocused.store(false);
		}

		g_overlay = nullptr;
		g_system = nullptr;
		g_aimPoses = nullptr;
		g_vrActive = false;

		logger::info("PrismaVR: Shutdown complete.");
	}

	bool IsVRActive()
	{
		return g_vrActive;
	}

	void SetViewPosition(uint64_t viewId, float x, float y, float z)
	{
		auto it = g_vrOverlays.find(viewId);
		if (it == g_vrOverlays.end()) return;

		it->second.posX = x;
		it->second.posY = y;
		it->second.posZ = z;
		it->second.modderPositioned = true;

		openvr::HmdMatrix34_t transform;
		BuildOverlayTransform(it->second, transform);
		VCallOvl(g_overlay, OVL_SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute(API)",
			it->second.handle, (int)openvr::TrackingUniverseStanding, &transform);
	}

	void SetViewScale(uint64_t viewId, float scale)
	{
		auto it = g_vrOverlays.find(viewId);
		if (it == g_vrOverlays.end()) return;

		it->second.widthMeters = scale;
		if (it->second.widthMeters < API_MIN_OVERLAY_WIDTH) it->second.widthMeters = API_MIN_OVERLAY_WIDTH;
		if (it->second.widthMeters > API_MAX_OVERLAY_WIDTH) it->second.widthMeters = API_MAX_OVERLAY_WIDTH;

		VCallOvl(g_overlay, OVL_SLOT::SetOverlayWidthInMeters, "SetOverlayWidthInMeters(API)",
			it->second.handle, it->second.widthMeters);
	}

	void SetViewWorldLock(uint64_t viewId, bool locked)
	{
		auto it = g_vrOverlays.find(viewId);
		if (it == g_vrOverlays.end()) return;

		it->second.worldLocked = locked;
	}

// ============================================================================
// C ABI export for OpenComposite VR keyboard integration
// ----------------------------------------------------------------------------
// OpenComposite's VR keyboard needs to deliver typed characters into Prisma UI
// text inputs WITHOUT routing through Windows messages. Why direct delivery:
//   1. Guarantees Skyrim never sees the keystroke → no game hotkeys triggered
//      (physical keyboard path has I=inventory, Tab=menu, etc. as constant risk)
//   2. Bypasses all WndProc subclass chain contention (Dekana's IME refactor,
//      other mods, Skyrim's own WndProc) — no ordering dependencies
//   3. Does not touch VR input paths at all — controllers/overlays untouched
//
// OpenComposite calls this via GetProcAddress(GetModuleHandle("PrismaUI.dll"),
// "PrismaVR_DeliverChar"). No link-time dependency; runtime lookup only.
//
// The Ultralight KeyEvent construction follows Prisma UI's canonical flatscreen
// InputHandler recipe: RawKeyDown → Char → KeyUp, with text/unmodified_text
// populated as UTF-8, virtual_key_code = GK_UNKNOWN, key_identifier empty.
// Ultralight silently drops characters if these fields are not set correctly.
// ============================================================================
extern "C" __declspec(dllexport) void PrismaVR_DeliverChar(wchar_t ch)
{
	// Gate: only deliver when VR is active, a Prisma overlay exists, a view
	// has been laser-targeted, and a text input field has DOM focus. If any
	// condition is false, silently drop — OC will skip the call in those cases.
	if (!g_vrActive) return;
	if (g_vrOverlays.empty()) return;
	if (g_keyboardTargetViewId.load() == 0) return;
	if (!g_textInputFocused.load()) return;

	auto targetView = FindKeyboardTargetView();
	if (!targetView) return;

	// Only accept printable chars + tab. Backspace / Enter / control keys
	// should come through a different mechanism — OC still posts those via
	// WM_OC_CHAR with mode=1 for Scaleform-aware handling.
	if (ch < 0x20 && ch != L'\t') return;

	// Build UTF-8 representation (Ultralight requires UTF-8 in text fields)
	wchar_t str[2] = { ch, 0 };
	char utf8[8] = { 0 };
	WideCharToMultiByte(CP_UTF8, 0, str, -1, utf8, sizeof(utf8), nullptr, nullptr);
	ultralight::String ul_text(utf8);

	// 1. RawKeyDown — Ultralight requires this before it will accept a Char event
	ultralight::KeyEvent keyDown;
	keyDown.type = ultralight::KeyEvent::kType_RawKeyDown;
	keyDown.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
	keyDown.native_key_code = 0;
	keyDown.modifiers = 0;
	keyDown.is_auto_repeat = false;
	keyDown.text = ul_text;
	keyDown.unmodified_text = ul_text;
	keyDown.key_identifier = "";
	PrismaVR_Bridge::FireKeyEvent(targetView, keyDown);

	// 2. Char — this is what actually inserts text into <input>/<textarea>
	ultralight::KeyEvent charEv;
	charEv.type = ultralight::KeyEvent::kType_Char;
	charEv.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
	charEv.native_key_code = 0;
	charEv.modifiers = 0;
	charEv.is_auto_repeat = false;
	charEv.text = ul_text;
	charEv.unmodified_text = ul_text;
	charEv.key_identifier = "";
	PrismaVR_Bridge::FireKeyEvent(targetView, charEv);

	// 3. KeyUp — clean release so next keystroke starts fresh
	ultralight::KeyEvent keyUp;
	keyUp.type = ultralight::KeyEvent::kType_KeyUp;
	keyUp.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
	keyUp.native_key_code = 0;
	keyUp.modifiers = 0;
	keyUp.is_auto_repeat = false;
	keyUp.text = ul_text;
	keyUp.unmodified_text = ul_text;
	keyUp.key_identifier = "";
	PrismaVR_Bridge::FireKeyEvent(targetView, keyUp);
}

// Direct delivery for control/navigation keys (Backspace, Arrow keys, Delete,
// Home, End, Enter, Tab, Escape). Same architecture as PrismaVR_DeliverChar
// but for non-printable keys that need proper Ultralight key_identifier strings
// for Chromium/Ultralight to recognize them as navigation actions instead of
// character input.
//
// Ultralight's virtual_key_code values (GK_*) match Windows VK_* codes directly
// (GK_BACK=0x08, GK_LEFT=0x25, etc.) so we pass the VK through unchanged. The
// key_identifier is populated via Ultralight's own GetKeyIdentifierFromVirtualKeyCode
// helper — this is the string Chromium's input handler uses to route the event.
//
// No Char event is fired (control keys don't produce characters). Just
// RawKeyDown + KeyUp with proper identification.
//
// OC calls this via GetProcAddress for mode==1 (control key) PostCharToGame
// invocations when OC_PRISMA_TEXT is set. Fall back to legacy WM_OC_CHAR
// PostMessage path if the export is missing (older OC builds).
extern "C" __declspec(dllexport) void PrismaVR_DeliverVKey(int vkCode)
{
	if (!g_vrActive) return;
	if (g_vrOverlays.empty()) return;
	if (g_keyboardTargetViewId.load() == 0) return;
	if (!g_textInputFocused.load()) return;

	auto targetView = FindKeyboardTargetView();
	if (!targetView) return;

	// Populate the key_identifier string Chromium needs to route navigation keys.
	// Ultralight ships this helper in KeyEvent.h — it maps VK codes to the
	// WebKit-legacy identifier strings ("Left", "Right", "U+0008", etc.).
	ultralight::String keyId;
	ultralight::GetKeyIdentifierFromVirtualKeyCode(vkCode, keyId);

	// 1. RawKeyDown
	ultralight::KeyEvent keyDown;
	keyDown.type = ultralight::KeyEvent::kType_RawKeyDown;
	keyDown.virtual_key_code = vkCode;
	keyDown.native_key_code = vkCode;
	keyDown.modifiers = 0;
	keyDown.is_auto_repeat = false;
	keyDown.key_identifier = keyId;
	PrismaVR_Bridge::FireKeyEvent(targetView, keyDown);

	// 2. KeyUp
	ultralight::KeyEvent keyUp;
	keyUp.type = ultralight::KeyEvent::kType_KeyUp;
	keyUp.virtual_key_code = vkCode;
	keyUp.native_key_code = vkCode;
	keyUp.modifiers = 0;
	keyUp.is_auto_repeat = false;
	keyUp.key_identifier = keyId;
	PrismaVR_Bridge::FireKeyEvent(targetView, keyUp);
}

} // namespace PrismaVR
