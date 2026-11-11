// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// OpenXR session: see openxr_session.h.
#include "openxr_session.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <unistd.h>  // (gettid: declare_thread)
#endif

#include "platform/host.h"
#include "runtime.h"
#include "vr_platform.h"
#include "vr.h"
#include "vr_framesynth.h"

namespace vr::xr {
namespace {
Pose to_pose(const XrPosef& p) {
    return {normalize(Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}),
            Vec3{p.position.x, p.position.y, p.position.z}};
}
XrPosef identity_pose() {
    XrPosef p{};
    p.orientation.w = 1;
    return p;
}
bool tracked(XrSpaceLocationFlags f) {
    return (f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (f & XR_SPACE_LOCATION_POSITION_VALID_BIT);
}
}  // namespace

const char* state_name(State s) {
    switch (s) {
    case State::Unavailable: return "unavailable";
    case State::Idle: return "idle";
    case State::Ready: return "ready";
    case State::Running: return "running";
    case State::Stopping: return "stopping";
    case State::Lost: return "lost";
    }
    return "?";
}

bool Session::load_loader() {
    if (gipa_) return true;
#ifdef _WIN32
    HMODULE lib = LoadLibraryW(L"openxr_loader.dll");
    if (lib) gipa_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(lib, "xrGetInstanceProcAddr"));
    library_ = lib;
#else
    void* lib = nullptr;
#ifndef __ANDROID__
    // a loader shipped next to the game first (WWHD_VR_BUILD_LOADER; Steam Frame - Native, as SteamOS's read-only
    // system need not have one), as the VR launcher does; then the system's
    lib = dlopen((host::exe_dir() + "/libopenxr_loader.so.1").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib) LOG("[vr] OpenXR loader: the one next to the game");
#endif
#ifdef __ANDROID__
    constexpr const char* kSystemLoader = "the APK's";
#else
    constexpr const char* kSystemLoader = "the system's";
#endif
    if (!lib && (lib = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL)))
        LOG("[vr] OpenXR loader: libopenxr_loader.so.1 (%s)", kSystemLoader);
    if (!lib && (lib = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL)))
        LOG("[vr] OpenXR loader: libopenxr_loader.so (%s)", kSystemLoader);
    if (lib) gipa_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(lib, "xrGetInstanceProcAddr"));
    library_ = lib;
#endif
    if (!gipa_) {
        why_ = "the OpenXR loader (" +
#ifdef _WIN32
               std::string("openxr_loader.dll next to the game") +
#else
               std::string("libopenxr_loader.so.1, package openxr-loader / libopenxr-loader1") +
#endif
               ") was not found";
        return false;
    }
    return true;
}

bool Session::check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char text[XR_MAX_RESULT_STRING_SIZE] = "";
    if (instance_ && xrResultToString_) xrResultToString_(instance_, r, text);
    else std::snprintf(text, sizeof text, "%d", int(r));
    LOG("[vr] %s failed: %s", what, text);
    if (why_.empty()) why_ = std::string(what) + " failed: " + text;
    if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) set_state(State::Lost);
    return false;
}

void Session::set_state(State s) {
    const State old = state_.exchange(s);
    if (old != s) LOG("[vr] session %s -> %s", state_name(old), state_name(s));
}

#ifdef __ANDROID__
namespace {
// The app's Java VM and Activity (a global reference, kept for the process: OpenXR keeps using them), from SDL
struct AndroidApp {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
};
const AndroidApp& android_app() {
    static const AndroidApp app = [] {
        AndroidApp a;
        JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        jobject local = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
        if (env && local) {
            env->GetJavaVM(&a.vm);
            a.activity = env->NewGlobalRef(local);
            env->DeleteLocalRef(local);
        }
        return a;
    }();
    return app;
}
}  // namespace
#endif

bool Session::init(const std::string& appName) {
    why_.clear();
    if (instance_) return true;
    if (!load_loader()) return false;
#ifdef __ANDROID__
    // Android (docs/VR_ANDROID.md): the loader needs the app's Java VM and context before any other call
    // (XR_KHR_loader_init_android), and the instance the VM and Activity (XR_KHR_android_create_instance)
    const AndroidApp& app = android_app();
    if (!app.vm || !app.activity) { why_ = "the Android activity was not available to OpenXR"; return false; }
    {
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        gipa_(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
        static bool loaderInitialized = false;
        if (initializeLoader && !loaderInitialized) {
            XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            li.applicationVM = app.vm;
            li.applicationContext = app.activity;
            const XrResult lr = initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li));
            if (XR_FAILED(lr)) { why_ = "xrInitializeLoaderKHR failed (" + std::to_string(int(lr)) + ")"; return false; }
            loaderInitialized = true;
        }
    }
#endif
    PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    gipa_(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
          reinterpret_cast<PFN_xrVoidFunction*>(&enumerateExtensions));
    gipa_(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&createInstance));
    if (!enumerateExtensions || !createInstance) { why_ = "the OpenXR loader is incomplete"; return false; }
    uint32_t count = 0;
    if (XR_FAILED(enumerateExtensions(nullptr, 0, &count, nullptr))) {
        why_ = "no OpenXR runtime is installed or active (set one in its settings, e.g. SteamVR, Monado, WMR)";
        return false;
    }
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    enumerateExtensions(nullptr, count, &count, extensions.data());
    const bool vulkan2 = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    });
    if (!vulkan2) { why_ = "the OpenXR runtime does not support Vulkan (XR_KHR_vulkan_enable2)"; return false; }
    // optional: the headset's refresh rate can be read and chosen (Meta runtimes, Monado, some others)
    refreshRateExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    });
    cylinderExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    });
    // Quest Super Resolution: the projection layer's quality sharpening (docs/VR_UPSCALING.md). Only on Android:
    // Meta's standalone compositor; a PC runtime offering it keeps its own filtering as before
#ifdef __ANDROID__
    layerSettingsExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    });
#endif
    std::vector<const char*> enabled = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    if (layerSettingsExt_) enabled.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    LOG("[vr] Quest Super Resolution (XR_FB_composition_layer_settings): %s", layerSettingsExt_ ? "offered" : "not offered");
    {
        // frame synthesis and depth (docs/VR_FRAME_SYNTHESIS.md, vr_framesynth.h): what the runtime offers, chosen by
        // vr.frameSynthesis; the extensions enabled only for the choice
        auto offered = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(),
                               [name](const XrExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
        };
        framesynth::Caps caps;
        caps.ext = offered(XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME);
        caps.fb = offered(XR_FB_SPACE_WARP_EXTENSION_NAME);
        caps.depth = offered(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
#if defined(__ANDROID__) || defined(__aarch64__)
        caps.standalone = true;
#endif
        const framesynth::Choice choice = framesynth::choose(config().frameSynthesis, caps);
        synthBackend_ = int(choice.backend);
        synthExt_ = choice.backend == framesynth::Backend::ExtFrameSynthesis;
        spaceWarpExt_ = choice.backend == framesynth::Backend::FbSpaceWarp;
        depthOnly_ = choice.depth;
        // (Depth for Virtual Desktop, vr.vdxrDepth: the runtime is known only after the instance, so on a PC with
        // Streaming optimal the extension is enabled whenever offered and used only on VDXR, below)
        vdxrDepthOffered_ = kPcVrBuild && caps.depth && choice.backend == framesynth::Backend::None &&
                            config().streamingOptimal && config().vdxrDepth;
        depthExt_ = choice.depth || vdxrDepthOffered_;
        if (synthExt_) enabled.push_back(XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME);
        if (spaceWarpExt_) enabled.push_back(XR_FB_SPACE_WARP_EXTENSION_NAME);
        if (depthExt_) enabled.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        // the motion work starts with the renderer's device (motion_pass, the shader twins): wanted for a backend
        // (and for SGSR2, docs/VR_UPSCALING.md: its motion vectors and depth)
        framesynth::motion_flag().store(choice.backend != framesynth::Backend::None ||
                                            config().upscaler == upscale::Mode::Sgsr2,
                                        std::memory_order_relaxed);
        LOG("[vr] frame synthesis: XR_EXT_frame_synthesis %s, XR_FB_space_warp %s, XR_KHR_composition_layer_depth %s; "
            "chosen: %s", caps.ext ? "offered" : "not offered", caps.fb ? "offered" : "not offered",
            caps.depth ? "offered" : "not offered", choice.why);
        framesynth::set_status(choice.why);
    }
    if (refreshRateExt_) enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (cylinderExt_) enabled.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    // (cpu_boost) the CPU level hinted around stalls: standalone headsets only (a PC's clocks are its own), when offered
    perfSettingsExt_ = !kPcVrBuild && std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    });
    if (perfSettingsExt_) enabled.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
    const bool androidCreate = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    });
    if (!androidCreate) { why_ = "the OpenXR runtime does not support Android (XR_KHR_android_create_instance)"; return false; }
    enabled.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    // the game's XR-critical threads declared to the runtime (declare_thread), when offered (Meta's runtime; DrBeef's
    // Quest ports declare their main and render threads the same way)
    threadSettingsExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    });
    if (threadSettingsExt_) enabled.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
#endif
    // Quest 3's controllers (Touch Plus): their own profile, offered with its extension (the Touch profile works too).
    // Optional, on every platform: PC runtimes for Quest (VDXR, Meta's) may offer it as well
    touchPlusExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    });
    if (touchPlusExt_) enabled.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    // Steam Frame's controllers: their own profile, offered with Valve's extension (not in the pinned headers: its
    // name as Valve documents it, partner.steamgames.com/doc/steamhardware/steamframe/input). Without it the
    // runtime binds them as Touch controllers
    static const char* const kFrameControllerExt = "XR_VALVE_frame_controller_interaction";
    frameControllerExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, kFrameControllerExt);
    });
    if (frameControllerExt_) enabled.push_back(kFrameControllerExt);
    // hands without controllers (Android XR's usual input; Quest's hand tracking): their aim and grip, pinch, grasp
    handInteractionExt_ = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e) {
        return !std::strcmp(e.extensionName, XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    });
    if (handInteractionExt_) enabled.push_back(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    LOG("[vr] controller profiles offered: Touch Plus %s, hand interaction %s, Steam Frame %s",
        touchPlusExt_ ? "yes" : "no", handInteractionExt_ ? "yes" : "no", frameControllerExt_ ? "yes" : "no");
    {
        // which extensions a runtime offers differs between runtimes (Lepton's Android runtime drops cylinder layers,
        // SteamVR on Linux takes OpenXR 1.0 only): both lists in the log (names only)
        std::string offered, wanted;
        for (const auto& e : extensions) offered += (offered.empty() ? "" : " ") + std::string(e.extensionName);
        for (const char* e : enabled) wanted += (wanted.empty() ? "" : " ") + std::string(e);
        LOG("[vr] OpenXR extensions offered (%zu): %s", extensions.size(), offered.c_str());
        LOG("[vr] OpenXR extensions enabled (%zu): %s", enabled.size(), wanted.c_str());
    }
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = app.vm;
    androidInfo.applicationActivity = app.activity;
    ci.next = &androidInfo;
#endif
    std::snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "%s", appName.c_str());
    std::snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "WWHD");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;  // 1.0 functionality only: every 1.x runtime accepts it
    ci.enabledExtensionCount = uint32_t(enabled.size());
    ci.enabledExtensionNames = enabled.data();
    XrResult r = createInstance(&ci, &instance_);
