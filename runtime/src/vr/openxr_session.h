// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// OpenXR session (docs/VR_ARCHITECTURE.md): the Khronos loader, instance, system, Vulkan graphics binding,
// reference spaces, stereo swapchains, controller actions, haptics and the session's state.
//
// The loader (openxr_loader.dll, libopenxr_loader.so.1) is opened at run time, as the renderer opens the
// Vulkan loader (gfx/vulkan/loader.h): without it, or without a runtime or headset, the game starts on the
// desktop as before and says why VR is not available.
//
// Threads: the render thread owns the session (create, frame loop, swapchains, events, teardown). The game
// thread reads tracking through locate_views() and sync_input(), which take the session's state lock and
// only use OpenXR functions without external synchronisation requirements on the session.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#ifdef __ANDROID__
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID  // the loader's and the instance's Android structures (docs/VR_ANDROID.md)
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vr_bindings.h"
#include "vr_tracking.h"
#include "vr_math.h"

namespace vr::xr {
bool hands_shared();  // one hands image for both eyes, side by side (WWHD_VR_HANDS_SHARED=0: one per eye)

// what the render thread needs to make the Vulkan instance and device the runtime will draw with
struct VulkanRequirements {
    uint32_t minApi = 0, maxApi = 0;  // VK_MAKE_API_VERSION
};

struct EyeView {
    Pose pose;  // tracking space (metres)
    Fov fov;
    // the runtime's own field of view of this eye, kept when the image is drawn with another (vr::as_rendered): the
    // frame submits the part of the image that shows it (fov_crop). All 0: the same as fov
    Fov display{0, 0, 0, 0};
    // the part of the eye's image written, from its top-left corner (0: all of it): the eye picture copied at its own
    // size for Quest Super Resolution to upscale (docs/VR_UPSCALING.md, "The picture at its own size")
    int rectWidth = 0, rectHeight = 0;
};
struct Views {
    bool valid = false;     // orientation and position tracked
    XrTime time = 0;        // the display time they were located for
    float heading = 0;      // (diagnostics: the frames' evenness) the view's heading in the game they were drawn at (radians)
    bool hasHeading = false;
    EyeView eye[2];
    Pose head;              // VIEW space in the tracking space (between the eyes)
};

struct HandState {
    bool active = false;    // the controller is connected and its pose valid (or held through a short gap)
    bool held = false;      // the pose is the last valid one, held through a tracking gap (vr_tracking.h)
    bool estimated = false; // the runtime reports the position valid but not tracked (its estimate: out of view)
    Pose grip, aim;         // tracking space
    float trigger = 0, grip_value = 0;
    bool stickClick = false;  // this controller's thumbstick pressed (physical side)
    float stickX = 0, stickY = 0;
};
struct InputState {
    bool valid = false;
    HandState hand[2];      // 0 left, 1 right (physical); vr_game.cpp maps them to the main / off hand
    uint32_t held = 0;      // bit (int)Action: held this update, main/off already resolved
};

// A flat picture in the tracking space (an OpenXR quad layer): the GamePad screen as a panel
struct Quad {
    bool visible = false;
    Pose pose;                    // its centre; the picture faces +Z (towards the viewer)
    float width = 0, height = 0;  // metres
    // 0 flat; above 0 a cylinder (XR_KHR_composition_layer_cylinder, when the runtime has it) of radius
    // distance / curvature, its axis through the viewer's side of the panel (1: centred on the viewer)
    float curvature = 0, distance = 1;
    bool tracking = false;        // (the UI layer's quad) its pose in the tracking space, not the view's
    // the part of the swapchain's image shown, from its top left corner, pixels; 0: the whole image (the GamePad panel:
    // one image for the GamePad screen and the VR menu, at the larger size, each drawn into its own part)
    uint32_t pixelWidth = 0, pixelHeight = 0;
};

// a quad showing part of its swapchain's image (the wrist HUD's panels share one picture)
struct SubQuad {
    Quad quad;
    int x = 0, y = 0, w = 0, h = 0;  // the part of the image, pixels
    bool inView = false;             // its pose in the headset's VIEW space (held in the view), not the tracking space
};

enum class State : int { Unavailable, Idle, Ready, Running, Stopping, Lost };
const char* state_name(State s);

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    int64_t format = 0;
    std::vector<VkImage> images;
    uint32_t acquired = UINT32_MAX;  // index of the acquired image, UINT32_MAX when none
};