#ifdef __ANDROID__
    // OpenXR 1.0 first (Steam Frame's Android runtime takes only 1.0); a runtime that takes only 1.1 (Android XR
    // implements 1.1) is asked again with 1.1: the game uses nothing of 1.1 either way (docs/VR_ANDROID.md)
    if (r == XR_ERROR_API_VERSION_UNSUPPORTED) {
        LOG("[vr] the OpenXR runtime does not take version 1.0; asking for 1.1");
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_1;
        r = createInstance(&ci, &instance_);
    }
#endif
    if (XR_FAILED(r)) {
        instance_ = XR_NULL_HANDLE;
        why_ = "xrCreateInstance failed (" + std::to_string(int(r)) + "): is the OpenXR runtime running?";
        return false;
    }
#define WWHD_XR_LOAD(name)                                                                     \
    if (XR_FAILED(gipa_(instance_, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name##_))) || !name##_) { \
        why_ = "the OpenXR runtime has no " #name;                                             \
        shutdown();                                                                            \
        return false;                                                                          \
    }
    WWHD_XR_FUNCTIONS(WWHD_XR_LOAD)
#undef WWHD_XR_LOAD
    if (refreshRateExt_) {
        gipa_(instance_, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateDisplayRefreshRatesFB_));
        gipa_(instance_, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetDisplayRefreshRateFB_));
        gipa_(instance_, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&xrRequestDisplayRefreshRateFB_));
        refreshRateExt_ = xrEnumerateDisplayRefreshRatesFB_ && xrGetDisplayRefreshRateFB_ && xrRequestDisplayRefreshRateFB_;
    }
    LOG("[vr] refresh rate selection (XR_FB_display_refresh_rate): %s; curved menus (XR_KHR_composition_layer_cylinder): %s",
        refreshRateExt_ ? "available" : "not offered by the runtime", cylinderExt_ ? "available" : "not offered");
    if (perfSettingsExt_) {
        gipa_(instance_, "xrPerfSettingsSetPerformanceLevelEXT",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrPerfSettingsSetPerformanceLevelEXT_));
        perfSettingsExt_ = xrPerfSettingsSetPerformanceLevelEXT_ != nullptr;
        LOG("[vr] CPU level hints (XR_EXT_performance_settings): %s",
            perfSettingsExt_ ? "available: Boost while the game is stalled (loads, shaders), else Sustained High"
                             : "not loadable");
    }
#ifdef __ANDROID__
    if (threadSettingsExt_) {
        gipa_(instance_, "xrSetAndroidApplicationThreadKHR",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrSetAndroidApplicationThreadKHR_));
        threadSettingsExt_ = xrSetAndroidApplicationThreadKHR_ != nullptr;
    }
    LOG("[vr] XR thread scheduling (XR_KHR_android_thread_settings): %s",
        threadSettingsExt_ ? "available: the game, render and end-frame threads are declared" : "not offered");
#endif
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties_(instance_, &ip))) {
        LOG("[vr] OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        // (vr.eyeCrop's Auto: SteamVR on Windows takes the whole symmetric image; VDXR, Meta's runtime and others the crop)
        std::string name = ip.runtimeName;
        for (char& c : name) c = char(std::tolower((unsigned char)c));
        steamvr_ = name.find("steamvr") != std::string::npos;
        runtimeName_ = name;
    }
    {
        // which build this is and what it runs on (vr_platform.h, docs/VR_STEAM_FRAME.md)
        const char* device = steam_frame_device();
        LOG("[vr] target: %s, %s%s%s", build_target(), build_arch(), device ? "; running on " : "", device ? device : "");
    }
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem_(instance_, &si, &system_);
    if (XR_FAILED(r)) {
        why_ = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? "no headset is connected (or the runtime cannot see it)"
                                                     : "xrGetSystem failed (" + std::to_string(int(r)) + ")";
        shutdown();
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties_(instance_, system_, &sp)))
        LOG("[vr] headset: %s (vendor %u), position tracking %u", sp.systemName, sp.vendorId,
            unsigned(sp.trackingProperties.positionTracking));
    {
        // "Oculus Quest2" (Meta's runtime, VDXR), "Meta Quest 3", "SteamVR/OpenXR : oculus" (a Quest through Link or
        // Steam Link on SteamVR)
        std::string name = sp.systemName;
        for (char& c : name) c = char(std::tolower((unsigned char)c));
        quest_ = name.find("quest") != std::string::npos ||
                 (name.find("steamvr") != std::string::npos && name.find("oculus") != std::string::npos);
        if (const char* e = std::getenv("WWHD_VR_QUEST_FIXES")) quest_ = std::atoi(e) != 0;
        LOG("[vr] Quest fixes (head and hand tracking gaps, recentre events, one input sync a frame, Quest 3 "
            "controllers on PC): %s", quest_ ? "on" : "off (not a Quest headset)");
        // a streamed headset (streamed()): on a PC, Virtual Desktop's VDXR, ALVR, or a Quest or Pico on any runtime;
        // a known wired one never (by its name: "SteamVR/OpenXR : playstation_vr2" for a PS VR2, "lighthouse" for an
        // Index or Vive); any other is measured in the session's first minute (begin_frame)
        const auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
        wired_ = has(name, "playstation_vr2") || has(name, "psvr2") || has(name, "lighthouse") || has(name, "index") ||
                 has(name, "vive") || has(name, "holographic") || has(name, "windows mixed reality") ||
                 has(name, "pimax") || has(name, "bigscreen");
        const bool pico = has(name, "pico") || has(runtimeName_, "pico");
        streamed_ = kPcVrBuild && !wired_ &&
                    (quest_ || pico || has(runtimeName_, "virtualdesktop") || has(runtimeName_, "alvr"));
        if (const char* e = std::getenv("WWHD_VR_STREAMED")) {
            streamed_ = std::atoi(e) != 0;
            streamedForced_ = true;
        }
        streamDetect_.done = !kPcVrBuild || streamed_ || wired_ || streamedForced_;
        LOG("[vr] streamed headset: %s", streamed_ ? "yes (the frame-timing shift off; Auto holds half the rate once there)"
                                         : streamedForced_ ? "no (WWHD_VR_STREAMED=0)"
                                         : wired_           ? "no (a wired headset)"
                                         : !kPcVrBuild      ? "no"
                                                            : "not known: its display times are measured for up to a minute");
        // vr.frameSynthesis auto on a PC with a Quest (VDXR, Link, Steam Link): no depth either (user, 2026-11-03; a
        // Quest 3 through VDXR showed no conversation text with depth sent). Chosen with the instance, before the
        // headset is known: the extension stays enabled, unused. On sends it as chosen
        if (kPcVrBuild && quest_ && depthOnly_ && config().frameSynthesis == framesynth::Mode::Auto) {
            depthOnly_ = false;
            const char* why = "off (auto: a Quest headset on a PC; vr.frameSynthesis=on sends depth)";
            LOG("[vr] frame synthesis: %s", why);
            framesynth::set_status(why);
        }
        // Depth for Virtual Desktop (vr.vdxrDepth, docs/VR_STREAMING.md): VDXR with Streaming optimal sends depth for
        // its reprojection and frame smoothing, whatever vr.frameSynthesis says (held while text, menus, the GamePad
        // screen or the item wheel show: end_frame)
        if (vdxrDepthOffered_ && !depthOnly_ && !synthBackend_ && has(runtimeName_, "virtualdesktop")) {
            depthOnly_ = true;
            const char* why = "depth for Virtual Desktop (VDXR, Streaming optimal: its reprojection and frame "
                              "smoothing get each eye's depth; not while text, menus or panels show)";
            LOG("[vr] frame synthesis: %s", why);
            framesynth::set_status(why);
        }
    }
    uint32_t views = 0;
    if (!check(xrEnumerateViewConfigurationViews_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                 &views, nullptr), "xrEnumerateViewConfigurationViews") ||
        views != 2) {
        if (why_.empty()) why_ = "the headset has no stereo view configuration";
        shutdown();
        return false;
    }
    for (auto& v : configViews_) v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    // (frame synthesis) the runtime's recommended motion vector and depth size, per view (the EXT) or for the system
    XrFrameSynthesisConfigViewEXT synthViews[2] = {{XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT},
                                                    {XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT}};
    if (synthExt_)
        for (int i = 0; i < 2; ++i) configViews_[i].next = &synthViews[i];
    xrEnumerateViewConfigurationViews_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views,
                                       configViews_.data());
    for (auto& v : configViews_) v.next = nullptr;
    if (synthExt_) {
        motionWidth_ = synthViews[0].recommendedMotionVectorImageRectWidth;
        motionHeight_ = synthViews[0].recommendedMotionVectorImageRectHeight;
    } else if (spaceWarpExt_) {
        XrSystemSpaceWarpPropertiesFB sw{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        props.next = &sw;
        if (XR_SUCCEEDED(xrGetSystemProperties_(instance_, system_, &props))) {
            motionWidth_ = sw.recommendedMotionVectorImageRectWidth;
            motionHeight_ = sw.recommendedMotionVectorImageRectHeight;
        }
    }
    if (synthBackend_)
        LOG("[vr] frame synthesis: recommended motion vector and depth size %ux%u", motionWidth_, motionHeight_);
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!check(xrGetVulkanGraphicsRequirements2KHR_(instance_, system_, &req), "xrGetVulkanGraphicsRequirements2KHR")) {
        shutdown();
        return false;
    }
    // XrVersion (major 16, minor 16, patch 32 bits) -> Vulkan API version
    auto vk = [](XrVersion v) {
        return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), 0);
    };
    requirements_ = {vk(req.minApiVersionSupported), vk(req.maxApiVersionSupported)};
    LOG("[vr] recommended eye image %ux%u, Vulkan %u.%u .. %u.%u", configViews_[0].recommendedImageRectWidth,
        configViews_[0].recommendedImageRectHeight, XR_VERSION_MAJOR(req.minApiVersionSupported),
        XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported),
        XR_VERSION_MINOR(req.maxApiVersionSupported));
    set_state(State::Idle);
    return true;
}

VkResult Session::create_vulkan_instance(const VkInstanceCreateInfo& ci, PFN_vkGetInstanceProcAddr gipa, VkInstance* out) {
    XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    info.systemId = system_;
    info.pfnGetInstanceProcAddr = gipa;
    info.vulkanCreateInfo = &ci;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!check(xrCreateVulkanInstanceKHR_(instance_, &info, out, &vr), "xrCreateVulkanInstanceKHR"))
        return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
}

VkPhysicalDevice Session::vulkan_physical_device(VkInstance instance) {
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = system_;
    info.vulkanInstance = instance;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (!check(xrGetVulkanGraphicsDevice2KHR_(instance_, &info, &device), "xrGetVulkanGraphicsDevice2KHR"))
        return VK_NULL_HANDLE;
    return device;
}

VkResult Session::create_vulkan_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& ci,
                                       PFN_vkGetInstanceProcAddr gipa, VkDevice* out) {
    XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    info.systemId = system_;
    info.pfnGetInstanceProcAddr = gipa;
    info.vulkanPhysicalDevice = physical;
    info.vulkanCreateInfo = &ci;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!check(xrCreateVulkanDeviceKHR_(instance_, &info, out, &vr), "xrCreateVulkanDeviceKHR"))
        return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
}

bool Session::begin(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                    uint32_t queueIndex, float renderScale) {
    if (!instance_ || session_) return session_ != XR_NULL_HANDLE;
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = queueIndex;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = system_;
    if (!check(xrCreateSession_(instance_, &ci, &session_), "xrCreateSession")) {
        session_ = XR_NULL_HANDLE;
        return false;
    }
    // STAGE (floor level, standing play) when the runtime has one, else LOCAL
    uint32_t n = 0;
    xrEnumerateReferenceSpaces_(session_, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    xrEnumerateReferenceSpaces_(session_, n, &n, spaces.data());
    stage_ = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = identity_pose();
    rs.referenceSpaceType = stage_ ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!check(xrCreateReferenceSpace_(session_, &rs, &space_), "xrCreateReferenceSpace")) { destroy_session(); return false; }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(xrCreateReferenceSpace_(session_, &rs, &view_), "xrCreateReferenceSpace (view)")) { destroy_session(); return false; }
    LOG("[vr] tracking space: %s", stage_ ? "STAGE" : "LOCAL");
    if (!create_actions() || !create_swapchains(renderScale)) { destroy_session(); return false; }
    set_state(State::Idle);
    return true;
}