class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { shutdown(); }

    // ---- start-up (render thread, before the renderer makes its Vulkan instance)
    // opens the loader and creates the instance and system; false (with why()) when VR cannot start
    bool init(const std::string& appName);
    const std::string& why() const { return why_; }
    bool initialised() const { return instance_ != XR_NULL_HANDLE; }
    VulkanRequirements requirements() const { return requirements_; }
    // XR_KHR_vulkan_enable2: the runtime creates the Vulkan instance / device with the extensions it needs
    VkResult create_vulkan_instance(const VkInstanceCreateInfo& ci, PFN_vkGetInstanceProcAddr gipa, VkInstance* out);
    VkPhysicalDevice vulkan_physical_device(VkInstance instance);
    VkResult create_vulkan_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& ci,
                                  PFN_vkGetInstanceProcAddr gipa, VkDevice* out);
    // the session on the renderer's device and graphics queue (render thread)
    bool begin(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
               uint32_t queueIndex, float renderScale);
    void shutdown();

    // ---- every frame (render thread)
    void poll_events();
    State state() const { return state_.load(); }
    bool running() const { return state_.load() == State::Running; }
    // the runtime's own session state (XR_SESSION_STATE_*): VISIBLE shows the frames, FOCUSED also gets input
    XrSessionState xr_state() const { return xrState_.load(); }
    // xrWaitFrame + xrBeginFrame; false when the session does not run. shouldRender: the runtime shows
    // this frame (otherwise end_frame without layers)
    bool begin_frame(bool& shouldRender);
    // (docs/VR_RUNTIME_PACING_PLAN.md) xrWaitFrame for the next frame, ahead of its begin_frame (which takes it; with
    // none waited it waits itself). A no-op while one is waited. One thread at a time
    bool wait_frame();
    bool frame_waited() const { return waited_.load(std::memory_order_acquire); }
    // the waited frame's display time and when xrWaitFrame returned it (valid while frame_waited())
    XrTime waited_display_time() const { return waitedTime_; }
    std::chrono::steady_clock::time_point waited_at() const { return waitedAt_; }
    // (vr_vulkan.cpp, the game's own hold on a runtime whose xrWaitFrame does not hold) the waited frame released later
    void set_waited_at(std::chrono::steady_clock::time_point t) { waitedAt_ = t; }
    // a runtime time on std::chrono::steady_clock (ns since its epoch), for each frame's deadline; 0: the runtime does
    // not offer the conversion (XR_KHR_convert_timespec_time / XR_KHR_win32_convert_performance_counter_time)
    int64_t to_steady_ns(XrTime t) const;
    bool time_conversion() const { return timeConvExt_; }
    // swapchains: 0 left eye, 1 right eye, kPanel the GamePad panel (made by ensure_panel), kWheel the item wheel
    // (ensure_wheel; docs/VR_ITEM_WHEEL.md)
    static constexpr int kPanel = 2, kWheel = 3, kWrist = 4;  // kWrist: the wrist HUD (ensure_wrist; docs/VR_WRIST_HUD.md)
    static constexpr int kLaser = 5;  // the aiming laser (ensure_laser; docs/VR_AIMING.md)
    // frame synthesis and depth (docs/VR_FRAME_SYNTHESIS.md): each eye's motion vector swapchain (kMotion + eye) and
    // depth swapchain (kDepth + eye), made with the eyes' when a backend or depth is chosen
    static constexpr int kMotion = 6, kDepth = 8;
    // (docs/VR_EYE_PROJECTION.md) the game's UI as its own layer (each eye with its own projection): ensure_ui
    static constexpr int kUi = 10;
    // (docs/VR_HANDS_LAYER_PLAN.md step 3) Link's hands as their own projection layer over the eyes': kHands + eye
    static constexpr int kHands = 11;
    bool ensure_panel(uint32_t width, uint32_t height);
    bool ensure_wheel(uint32_t width, uint32_t height);
    bool ensure_wrist(uint32_t width, uint32_t height);
    bool ensure_laser(uint32_t width, uint32_t height);
    // the swapchains' Vulkan format (VK_FORMAT_*): the eyes', the panel's and the wheel's
    int64_t format() const { return format_; }
    // acquire and wait for a swapchain's image; returns its VkImage (VK_NULL_HANDLE on failure)
    VkImage acquire(int index);
    void release(int index);
    // submits the projection layer drawn with `views` (or no layer when !draw) and, when given and visible,
    // the panel's quad layer in front of it, then the item wheel's (blended by its alpha)
    // and the wrist HUD's panels (blended by their alpha; `wristCount` of them, parts of kWrist's image), then the
    // aiming laser's beam and dot (the same way; `laserCount` of them, parts of kLaser's image)
    // (ui: the game's UI layer, a quad in the view space, head-locked, over the eyes' pictures and under the panels)
    void end_frame(const Views& views, bool draw, const Quad* panel = nullptr, const Quad* wheel = nullptr,
                   const SubQuad* wrist = nullptr, int wristCount = 0, const SubQuad* laser = nullptr, int laserCount = 0,
                   const Quad* ui = nullptr, const Views* hands = nullptr);
    bool ensure_ui(uint32_t width, uint32_t height);
    // the hands layer's two swapchains (made once, the eyes' size); false: not (yet)
    bool ensure_hands(uint32_t width, uint32_t height);
    void set_hands_premultiplied(bool on) { hands_premultiplied_ = on; }  // (the renderer: coverage from depth)
    const Swapchain& swapchain(int index) const { return swapchains_[index]; }
    // XR_FB_display_refresh_rate (when the runtime has it): the rates offered, and a request (the runtime may
    // refuse or choose another). refresh_rate(): the runtime's rate, else the one its frame period implies
    std::vector<float> refresh_rates();
    float refresh_rate();
    // asks for an offered rate within 0.6 Hz of hz (none other: the setting names that rate); false when the runtime
    // cannot choose, does not offer it, or refuses. The change itself arrives as an event (logged), refresh_rate() then
    // says it
    bool request_refresh_rate(float hz);
    bool refresh_rate_choosable() const { return refreshRateExt_; }
    // curved panels are possible (XR_KHR_composition_layer_cylinder)
    bool curved_panels() const { return cylinderExt_; }
    // Quest Super Resolution (docs/VR_UPSCALING.md): the runtime offers XR_FB_composition_layer_settings, and whether
    // the projection layer asks for its quality sharpening (the compositor's SGSR-based upscale), set each frame
    bool super_resolution_offered() const { return layerSettingsExt_; }
    // frame synthesis (docs/VR_FRAME_SYNTHESIS.md): the backend chosen when VR started (its swapchains made), and
    // whether depth goes with the projection layer on its own
    int synthesis_backend() const { return synthBackend_; }  // (vr::framesynth::Backend)
    bool depth_submission() const { return depthOnly_ && swapchains_[kDepth].handle && swapchains_[kDepth + 1].handle; }
    bool has_synthesis_swapchains() const {
        return synthBackend_ && swapchains_[kMotion].handle && swapchains_[kMotion + 1].handle && swapchains_[kDepth].handle &&
               swapchains_[kDepth + 1].handle;
    }
    // the next xrEndFrame's motion and depth (set by the renderer for the frame it drew; one end_frame takes it, so a
    // frame shown again carries none): per eye whether its images were written, and the depth's planes in metres
    struct SynthFrame {
        bool motion[2] = {}, depth[2] = {};
        float nearZ = 0, farZ = 0;
    };
    void set_synthesis_frame(const SynthFrame& f) {
        std::lock_guard<std::mutex> l(synthMutex_);
        synthFrame_ = f;
        synthPending_ = true;
    }
    void set_super_resolution(bool on) { superResolution_.store(on, std::memory_order_relaxed); }
    XrTime predicted_display_time() const { return predictedTime_.load(); }
    // the runtime's own frame period (xrWaitFrame's predictedDisplayPeriod): may be longer than a refresh while the
    // runtime's frame smoothing runs (VDXR reports two refreshes while Virtual Desktop's SSW is active). The game's
    // timing in refreshes uses refresh_period(); predicted_period() is the spacing the runtime asks for
    XrDuration predicted_period() const { return predictedPeriod_.load(); }
    // one display refresh (ns), set at each xrWaitFrame: the headset's refresh rate (XR_FB_display_refresh_rate), else
    // the runtime's period unless it jumped to about twice the last refresh or more (docs/VR_STREAMING_AUDIT.md, finding 12)
    XrDuration refresh_period() const { return refreshPeriod_.load(); }

    // ---- tracking (any thread)
    bool locate_views(XrTime time, Views& out);
    bool sync_input(XrTime time, bool leftHanded, InputState& out);
    // the runtime moved the tracking space's origin (XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: a recentre on
    // the headset, the boundary set up again): true once after each, for the game to recentre (vr_game.cpp)
    bool take_space_changed() { return spaceChanged_.exchange(false); }
    void haptic(int physicalHand, float amplitude, float seconds);
    // the interaction profile the runtime uses for a hand ("" when no controller is active there)
    std::string interaction_profile(int physicalHand);
    // the recommended (render-scaled) size of an eye's image
    uint32_t eye_width() const { return swapchains_[0].width; }
    uint32_t eye_height() const { return swapchains_[0].height; }
    // (diagnostics, docs/VR_EYE_PROJECTION.md) the runtime's recommended image size, and what the last frame submitted
    // for an eye: its field of view and its part of the image
    uint32_t recommended_width() const { return configViews_[0].recommendedImageRectWidth; }
    uint32_t recommended_height() const { return configViews_[0].recommendedImageRectHeight; }
    struct Submitted { Fov fov; int x = 0, y = 0, width = 0, height = 0; bool valid = false; };
    Submitted submitted(int eye) {
        std::lock_guard<std::mutex> l(submittedMutex_);
        return submitted_[eye & 1];
    }
    bool stage_space() const { return stage_; }
    // a Quest 2 or 3 (or another Quest), on any runtime that names it (Meta's, Virtual Desktop's VDXR, SteamVR through
    // Link or Steam Link): the Quest-only fixes (docs/VR_ANDROID.md) apply to it alone, other headsets (PS VR2) run as
    // before. WWHD_VR_QUEST_FIXES=1 or 0 forces them on or off
    bool quest() const { return quest_; }
    // a streamed headset on a PC (docs/VR_STREAMING.md): the pictures go to it compressed, over Wi-Fi or a cable: Virtual
    // Desktop's VDXR, ALVR, a Quest or a Pico on any PC runtime (Link, Air Link, Steam Link, Pico Connect), or any other
    // headset not known to be wired whose display times turn out irregular in the session's first minute (begin_frame).
    // Known wired headsets (PS VR2, Index, Vive, Windows Mixed Reality, Pimax, Bigscreen) never are. Its display times
    // move with the network; the frame-timing shift (vr_vulkan.cpp note_display) and Auto's return to the full rate are
    // left out there. WWHD_VR_STREAMED=1 or 0 forces it (no detection)
    bool streamed() const { return streamed_.load(std::memory_order_relaxed); }
    // (docs/diagnosis/QUEST3_PERFORMANCE_AUDIT.md) the calling thread declared XR-critical to the runtime, once per session and kind,
    // with XR_KHR_android_thread_settings (Android only, when the runtime offers it): the runtime then schedules it as
    // its kind (Meta's runtime: priority and the performance cores). The game thread, the render thread and the
    // end-frame thread call it; a no-op elsewhere
    enum class XrThread : int { GameMain = 0, RenderMain = 1, RenderWorker = 2 };
    void declare_thread(XrThread kind);
    // (docs/diagnosis/QUEST3_IMPLEMENTATION_ROADMAP.md) the CPU's performance level hinted to the runtime with XR_EXT_performance_settings
    // (standalone builds, when offered): Boost while the game is stalled (loads, shaders: the render thread showing its
    // last frame again), back to Sustained High (the runtime's default) after. Render thread; a no-op elsewhere
    void cpu_boost(bool on);
    // (movement smoothing, vr_vulkan.cpp headset_frame) the next end_frame is the last frame shown again: send its depth
    // again with it (depth alone only; frame synthesis's motion vectors are not reused)
    void reuse_last_depth() { reuseDepth_.store(true, std::memory_order_relaxed); }
    bool depth_only() const { return depthOnly_; }
    // (poll_perf_metrics) the runtime's counters as last read (XR_META_performance_metrics, read once a second): the app's
    // and the compositor's GPU time a frame (ms), the GPU's and the busiest CPU core's use (%), the app's CPU time a frame
    // (ms); -1 where the runtime has none. valid: something read
    struct GpuCounters {
        bool valid = false;
        float appGpuMs = -1, compositorGpuMs = -1, gpuBusy = -1, appCpuMs = -1, cpuWorst = -1;
    };
    GpuCounters gpu_counters() const;
    // the optional controller profiles' bindings (create_actions): always on Android, on PC for a Quest only