bool Session::create_actions() {
    XrActionSetCreateInfo as{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(as.actionSetName, sizeof as.actionSetName, "gameplay");
    std::snprintf(as.localizedActionSetName, sizeof as.localizedActionSetName, "Gameplay");
    if (!check(xrCreateActionSet_(instance_, &as, &actionSet_), "xrCreateActionSet")) return false;
    xrStringToPath_(instance_, "/user/hand/left", &hands_[0]);
    xrStringToPath_(instance_, "/user/hand/right", &hands_[1]);
    auto make = [&](XrAction& a, XrActionType type, const char* name, const char* label) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        ai.actionType = type;
        std::snprintf(ai.actionName, sizeof ai.actionName, "%s", name);
        std::snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", label);
        ai.countSubactionPaths = 2;
        ai.subactionPaths = hands_;
        return check(xrCreateAction_(actionSet_, &ai, &a), name);
    };
    if (!make(gripPose_, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand grip") ||
        !make(aimPose_, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Hand aim") ||
        !make(trigger_, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") ||
        !make(squeeze_, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip") ||
        !make(stick_, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick") ||
        !make(primary_, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "Primary button") ||
        !make(secondary_, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Secondary button") ||
        !make(stickClick_, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click") ||
        !make(menu_, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu") ||
        !make(haptic_, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration"))
        return false;
    struct B { XrAction action; const char* path; };
    auto suggest = [&](const char* profile, std::initializer_list<B> list) {
        std::vector<XrActionSuggestedBinding> bindings;
        for (const auto& b : list) {
            XrPath p;
            if (XR_SUCCEEDED(xrStringToPath_(instance_, b.path, &p))) bindings.push_back({b.action, p});
        }
        XrPath profilePath;
        xrStringToPath_(instance_, profile, &profilePath);
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = profilePath;
        s.countSuggestedBindings = uint32_t(bindings.size());
        s.suggestedBindings = bindings.data();
        const XrResult r = xrSuggestInteractionProfileBindings_(instance_, &s);
        if (XR_FAILED(r)) LOG("[vr] bindings for %s not accepted (%d)", profile, int(r));
    };
    // Khronos simple controller: every runtime has it (select = trigger, menu)
    suggest("/interaction_profiles/khr/simple_controller", {
        {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
        {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
        {trigger_, "/user/hand/left/input/select/click"}, {trigger_, "/user/hand/right/input/select/click"},
        {menu_, "/user/hand/left/input/menu/click"}, {menu_, "/user/hand/right/input/menu/click"},
        {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    // Oculus Touch (Quest, Rift; many runtimes emulate it for other controllers)
    suggest("/interaction_profiles/oculus/touch_controller", {
        {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
        {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
        {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
        {squeeze_, "/user/hand/left/input/squeeze/value"}, {squeeze_, "/user/hand/right/input/squeeze/value"},
        {stick_, "/user/hand/left/input/thumbstick"}, {stick_, "/user/hand/right/input/thumbstick"},
        {stickClick_, "/user/hand/left/input/thumbstick/click"}, {stickClick_, "/user/hand/right/input/thumbstick/click"},
        {primary_, "/user/hand/left/input/x/click"}, {primary_, "/user/hand/right/input/a/click"},
        {secondary_, "/user/hand/left/input/y/click"}, {secondary_, "/user/hand/right/input/b/click"},
        {menu_, "/user/hand/left/input/menu/click"},
        {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    // Quest 3's Touch Plus controllers: the Touch profile's buttons, under their own profile (docs/VR_ANDROID.md)
    if (touchPlusExt_ && optional_profiles())
        suggest("/interaction_profiles/meta/touch_controller_plus", {
            {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
            {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
            {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
            {squeeze_, "/user/hand/left/input/squeeze/value"}, {squeeze_, "/user/hand/right/input/squeeze/value"},
            {stick_, "/user/hand/left/input/thumbstick"}, {stick_, "/user/hand/right/input/thumbstick"},
            {stickClick_, "/user/hand/left/input/thumbstick/click"}, {stickClick_, "/user/hand/right/input/thumbstick/click"},
            {primary_, "/user/hand/left/input/x/click"}, {primary_, "/user/hand/right/input/a/click"},
            {secondary_, "/user/hand/left/input/y/click"}, {secondary_, "/user/hand/right/input/b/click"},
            {menu_, "/user/hand/left/input/menu/click"},
            {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    // Steam Frame's controllers (their inputs as Valve lists them, partner.steamgames.com/doc/steamhardware/
    // steamframe/input): the right one has A, B, X, Y and a menu button, the left a D-pad and a view button. A and B
    // are the right one's; the left hand's X and Y (the game's X and Y) are the D-pad's up and left, where they sit on
    // the GamePad; the menu (Plus) is the left view button and the right menu button
    if (frameControllerExt_ && optional_profiles())
        suggest("/interaction_profiles/valve/frame_controller_valve", {
            {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
            {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
            {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
            {squeeze_, "/user/hand/left/input/squeeze/value"}, {squeeze_, "/user/hand/right/input/squeeze/value"},
            {stick_, "/user/hand/left/input/thumbstick"}, {stick_, "/user/hand/right/input/thumbstick"},
            {stickClick_, "/user/hand/left/input/thumbstick/click"}, {stickClick_, "/user/hand/right/input/thumbstick/click"},
            {primary_, "/user/hand/left/input/dpad_up/click"}, {primary_, "/user/hand/right/input/a/click"},
            {secondary_, "/user/hand/left/input/dpad_left/click"}, {secondary_, "/user/hand/right/input/b/click"},
            {menu_, "/user/hand/left/input/view/click"}, {menu_, "/user/hand/right/input/menu/click"},
            {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    // hands (XR_EXT_hand_interaction, /interaction_profiles/ext/hand_interaction_ext): the arms follow them, a pinch is
    // the trigger, a grasp the grip. Hands have no buttons or sticks: moving and the game's buttons need a controller
    if (handInteractionExt_ && optional_profiles())
        suggest("/interaction_profiles/ext/hand_interaction_ext", {
            {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
            {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
            {trigger_, "/user/hand/left/input/pinch_ext/value"}, {trigger_, "/user/hand/right/input/pinch_ext/value"},
            {squeeze_, "/user/hand/left/input/grasp_ext/value"}, {squeeze_, "/user/hand/right/input/grasp_ext/value"}});
    // Valve Index (no menu button: bind one in the runtime's binding editor, docs/VR_CONTROLS.md)
    suggest("/interaction_profiles/valve/index_controller", {
        {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
        {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
        {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
        {squeeze_, "/user/hand/left/input/squeeze/value"}, {squeeze_, "/user/hand/right/input/squeeze/value"},
        {stick_, "/user/hand/left/input/thumbstick"}, {stick_, "/user/hand/right/input/thumbstick"},
        {stickClick_, "/user/hand/left/input/thumbstick/click"}, {stickClick_, "/user/hand/right/input/thumbstick/click"},
        {primary_, "/user/hand/left/input/a/click"}, {primary_, "/user/hand/right/input/a/click"},
        {secondary_, "/user/hand/left/input/b/click"}, {secondary_, "/user/hand/right/input/b/click"},
        {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    // HTC Vive wands: the trackpad as the stick
    suggest("/interaction_profiles/htc/vive_controller", {
        {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
        {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
        {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
        {squeeze_, "/user/hand/left/input/squeeze/click"}, {squeeze_, "/user/hand/right/input/squeeze/click"},
        {stick_, "/user/hand/left/input/trackpad"}, {stick_, "/user/hand/right/input/trackpad"},
        {stickClick_, "/user/hand/left/input/trackpad/click"}, {stickClick_, "/user/hand/right/input/trackpad/click"},
        {menu_, "/user/hand/left/input/menu/click"}, {secondary_, "/user/hand/right/input/menu/click"},
        {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    // Windows Mixed Reality
    suggest("/interaction_profiles/microsoft/motion_controller", {
        {gripPose_, "/user/hand/left/input/grip/pose"}, {gripPose_, "/user/hand/right/input/grip/pose"},
        {aimPose_, "/user/hand/left/input/aim/pose"}, {aimPose_, "/user/hand/right/input/aim/pose"},
        {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
        {squeeze_, "/user/hand/left/input/squeeze/click"}, {squeeze_, "/user/hand/right/input/squeeze/click"},
        {stick_, "/user/hand/left/input/thumbstick"}, {stick_, "/user/hand/right/input/thumbstick"},
        {stickClick_, "/user/hand/left/input/thumbstick/click"}, {stickClick_, "/user/hand/right/input/thumbstick/click"},
        {primary_, "/user/hand/left/input/trackpad/click"}, {primary_, "/user/hand/right/input/trackpad/click"},
        {menu_, "/user/hand/left/input/menu/click"}, {secondary_, "/user/hand/right/input/menu/click"},
        {haptic_, "/user/hand/left/output/haptic"}, {haptic_, "/user/hand/right/output/haptic"}});
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    if (!check(xrAttachSessionActionSets_(session_, &attach), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.poseInActionSpace = identity_pose();
        sc.subactionPath = hands_[h];
        sc.action = gripPose_;
        if (!check(xrCreateActionSpace_(session_, &sc, &gripSpace_[h]), "xrCreateActionSpace (grip)")) return false;
        sc.action = aimPose_;
        if (!check(xrCreateActionSpace_(session_, &sc, &aimSpace_[h]), "xrCreateActionSpace (aim)")) return false;
    }
    return true;
}

bool Session::create_swapchains(float renderScale) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats_(session_, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats_(session_, n, &n, formats.data());
    // sRGB first: the compositor then reads the colours the window shows (the blit converts linear scans)
    const int64_t wanted[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_FORMAT_B8G8R8A8_UNORM};
    int64_t format = 0;
    for (int64_t w : wanted)
        if (std::find(formats.begin(), formats.end(), w) != formats.end()) { format = w; break; }
    if (!format) { why_ = "the OpenXR runtime offers no 8-bit RGBA swapchain format"; LOG("[vr] %s", why_.c_str()); return false; }
    format_ = format;
    for (int eye = 0; eye < 2; ++eye) {
        const auto& v = configViews_[eye];
        if (!make_swapchain(swapchains_[eye],
                            std::clamp(uint32_t(std::lround(v.recommendedImageRectWidth * renderScale)), 64u, v.maxImageRectWidth),
                            std::clamp(uint32_t(std::lround(v.recommendedImageRectHeight * renderScale)), 64u, v.maxImageRectHeight),
                            eye ? "right eye" : "left eye"))
            return false;
    }
    // frame synthesis and depth (docs/VR_FRAME_SYNTHESIS.md): per eye a motion vector image (RGBA16F) and a depth
    // image, at the runtime's recommended size (else a quarter of the eye's for motion, half for depth alone). A
    // runtime without these formats, or a failure here, turns the feature off; the eyes are unaffected
    if (synthBackend_ || depthOnly_) {
        auto has = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
        int64_t depthFormat = 0;
        for (int64_t f : {int64_t(VK_FORMAT_D24_UNORM_S8_UINT), int64_t(VK_FORMAT_D32_SFLOAT),
                          int64_t(VK_FORMAT_X8_D24_UNORM_PACK32), int64_t(VK_FORMAT_D16_UNORM)})
            if (has(f)) { depthFormat = f; break; }
        const bool motionFormat = has(VK_FORMAT_R16G16B16A16_SFLOAT);
        const uint32_t ew = swapchains_[0].width, eh = swapchains_[0].height;
        const uint32_t w = motionWidth_ ? motionWidth_ : std::max(64u, synthBackend_ ? ew / 4 : ew / 2);
        const uint32_t h = motionHeight_ ? motionHeight_ : std::max(64u, synthBackend_ ? eh / 4 : eh / 2);
        bool ok = depthFormat && (!synthBackend_ || motionFormat);
        for (int eye = 0; ok && eye < 2; ++eye) {
            if (synthBackend_)
                ok = make_swapchain(swapchains_[kMotion + eye], w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
                                    eye ? "right motion vectors" : "left motion vectors");
            if (ok)
                ok = make_swapchain(swapchains_[kDepth + eye], w, h, depthFormat,
                                    XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
                                    eye ? "right depth" : "left depth");
        }
        if (!ok) {
            LOG("[vr] frame synthesis: its swapchains could not be made (depth format %lld, RGBA16F %s): off",
                (long long)depthFormat, motionFormat ? "offered" : "not offered");
            for (int i : {kMotion, kMotion + 1, kDepth, kDepth + 1})
                if (swapchains_[i].handle) { xrDestroySwapchain_(swapchains_[i].handle); swapchains_[i] = {}; }
            synthBackend_ = 0;
            depthOnly_ = false;
        }
    }
    return true;
}

bool Session::ensure_panel(uint32_t width, uint32_t height) {
    auto& sc = swapchains_[kPanel];
    if (sc.handle && sc.width == width && sc.height == height) return true;
    if (!session_ || !format_ || !width || !height) return false;
    if (sc.handle) {
        if (sc.acquired != UINT32_MAX) return true;  // in use this frame: the new size waits for the next one
        xrDestroySwapchain_(sc.handle);
        sc = {};
    }
    return make_swapchain(sc, width, height, "GamePad panel");
}

bool Session::ensure_ui(uint32_t width, uint32_t height) {
    // made once, at the UI image's size once that has held for 30 frames (copied 1:1; made at the first frame's size,
    // before the game's aspect settled, it squashed the UI). Never destroyed and made again mid-session: SteamVR hung
    // there (PS VR2, 2026-11-03, frame 3); a later size scales in the copy
    auto& sc = swapchains_[kUi];
    if (sc.handle) return true;
    if (!session_ || !format_ || !width || !height) return false;
    if (width != uiWaitWidth_ || height != uiWaitHeight_) {
        uiWaitWidth_ = width, uiWaitHeight_ = height;
        uiWaitFrames_ = 0;
    }
    if (++uiWaitFrames_ < 30) return false;
    return make_swapchain(sc, width, height, "game UI layer");
}
bool Session::ensure_wheel(uint32_t width, uint32_t height) {
    auto& sc = swapchains_[kWheel];
    if (sc.handle && sc.width == width && sc.height == height) return true;
    if (!session_ || !format_ || !width || !height || sc.handle) return false;  // one size, made once
    return make_swapchain(sc, width, height, "item wheel");
}

bool Session::ensure_wrist(uint32_t width, uint32_t height) {
    auto& sc = swapchains_[kWrist];
    if (sc.handle && sc.width == width && sc.height == height) return true;
    if (!session_ || !format_ || !width || !height || sc.handle) return false;  // one size, made once
    return make_swapchain(sc, width, height, "wrist HUD");
}

bool Session::ensure_laser(uint32_t width, uint32_t height) {
    auto& sc = swapchains_[kLaser];
    if (sc.handle && sc.width == width && sc.height == height) return true;
    if (!session_ || !format_ || !width || !height || sc.handle) return false;  // one size, made once
    return make_swapchain(sc, width, height, "aiming laser");
}

bool Session::make_swapchain(Swapchain& sc, uint32_t width, uint32_t height, const char* what) {
    return make_swapchain(sc, width, height, format_,
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                              XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
                          what);
}

bool Session::make_swapchain(Swapchain& sc, uint32_t width, uint32_t height, int64_t format, XrSwapchainUsageFlags usage,
                             const char* what) {
    {
        sc.width = width;
        sc.height = height;
        sc.format = format;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = usage;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = sc.width;
        ci.height = sc.height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!check(xrCreateSwapchain_(session_, &ci, &sc.handle), "xrCreateSwapchain")) return false;
        uint32_t count = 0;
        xrEnumerateSwapchainImages_(sc.handle, 0, &count, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (!check(xrEnumerateSwapchainImages_(sc.handle, count, &count,
                                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                   "xrEnumerateSwapchainImages"))
            return false;
        sc.images.clear();
        for (const auto& i : images) sc.images.push_back(i.image);
        LOG("[vr] %s swapchain %ux%u format %lld, %u images", what, sc.width, sc.height, (long long)format, count);
    }
    return true;
}

std::vector<float> Session::refresh_rates() {
    std::vector<float> rates;
    if (!refreshRateExt_ || !session_) return rates;
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateDisplayRefreshRatesFB_(session_, 0, &n, nullptr)) || !n) return rates;
    rates.resize(n);
    if (XR_FAILED(xrEnumerateDisplayRefreshRatesFB_(session_, n, &n, rates.data()))) n = 0;
    rates.resize(n);
    return rates;
}

float Session::refresh_rate() {
    float hz = 0;
    if (refreshRateExt_ && session_ && XR_SUCCEEDED(xrGetDisplayRefreshRateFB_(session_, &hz)) && hz > 0) return hz;
    const XrDuration period = predictedPeriod_.load();
    return period > 0 ? float(1e9 / double(period)) : 0.0f;
}

bool Session::request_refresh_rate(float hz) {
    if (!refreshRateExt_ || !session_ || !(hz > 0)) return false;
    const auto rates = refresh_rates();
    std::string list;
    for (float r : rates) list += (list.empty() ? "" : ", ") + std::to_string(int(std::lround(r)));
    float match = 0;
    for (float r : rates)
        if (std::fabs(r - hz) < 0.6f && (match == 0 || std::fabs(r - hz) < std::fabs(match - hz))) match = r;
    if (match == 0) {
        LOG("[vr] refresh rates offered: %s Hz; %.0f Hz is not among them: the headset stays at %.0f Hz", list.c_str(),
            double(hz), double(refresh_rate()));
        return false;
    }
    const XrResult r = xrRequestDisplayRefreshRateFB_(session_, match);
    LOG("[vr] refresh rates offered: %s Hz; asked for %.0f Hz (%s)", list.c_str(), double(match),
        XR_SUCCEEDED(r) ? "accepted" : "refused");
    return XR_SUCCEEDED(r);
}

void Session::poll_events() {
    if (!instance_) return;
    XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent_(instance_, &e) == XR_SUCCESS) {
        switch (e.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& s = *reinterpret_cast<XrEventDataSessionStateChanged*>(&e);
            static const char* const names[] = {"unknown", "idle", "ready", "synchronized", "visible", "focused",
                                                "stopping", "loss pending", "exiting"};
            const int n = int(s.state);
            LOG("[vr] runtime session state: %s", n >= 0 && n < 9 ? names[n] : "?");
            xrState_ = s.state;
            if (s.state == XR_SESSION_STATE_IDLE) vr::set_xr_session_idle();
            vr::set_xr_unfocused(s.state != XR_SESSION_STATE_FOCUSED);  // (vr.h headset_away: standalone headsets)
            switch (s.state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (check(xrBeginSession_(session_, &bi), "xrBeginSession")) {
                    sessionBegins_.fetch_add(1, std::memory_order_relaxed);  // (declare_thread: again for this session)
                    set_state(State::Running);
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                check(xrEndSession_(session_), "xrEndSession");
                set_state(State::Idle);
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                // the runtime ends the session (headset removed, runtime quitting): back to the desktop
                LOG("[vr] the runtime ended the session (%s)",
                    s.state == XR_SESSION_STATE_EXITING ? "exiting" : "loss pending");
                set_state(State::Lost);
                break;
            default: break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOG("[vr] the OpenXR instance will be lost (the runtime is going away)");
            set_state(State::Lost);
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& r = *reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&e);
            LOG("[vr] headset refresh rate %.0f -> %.0f Hz", double(r.fromDisplayRefreshRate), double(r.toDisplayRefreshRate));
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            // a recentre on the headset (Quest: the Meta button held, or the headset put on again) or the boundary set
            // up again moves the origin of LOCAL or STAGE: the game recentres on it (vr_game.cpp), and nothing held
            // from before it (the head's or the hands' last poses) is shown in the new space
            const auto& c = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&e);
            const XrReferenceSpaceType ours = stage_ ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
            LOG("[vr] the runtime moves the %s space's origin%s", c.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE"
                : c.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL" : "VIEW",
                !quest_ ? " (not a Quest: nothing to do)" : c.referenceSpaceType == ours ? ": recentring"
                                                     : " (not the tracking space: nothing to do)");
            if (c.referenceSpaceType == ours) {
                // (every headset: the head's last good position is from the old space, so a jump to the new one is
                // no tracking error: locate_views)
                std::lock_guard<std::mutex> lock(trackingMutex_);
                lastViews_.valid = false;
                jumpSince_ = -1;
                for (int h = 0; h < 2; ++h) gripHold_[h] = {}, aimHold_[h] = {};
                if (quest_) {
                    lastSyncTime_ = 0;
                    spaceChanged_ = true;
                    ++diag_.spaceChanges;
                }
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            LOG("[vr] controllers changed: left %s, right %s", interaction_profile(0).c_str(), interaction_profile(1).c_str());
            break;
        default: break;
        }
        e = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool Session::begin_frame(bool& shouldRender) {
    shouldRender = false;
    if (!running()) return false;
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!check(xrWaitFrame_(session_, &wi, &fs), "xrWaitFrame")) return false;
    predictedTime_ = fs.predictedDisplayTime;
    if (fs.predictedDisplayPeriod > 0) predictedPeriod_ = fs.predictedDisplayPeriod;
    if (!streamDetect_.done) {
        // a headset neither known streamed nor wired (streamed()): its display times in windows of kDetectFrames steps.
        // A streamed runtime's move with the network (a Quest through VDXR on Wi-Fi: 24 to 155 ms apart); a wired one's
        // are whole refresh periods apart, a late frame included. kDetectShare uneven in a window: streamed from then on;
        // kDetectWindows steady windows: not (and no more measuring)
        constexpr uint64_t kDetectFrames = 900;  // (10 s at 90 Hz)
        constexpr double kDetectShare = 0.05;
        constexpr int kDetectWindows = 4;
        auto& d = streamDetect_;
        const double period = double(predictedPeriod_.load());
        if (d.last > 0 && fs.predictedDisplayTime > d.last && period > 0) {
            const double step = double(fs.predictedDisplayTime - d.last);
            ++d.frames;
            if (std::fabs(step - std::max(1.0, std::round(step / period)) * period) > 0.25 * period) ++d.uneven;
        }
        d.last = fs.predictedDisplayTime;
        if (d.frames >= kDetectFrames) {
            const double share = double(d.uneven) / double(d.frames);
            ++d.windows;
            if (share >= kDetectShare) {
                streamed_ = true;
                d.done = true;
                LOG("[vr] streamed headset: yes, from its display times (%.1f%% of %llu frames uneven, window %d): the "
                    "frame-timing shift off; Auto holds half the rate once there", 100.0 * share,
                    (unsigned long long)d.frames, d.windows);
            } else if (d.windows >= kDetectWindows) {
                d.done = true;
                LOG("[vr] streamed headset: no (display times steady: %.1f%% uneven in the last of %d windows)",
                    100.0 * share, d.windows);
            }
            d.frames = d.uneven = 0;
        }
    }
    if (streamed_) {
        // display-time steadiness (docs/VR_STREAMING.md): each step from the last display time against whole periods; a
        // streamed headset's times move with the network (a Quest through VDXR on Wi-Fi: up to 155 ms apart)
        auto& d = streamDiag_;
        const double period = double(predictedPeriod_.load());
        if (d.last > 0 && fs.predictedDisplayTime > d.last && period > 0) {
            const double step = double(fs.predictedDisplayTime - d.last);
            const double off = std::fabs(step - std::max(1.0, std::round(step / period)) * period);
            ++d.frames;
            if (off > 0.25 * period) ++d.uneven;
            d.worstMs = std::max(d.worstMs, off * 1e-6);
        }
        d.last = fs.predictedDisplayTime;
        const double now = double(fs.predictedDisplayTime) * 1e-9;
        if (d.logAt == 0) d.logAt = now;
        if (now - d.logAt >= 10.0 && d.frames) {
            LOG("[vr] streamed display times (10 s): %llu frames, %llu uneven (%.1f%%, over a quarter refresh off a whole "
                "number of refreshes), the farthest %.1f ms off", (unsigned long long)d.frames,
                (unsigned long long)d.uneven, 100.0 * double(d.uneven) / double(d.frames), d.worstMs);
            const XrTime last = d.last;
            d = {};
            d.last = last;
            d.logAt = now;
        }
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult r = xrBeginFrame_(session_, &bi);
    if (!check(r, "xrBeginFrame")) return false;
    frameBegun_ = true;
    shouldRender = fs.shouldRender == XR_TRUE;
    return true;
}

VkImage Session::acquire(int eye) {
    auto& sc = swapchains_[eye];
    if (!sc.handle) return VK_NULL_HANDLE;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    if (!check(xrAcquireSwapchainImage_(sc.handle, &ai, &index), "xrAcquireSwapchainImage")) return VK_NULL_HANDLE;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100'000'000;  // 100 ms: a compositor that stopped must not hang the game
    const XrResult r = xrWaitSwapchainImage_(sc.handle, &wi);
    if (r == XR_TIMEOUT_EXPIRED || !check(r, "xrWaitSwapchainImage")) {
        if (r == XR_TIMEOUT_EXPIRED) LOG("[vr] eye %d swapchain image wait timed out", eye);
        // an acquired image must still be released (the wait may be retried, but not here)
        sc.acquired = index;
        release(eye);
        return VK_NULL_HANDLE;
    }
    sc.acquired = index;
    return sc.images[index];
}

void Session::release(int eye) {
    auto& sc = swapchains_[eye];
    if (sc.acquired == UINT32_MAX) return;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(xrReleaseSwapchainImage_(sc.handle, &ri), "xrReleaseSwapchainImage");
    sc.acquired = UINT32_MAX;
}

void Session::end_frame(const Views& views, bool draw, const Quad* panel, const Quad* wheel, const SubQuad* wrist,
                        int wristCount, const SubQuad* laser, int laserCount, const Quad* ui) {
    if (!frameBegun_) return;
    frameBegun_ = false;
    XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    // frame synthesis and depth (docs/VR_FRAME_SYNTHESIS.md): this frame's, taken once (a frame shown again has none)
    SynthFrame synth;
    bool synthNow = false;
    {
        std::lock_guard<std::mutex> l(synthMutex_);
        if (synthPending_) { synth = synthFrame_; synthNow = true; synthPending_ = false; }
    }
    XrFrameSynthesisInfoEXT synthInfo[2] = {{XR_TYPE_FRAME_SYNTHESIS_INFO_EXT}, {XR_TYPE_FRAME_SYNTHESIS_INFO_EXT}};
    XrCompositionLayerSpaceWarpInfoFB warpInfo[2] = {{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},
                                                     {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}};
    XrCompositionLayerDepthInfoKHR depthInfo[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                   {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    XrCompositionLayerQuad wheelQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad wristQuads[5] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD},
                                             {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD},
                                             {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerQuad laserQuads[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerQuad uiQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[12] = {};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = predictedTime_.load();
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (draw && views.valid) {
#ifdef _WIN32
        constexpr bool kLinuxBuild = false;  // (vr.eyeCrop's Auto: every runtime but SteamVR on Windows)
#else
        constexpr bool kLinuxBuild = true;
#endif
        const bool eyeCrop = eye_crop_on(config().eyeCrop, kLinuxBuild, steamvr_);
        static int eyeCropLogged = -1;
        if (int(eyeCrop) != eyeCropLogged) {
            eyeCropLogged = int(eyeCrop);
            LOG("[vr] eyes submitted %s (vr.eyeCrop %s, %s runtime)",
                eyeCrop ? "as their own fields of view (cropped)" : "as drawn (one symmetric field of view)",
                config().eyeCrop == EyeCrop::On ? "on" : config().eyeCrop == EyeCrop::Off ? "off" : "auto",
                steamvr_ ? "a SteamVR" : "a non-SteamVR");
        }
        for (int eye = 0; eye < 2; ++eye) {
            const auto& v = views.eye[eye];
            pv[eye].pose.orientation = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w};
            pv[eye].pose.position = {v.pose.position.x, v.pose.position.y, v.pose.position.z};
            // (the picture at its own size in the image's top-left corner: only that part, the compositor's Super
            // Resolution upscaling it to the display; an eye crop is then a part of that part)
            const bool part = v.rectWidth > 0 && v.rectHeight > 0 && v.rectWidth <= int(swapchains_[eye].width) &&
                              v.rectHeight <= int(swapchains_[eye].height);
            const int width = part ? v.rectWidth : int(swapchains_[eye].width);
            const int height = part ? v.rectHeight : int(swapchains_[eye].height);
            pv[eye].fov = {v.fov.left, v.fov.right, v.fov.up, v.fov.down};
            pv[eye].subImage.swapchain = swapchains_[eye].handle;
            pv[eye].subImage.imageRect = {{0, 0}, {width, height}};
            const bool asDrawn = v.display.left == 0 && v.display.right == 0 && v.display.up == 0 && v.display.down == 0;
            // vr.eyeCrop (vr_config.h; docs/VR_MENU.md, "Double vision on Linux", "Bubble vision on Quest and VDXR")
            if (eyeCrop && !asDrawn) {
                // only the part showing this eye's own field of view, with that part's exact field of view: the
                // display's projection, which the runtime shows as it is (the quad layers then line up with it)
                // (with the margin beyond it, vr::fov_margin: as_rendered widens `display` with the drawn field of view)
                const FovCrop c = fov_crop(v.fov, v.display, width, height);
                if (c.width > 0 && c.height > 0) {
                    pv[eye].fov = {c.fov.left, c.fov.right, c.fov.up, c.fov.down};
                    pv[eye].subImage.imageRect = {{c.x, c.y}, {c.width, c.height}};
                    static bool logged[2] = {};
                    if (!logged[eye]) {
                        logged[eye] = true;
                        LOG("[vr] %s eye submitted as its own field of view: pixels %d..%d x %d..%d of %dx%d",
                            eye ? "right" : "left", c.x, c.x + c.width, c.y, c.y + c.height, width, height);
                    }
                }
            }
            std::lock_guard<std::mutex> l(submittedMutex_);
            const auto& r = pv[eye].subImage.imageRect;
            submitted_[eye] = {{pv[eye].fov.angleLeft, pv[eye].fov.angleRight, pv[eye].fov.angleUp, pv[eye].fov.angleDown},
                               r.offset.x, r.offset.y, r.extent.width, r.extent.height, true};
        }
        // (frame synthesis) each eye's motion vectors and depth with its view, or its depth alone; only for a frame whose
        // images the renderer wrote, with valid planes (nearZ == farZ is invalid)
        const bool planesOk = synthNow && synth.nearZ > 0 && synth.farZ > synth.nearZ;
        // the depth alone (XR_KHR_composition_layer_depth) not while the game's UI layer shows text or a menu (its layouts
        // drawn shrunk: vr::layout_scale): a runtime may composite the layers by it, and the UI quad (kUiDistance, 3.5 m)
        // went behind the scene nearer than that. Quest 3 through VDXR, 2026-11-03: no conversation text in first person
        // with depth sent (the person talked to, 1 to 2 m away, over it); menus send no depth and showed. Normal play's
        // HUD (full size, mostly on the wrists) keeps it. Only reprojection's depth hint is lost meanwhile
        // (and while the GamePad screen or the item wheel is shown: quads in front of the scene too, 2026-11-05)
        const bool uiShown = (ui && ui->visible && swapchains_[kUi].handle && view_ && vr::layout_scale() < 1.0f) ||
                             (depthOnly_ && ((panel && panel->visible) || (wheel && wheel->visible)));
        {
            static int logged = -1;
            const bool held = depthOnly_ && planesOk && uiShown;
            if (depthOnly_ && planesOk && int(held) != logged) {
                logged = int(held);
                LOG("[vr frame synthesis] depth %s", held ? "not sent while the game's UI layer is shown (text, menus)"
                                                          : "sent again (no game UI shown)");
            }
        }
        const XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};
        for (int eye = 0; eye < 2 && planesOk; ++eye) {
            const auto& ms = swapchains_[kMotion + eye];
            const auto& ds = swapchains_[kDepth + eye];
            // the same part of the picture as the eye's colour subimage (its own field of view's crop, scaled): the
            // view's fov describes that part, and the motion and depth must match it
            const auto& es = swapchains_[eye];
            const XrRect2Di& cr = pv[eye].subImage.imageRect;
            auto part = [&](const Swapchain& sc) {
                if (!es.width || !es.height) return XrRect2Di{{0, 0}, {int32_t(sc.width), int32_t(sc.height)}};
                const double kx = double(sc.width) / es.width, ky = double(sc.height) / es.height;
                const int32_t x = int32_t(std::lround(cr.offset.x * kx)), y = int32_t(std::lround(cr.offset.y * ky));
                const int32_t w = std::clamp(int32_t(std::lround(cr.extent.width * kx)), 1, int32_t(sc.width) - x);
                const int32_t h = std::clamp(int32_t(std::lround(cr.extent.height * ky)), 1, int32_t(sc.height) - y);
                return XrRect2Di{{x, y}, {w, h}};
            };
            const XrSwapchainSubImage motionImage{ms.handle, part(ms), 0};
            const XrSwapchainSubImage depthImage{ds.handle, part(ds), 0};
            if (has_synthesis_swapchains() && synth.motion[eye] && synth.depth[eye]) {
                if (synthExt_) {
                    auto& f = synthInfo[eye];
                    f.motionVectorSubImage = motionImage;
                    // (the renderer writes NDC current - previous of the whole eye image; a cropped eye's view is a part
                    // of it, whose NDC spans full/part as much: a motion of d in the image's NDC is d * full / part in
                    // the view's. 1 when the whole image is submitted: each eye's own projection, the default)
                    const float kx = cr.extent.width > 0 && es.width ? float(es.width) / float(cr.extent.width) : 1.0f;
                    const float ky = cr.extent.height > 0 && es.height ? float(es.height) / float(cr.extent.height) : 1.0f;
                    f.motionVectorScale = {kx, ky, 1, 0};
                    f.motionVectorOffset = {0, 0, 0, 0};
                    f.appSpaceDeltaPose = identity;      // (the motion vectors carry the locomotion: docs)
                    f.depthSubImage = depthImage;
                    f.minDepth = 0, f.maxDepth = 1, f.nearZ = synth.nearZ, f.farZ = synth.farZ;
                    pv[eye].next = &f;
                } else if (spaceWarpExt_) {
                    // (XR_FB_space_warp has no motion scale: a cropped eye's motion stays in the whole image's NDC, too
                    // small by part / full; only with vr.eyeCrop on and WWHD_VR_EYE_PROJECTION=shared, and only where
                    // the runtime lacks XR_EXT_frame_synthesis)
                    auto& f = warpInfo[eye];
                    f.motionVectorSubImage = motionImage;
                    f.appSpaceDeltaPose = identity;
                    f.depthSubImage = depthImage;
                    f.minDepth = 0, f.maxDepth = 1, f.nearZ = synth.nearZ, f.farZ = synth.farZ;
                    pv[eye].next = &f;
                }
            } else if (depthOnly_ && ds.handle && synth.depth[eye] && !uiShown) {
                auto& d = depthInfo[eye];
                d.subImage = depthImage;
                d.minDepth = 0, d.maxDepth = 1, d.nearZ = synth.nearZ, d.farZ = synth.farZ;
                pv[eye].next = &d;
            }
        }
        layer.space = space_;
        layer.viewCount = 2;
        layer.views = pv;
        // Quest Super Resolution: the compositor upscales and sharpens the eye images (a hint it may ignore)
        static XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
        if (layerSettingsExt_ && superResolution_.load(std::memory_order_relaxed)) {
            settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
            layer.next = &settings;
        }
        layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
    }
    // (docs/VR_EYE_PROJECTION.md) the game's UI layer: head-locked in the view space, its premultiplied alpha over the eyes
    const auto& us = swapchains_[kUi];
    if (ui && ui->visible && us.handle && view_ && ei.layerCount) {
        uiQuad.space = ui->tracking ? space_ : view_;  // (a conversation's text held in the world: tracking space)
        uiQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        uiQuad.subImage.swapchain = us.handle;
        uiQuad.subImage.imageRect = {{0, 0}, {int32_t(us.width), int32_t(us.height)}};
        const auto& q = ui->pose;
        uiQuad.pose.orientation = {q.orientation.x, q.orientation.y, q.orientation.z, q.orientation.w};
        uiQuad.pose.position = {q.position.x, q.position.y, q.position.z};
        uiQuad.size = {ui->width, ui->height};
        layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiQuad);
    }
    const auto& ps = swapchains_[kPanel];
    if (panel && panel->visible && ps.handle) {
        quad.space = space_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = ps.handle;
        quad.subImage.imageRect = {{0, 0}, {int32_t(ps.width), int32_t(ps.height)}};
        const auto& q = panel->pose;
        quad.pose.orientation = {q.orientation.x, q.orientation.y, q.orientation.z, q.orientation.w};
        quad.pose.position = {q.position.x, q.position.y, q.position.z};
        quad.size = {panel->width, panel->height};
        if (cylinderExt_ && panel->curvature > 0.01f) {
            // the same panel bent: a cylinder of radius distance / curvature whose surface passes through the
            // panel's centre; its pose is the axis, behind the centre towards the viewer
            const float radius = std::max(panel->distance / panel->curvature, panel->width / (2.0f * kPi) + 0.01f);
            const Vec3 axis = q.position + rotate(q.orientation, Vec3{0, 0, radius});
            cylinder.space = space_;
            cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cylinder.subImage = quad.subImage;
            cylinder.pose.orientation = quad.pose.orientation;
            cylinder.pose.position = {axis.x, axis.y, axis.z};
            cylinder.radius = radius;
            cylinder.centralAngle = std::min(panel->width / radius, 2.0f * kPi);
            cylinder.aspectRatio = panel->width / std::max(panel->height, 1e-3f);
            layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&cylinder);
        } else {
            layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
        }
    }
    const auto& ws = swapchains_[kWheel];
    if (wheel && wheel->visible && ws.handle && ei.layerCount) {
        // the item wheel: a flat quad, its picture's alpha (premultiplied) blended over what is behind it
        wheelQuad.space = space_;
        wheelQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        wheelQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        wheelQuad.subImage.swapchain = ws.handle;
        wheelQuad.subImage.imageRect = {{0, 0}, {int32_t(ws.width), int32_t(ws.height)}};
        const auto& q = wheel->pose;
        wheelQuad.pose.orientation = {q.orientation.x, q.orientation.y, q.orientation.z, q.orientation.w};
        wheelQuad.pose.position = {q.position.x, q.position.y, q.position.z};
        wheelQuad.size = {wheel->width, wheel->height};
        layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&wheelQuad);
    }
    const auto& rs = swapchains_[kWrist];
    for (int i = 0; wrist && rs.handle && ei.layerCount && i < std::min(wristCount, 5); ++i) {
        // the wrist HUD's panels: flat quads, parts of one picture, its alpha (premultiplied) over what is behind; the
        // Free Roam label (the fifth) held in the view
        const SubQuad& w = wrist[i];
        if (!w.quad.visible || w.w <= 0 || w.h <= 0) continue;
        XrCompositionLayerQuad& q = wristQuads[i];
        q.space = w.inView ? view_ : space_;
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = rs.handle;
        q.subImage.imageRect = {{w.x, w.y}, {w.w, w.h}};
        q.pose.orientation = {w.quad.pose.orientation.x, w.quad.pose.orientation.y, w.quad.pose.orientation.z,
                              w.quad.pose.orientation.w};
        q.pose.position = {w.quad.pose.position.x, w.quad.pose.position.y, w.quad.pose.position.z};
        q.size = {w.quad.width, w.quad.height};
        layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&q);
    }
    const auto& ls = swapchains_[kLaser];
    for (int i = 0; laser && ls.handle && ei.layerCount && i < std::min(laserCount, 2); ++i) {
        // the aiming laser: its beam and its dot, parts of one picture, its alpha (premultiplied) over the scene
        const SubQuad& l = laser[i];
        if (!l.quad.visible || l.w <= 0 || l.h <= 0) continue;
        XrCompositionLayerQuad& q = laserQuads[i];
        q.space = space_;
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = ls.handle;
        q.subImage.imageRect = {{l.x, l.y}, {l.w, l.h}};
        q.pose.orientation = {l.quad.pose.orientation.x, l.quad.pose.orientation.y, l.quad.pose.orientation.z,
                              l.quad.pose.orientation.w};
        q.pose.position = {l.quad.pose.position.x, l.quad.pose.position.y, l.quad.pose.position.z};
        q.size = {l.quad.width, l.quad.height};
        layers[ei.layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&q);
    }
    if (ei.layerCount) ei.layers = layers;
    check(xrEndFrame_(session_, &ei), "xrEndFrame");
}

bool Session::locate_views(XrTime time, Views& out) {
    std::lock_guard<std::mutex> lock(trackingMutex_);
    out.valid = false;
    if (!session_ || !running() || time <= 0) return false;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = space_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    const bool located = XR_SUCCEEDED(xrLocateViews_(session_, &li, &vs, 2, &n, views)) && n == 2;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // the last views with a valid position, turned with the head when its orientation is valid (3 degrees of freedom);
    // false when there are none recent enough (`since`: from when the hold counts)
    auto hold = [&](bool orientation, double since, double limit) {
        if (!lastViews_.valid || now - since > limit) return false;
        out = lastViews_;
        if (orientation) {
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            const Quat turned = XR_SUCCEEDED(xrLocateSpace_(view_, space_, time, &head)) &&
                                        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
                                    ? to_pose(head.pose).orientation : to_pose(views[0].pose).orientation;
            const Quat back = conjugate(lastViews_.head.orientation);
            for (int eye = 0; eye < 2; ++eye) {
                // each eye where it was from the head, turned with it
                const Vec3 rel = rotate(back, lastViews_.eye[eye].pose.position - lastViews_.head.position);
                out.eye[eye].pose.position = lastViews_.head.position + rotate(turned, rel);
                out.eye[eye].pose.orientation = to_pose(views[eye].pose).orientation;
                out.eye[eye].fov = {views[eye].fov.angleLeft, views[eye].fov.angleRight, views[eye].fov.angleUp,
                                    views[eye].fov.angleDown};
                out.eye[eye].display = out.eye[eye].fov;
            }
            out.head.orientation = turned;
            ++diag_.headTurned;
        } else {
            ++diag_.headFrozen;
        }
        out.time = time;
        out.valid = true;
        return true;
    };
    if (!located || !tracked(vs.viewStateFlags)) {
        // A gap in the head's tracking (inside-out headsets lose the position for moments: low light, a plain wall, the
        // boundary, a wireless hitch; the runtime then reports the orientation alone, or nothing). The camera fell back
        // to the game's own for those frames and the view snapped away and back (Quest 3 reports). Now the last views
        // with a valid position are kept for up to kHoldSeconds, turned with the head when its orientation is valid
        // (3 degrees of freedom); only a longer loss reports no views, as before. Every headset (was a Quest only)
        if (quest_ && (!lastViews_.valid || now - lastViewsAt_ > kHoldSeconds)) {
            ++diag_.headLost;
            quest_diag_log(now);
        }
        const bool orientation = located && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (!hold(orientation, jumpSince_ >= 0 ? jumpSince_ : lastViewsAt_, jumpSince_ >= 0 ? kJumpAcceptSeconds : kHoldSeconds))
            return false;
        headHeld_ = true;
        if (quest_) quest_diag_log(now);
        return true;
    }
    for (int eye = 0; eye < 2; ++eye) {
        out.eye[eye].pose = to_pose(views[eye].pose);
        out.eye[eye].fov = {views[eye].fov.angleLeft, views[eye].fov.angleRight, views[eye].fov.angleUp,
                            views[eye].fov.angleDown};
        out.eye[eye].display = out.eye[eye].fov;
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace_(view_, space_, time, &head)) && tracked(head.locationFlags))
        out.head = to_pose(head.pose);
    else
        out.head = {out.eye[0].pose.orientation, lerp(out.eye[0].pose.position, out.eye[1].pose.position, 0.5f)};
    test_head_jump(now, out);
    // a jump no head makes (docs/VR_CONTROLS.md, "Tracking gaps"): a PSVR2 that lost the room looking down reported the
    // head 30 to 40 m away as tracked, and the view flew off with it (the sky). Held as a gap, turned with the head,
    // until the runtime's position is back near the last good one (vr_tracking.h head_out_of_reach: up and down only
    // as far as at the jump); still away after kJumpAcceptSeconds, it is taken
    if (lastViews_.valid && finite(out.head.position)) {
        const float away = length(out.head.position - lastViews_.head.position);
        if (head_out_of_reach(out.head.position, lastViews_.head.position, now - lastViewsAt_,
                              jumpSince_ >= 0 ? now - jumpSince_ : -1.0)) {
            if (jumpSince_ < 0) {
                jumpSince_ = now;
                ++jumpsHeld_;
                LOG("[vr] head tracking jumped %.2f m in %.0f ms (no head moves that fast): the last good position held, "
                    "turned with the head (%llu times this session)", double(away), (now - lastViewsAt_) * 1000.0,
                    (unsigned long long)jumpsHeld_);
            }
            const bool orientation = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            if (now - jumpSince_ <= kJumpAcceptSeconds && hold(orientation, jumpSince_, kJumpAcceptSeconds)) {
                headHeld_ = true;
                return true;
            }
            LOG("[vr] head tracking still %.2f m away after %.0f s: the runtime's position taken", double(away),
                kJumpAcceptSeconds);
        } else if (jumpSince_ >= 0) {
            LOG("[vr] head tracking back after %.2f s held", now - jumpSince_);
        }
    }
    jumpSince_ = -1;
    // (once) the runtime's eyes against the head: the game draws both with the head's orientation and one symmetric
    // field of view (vr::as_rendered), so a canted or asymmetric headset relies on the runtime reprojecting them
    static bool eyesLogged = false;
    if (!eyesLogged) {
        eyesLogged = true;
        constexpr float deg = 57.2957795f;
        const Quat inv = conjugate(out.head.orientation);
        for (int eye = 0; eye < 2; ++eye) {
            const Quat rel = normalize(inv * out.eye[eye].pose.orientation);
            const Vec3 at = rotate(inv, out.eye[eye].pose.position - out.head.position);
            const auto& f = out.eye[eye].fov;
            LOG("[vr] %s eye: %.1f mm right, %.1f up, %.1f forward of the head; turned %.2f deg (yaw %.2f); "
                "field of view left %.1f right %.1f up %.1f down %.1f deg",
                eye ? "right" : "left", double(at.x * 1000), double(at.y * 1000), double(-at.z * 1000),
                double(angle_between(out.head.orientation, out.eye[eye].pose.orientation) * deg),
                double(yaw_of(rel) * deg), double(f.left * deg), double(f.right * deg), double(f.up * deg),
                double(f.down * deg));
        }
    }
    out.time = time;
    out.valid = true;
    // back from a held head: eased from where it was held (the held views' head stayed at lastViews_'s), not snapped
    if (headHeld_ && lastViews_.valid) {
        easeOffset_ = lastViews_.head.position - out.head.position;
        easeSince_ = now;
    }
    headHeld_ = false;
    lastViews_ = out;  // (the tracked views: what the next read's jump is measured from)
    lastViewsAt_ = now;
    if (const Vec3 e = head_ease(easeOffset_, easeSince_, now); length(e) > 0) {
        out.head.position = out.head.position + e;
        for (int eye = 0; eye < 2; ++eye) out.eye[eye].pose.position = out.eye[eye].pose.position + e;
    }
    if (quest_) {
        ++diag_.headTracked;
        if (diag_.lastTime > 0 && time > diag_.lastTime)
            diag_.gapMaxMs = std::max(diag_.gapMaxMs, double(time - diag_.lastTime) * 1e-6);
        diag_.lastTime = time;
        quest_diag_log(lastViewsAt_);
    }
    return true;
}

// tests in the headset (docs/VR_CONTROLS.md, "Jumps no head makes"): WWHD_VR_TEST_HEAD_JUMP fakes the PSVR2's tracking
// glitch of 2026-10-31 every 20 s, for 3 s, on the tracked head and eyes (the controllers are left as they are).
// `jump`: the head 0.72 m across and 1.25 m lower at once, then back (the jump hold and the ease back); `sink`: lowered
// 1.25 m over the 3 s, slower than the jump rule catches, then snapped back up (the floor limit and the ease)
void Session::test_head_jump(double now, Views& v) {
    static const int mode = [] {
        const char* e = std::getenv("WWHD_VR_TEST_HEAD_JUMP");
        const int m = !e ? 0 : !std::strcmp(e, "jump") ? 1 : !std::strcmp(e, "sink") ? 2 : 0;
        if (e) LOG("[vr] test: WWHD_VR_TEST_HEAD_JUMP=%s: %s", e,
                   m ? "the head's tracking faked wrong for 3 s every 20 s" : "unknown (jump or sink); off");
        return m;
    }();
    if (!mode) return;
    constexpr double kPeriod = 20.0, kLength = 3.0;
    if (testJumpStart_ < 0) testJumpStart_ = now;
    const double phase = std::fmod(now - testJumpStart_, kPeriod) - (kPeriod - kLength);  // 0..kLength: faked
    static bool on = false;
    if ((phase >= 0) != on) {
        on = phase >= 0;
        LOG("[vr] test: head tracking %s", on ? (mode == 1 ? "faked 1.44 m away (1.25 m lower)" : "faked sinking 1.25 m")
                                             : "real again");
    }
    if (phase < 0) return;
    const Vec3 offset = mode == 1 ? Vec3{0.72f, -1.25f, 0} : Vec3{0, -1.25f * float(phase / kLength), 0};
    v.head.position = v.head.position + offset;
    for (int eye = 0; eye < 2; ++eye) v.eye[eye].pose.position = v.eye[eye].pose.position + offset;
}

void Session::quest_diag_log(double now) {
    if (diag_.logAt == 0) diag_.logAt = now;
    if (now - diag_.logAt < 10.0) return;
    auto& d = diag_;
    const auto u = [](uint64_t v) { return (unsigned long long)v; };
    LOG("[vr] Quest tracking (10 s): head %llu tracked, %llu position held and turned, %llu frozen, %llu lost (over "
        "%.0f s); display times up to %.1f ms apart; input %llu syncs, %llu reads reused; %llu recentres from the "
        "runtime", u(d.headTracked), u(d.headTurned), u(d.headFrozen), u(d.headLost), kHoldSeconds, d.gapMaxMs,
        u(d.syncs), u(d.syncsReused), u(d.spaceChanges));
    for (int h = 0; h < 2; ++h)
        LOG("[vr] Quest tracking (10 s): %s hand %llu tracked, %llu untracked (the runtime's estimate, followed), "
            "%llu no position, %llu held", h ? "right" : "left", u(d.handTracked[h]), u(d.handEstimate[h]),
            u(d.handLost[h]), u(d.handHeld[h]));
    const XrTime lastTime = d.lastTime;
    d = {};
    d.lastTime = lastTime;
    d.logAt = now;
}

void Session::declare_thread(XrThread kind) {
#ifdef __ANDROID__
    const int k = int(kind);
    if (!threadSettingsExt_ || !xrSetAndroidApplicationThreadKHR_ || !session_ || k < 0 || k > 2) return;
    const uint32_t begins = sessionBegins_.load(std::memory_order_relaxed);
    if (!begins || threadDeclared_[k].exchange(begins, std::memory_order_relaxed) == begins) return;
    static const XrAndroidThreadTypeKHR kTypes[3] = {XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR,
                                                     XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR,
                                                     XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR};
    static const char* const kNames[3] = {"the game thread (application main)", "the render thread (renderer main)",
                                          "the end-frame thread (renderer worker)"};
    const uint32_t tid = uint32_t(gettid());
    const XrResult r = xrSetAndroidApplicationThreadKHR_(session_, kTypes[k], tid);
    LOG("[vr] XR thread scheduling: %s, thread %u: %s (%d)", kNames[k], tid, XR_SUCCEEDED(r) ? "declared" : "refused",
        int(r));
#else
    (void)kind;
#endif
}

void Session::cpu_boost(bool on) {
    if (!perfSettingsExt_ || !xrPerfSettingsSetPerformanceLevelEXT_ || !session_ || !running()) return;
    // (a new session starts at the runtime's default, Sustained High)
    const uint32_t begins = sessionBegins_.load(std::memory_order_relaxed);
    if (cpuBoostSession_ != begins) cpuBoostSession_ = begins, cpuBoost_ = false;
    if (on == cpuBoost_) return;
    const XrResult r = xrPerfSettingsSetPerformanceLevelEXT_(
        session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
        on ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    cpuBoost_ = on;
    if (on) ++cpuBoosts_;
    if (cpuBoosts_ <= 20 || XR_FAILED(r))
        LOG("[vr] CPU level: %s (%s; %llu boosts so far)", on ? "Boost (the game is stalled)" : "Sustained High",
            XR_SUCCEEDED(r) ? "accepted" : "refused", (unsigned long long)cpuBoosts_);
}

bool Session::sync_input(XrTime time, bool leftHanded, InputState& out) {
    std::lock_guard<std::mutex> lock(trackingMutex_);
    if (running()) declare_thread(XrThread::GameMain);  // (the game thread syncs the input: declared once per session)
    // One xrSyncActions a headset frame: the pad, the stick turn, the arms and the camera each asked for the input of
    // the same frame (the same display time), up to four syncs a frame. Some runtimes take input once a frame, and the
    // reads then disagreed within a frame; now the first read of a display time is kept for the others
    if (!quest_) return sync_input_locked(time, leftHanded, out);
    if (time > 0 && time == lastSyncTime_ && leftHanded == lastSyncLeft_) {
        ++diag_.syncsReused;
        out = lastInput_;
        return lastSyncOk_;
    }
    const bool ok = sync_input_locked(time, leftHanded, out);
    ++diag_.syncs;
    lastSyncTime_ = time;
    lastSyncLeft_ = leftHanded;
    lastSyncOk_ = ok;
    lastInput_ = out;
    return ok;
}

bool Session::sync_input_locked(XrTime time, bool leftHanded, InputState& out) {
    out = {};
    if (!session_ || !running() || time <= 0) return false;
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    const XrResult r = xrSyncActions_(session_, &si);
    const double now = double(time) * 1e-9;
    // XR_SESSION_NOT_FOCUSED is a success code: inputs read as inactive. Each different result is logged once
    // in a row (input diagnostics: docs/VR_TESTING.md)
    static XrResult lastResult = XR_SUCCESS;
    if (r != lastResult) {
        char text[XR_MAX_RESULT_STRING_SIZE] = "";
        xrResultToString_(instance_, r, text);
        LOG("[vr] input: xrSyncActions %s", text);
        lastResult = r;
    }
    if (XR_FAILED(r)) {
        // no input this time (buttons released), but the hands keep their last poses through a short gap
        for (int h = 0; h < 2; ++h) {
            auto& hand = out.hand[h];
            hand.active = hold_pose(gripHold_[h], false, false, {}, now, kHoldSeconds, hand.grip, &hand.held);
            Pose aim;
            hand.aim = hold_pose(aimHold_[h], false, false, {}, now, kHoldSeconds, aim) ? aim : hand.grip;
        }
        out.valid = out.hand[0].active || out.hand[1].active;
        return out.valid;
    }
    auto boolean = [&](XrAction a, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = hands_[h];
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean_(session_, &gi, &s)) && s.isActive && s.currentState;
    };
    auto value = [&](XrAction a, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = hands_[h];
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat_(session_, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
    };
    // hysteresis-free thresholds for the analogue inputs used as buttons
    constexpr float kPress = 0.6f;
    for (int h = 0; h < 2; ++h) {
        auto& hand = out.hand[h];
        XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION}, aim{XR_TYPE_SPACE_LOCATION};
        // a pose without a valid position (a moment out of the cameras' view) keeps the last valid one, with a fresh
        // orientation when there is one, for up to kHoldSeconds (vr_tracking.h); only a longer loss is untracked
        const bool gl = XR_SUCCEEDED(xrLocateSpace_(gripSpace_[h], space_, time, &grip));
        const bool al = XR_SUCCEEDED(xrLocateSpace_(aimSpace_[h], space_, time, &aim));
        auto pos = [](const XrSpaceLocation& l) { return (l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0; };
        auto posTracked = [](const XrSpaceLocation& l) { return (l.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0; };
        auto ori = [](const XrSpaceLocation& l) { return (l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0; };
        // (a valid but untracked position, the runtime's estimate out of its cameras' view, is a gap too: track_pose.
        // Not on a Quest: Meta's estimate from the controller's own sensors is what its runtime shows, and holding
        // the last tracked pose instead froze a hand lowered out of view for up to kHoldSeconds, then jumped; PC
        // Quest 3 logs had the left hand held for half of some 10 s windows. Only a missing position is a gap there)
        auto usable = [&](const XrSpaceLocation& l) { return posTracked(l) || (quest_ && pos(l)); };
        // a controller farther from the head than an arm reaches (the runtime lost the room: a PSVR2 reported both 30 to
        // 40 m away, as tracked) is a gap too, against the last good head (locate_views)
        auto nearHead = [&](const XrSpaceLocation& l) {
            return !lastViews_.valid || length(to_pose(l.pose).position - lastViews_.head.position) <= kHandFromHeadMetres;
        };
        hand.active = track_pose(gripHold_[h], gl && pos(grip) && nearHead(grip), gl && usable(grip), gl && ori(grip), to_pose(grip.pose),
                                 now, kHoldSeconds, hand.grip, &hand.held);
        hand.estimated = gl && pos(grip) && !posTracked(grip);
        if (quest_) {
            if (!gl || !pos(grip)) ++diag_.handLost[h];
            else if (posTracked(grip)) ++diag_.handTracked[h];
            else ++diag_.handEstimate[h];
            if (hand.held) ++diag_.handHeld[h];
        }
        Pose aimPose;
        hand.aim = track_pose(aimHold_[h], al && pos(aim) && nearHead(aim), al && usable(aim), al && ori(aim), to_pose(aim.pose), now,
                              kHoldSeconds, aimPose)
                       ? aimPose : hand.grip;
        hand.trigger = value(trigger_, h);
        hand.grip_value = value(squeeze_, h);
        hand.stickClick = boolean(stickClick_, h);
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_;
        gi.subactionPath = hands_[h];
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f_(session_, &gi, &st)) && st.isActive) {
            hand.stickX = st.currentState.x;
            hand.stickY = st.currentState.y;
        }
        // the main hand holds the sword: the right controller, or the left one in left-handed mode
        const bool main = (h == 1) != leftHanded;
        auto set = [&](Action m, Action o, bool on) {
            if (on) out.held |= 1u << int(main ? m : o);
        };
        set(Action::MainTrigger, Action::OffTrigger, hand.trigger >= kPress);
        set(Action::MainGrip, Action::OffGrip, hand.grip_value >= kPress);
        set(Action::MainPrimary, Action::OffPrimary, boolean(primary_, h));
        set(Action::MainSecondary, Action::OffSecondary, boolean(secondary_, h));
        set(Action::MainStickClick, Action::OffStickClick, boolean(stickClick_, h));
        if (boolean(menu_, h)) out.held |= 1u << int(Action::Menu);
        // the physical right stick pushed down (whatever the handedness: it is the turning stick)
        if (h == 1 && rightStickDown_.update(hand.stickX, hand.stickY)) out.held |= 1u << int(Action::RightStickDown);
    }
    out.valid = true;
    return true;
}

void Session::haptic(int physicalHand, float amplitude, float seconds) {
    std::lock_guard<std::mutex> lock(trackingMutex_);
    if (!session_ || !running() || amplitude <= 0 || physicalHand < 0 || physicalHand > 1) return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    v.duration = XrDuration(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    hi.subactionPath = hands_[physicalHand];
    xrApplyHapticFeedback_(session_, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

std::string Session::interaction_profile(int physicalHand) {
    std::lock_guard<std::mutex> lock(trackingMutex_);
    if (!session_ || physicalHand < 0 || physicalHand > 1) return "";
    XrInteractionProfileState s{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_FAILED(xrGetCurrentInteractionProfile_(session_, hands_[physicalHand], &s)) || !s.interactionProfile) return "";
    char text[XR_MAX_PATH_LENGTH] = "";
    uint32_t n = 0;
    if (XR_FAILED(xrPathToString_(instance_, s.interactionProfile, sizeof text, &n, text))) return "";
    return text;
}

void Session::destroy_session() {
    std::lock_guard<std::mutex> lock(trackingMutex_);
    for (auto& sc : swapchains_) {
        if (sc.handle) xrDestroySwapchain_(sc.handle);
        sc = {};
    }
    for (int h = 0; h < 2; ++h) {
        if (gripSpace_[h]) xrDestroySpace_(gripSpace_[h]);
        if (aimSpace_[h]) xrDestroySpace_(aimSpace_[h]);
        gripSpace_[h] = aimSpace_[h] = XR_NULL_HANDLE;
    }
    if (actionSet_) xrDestroyActionSet_(actionSet_);  // destroys its actions
    actionSet_ = XR_NULL_HANDLE;
    if (view_) xrDestroySpace_(view_);
    if (space_) xrDestroySpace_(space_);
    view_ = space_ = XR_NULL_HANDLE;
    if (session_) xrDestroySession_(session_);
    session_ = XR_NULL_HANDLE;
    frameBegun_ = false;
}

void Session::shutdown() {
    if (instance_) {
        if (session_ && running()) {
            xrRequestExitSession_(session_);
            xrEndSession_(session_);
        }
        destroy_session();
        if (xrDestroyInstance_) xrDestroyInstance_(instance_);
        instance_ = XR_NULL_HANDLE;
    }
    system_ = XR_NULL_SYSTEM_ID;
    set_state(State::Unavailable);
    // the loader stays loaded: Vulkan objects the runtime created may still point into it
}

}  // namespace vr::xr