#ifdef __ANDROID__
    bool optional_profiles() const { return true; }
#else
    bool optional_profiles() const { return quest_; }
#endif

private:
    bool hands_premultiplied_ = false;

    bool load_loader();
    bool check(XrResult r, const char* what);
    bool create_actions();
    bool create_swapchains(float renderScale);
    bool make_swapchain(Swapchain& sc, uint32_t width, uint32_t height, const char* what);
    void destroy_session();
    void set_state(State s);

    std::string why_;
    void* library_ = nullptr;
    PFN_xrGetInstanceProcAddr gipa_ = nullptr;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE;
    bool stage_ = false;
    bool quest_ = false;
    // (2026-11-13) the headset model for its own controller profile and calibration (vr.calibration.<model>.*)
    enum class ControllerModel { Other, Quest3, Quest3S };
    ControllerModel controllerModel_ = ControllerModel::Other;
    void apply_controller_calibration(int hand, Pose& grip, Pose& aim) const;
    std::atomic<bool> streamed_{false};
    bool vdxrDepthOffered_ = false;  // (vr.vdxrDepth) the depth extension enabled for VDXR, used once the runtime is known  // (streamed(); detection may set it on later, read by the render thread)
    bool wired_ = false;           // a headset known to be wired by its name (never streamed by detection)
    bool streamedForced_ = false;  // WWHD_VR_STREAMED chose it
    // (begin_frame) the first minute's display times of a headset neither known streamed nor wired: windows of
    // kDetectFrames steps against whole refresh periods; kDetectShare of them uneven makes it streamed
    struct StreamDetect {
        XrTime last = 0;
        uint64_t frames = 0, uneven = 0;
        int windows = 0;
        bool done = false;
    } streamDetect_;
    bool steamvr_ = false;
    std::string runtimeName_;  // the runtime's name, lower case (streamed_)
    // display-time steadiness, a streamed headset only (begin_frame, every 10 s): successive predicted display times
    // against whole refresh periods
    struct StreamDiag {
        XrTime last = 0;
        uint64_t frames = 0, uneven = 0;  // uneven: more than a quarter period off a whole number of periods
        double worstMs = 0, logAt = 0;    // the farthest off, ms
    } streamDiag_;
    uint32_t uiWaitWidth_ = 0, uiWaitHeight_ = 0, uiWaitFrames_ = 0;  // (ensure_ui) the UI image's size, and frames it held  // the runtime is SteamVR (its name says so): vr.eyeCrop's Auto
    VulkanRequirements requirements_;
    std::array<Swapchain, 13> swapchains_{};
    int64_t format_ = 0;
    bool refreshRateExt_ = false, cylinderExt_ = false;
    bool layerSettingsExt_ = false;  // XR_FB_composition_layer_settings (Meta's runtime)
    // frame synthesis (docs/VR_FRAME_SYNTHESIS.md): the extensions enabled, the backend (vr::framesynth::Backend),
    // depth on its own, the runtime's recommended motion vector size, and the next frame's data
    bool synthExt_ = false, spaceWarpExt_ = false, depthExt_ = false;
    int synthBackend_ = 0;
    bool depthOnly_ = false;
    uint32_t motionWidth_ = 0, motionHeight_ = 0;
    std::mutex synthMutex_;
    std::mutex submittedMutex_;
    Submitted submitted_[2];
    SynthFrame synthFrame_;
    bool synthPending_ = false;
    // (movement smoothing) the last frame's depth, for a frame shown again (reuse_last_depth): its images are the depth
    // swapchains' last released ones, the ones that frame wrote
    SynthFrame synthLast_;
    bool synthLastValid_ = false;
    std::atomic<bool> reuseDepth_{false};
    bool make_swapchain(Swapchain& sc, uint32_t width, uint32_t height, int64_t format, XrSwapchainUsageFlags usage,
                        const char* what);
    std::atomic<bool> superResolution_{false};
    // optional controller profiles, enabled when the runtime offers them (Android and PC runtimes alike: VDXR,
    // Meta's PC runtime and SteamVR may offer them too)
    bool touchPlusExt_ = false;  // XR_META_touch_controller_plus (Quest 3's controllers)
    bool frameControllerExt_ = false;  // XR_VALVE_frame_controller_interaction (Steam Frame's controllers)
    bool handInteractionExt_ = false;  // XR_EXT_hand_interaction (hands without controllers: Android XR, Quest)
    PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_ = nullptr;
    // (declare_thread) XR_KHR_android_thread_settings enabled and its function; the session's begin count, and the
    // count each kind of thread was last declared for
    bool threadSettingsExt_ = false;
#ifdef __ANDROID__
    PFN_xrSetAndroidApplicationThreadKHR xrSetAndroidApplicationThreadKHR_ = nullptr;
#endif
    std::atomic<uint32_t> sessionBegins_{0};
    // (cpu_boost) XR_EXT_performance_settings enabled, its function, the level asked for (and the session it was for)
    bool perfSettingsExt_ = false;
    // (docs/VR_PERFORMANCE.md, "GPU clock") XR_META_recommended_layer_resolution: asked each frame for the projection
    // layer's recommended size (not followed: the eye pictures stay at the Headset resolution); taking part in Meta's
    // dynamic resolution is what lets a Quest 3's GPU reach level 5, and with the manifest's CPU-for-GPU trade level 6
    bool layerResolutionExt_ = false;
    PFN_xrGetRecommendedLayerResolutionMETA xrGetRecommendedLayerResolutionMETA_ = nullptr;
    uint64_t layerResolutionAsks_ = 0;
    int32_t lastRecommendedW_ = 0, lastRecommendedH_ = 0;
    PFN_xrPerfSettingsSetPerformanceLevelEXT xrPerfSettingsSetPerformanceLevelEXT_ = nullptr;
    // (poll_perf_metrics) XR_META_performance_metrics
    void poll_perf_metrics();
    bool perfMetricsExt_ = false;
    PFN_xrEnumeratePerformanceMetricsCounterPathsMETA xrEnumeratePerformanceMetricsCounterPathsMETA_ = nullptr;
    PFN_xrSetPerformanceMetricsStateMETA xrSetPerformanceMetricsStateMETA_ = nullptr;
    PFN_xrQueryPerformanceMetricsCounterMETA xrQueryPerformanceMetricsCounterMETA_ = nullptr;
    uint32_t perfMetricsSession_ = ~0u;
    std::vector<XrPath> perfPaths_;
    std::vector<std::string> perfNames_;
    std::vector<double> perfSums_, perfMax_;
    uint32_t perfReads_ = 0;
    std::chrono::steady_clock::time_point perfLastRead_{}, perfLastLog_{};
    mutable std::mutex perfMutex_;
    GpuCounters perfLatest_;
    bool cpuBoost_ = false;
    uint32_t cpuBoostSession_ = 0;
    uint64_t cpuBoosts_ = 0;
    std::atomic<uint32_t> threadDeclared_[3] = {};
    std::array<XrViewConfigurationView, 2> configViews_{};
    std::atomic<State> state_{State::Unavailable};
    std::atomic<XrSessionState> xrState_{XR_SESSION_STATE_UNKNOWN};
    std::atomic<XrTime> predictedTime_{0};
    std::atomic<XrDuration> predictedPeriod_{11'111'111};
    std::atomic<XrDuration> refreshPeriod_{11'111'111};
    std::atomic<float> refreshHz_{0};  // (the headset's rate read at the last xrWaitFrame; 0: not read)
    bool frameBegun_ = false;
    // the frame xrWaitFrame returned, not yet begun (wait_frame writes, begin_frame reads after end_thread::wait)
    bool timeConvExt_ = false;
    PFN_xrVoidFunction timeConv_ = nullptr;
    std::atomic<bool> waited_{false};
    XrTime waitedTime_ = 0;
    XrDuration waitedPeriod_ = 0;
    bool waitedShouldRender_ = false;
    std::chrono::steady_clock::time_point waitedAt_{};
    std::mutex trackingMutex_;  // the game thread's locate/sync against session teardown
    // each hand's last valid grip and aim poses, held through short tracking gaps (vr_tracking.h)
    PoseHold gripHold_[2], aimHold_[2];
    StickDown rightStickDown_;  // the right stick held down (crouch), with hysteresis
    static constexpr double kHoldSeconds = 2.0;
    // the head through a gap in its tracking (locate_views): the last views with a valid position, and when (steady
    // clock, seconds)
    Views lastViews_;
    double lastViewsAt_ = -1;
    // a tracked head position farther from the last good one than a head can move (PSVR2 on SteamVR, tracking lost
    // looking at the floor: the runtime reported the head and controllers 30 to 40 m away, as tracked): the head is held
    // as in a gap, since `jumpSince_` (steady clock, seconds; -1: none), for up to kJumpAcceptSeconds
    // (reach: vr_tracking.h head_out_of_reach)
    static constexpr double kJumpAcceptSeconds = 10.0;  // still away then: the runtime moved its origin, taken
    static constexpr float kHandFromHeadMetres = 1.5f;  // a controller farther from the last good head: a gap
    double jumpSince_ = -1;
    uint64_t jumpsHeld_ = 0;
    // the head was held the last read (a gap or a jump), and back from it the view eases to the tracked position
    // (vr_tracking.h head_ease): held minus tracked, since when
    bool headHeld_ = false;
    Vec3 easeOffset_;
    double easeSince_ = -1;
    // tests (WWHD_VR_TEST_HEAD_JUMP, locate_views): the head's tracking faked wrong now and then; since when it runs
    double testJumpStart_ = -1;
    // tracking diagnostics, a Quest only (quest_diag_log, every 10 s): what the runtime reported and what the fixes did
    struct QuestDiag {
        uint64_t headTracked = 0, headTurned = 0, headFrozen = 0, headLost = 0;  // locate_views reads
        uint64_t handTracked[2] = {}, handEstimate[2] = {}, handLost[2] = {}, handHeld[2] = {};
        uint64_t syncs = 0, syncsReused = 0, spaceChanges = 0;
        XrTime lastTime = 0;
        double gapMaxMs = 0;  // the longest step between successive display times asked for
        double logAt = 0;
    } diag_;
    void quest_diag_log(double now);  // (trackingMutex_ held)
    void test_head_jump(double now, Views& v);  // (trackingMutex_ held; WWHD_VR_TEST_HEAD_JUMP)
    std::atomic<bool> spaceChanged_{false};
    // one input sync a headset frame (sync_input): the last result, for the display time it was read for
    InputState lastInput_;
    XrTime lastSyncTime_ = 0;
    bool lastSyncLeft_ = false, lastSyncOk_ = false;
    bool sync_input_locked(XrTime time, bool leftHanded, InputState& out);

    // actions
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE, stick_ = XR_NULL_HANDLE;
    XrAction primary_ = XR_NULL_HANDLE, secondary_ = XR_NULL_HANDLE, stickClick_ = XR_NULL_HANDLE;
    XrAction menu_ = XR_NULL_HANDLE, haptic_ = XR_NULL_HANDLE;
    XrPath hands_[2]{};
    XrSpace gripSpace_[2]{}, aimSpace_[2]{};

    // functions (loaded through xrGetInstanceProcAddr)
#define WWHD_XR_FUNCTIONS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) \
    X(xrDestroySpace) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrRequestExitSession) X(xrPollEvent) X(xrResultToString) X(xrWaitFrame) X(xrBeginFrame) \
    X(xrEndFrame) X(xrLocateViews) X(xrLocateSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrCreateActionSpace) \
    X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
    X(xrGetActionStatePose) X(xrApplyHapticFeedback) X(xrGetCurrentInteractionProfile) X(xrPathToString) \
    X(xrGetVulkanGraphicsRequirements2KHR) X(xrCreateVulkanInstanceKHR) X(xrGetVulkanGraphicsDevice2KHR) \
    X(xrCreateVulkanDeviceKHR)
#define WWHD_XR_MEMBER(name) PFN_##name name##_ = nullptr;
    WWHD_XR_FUNCTIONS(WWHD_XR_MEMBER)
#undef WWHD_XR_MEMBER
};

}  // namespace vr::xr
