// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR launcher (docs/VR_LAUNCHER.md): sets the VR settings before the game starts and starts it, normally or in
// VR. SDL3 + Dear ImGui with the SDL_Renderer backend, as the setup program (tools/installer/gui).
//
// One source of truth: the settings are the game's own settings file (host::config_dir()/settings.ini, the same
// rule the game uses: portable folder, %APPDATA%\WWHD, $XDG_CONFIG_HOME/wwhd ...), read and written through
// vr/vr_settings_file.h with the definitions, defaults and ranges of vr/vr_config.h and vr/vr_bindings.h. The
// launcher writes only the vr.* keys and the game graphics keys it shows (resScale, fxaa, aniso, perfOverlay,
// prepareShaders, logFile);
// everything else in the file stays as it is. Its own state (where the game is) goes to vr-launcher.ini next to it.
//
// The game does not need the launcher: it reads the same file at start (and the settings overlay, F1, changes the
// same settings while it runs). The game keeps its settings in memory and rewrites the file when one of them
// changes, so the launcher does not save while a game it started is running.
#define NOMINMAX
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define XR_NO_PROTOTYPES
#ifdef __ANDROID__
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID  // the loader's and the instance's Android structures, as the game (openxr_session.h)
#endif
#include <openxr/openxr.h>
#ifdef __ANDROID__
#include <openxr/openxr_platform.h>
#endif

#include "extract_runner.h"
#include "dll_loader.h"
#include "gfx/vulkan/ngx_core.h"
#include "game_install.h"
#include "platform/host.h"
#include "build_info.h"
#include "platform/start_folder.h"
#include "vr/vr_bindings.h"
#include "vr/vr_config.h"
#include "vr/vr_settings_file.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef __ANDROID__
#include <algorithm>
#include <jni.h>
#include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------------------------
// files

std::string settings_path() {
    if (const char* e = SDL_getenv("WWHD_SETTINGS")) return e;
    return host::config_dir() + "/settings.ini";
}
std::string launcher_path() { return host::config_dir() + "/vr-launcher.ini"; }
bool replace(const std::string& from, const std::string& to) { return host::replace_file(from, to); }
std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
// "~" or "~/..." in a saved path: the current user's home folder (the game is started without a shell, which
// would otherwise expand it), so vr-launcher.ini need not name one user's home
std::string expand_home(const std::string& p) {
    if (p.empty() || p[0] != '~' || (p.size() > 1 && p[1] != '/' && p[1] != '\\')) return p;
    const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);  // ends with a separator
    if (!home || !*home) return p;
    std::string h = home;
    while (h.size() > 1 && (h.back() == '/' || h.back() == '\\')) h.pop_back();
    return h + p.substr(1);
}
bool exists(const std::string& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(std::filesystem::u8path(expand_home(p)), ec);
}

// the game's graphics settings the launcher shows (their keys and values as the game saves them)
struct GameGraphics {
    // (as the game: PC VR at 3x unless one is saved, vr_vulkan.cpp apply_vr_res_default; the launcher starts VR)
#if defined(__ANDROID__) || defined(__aarch64__)
    float resScale = 1;
#else
    float resScale = 3;
#endif
    bool fxaa = false, aniso = true, perfOverlay = false;
    bool prepareShaders = true;  // (the game's Prepare shaders at start, gfx/shader_prepare.h: missing = on)
    ngx::Mode dlss = ngx::Mode::Off;  // NVIDIA DLSS ("dlss", docs/VR_DLSS.md); the game runs Off unless it is offered
    bool dlss5 = false;               // DLSS 5 Neural Rendering ("dlss5"): its own toggle
    ngx::NrSettings nr;               // and its own settings ("dlss5.*", ngx_core.h NrSettings)
    std::vector<std::string> dlls;    // Extra DLLs ("extraDlls", dll_loader.h): loaded from where they are at every start
    // the game's log file ("logFile", main.cpp start_log_file; F1 > Graphics > Write a log file): captures/wwhd.log.
    // On unless saved off, on every platform (as the game reads it)
    bool logFile = true;
};
constexpr float kResScales[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};  // (4x: added 2026-11-04 for PC VR)
GameGraphics read_graphics(const vr::settings::File& f) {
    GameGraphics g;
    std::string v;
    float x;
    if (f.get("resScale", v) && vr::parse_float(v, x)) {
        g.resScale = kResScales[0];
        for (float s : kResScales)
            if (std::fabs(s - x) < std::fabs(g.resScale - x)) g.resScale = s;
    }
    if (f.get("fxaa", v)) vr::parse_bool(v, g.fxaa);
    if (f.get("aniso", v)) vr::parse_bool(v, g.aniso);
    if (f.get("perfOverlay", v)) vr::parse_bool(v, g.perfOverlay);
    if (f.get("prepareShaders", v)) vr::parse_bool(v, g.prepareShaders);
    if (f.get("dlss", v)) g.dlss = ngx::parse_mode(v);
    if (f.get("dlss5", v)) vr::parse_bool(v, g.dlss5);
    g.nr = ngx::nr_from_kv([&](const char* k, std::string& out) { return f.get(k, out); });
    if (f.get(dll_loader::kSettingKey, v)) g.dlls = dll_loader::split_setting(v);
    if (f.get("logFile", v)) vr::parse_bool(v, g.logFile);
    return g;
}
void write_graphics(vr::settings::File& f, const GameGraphics& g, const GameGraphics& loaded) {
    // only what the player changed here: a value the launcher merely showed is not rewritten
    char buf[16];
    if (g.resScale != loaded.resScale) { std::snprintf(buf, sizeof buf, "%g", double(g.resScale)); f.set("resScale", buf); }
    if (g.fxaa != loaded.fxaa) f.set("fxaa", g.fxaa ? "1" : "0");
    if (g.aniso != loaded.aniso) f.set("aniso", g.aniso ? "1" : "0");
    if (g.perfOverlay != loaded.perfOverlay) f.set("perfOverlay", g.perfOverlay ? "1" : "0");
    if (g.prepareShaders != loaded.prepareShaders) f.set("prepareShaders", g.prepareShaders ? "1" : "0");
    if (g.dlss != loaded.dlss) f.set("dlss", ngx::mode_key(g.dlss));
    if (g.dlss5 != loaded.dlss5) f.set("dlss5", g.dlss5 ? "1" : "0");
    if (g.dlls != loaded.dlls) f.set(dll_loader::kSettingKey, dll_loader::join_setting(g.dlls));
    if (g.logFile != loaded.logFile) f.set("logFile", g.logFile ? "1" : "0");
    const auto now = ngx::nr_to_kv(g.nr), was = ngx::nr_to_kv(loaded.nr);
    for (size_t i = 0; i < now.size(); ++i)
        if (now[i].second != was[i].second) f.set(now[i].first, now[i].second);
}

// ---------------------------------------------------------------------------------------------
// OpenXR: which runtime is active (from its manifest, nothing started), and on request whether a headset is there

std::string read_text(const std::string& path) {
    std::FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(std::filesystem::u8path(path).c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
// the "name" of the manifest's "runtime" object (a small search: the manifest format is fixed)
std::string manifest_name(const std::string& json) {
    size_t r = json.find("\"runtime\"");
    size_t k = json.find("\"name\"", r == std::string::npos ? 0 : r);
    if (k == std::string::npos) return {};
    size_t q1 = json.find('"', json.find(':', k) + 1);
    size_t q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
    return q2 == std::string::npos ? std::string() : json.substr(q1 + 1, q2 - q1 - 1);
}
struct RuntimeInfo {
    std::string manifest, name;
};
RuntimeInfo active_runtime() {
    RuntimeInfo r;
    if (const char* e = SDL_getenv("XR_RUNTIME_JSON"); e && *e) r.manifest = e;
#ifdef _WIN32
    if (r.manifest.empty()) {
        wchar_t buf[1024];
        DWORD size = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, buf,
                         &size) == ERROR_SUCCESS) {
            int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string u(n > 0 ? size_t(n - 1) : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, u.data(), n, nullptr, nullptr);
            r.manifest = u;
        }
    }
#else
    if (r.manifest.empty()) {
        std::vector<std::string> candidates;
        if (const char* x = SDL_getenv("XDG_CONFIG_HOME"); x && *x) candidates.push_back(std::string(x) + "/openxr/1/active_runtime.json");
        if (const char* h = SDL_getenv("HOME"); h && *h) candidates.push_back(std::string(h) + "/.config/openxr/1/active_runtime.json");
        candidates.push_back("/etc/xdg/openxr/1/active_runtime.json");
        candidates.push_back("/usr/share/openxr/1/active_runtime.json");
        for (const auto& c : candidates)
            if (exists(c)) { r.manifest = c; break; }
    }
#endif
    if (!r.manifest.empty()) r.name = manifest_name(read_text(r.manifest));
    return r;
}

struct Probe {
    std::atomic<bool> running{false};
    std::mutex mutex;
    bool done = false, loader = false, instance = false, headset = false, refreshRateExt = false;
    // what the runtime offers for the upscaling and frame synthesis choices (Graphics: what Auto picks here)
    bool superResolutionExt = false;  // XR_FB_composition_layer_settings (Meta's compositor: Quest Super Resolution)
    bool frameSynthesisExt = false;   // XR_EXT_frame_synthesis or XR_FB_space_warp
    std::string runtime, headsetName, error;
    // the refresh rate (the game's --vr-refresh-probe: Hz the headset runs, the rates the game may ask for); `fresh`
    // until the window has applied it to the settings
    bool refreshKnown = false, fresh = false;
    int streamed = -1;  // the headset is streamed (1), wired (0), not said (-1): Streaming optimal (Detect Headset)
    float refreshHz = 0;
    std::vector<float> offered;
    std::string refreshError, refreshMessage;
    std::string gameExe;  // (the window's choice, read by the worker)
} g_probe;
std::string find_game(const std::string& configured);
bool is_game_itself(const std::string& exe);

#ifndef __ANDROID__
// Runs the game program with one of its probe options (`flag`) and returns the text after its last "<tag>: " line in
// `line`, waiting `seconds` at most. false with `error` set if it could not start or did not answer
bool run_game_probe(const std::string& exe, const char* flag, const std::string& tag, int seconds, std::string& line,
                    std::string& error) {
    const char* argv[] = {exe.c_str(), flag, nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) {
        error = std::string("the game could not be started: ") + SDL_GetError();
        return false;
    }
    // its output until it ends
    SDL_IOStream* out = static_cast<SDL_IOStream*>(SDL_GetPointerProperty(SDL_GetProcessProperties(p), SDL_PROP_PROCESS_STDOUT_POINTER, nullptr));
    std::string text;
    char buf[4096];
    const Uint64 until = SDL_GetTicks() + Uint64(seconds) * 1000;
    while (out && SDL_GetTicks() < until) {
        const size_t n = SDL_ReadIO(out, buf, sizeof buf);
        if (n) { text.append(buf, n); continue; }
        if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
        SDL_Delay(10);
    }
    if (SDL_GetTicks() >= until) SDL_KillProcess(p, true);
    int code = 0;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    const std::string prefix = tag + ": ";
    const size_t at = text.rfind(prefix);
    if (at == std::string::npos) {
        error = "the game did not answer (exit " + std::to_string(code) + ")";
        return false;
    }
    const size_t from = at + prefix.size(), end = text.find('\n', from);
    line = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}
#endif

// The headset's refresh rate from the game program (`wwhd --vr-refresh-probe`, vr/vr_probe.cpp): OpenXR tells it only
// to a running session, which the game's own code starts. Only the game itself: the release's program starts the game.
// true with `hz` (and `offered`) read, else `error` says why
bool probe_refresh_rate(const std::string& exe, float& hz, std::vector<float>& offered, int& streamed, std::string& error) {
    streamed = -1;  // (not said: an older game program)
#ifdef __ANDROID__
    (void)exe; (void)hz; (void)offered;
    error = "not on Android yet: the game asks the headset for the chosen rate when it starts";
    return false;
#else
    if (exe.empty() || !is_game_itself(exe)) {
        error = "the game program (wwhd) was not found next to the launcher, so the rate could not be read";
        return false;
    }
    // 30 s at most (it gives up after 15 s without a session)
    std::string line;
    if (!run_game_probe(exe, "--vr-refresh-probe", "vr-refresh-probe", 30, line, error)) return false;
    if (!line.compare(0, 6, "error=")) {
        error = line.substr(6);
        return false;
    }
    float current = 0;
    if (std::sscanf(line.c_str(), "current=%f", &current) != 1 || !(current > 0)) {
        error = "the runtime reported no refresh rate";
        return false;
    }
    hz = current;
    offered.clear();
    if (const size_t o = line.find("offered="); o != std::string::npos) {
        const char* c = line.c_str() + o + 8;
        while (*c && *c != ' ') {
            char* end = nullptr;
            const float r = std::strtof(c, &end);
            if (end == c) break;
            if (r > 0) offered.push_back(r);
            c = *end == ',' ? end + 1 : end;
        }
    }
    if (const size_t st = line.find("streamed="); st != std::string::npos) streamed = line[st + 9] == '1' ? 1 : 0;
    return true;
#endif
}

void* open_loader() {
#ifdef _WIN32
    const std::wstring dir = std::filesystem::u8path(host::exe_dir()).wstring();
    HMODULE m = LoadLibraryW((dir + L"\\openxr_loader.dll").c_str());
    if (!m) m = LoadLibraryW(L"openxr_loader.dll");
    return m;
#else
    void* m = dlopen((host::exe_dir() + "/libopenxr_loader.so.1").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) m = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m) m = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    return m;
#endif
}
PFN_xrGetInstanceProcAddr loader_gipa(void* m) {
#ifdef _WIN32
    return reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(static_cast<HMODULE>(m), "xrGetInstanceProcAddr"));
#else
    return reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(m, "xrGetInstanceProcAddr"));
#endif
}

// Starts the runtime the way the game would (SteamVR may open), asks for a headset, closes again. A worker thread:
// a runtime that starts slowly never blocks the window.
// gameExe: the game program, for the refresh rate (probe_refresh_rate)
void probe_headset(const std::string& gameExe) {
    if (g_probe.running.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        g_probe.gameExe = gameExe;
    }
    std::thread([] {
        Probe r;
        void* m = open_loader();
        PFN_xrGetInstanceProcAddr gipa = m ? loader_gipa(m) : nullptr;
        r.loader = gipa != nullptr;
#ifdef __ANDROID__
        // Android (Quest, Steam Frame - Lepton): the loader needs the app's Java VM and context before any other call
        // (XR_KHR_loader_init_android), and the instance the VM and Activity (XR_KHR_android_create_instance), as the
        // game's session does (vr/openxr_session.cpp). The launcher's activity, a global reference kept for the process
        static JavaVM* vm = nullptr;
        static jobject activity = nullptr;
        static bool loaderInitialized = false;
        if (gipa && !activity) {
            JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
            jobject local = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
            if (env && local) {
                env->GetJavaVM(&vm);
                activity = env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
            }
        }
        if (gipa && (!vm || !activity)) {
            r.error = "the Android activity was not available to OpenXR";
            gipa = nullptr;
        }
        if (gipa && !loaderInitialized) {
            PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
            gipa(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
            if (initializeLoader) {
                XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
                li.applicationVM = vm;
                li.applicationContext = activity;
                const XrResult lr = initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li));
                if (XR_FAILED(lr)) {
                    r.error = "the OpenXR loader could not start (xrInitializeLoaderKHR " + std::to_string(int(lr)) + ")";
                    gipa = nullptr;
                } else {
                    loaderInitialized = true;
                }
            }
        }
        if (!gipa && r.error.empty())
#else
        if (!gipa)
#endif
        {
            r.error = "the OpenXR loader was not found (openxr_loader.dll / libopenxr_loader.so.1)";
        } else if (gipa) {
            PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
            PFN_xrCreateInstance create = nullptr;
            gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&enumerate));
            gipa(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&create));
            uint32_t n = 0;
            if (enumerate && XR_SUCCEEDED(enumerate(nullptr, 0, &n, nullptr))) {
                std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES});
                enumerate(nullptr, n, &n, ext.data());
                for (const auto& e : ext) {
                    if (!std::strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) r.refreshRateExt = true;
                    if (!std::strcmp(e.extensionName, "XR_FB_composition_layer_settings")) r.superResolutionExt = true;
                    if (!std::strcmp(e.extensionName, "XR_EXT_frame_synthesis") ||
                        !std::strcmp(e.extensionName, "XR_FB_space_warp"))
                        r.frameSynthesisExt = true;
                }
            }
            XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
            std::snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "WWHD VR launcher");
            ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
#ifdef __ANDROID__
            const char* androidExtension = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
            XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
            androidInfo.applicationVM = vm;
            androidInfo.applicationActivity = activity;
            ci.next = &androidInfo;
            ci.enabledExtensionCount = 1;
            ci.enabledExtensionNames = &androidExtension;
#endif
            XrInstance instance = XR_NULL_HANDLE;
            XrResult cr = create ? create(&ci, &instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
#ifdef __ANDROID__
            // a runtime that takes only OpenXR 1.1 (Android XR) is asked again with 1.1, as the game does
            if (cr == XR_ERROR_API_VERSION_UNSUPPORTED) {
                ci.applicationInfo.apiVersion = XR_API_VERSION_1_1;
                cr = create(&ci, &instance);
            }
#endif
            if (XR_FAILED(cr)) {
                r.error = "no OpenXR runtime could be started (" + std::to_string(int(cr)) + ")";
            } else {
                r.instance = true;
                PFN_xrGetInstanceProperties props = nullptr;
                PFN_xrGetSystem getSystem = nullptr;
                PFN_xrGetSystemProperties sysProps = nullptr;
                PFN_xrDestroyInstance destroy = nullptr;
                gipa(instance, "xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction*>(&props));
                gipa(instance, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&getSystem));
                gipa(instance, "xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction*>(&sysProps));
                gipa(instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroy));
                XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
                if (props && XR_SUCCEEDED(props(instance, &ip))) {
                    char v[64];
                    std::snprintf(v, sizeof v, " %u.%u.%u", XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                                  XR_VERSION_PATCH(ip.runtimeVersion));
                    r.runtime = std::string(ip.runtimeName) + v;
                }
                XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
                si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                XrSystemId system = XR_NULL_SYSTEM_ID;
                const XrResult sr = getSystem ? getSystem(instance, &si, &system) : XR_ERROR_FUNCTION_UNSUPPORTED;
                if (XR_SUCCEEDED(sr)) {
                    r.headset = true;
                    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
                    if (sysProps && XR_SUCCEEDED(sysProps(instance, system, &sp))) r.headsetName = sp.systemName;
                } else {
                    r.error = sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? "the runtime sees no headset (connect it and wake it up)"
                                                                     : "the runtime did not answer for a headset (" + std::to_string(int(sr)) + ")";
                }
                if (destroy) destroy(instance);
            }
        }
        // with a headset, its refresh rate (the game's probe: a short session)
        bool refreshKnown = false;
        float refreshHz = 0;
        std::vector<float> offered;
        std::string refreshError;
        int streamed = -1;
        if (r.headset) {
            std::string exe;
            {
                std::lock_guard<std::mutex> lock(g_probe.mutex);
                exe = g_probe.gameExe;
            }
            refreshKnown = probe_refresh_rate(exe, refreshHz, offered, streamed, refreshError);
        }
        // the loader stays loaded: unloading a runtime that started threads is not safe everywhere
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        g_probe.refreshKnown = refreshKnown;
        g_probe.refreshHz = refreshHz;
        g_probe.offered = offered;
        g_probe.streamed = streamed;
        g_probe.refreshError = refreshError;
        g_probe.fresh = true;
        g_probe.done = true;
        g_probe.loader = r.loader;
        g_probe.instance = r.instance;
        g_probe.headset = r.headset;
        g_probe.refreshRateExt = r.refreshRateExt;
        g_probe.superResolutionExt = r.superResolutionExt;
        g_probe.frameSynthesisExt = r.frameSynthesisExt;
        g_probe.runtime = r.runtime;
        g_probe.headsetName = r.headsetName;
        g_probe.error = r.error;
        g_probe.running = false;
    }).detach();
}

// Detect PC (Graphics > NVIDIA DLSS): the game's NVIDIA check on a device of its own (`wwhd --dlss-probe`,
// gfx/vulkan/ngx_vk.h), which writes dlss-status.ini; the window reads that file again when `fresh`
struct PcProbe {
    std::atomic<bool> running{false};
    std::mutex mutex;
    bool done = false, fresh = false;
    std::string error;  // why the check did not run, empty if it did
    std::string line;   // the probe's own answer ("NVIDIA DLSS ..."), shown if its status file could not be read
} g_pc;

// a finished Detect PC: the status file the game wrote, else the probe's answer itself (a folder it could not write)
ngx::StatusFile read_pc_status(const std::string& line) {
    ngx::StatusFile f = ngx::parse_status_file(read_text(host::config_dir() + "/" + ngx::kStatusFileName));
    if (!f.present && !line.empty() && line.compare(0, 6, "error=")) {
        f.present = true;
        f.status = line;
    }
    return f;
}

// a DLSS 5 model of the player's own next to the game (nvngx_dlssnr.dll, modified or not: NVIDIA publishes none, and
// NGX looks for feature libraries in the program's folder first). Its path, empty if there is none
std::string own_dlss5_copy(const std::string& gameExe) {
    if (gameExe.empty()) return {};
    std::error_code ec;
    const std::filesystem::path dll = std::filesystem::u8path(gameExe).parent_path() / "nvngx_dlssnr.dll";
    if (!std::filesystem::exists(dll, ec)) return {};
    const auto u8 = dll.u8string();
    return std::string(u8.begin(), u8.end());
}

void probe_pc(const std::string& gameExe) {
    if (g_pc.running.exchange(true)) return;
    std::thread([gameExe] {
        std::string error, line;
#ifdef __ANDROID__
        error = "not on Android: NVIDIA DLSS is for PCs with an NVIDIA RTX GPU";
#else
        if (gameExe.empty() || !is_game_itself(gameExe))
            error = "the game program (wwhd) was not found next to the launcher";
        else if (run_game_probe(gameExe, "--dlss-probe", "dlss-probe", 30, line, error) && !line.compare(0, 6, "error="))
            error = line.substr(6);
#endif
        std::lock_guard<std::mutex> lock(g_pc.mutex);
        g_pc.error = error;
        g_pc.line = line;
        g_pc.done = g_pc.fresh = true;
        g_pc.running = false;
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// the game

struct Launch {
    SDL_Process* process = nullptr;
    std::chrono::steady_clock::time_point started;
    std::string message;  // the last launch's result
    bool error = false;
} g_launch;

#ifdef _WIN32
constexpr const char* kGameNames[] = {"wwhd.exe", "wind-waker-hd-vr.exe", "Wind Waker HD.exe"};
#else
constexpr const char* kGameNames[] = {"wwhd", "wind-waker-hd-vr", "wind-waker-hd"};
#endif

#ifdef __ANDROID__
// Android (docs/VR_ANDROID.md, "The VR launcher"): the launcher is its own activity (VrLauncherActivity, its own
// process); the game is the app's game activity. These call the launcher activity's Java methods: launchGame(String[])
// starts the game activity with these arguments and closes the launcher, openSetup() the app's setup screens
bool android_call(const char* method, const std::vector<std::string>* args) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, method, args ? "([Ljava/lang/String;)V" : "()V");
    bool ok = m != nullptr;
    if (ok && args) {
        jclass str = env->FindClass("java/lang/String");
        jobjectArray a = env->NewObjectArray(jsize(args->size()), str, nullptr);
        for (size_t i = 0; i < args->size(); ++i) {
            jstring v = env->NewStringUTF((*args)[i].c_str());
            env->SetObjectArrayElement(a, jsize(i), v);
            env->DeleteLocalRef(v);
        }
        env->CallVoidMethod(activity, m, a);
        env->DeleteLocalRef(a);
        env->DeleteLocalRef(str);
    } else if (ok) {
        env->CallVoidMethod(activity, m);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ok = false;
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return ok;
}
// a launcher activity method returning a long (one string argument) or a boolean (none)
long long android_call_long(const char* method, const std::string& arg) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
    if (!env || !activity) return -1;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, method, "(Ljava/lang/String;)J");
    long long r = -1;
    if (m) {
        jstring a = env->NewStringUTF(arg.c_str());
        r = env->CallLongMethod(activity, m, a);
        env->DeleteLocalRef(a);
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); r = -1; }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return r;
}
bool android_call_bool(const char* method) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, method, "()Z");
    bool r = m && env->CallBooleanMethod(activity, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); r = false; }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return r;
}
// the app's external files folder (where the game runs, its settings and its default game folder "game")
std::string android_files() {
    const char* d = SDL_GetAndroidExternalStoragePath();
    return d ? d : "";
}
#endif

// The game to start: the configured one, else next to the launcher the game itself (source builds) or the release's
// setup program (which starts the prepared game and passes --vr / --no-vr on)
std::string find_game(const std::string& configured) {
#ifdef __ANDROID__
    (void)configured;
    return "android";  // (the app's game activity: always there)
#endif
    if (exists(configured)) return expand_home(configured);
    for (const char* n : kGameNames) {
        const std::string p = host::exe_dir() + "/" + n;
        if (exists(p)) return p;
    }
    return {};
}
// (a release, 2026-11-11) the game setup built, installed beside this launcher in <data>/bin: <data>/install.json
// names its game files ("game_dir", relative to <data> or absolute), as setup starts it (--game, --save <data>/save,
// in <data>). Empty when this launcher is not in such a folder.
// (2026-11-11) --game DIR / --save DIR on the launcher's command line: setup passes the player's game files and the
// saves' folder when it opens the launcher after preparing the game (setup_gui.cpp launch_game, setup.py launch)
std::string g_argGame, g_argSave;
std::string installed_game_dir(const std::filesystem::path& data) {
    std::FILE* f = std::fopen((data / "install.json").string().c_str(), "rb");
    if (!f) return {};
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    const size_t key = text.find("\"game_dir\"");
    if (key == std::string::npos) return {};
    size_t i = text.find('"', text.find(':', key) + 1);
    if (i == std::string::npos) return {};
    std::string v;
    for (++i; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;  // (\\ and \" in a path)
        v += text[i];
    }
    if (v.empty()) return {};
    const std::filesystem::path p = std::filesystem::u8path(v);
    return utf8(p.is_absolute() ? p : data / p);
}
bool is_game_itself(const std::string& exe) {
    const std::string name = utf8(std::filesystem::u8path(exe).filename());
    return name == "wwhd" || name == "wwhd.exe";
}

bool game_running() {
    if (!g_launch.process) return false;
    int code = 0;
    if (!SDL_WaitProcess(g_launch.process, false, &code)) return true;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_launch.started).count();
    if (code != 0) {
        g_launch.error = true;
        g_launch.message = "The game ended with exit code " + std::to_string(code) +
                           (secs < 15 ? " right after starting: see its log (wwhd.log / data/captures)." : ".");
    } else {
        g_launch.error = false;
        g_launch.message = "The game has closed.";
    }
    SDL_DestroyProcess(g_launch.process);
    g_launch.process = nullptr;
    return false;
}

void start_game(const std::string& exe, const std::string& gameDir, bool vrStart, bool vrTest = false) {
#ifdef __ANDROID__
    // the game activity with the same arguments the desktop launcher gives wwhd; the launcher then closes
    (void)exe;
    std::vector<std::string> launch = {vrStart ? "--vr" : "--no-vr"};
    if (!vrStart) launch.push_back("--no-stereo");  // Play Flat: the plain picture, not the saved 3D output
    if (vrTest) launch.push_back("--vr-test");
    if (!gameDir.empty()) {
        launch.push_back("--game");
        launch.push_back(expand_home(gameDir));
    }
    g_launch.error = !android_call("launchGame", &launch);
    g_launch.message = g_launch.error ? "The game could not be started." : "The game is starting.";
    return;
#endif
    if (game_running()) return;  // one at a time
    if (exe.empty() || !exists(exe)) {
        g_launch.error = true;
        g_launch.message = "The game was not found. Set its path in General > Game.";
        return;
    }
    // arguments as a list (no shell): spaces and quotes in paths are safe
    std::vector<std::string> args = {exe, vrStart ? "--vr" : "--no-vr"};
    // Play Flat: the plain picture, not the desktop 3D output a setting may have saved (wwhd itself only:
    // the release's program passes its own arguments on)
    if (!vrStart && is_game_itself(exe)) args.push_back("--no-stereo");
    if (vrTest) args.push_back("--vr-test");  // the Forsaken Fortress test environment (docs/VR_TEST_ENVIRONMENT.md)
    std::string cwd = utf8(std::filesystem::u8path(exe).parent_path());
    if (is_game_itself(exe)) {
        // the game files: as set here (setup passes them with --game), else those of the game setup installed beside
        // this launcher (<data>/install.json); the saves: setup's --save, else <data>/save; it runs in <data>, as
        // setup starts it
        const auto data = std::filesystem::u8path(exe).parent_path().parent_path();
        const std::string installed = installed_game_dir(data);
        const std::string game = !gameDir.empty() ? expand_home(gameDir) : installed;
        if (!game.empty()) {
            args.push_back("--game");
            args.push_back(game);
        }
        const std::string save = !g_argSave.empty() ? g_argSave : !installed.empty() ? utf8(data / "save") : std::string();
        if (!save.empty()) {
            args.push_back("--save");
            args.push_back(save);
        }
        if (!installed.empty()) cwd = utf8(data);
    }
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv.data());
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    // its own process: closing the launcher never ends the game
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    g_launch.process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_launch.process) {
        g_launch.error = true;
        g_launch.message = std::string("The game could not be started: ") + SDL_GetError();
        return;
    }
    g_launch.started = std::chrono::steady_clock::now();
    g_launch.error = false;
    g_launch.message = vrTest ? "The game is starting in VR; the test environment opens once your save is loaded."
                     : vrStart ? "The game is starting in VR." : "The game is starting on the desktop.";
}

// ---------------------------------------------------------------------------------------------
// the window's state

struct State {
    vr::settings::File file, launcherFile;
    vr::Config config, saved;
    vr::Bindings bindings, savedBindings;
    GameGraphics graphics, savedGraphics;
    std::string gameExe, gameDir;
    std::string savedGameExe, savedGameDir;
    std::vector<std::string> problems;  // values in the file that were not usable (defaults kept)
    std::string status, error;
    RuntimeInfo runtime;
    ngx::StatusFile dlss;  // what the game's last start found (dlss-status.ini, written by the game)
} S;

void load() {
    S.error.clear();
    S.problems.clear();
    std::string err;
    if (!S.file.load(settings_path(), err)) S.error = err + " (defaults shown; saving will try again)";
    S.config = S.saved = vr::settings::read_config(S.file, &S.problems);
    S.bindings = S.savedBindings = vr::settings::read_bindings(S.file, &S.problems);
    S.graphics = S.savedGraphics = read_graphics(S.file);
    S.launcherFile.load(launcher_path(), err);
    S.launcherFile.get("launcher.gameExe", S.gameExe);
    S.launcherFile.get("launcher.gameDir", S.gameDir);
    if (!g_argGame.empty()) S.gameDir = g_argGame;  // (setup's: the game it just prepared)
    S.savedGameExe = S.gameExe;
    S.savedGameDir = S.gameDir;
    S.runtime = active_runtime();
    S.dlss = ngx::parse_status_file(read_text(host::config_dir() + "/" + ngx::kStatusFileName));
}

bool changed() {
    return vr::to_kv(S.config) != vr::to_kv(S.saved) || S.bindings.to_kv() != S.savedBindings.to_kv() ||
           S.graphics.resScale != S.savedGraphics.resScale || S.graphics.fxaa != S.savedGraphics.fxaa ||
           S.graphics.aniso != S.savedGraphics.aniso || S.graphics.perfOverlay != S.savedGraphics.perfOverlay ||
           S.graphics.prepareShaders != S.savedGraphics.prepareShaders || S.graphics.dlss != S.savedGraphics.dlss ||
           S.graphics.dlss5 != S.savedGraphics.dlss5 || !(S.graphics.nr == S.savedGraphics.nr) ||
           S.graphics.dlls != S.savedGraphics.dlls || S.graphics.logFile != S.savedGraphics.logFile ||
           S.gameExe != S.savedGameExe || S.gameDir != S.savedGameDir;
}

// (docs/VR_LAUNCHER.md, "Settings from an older build") the build that last saved the settings file through the
// launcher: a file from another build may hold the defaults that build had (the game keeps every key it reads, so a
// changed default never reaches it). Asked once per build; any save here records this one
constexpr const char* kSettingsBuildKey = "settingsBuild";
std::string settings_build() {
    std::string v;
    return S.file.get(kSettingsBuildKey, v) ? v : std::string();
}
bool old_settings() {
    return exists(settings_path()) && settings_build() != build::version();
}

bool save() {
    S.error.clear();
    if (game_running()) {
        S.error = "The game is running and rewrites its settings file: close it first, or change the settings in the "
                  "game (F1).";
        return false;
    }
    // re-read first: keep every change another program made to keys the launcher does not own
    vr::settings::File f;
    std::string err;
    if (!f.load(settings_path(), err)) { S.error = "Not saved: " + err; return false; }
    vr::settings::write(f, S.config, S.bindings);
    write_graphics(f, S.graphics, S.savedGraphics);
    f.set(kSettingsBuildKey, build::version());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(settings_path()).parent_path(), ec);
    if (!f.save(settings_path(), err, replace)) { S.error = "Not saved: " + err; return false; }
    vr::settings::File lf;
    lf.load(launcher_path(), err);
    lf.set("launcher.format", "1");
    lf.set("launcher.gameExe", S.gameExe);
    lf.set("launcher.gameDir", S.gameDir);
    if (!lf.save(launcher_path(), err, replace)) { S.error = "The VR settings were saved, the launcher's own were not: " + err; }
    S.file = f;
    vr::validate(S.config);
    S.saved = S.config;
    S.savedBindings = S.bindings;
    S.savedGraphics = S.graphics;
    S.savedGameExe = S.gameExe;
    S.savedGameDir = S.gameDir;
    S.status = "Saved to " + settings_path();
    return S.error.empty();
}

// the settings file kept as it is, recorded as this build's (not asked again)
bool keep_old_settings() {
    if (game_running()) { S.error = "Close the game first: it rewrites its settings file."; return false; }
    vr::settings::File f;
    std::string err;
    if (!f.load(settings_path(), err)) { S.error = "Not saved: " + err; return false; }
    f.set(kSettingsBuildKey, build::version());
    if (!f.save(settings_path(), err, replace)) { S.error = "Not saved: " + err; return false; }
    S.file = f;
    return true;
}
// the settings file set aside (renamed with the time, not deleted) and a new one written with this build's defaults
bool fresh_settings(std::string& backup) {
    if (game_running()) { S.error = "Close the game first: it rewrites its settings file."; return false; }
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    backup = settings_path() + ".bak-" + stamp;
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(settings_path()), std::filesystem::u8path(backup), ec);
    if (ec) { S.error = "Could not set the old settings aside: " + ec.message(); return false; }
    load();         // (no file: every setting at this build's default)
    if (!save()) return false;  // (every VR key written at its default; the game's other settings left to their defaults)
    S.status = "Fresh settings with this build's defaults; the old file is " + backup;
    return true;
}

// ---------------------------------------------------------------------------------------------
// widgets

void help(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
// a slider with the setting's own range (vr_config.h)
bool slider(const char* label, const char* key, float& v, const char* format) {
    float lo = 0, hi = 1;
    vr::float_range(key, lo, hi);
    const bool c = ImGui::SliderFloat(label, &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%g .. %g); Ctrl+click to type", key, double(lo), double(hi));
    return c;
}
bool restore_button(const char* id) {
    ImGui::PushID(id);
    const bool r = ImGui::SmallButton("Restore this section's defaults");
    ImGui::PopID();
    return r;
}
template <class E>
void choice(const char* label, E& value, std::initializer_list<const char*> names) {
    int i = int(value);
    std::vector<const char*> v(names);
    if (ImGui::Combo(label, &i, v.data(), int(v.size()))) value = E(i);
}

// A finished Detect Headset, once: the headset's refresh rate into the settings (vr_config.h apply_detected_refresh:
// that rate, the frame rate capped at it), saved at once when nothing else was waiting to be saved; else left for Save
// with the rest. A rate not read changes nothing
void apply_detected_refresh() {
    bool known, headset;
    float hz;
    std::vector<float> offered;
    std::string error;
    int streamed;
    {
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        if (!g_probe.fresh) return;
        g_probe.fresh = false;
        streamed = g_probe.streamed;
        known = g_probe.refreshKnown;
        headset = g_probe.headset;
        hz = g_probe.refreshHz;
        offered = g_probe.offered;
        error = g_probe.refreshError;
    }
    std::string message;
    if (!known) {
        if (headset) message = "Refresh rate not read: " + error + ". The settings are unchanged.";
    } else {
        const bool clean = !changed();
        const vr::DetectedRefresh d = vr::apply_detected_refresh(S.config, hz, offered);
        message = d.message;
        // Streaming optimal from the runtime (docs/VR_STREAMING.md): on for a streamed headset, off for a wired one
        bool streamChanged = false;
        if (streamed >= 0 && S.config.streamingOptimal != (streamed == 1)) {
            S.config.streamingOptimal = streamed == 1;
            streamChanged = true;
        }
        // a streamed headset: the best settings for the stream (asked for, 2026-11-05): Streaming optimal on, Auto Mode
        // (apply_detected_refresh: there its streamed optimal profile, a frame on every refresh, interp_pacing.h
        // kAutoWindowStreamed) and frame interpolation for VR on (the rates above 30 fps need it). A wired headset:
        // Streaming optimal off, Auto Mode's wired optimal profile
        if (streamed == 1 && d.applied && !S.config.autoFrameRate) {
            S.config.autoFrameRate = true;
            streamChanged = true;
        }
        if (streamed >= 0)
            message += streamed == 1
                           ? " A streamed headset: set for streaming (Streaming optimal on; Auto Mode streamed optimal: a "
                             "frame on every refresh, held steady; 90 or 120 Hz in the streaming app is smoother still if "
                             "this computer keeps up)."
                           : " A wired headset: Streaming optimal off (Auto Mode wired optimal).";
        if (d.applied || streamChanged) {
            if (!clean) message += " Save to keep it (other changes are waiting to be saved too).";
            else if (save()) message += " Saved.";
            else message += " Not saved: " + S.error;
        }
    }
    std::lock_guard<std::mutex> lock(g_probe.mutex);
    g_probe.refreshMessage = message;
}

// Detect Headset: what the last check found, and its button (General tab, on every platform)
void headset_section() {
    {
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        if (g_probe.running) ImGui::TextDisabled("Asking the runtime for a headset...");
        else if (g_probe.done) {
            if (g_probe.headset)
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "Headset found: %s (runtime %s)", g_probe.headsetName.c_str(), g_probe.runtime.c_str());
            else
                ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "No headset: %s", g_probe.error.c_str());
            if (!g_probe.refreshMessage.empty())
                ImGui::TextWrapped("%s", g_probe.refreshMessage.c_str());
        } else {
            ImGui::TextDisabled("Headset: not checked yet");
        }
    }
    ImGui::BeginDisabled(g_probe.running.load());
    if (ImGui::Button("Detect Headset")) probe_headset(find_game(S.gameExe));
    ImGui::EndDisabled();
#ifdef __ANDROID__
    help("Asks the headset's own OpenXR runtime for the headset, and what it offers the game (Quest Super Resolution, "
         "frame synthesis: Graphics > Upscaling and frame synthesis says what Auto picks). The refresh rate is asked for "
         "by the game when it starts.");
#else
    help("This starts the runtime briefly (SteamVR may open) and, with a headset, a second-long session of the game's to "
         "read the refresh rate it runs at. That rate is then selected (Graphics) with the frame rate capped at it "
         "(Capped 90 FPS at 90 Hz). A found headset is not yet a running VR session: the game starts that, and says in "
         "its log ([vr] lines) and settings overlay whether it runs.");
#endif
}

void tab_general() {
    ImGui::SeparatorText("Start");
    const std::string exe = find_game(S.gameExe);
    const bool running = game_running();
    ImGui::BeginDisabled(running || exe.empty());
    if (ImGui::Button("Play VR", ImVec2(180, 40))) {
        if (!changed() || save()) start_game(exe, S.gameDir, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Play Flat", ImVec2(180, 40))) {
        if (!changed() || save()) start_game(exe, S.gameDir, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("VR Weapons Test", ImVec2(240, 40))) {
        if (!changed() || save()) start_game(exe, S.gameDir, true, true);
    }
    ImGui::EndDisabled();
    if (running) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "The game is running (started from here).");
    if (!g_launch.message.empty())
        ImGui::TextColored(g_launch.error ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1), "%s", g_launch.message.c_str());
    help("Unsaved changes are saved first. The game also starts without the launcher: --vr / --no-vr choose for one start, "
         "\"Start in VR\" below for every start. VR test: once you load your save, Link goes to Forsaken Fortress with every "
         "item (--vr-test); your world comes back as it was when you leave it (F1 > VR > Leave). docs/VR_TEST_ENVIRONMENT.md");
    ImGui::Checkbox("Start in VR (also without the launcher)", &S.config.enabled);

#ifdef __ANDROID__
    // Android: the headset's own OpenXR runtime, and the app's own game program; the game's folder and the setup
    ImGui::SeparatorText("Game");
    {
        const std::string dir = S.gameDir.empty() ? android_files() + "/game" : expand_home(S.gameDir);
        std::error_code ec;
        const bool there = std::filesystem::is_regular_file(std::filesystem::u8path(dir) / "code" / "cking.rpx", ec);
        ImGui::TextWrapped("Game folder: %s", dir.c_str());
        if (there) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "The game's files are there.");
        else ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "No game files there yet: extract them in the Install game tab, or "
                                                          "use the app's setup.");
    }
    ImGui::SeparatorText("Headset");
    headset_section();
    ImGui::SeparatorText("Setup");
    if (ImGui::Button("Set up your game (the app's setup screens)")) android_call("openSetup", nullptr);
    help("The app's own setup: choose your game files, and (in builds with it) prepare the game on this device.");
    if (ImGui::Button("Open the VR documentation"))
        SDL_OpenURL("https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/blob/main/docs/VR_ANDROID.md");
    return;
#endif
    ImGui::SeparatorText("OpenXR");
    ImGui::Text("Active runtime: %s", S.runtime.manifest.empty() ? "none set" : S.runtime.name.empty() ? "set (unnamed)" : S.runtime.name.c_str());
    if (!S.runtime.manifest.empty()) help(S.runtime.manifest.c_str());
    else help("Install an OpenXR runtime (SteamVR, Monado, WiVRn, Meta Quest Link, Windows Mixed Reality) and make it the active one.");
    headset_section();

    ImGui::SeparatorText("Game");
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", S.gameExe.c_str());
    if (ImGui::InputText("Game program", buf, sizeof buf)) S.gameExe = buf;
    help(exe.empty() ? "Not found next to the launcher: enter the path of wwhd(.exe), or of the release's Wind Waker HD program."
                     : ("Starts: " + exe).c_str());
    std::snprintf(buf, sizeof buf, "%s", S.gameDir.c_str());
    if (ImGui::InputText("Game files (--game)", buf, sizeof buf)) S.gameDir = buf;
    help("Only for a game built from source (wwhd): the folder with code, content and meta. A release's own program knows it. "
         "No game folder yet? The Install game tab extracts it from your disc image or Cemu archive and sets it here.");
    if (ImGui::Button("Open the settings folder")) {
        const std::string dir = utf8(std::filesystem::u8path(settings_path()).parent_path());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        SDL_OpenURL(("file:///" + dir).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the VR documentation"))
        SDL_OpenURL("https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/blob/main/docs/VR_LAUNCHER.md");
}

// the wrist displays (docs/VR_WRIST_HUD.md): which, their size, when they show and each one's place on its wrist
void tab_wrist_displays(vr::Config& c) {
    ImGui::SeparatorText("Wrist HUD");
    ImGui::Checkbox("Hearts on top of the left wrist", &c.armHearts);
    ImGui::Checkbox("Item buttons under the left wrist", &c.wristItems);
    ImGui::Checkbox("Magic on top of the right wrist", &c.wristMagic);
    ImGui::Checkbox("Rupees under the right wrist", &c.wristRupees);
    slider("Along the forearm", "vr.wristHudAlong", c.wristHudAlong, "%.3f m from the grip");
    slider("Off the wrist", "vr.wristHudLift", c.wristHudLift, "%.3f m");
    slider("Size", "vr.wristHudScale", c.wristHudScale, "%.2fx");
    help("Your hearts, the items on X, Y and R, your magic and your rupees on your wrists, as the game has them. Each shows while you "
         "turn it towards your eyes, in normal play. Nothing else of the game's HUD is shown in VR, except the action "
         "prompt and dialogue.");
    if (restore_button("wrist")) {
        const vr::Config d;
        c.armHearts = d.armHearts; c.wristItems = d.wristItems; c.wristRupees = d.wristRupees; c.wristMagic = d.wristMagic;
        c.wristHudAlong = d.wristHudAlong; c.wristHudLift = d.wristHudLift; c.wristHudScale = d.wristHudScale;
    }
}

void tab_camera() {
    auto& c = S.config;
    ImGui::SeparatorText("Menu panel (pause, item, map and bag menus)");
    choice("Show the GamePad screen", c.gamepadScreen, {"While its menus are open", "Always", "Never"});
    choice("Placement", c.menuPlacement, {"Where you look when it opens", "Follows your head", "Straight ahead"});
    slider("Distance", "vr.menuDistance", c.menuDistance, "%.2f m");
    slider("Height", "vr.menuHeight", c.menuHeight, "%+.2f m");
    slider("Size", "vr.menuScale", c.menuScale, "%.2fx");
    slider("Curvature", "vr.menuCurvature", c.menuCurvature, "%.2f");
    help("Curvature needs a runtime with curved layers (XR_KHR_composition_layer_cylinder, e.g. SteamVR); others show it flat.");
    if (restore_button("menu")) {
        const vr::Config d;
        c.gamepadScreen = d.gamepadScreen; c.menuPlacement = d.menuPlacement; c.menuDistance = d.menuDistance;
        c.menuHeight = d.menuHeight; c.menuScale = d.menuScale; c.menuCurvature = d.menuCurvature;
    }
    ImGui::SeparatorText("Title, file select and cutscenes");
    slider("Title and file select distance", "vr.menuZoomOut", c.menuZoomOut, "%.2fx");
    ImGui::Checkbox("Look around in cutscenes", &c.cutsceneFollow);
    slider("Cutscene camera distance", "vr.cutsceneZoomOut", c.cutsceneZoomOut, "%.2fx");
    slider("Cutscene and dialogue text distance", "vr.textZoomOut", c.textZoomOut, "%.2fx");
    slider("Cutscene and dialogue text height", "vr.textHeight", c.textHeight, "%+.2f");
    slider("Telescope eyepiece size", "vr.telescopeSize", c.telescopeSize, "%.2fx");
    slider("Picto Box viewfinder size", "vr.pictoBoxSize", c.pictoBoxSize, "%.2fx");
    help("Textboxes and subtitles of conversations and cutscenes: smaller around the centre of the view (as if farther "
         "away) and higher or lower (share of half the view's height). The gameplay HUD keeps its size.");
    help("How much farther the game's own camera is from what it shows (1.5 = 50 % farther). The gameplay view is not affected.");
    if (restore_button("scenes")) {
        const vr::Config d;
        c.menuZoomOut = d.menuZoomOut; c.cutsceneFollow = d.cutsceneFollow; c.cutsceneZoomOut = d.cutsceneZoomOut;
        c.textZoomOut = d.textZoomOut; c.textHeight = d.textHeight; c.telescopeSize = d.telescopeSize;
        c.pictoBoxSize = d.pictoBoxSize;
    }
    ImGui::SeparatorText("Player camera (normal play)");
    {
        int view = c.startThirdPerson ? 1 : 0;
        if (ImGui::Combo("View at the start", &view, "First person (Link's eyes)\0Third person (the game's camera)\0"))
            c.startThirdPerson = view == 1;
    }
    help("A short click of the right thumbstick switches between them in the game.");
    slider("HUD position", "vr.hudInset", c.hudInset, "%.2f");
    help("The game's own HUD (item buttons, rupees; the hearts in third person, on the arm in first person) is drawn this "
         "share of the way from the view's centre to its edges: at 1 it sits at the picture's edges, outside what most "
         "headsets' lenses show. Lower brings it nearer the centre.");
    slider("HUD size", "vr.hudSize", c.hudSize, "%.2fx");
    help("The HUD's size: 1 is the game's own, 0.5 half of it. The hearts on the arm have their own size below.");
    slider("Height offset", "vr.heightOffset", c.heightOffset, "%+.2f m");
    help("Seated play: raise the view with the height offset (the view is calibrated to your eye height at each recentre).");
    slider("View forward / back", "vr.eyeForward", c.eyeForward, "%+.2f m");
    help("First person: the view in front of (+) or behind (-) Link's eyes, as the game places them. 0 is his eyes; "
         "behind them shows less of his body when you look down.");
    {
        // the scale in use: matched to Link's arms (from your arm length) or set directly; moving the slider sets it
        int mode = c.matchLinkBody ? 0 : 1;
        if (ImGui::Combo("World scale", &mode, "Matched to Link's arms\0Set directly\0")) {
            if (mode == 1 && c.matchLinkBody) c.unitsPerMetre = std::round(vr::matched_world_scale(c.realArmLength));
            c.matchLinkBody = mode == 0;
        }
        float scale = c.matchLinkBody ? vr::matched_world_scale(c.realArmLength) : c.unitsPerMetre;
        if (slider("World scale (units per metre)", "vr.unitsPerMetre", scale, "%.0f units per metre")) {
            c.unitsPerMetre = scale;
            c.matchLinkBody = false;
        }
    }
    help("Game units per real metre. Lower: the world looks bigger around you; higher: smaller (100 is about the game's "
         "own size, Link about 1.2 m tall). Matched: from your arm length (Controllers), so your hands are drawn where "
         "they are; moving the slider sets the scale directly.");
    slider("Lean limit", "vr.maxHeadOffset", c.maxHeadOffset, "%.2f m");
    help("How far your head may lean or walk away from Link before the view is held back. Recentre: hold both thumbstick "
         "clicks for a moment in the game (or Settings > VR > Recentre). The view also recentres on Link at every stage "
         "change.");
    ImGui::Checkbox("Show Link's head in first person", &c.showBody);
    ImGui::Checkbox("Hide Link's body", &c.hideBody);
    help("First person: only his arms, hands and whatever is in them are drawn; not his body, legs and head, nor the "
         "shield and sword on his back. His shadow goes with his body. Also in the game: F1 > VR.");
    ImGui::Checkbox("Hide Link's arms", &c.hideArms);
    help("First person: his arms are not drawn, his hands (and whatever is in them) are. With Hide Link's body as well, "
         "only the hands are left. Also in the game: F1 > VR.");
    help("The field of view is always your headset's own; it is not a setting.");
    if (restore_button("camera")) {
        const vr::Config d;
        c.startThirdPerson = d.startThirdPerson; c.hudInset = d.hudInset; c.hudSize = d.hudSize; c.heightOffset = d.heightOffset;
        c.eyeForward = d.eyeForward;
        c.matchLinkBody = d.matchLinkBody; c.unitsPerMetre = d.unitsPerMetre; c.maxHeadOffset = d.maxHeadOffset;
        c.showBody = d.showBody;
        c.hideBody = d.hideBody;
        c.hideArms = d.hideArms;
    }
    tab_wrist_displays(c);
    ImGui::SeparatorText("Comfort");
    choice("Turning", c.turn, {"Snap", "Smooth"});
    if (c.turn == vr::Turn::Snap) slider("Snap angle", "vr.snapDegrees", c.snapDegrees, "%.0f degrees");
    else slider("Turn speed", "vr.smoothDegreesPerSecond", c.smoothDegreesPerSecond, "%.0f degrees/s");
    choice("Movement direction", c.moveDirection, {"Where you look", "Where the left controller points"});
    slider("Movement speed", "vr.moveSpeed", c.moveSpeed, "%.2fx");
    slider("Body turns after", "vr.bodyFollowDegrees", c.bodyFollowDegrees, "%.0f degrees");
    help("Standing still in first person, Link turns to face where you look once your head has turned this far from "
         "his body, and stops within 5 degrees of your view.");
    ImGui::Checkbox("Turn the view in spin attacks", &c.spinCamera);
    help("First person: the view turns round with Link during the spin attack and the Hurricane Spin. Off keeps the view "
         "still while he spins. Also in the game: F1 > VR.");
    slider("Spin speed", "vr.spinSpeed", c.spinSpeed, "%.2fx");
    help("The spin attack and the Hurricane Spin slowed together with the view's turn: their animation and Link's turn "
         "stay in step. 1.00x is the game's own speed. Also in the game: F1 > VR.");
    // (vr.eyeCrop, "Eye field of view fix", is no longer offered: each eye is drawn with its own projection; the key is
    // still read from settings files. docs/VR_MENU.md)
    if (restore_button("comfort")) {
        const vr::Config d;
        c.turn = d.turn; c.snapDegrees = d.snapDegrees; c.smoothDegreesPerSecond = d.smoothDegreesPerSecond;
        c.moveDirection = d.moveDirection; c.moveSpeed = d.moveSpeed; c.bodyFollowDegrees = d.bodyFollowDegrees;
        c.spinCamera = d.spinCamera;
        c.spinSpeed = d.spinSpeed;
    }
}

void tab_controls() {
    auto& c = S.config;
    ImGui::SeparatorText("Hands");
    ImGui::Checkbox("Left-handed (sword in the left hand)", &c.leftHanded);
    slider("Stick dead zone", "vr.stickDeadZone", c.stickDeadZone, "%.2f");
    ImGui::SeparatorText("Wind Waker");
    ImGui::Checkbox("Conduct with your hands too", &c.batonHands);
    slider("Hand reach for a direction", "vr.batonHandReach", c.batonHandReach, "%.2f m");
    help("While conducting, the left stick plays the notes and the right stick the beat, as on the GamePad. With "
         "this on, the hand holding the wand (the sword hand) plays the notes and the other hand the beat, held up, "
         "down, left or right of where it was when conducting began (a stick pushed wins over its hand). The notes "
         "need 85 % of the reach, at the wand's tip, or a quick flick that way; the wand hand's trigger holds the middle note.");
    ImGui::SeparatorText("Buttons: which GamePad button each controller input presses");
    help("Sword hand = the right controller (left when left-handed). The game decides what a button does where (A talks, "
         "opens, picks up; ZL targets; ZR shields...).");
    std::vector<const char*> names;
    for (const auto& b : vr::kButtonNames) names.push_back(b.name[0] ? b.name : "(nothing)");
    if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        for (int i = 0; i < vr::kActionCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(vr::kActions[i].label);
            ImGui::TableNextColumn();
            int sel = 0;
            for (int k = 0; k < int(names.size()); ++k)
                if (vr::kButtonNames[k].bit == S.bindings.button[i]) sel = k;
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("##b", &sel, names.data(), int(names.size()))) S.bindings.button[i] = vr::kButtonNames[sel].bit;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (restore_button("buttons")) S.bindings = vr::Bindings{};
    ImGui::SeparatorText("Fixed in this version");
    ImGui::BulletText("Movement: left stick (with the dead zone above). Turning: right stick (Camera tab).");
    ImGui::BulletText("Sword: swing the sword-hand controller (the sword-hand secondary button also presses B).");
    ImGui::BulletText("Recentre: both thumbstick clicks held together.");
    ImGui::BulletText("Aiming the bow, hookshot and boomerang: the right stick while aiming.");
    ImGui::SeparatorText("Not supported yet");
    ImGui::BulletText("Direct shortcuts for the inventory and the map: open them through the pause menu (menu button) or");
    ImGui::TextUnformatted("    bind a button to the GamePad button the game uses for them.");
    ImGui::BulletText("Touching the GamePad panel with a controller.");
    ImGui::BulletText("Aiming items with the controller's pointing direction.");
    ImGui::SeparatorText("Swing sensitivity");
    slider("Hand speed to start a swing", "vr.swingStartSpeed", c.swingStartSpeed, "%.1f m/s");
    slider("Hand speed that ends it", "vr.swingEndSpeed", c.swingEndSpeed, "%.1f m/s");
    slider("Fast for at least", "vr.swingMinTime", c.swingMinTime, "%.2f s");
    help("A swing starts when your sword hand moves this fast for this long, and ends when it slows below the end speed. "
         "Lower speeds catch slower swings; too low and ordinary hand movement starts them.");
    slider("Side slice: hand travel", "vr.swingAcrossDistance", c.swingAcrossDistance, "%.2f m");
    slider("Down swing: hand travel", "vr.swingDownDistance", c.swingDownDistance, "%.2f m");
    help("Link attacks (animation and sound) once a swing has carried your hand this far across (left to right or right "
         "to left) or down. Lower is more sensitive; very low lets vigorous wrist turns count. Thrusts, upward swings and "
         "wind-ups do not attack.");
    slider("Pause between swings", "vr.swingCooldown", c.swingCooldown, "%.2f s");
    help("After a swing, another the same way waits this long (one back the other way does not).");
    if (restore_button("sensitivity")) {
        const vr::Config d;
        c.swingStartSpeed = d.swingStartSpeed; c.swingEndSpeed = d.swingEndSpeed; c.swingMinTime = d.swingMinTime;
        c.swingAcrossDistance = d.swingAcrossDistance; c.swingDownDistance = d.swingDownDistance;
        c.swingCooldown = d.swingCooldown;
    }
    ImGui::SeparatorText("Sword and arms");
    slider("Sword tilt", "vr.gripPitchDegrees", c.gripPitchDegrees, "%.0f degrees");
    help("The blade's angle up from where your controller points.");
    slider("Sword wrist straight", "vr.swordWristStraight", c.swordWristStraight, "%.2f");
    help("1: Link's sword hand stays in line with his forearm and the sword turns with it; 0: the hand turns to hold the "
         "sword as your controller does (the wrist may bend).");
    slider("Skull Hammer roll", "vr.hammerRollDegrees", c.hammerRollDegrees, "%.0f degrees");
    help("The Skull Hammer turned about its handle. 180 (the default): its flat end faces down; 0: its face points "
         "away from the top of your controller.");
    slider("Hit vibration", "vr.hapticStrength", c.hapticStrength, "%.2f");
    ImGui::Checkbox("Sword follows the controller", &c.trackedSword);
    ImGui::Checkbox("Physical sword", &c.physicalSword);
    help("While your sword hand swings, the blade cuts grass and hits enemies along its path, without Link's attack "
         "animation (as in Twilight Princess VR). Full swings still play his attack.");
    ImGui::Checkbox("Item wheel", &c.itemWheel);
    choice("Item wheel puts items on", c.itemWheelButton, {"X (shield hand primary)", "Y (shield hand secondary)", "R (sword hand grip)"});
    help("Reach over your shield hand's shoulder and hold its grip to open the item wheel, choose with the right stick "
         "(left / right: item, up / down: bag), and pull the arm forward: the item goes on that button and Link takes it "
         "out. Let go of the grip at the shoulder, or click the right stick, to cancel.");
    ImGui::Checkbox("Link's arms follow the controllers", &c.armIK);
    ImGui::Checkbox("Detached Arms", &c.detachedArms);
    help("Allows VR arms and hands to move more freely by reducing constraints imposed by the player body model: Link's "
         "hands go where your controllers are even beyond his shoulders' reach (his arms move out from his shoulders), "
         "and his wrists turn as far as yours. Takes effect at once, also while playing.");
    ImGui::Checkbox("Aiming laser", &c.aimLaser);
    help("A red line from the bow, hookshot, boomerang or grappling hook in the sword hand to where it aims; a bright "
         "dot where the game takes the target. They aim where that controller points.");
    slider("Your arm length", "vr.realArmLength", c.realArmLength, "%.2f m");
    help("Shoulder to the middle of your fist, arm straight. With the world sized to Link it sets the world scale.");
    if (restore_button("sword")) {
        const vr::Config d;
        c.leftHanded = d.leftHanded; c.stickDeadZone = d.stickDeadZone; c.batonHands = d.batonHands; c.batonHandReach = d.batonHandReach; c.swingStartSpeed = d.swingStartSpeed; c.swingFullDegrees = d.swingFullDegrees; c.swingAcrossDistance = d.swingAcrossDistance; c.swingDownDistance = d.swingDownDistance;
        c.swingEndSpeed = d.swingEndSpeed; c.gripPitchDegrees = d.gripPitchDegrees; c.swordWristStraight = d.swordWristStraight; c.hammerRollDegrees = d.hammerRollDegrees; c.hapticStrength = d.hapticStrength;
        c.trackedSword = d.trackedSword; c.physicalSword = d.physicalSword; c.itemWheel = d.itemWheel; c.itemWheelButton = d.itemWheelButton; c.armIK = d.armIK; c.detachedArms = d.detachedArms; c.aimLaser = d.aimLaser; c.realArmLength = d.realArmLength;
    }
}

// what Auto picks on this device (vr/vr_upscale.h choose(), by what Detect Headset and Detect PC found); the game
// decides for itself at its start, and F1 > VR says what it uses
std::string auto_pick_text() {
    bool checked, superResolution, synthesis;
    {
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        checked = g_probe.done && g_probe.instance;
        superResolution = g_probe.superResolutionExt;
        synthesis = g_probe.frameSynthesisExt;
    }
    std::string s = "Auto on this device: ";
#if defined(__ANDROID__)
    if (!checked) return s + "press Detect Headset (General) to see (Quest Super Resolution on a Quest, else SGSR1).";
    s += superResolution ? "Quest Super Resolution (the runtime offers it)" : "SGSR1 (the runtime has no Quest Super Resolution)";
#elif defined(WWHD_STEAM_FRAME_NATIVE) || defined(__aarch64__)
    (void)superResolution;
    s += "SGSR1 (standalone arm64 headset)";
    if (!checked) return s + ". Detect Headset (General) checks frame synthesis.";
#else
    (void)superResolution;
    s += "SGSR1 where the eye picture is upscaled";
#endif
    if (checked) s += synthesis ? "; frame synthesis: offered by the runtime" : "; frame synthesis: not offered by the runtime";
    return s + ".";
}

// VR upscaling and frame synthesis (docs/VR_UPSCALING.md, docs/VR_FRAME_SYNTHESIS.md)
void tab_graphics_upscaling() {
    auto& c = S.config;
    ImGui::SeparatorText("Upscaling, frame synthesis and dynamic resolution");
    {
        // VR upscaling (docs/VR_UPSCALING.md)
        // the choices shown (DLSS is not one: docs/VR_DLSS.md; a saved "dlss" shows and runs as Auto)
        static const vr::upscale::Mode kModes[] = {vr::upscale::Mode::Auto, vr::upscale::Mode::Off,
                                                   vr::upscale::Mode::Sgsr1, vr::upscale::Mode::SuperResolution,
                                                   vr::upscale::Mode::Sgsr2, vr::upscale::Mode::Fsr1};
        static const char* const kUpscalers[] = {"Auto", "Off", "SGSR1 (Snapdragon Game Super Resolution)",
                                                 "Quest Super Resolution", "SGSR2 (temporal, Snapdragon)",
                                                 "FSR 1 (AMD FidelityFX)"};
        int u = 0;
        for (int i = 0; i < int(std::size(kModes)); ++i)
            if (kModes[i] == c.upscaler) u = i;
        if (ImGui::Combo("VR upscaling", &u, kUpscalers, int(std::size(kUpscalers)))) c.upscaler = kModes[u];
        help("How each eye's picture is scaled up to the headset's resolution. Off by default on PC VR (Windows, Linux), Auto on standalone headsets. Auto picks the best this device offers: "
             "Quest Super Resolution on a Quest, else SGSR1 where the picture is upscaled (the Steam Frame, a PC). A choice the device does not offer falls back to the "
             "plain scaling. The game shows what is in use in F1 > VR. SGSR2 (by choice only): Qualcomm's temporal "
             "upscaler for Quest and Steam Frame: the game's motion vectors and the previous frames give more detail and "
             "steadier edges, at a higher cost; fast motion can leave trails.");
        ImGui::TextWrapped("%s", auto_pick_text().c_str());
        if (c.upscaler == vr::upscale::Mode::Sgsr1 || c.upscaler == vr::upscale::Mode::Auto) {
            ImGui::SliderFloat("SGSR1 sharpness", &c.upscaleSharpness, 1.0f, 2.0f, "%.2f");
            help("SGSR1's edge sharpening: 1 soft, 2 Qualcomm's default.");
        }
        if (c.upscaler == vr::upscale::Mode::Fsr1) {
            ImGui::SliderFloat("FSR 1 sharpening (stops softer)", &c.fsrSharpness, 0.0f, 2.0f, "%.2f");
            help("FSR 1 (AMD FidelityFX Super Resolution 1): EASU upscaling then RCAS sharpening; upscaling only, no frame "
                 "generation. 0 is the sharpest; each stop halves the sharpening (AMD's samples use 0.2).");
        }
    }
    {
        // VR frame synthesis (docs/VR_FRAME_SYNTHESIS.md)
        static const char* const kSynthesis[] = {"Auto", "Off", "On"};
        int f = int(c.frameSynthesis);
        if (ImGui::Combo("VR frame synthesis", &f, kSynthesis, 3)) c.frameSynthesis = vr::framesynth::Mode(f);
        help("Off by default on PC VR (Windows, Linux), Auto on standalone headsets. The headset makes every other frame itself from the game's motion vectors and depth while the game draws "
             "at half the refresh rate (Meta's Application SpaceWarp on a Quest, frame synthesis on the Steam Frame). "
             "Auto: on standalone headsets that offer it; on a PC the game sends depth for better reprojection. The game "
             "shows what is in use in F1 > VR.");
    }
    {
        // VR dynamic resolution (docs/VR_DYNAMIC_RESOLUTION.md, vr/vr_dynres.h)
        static const char* const kDynres[] = {"Off", "Quality (0.875x to 1x)", "Balanced (0.75x to 1x)",
                                              "Performance (0.625x to 1x)", "Custom"};
        int d = int(c.dynamicResolution);
        if (ImGui::Combo("VR dynamic resolution", &d, kDynres, 5)) c.dynamicResolution = vr::dynres_vr::Mode(d);
        help("While the GPU holds the headset's frame rate back, each eye is drawn at a lower resolution (of the internal "
             "resolution, in steps of 1/8), and higher again once there is room. Measured on the GPU: a frame rate held "
             "back by the CPU keeps the resolution. On a Quest or Steam Frame - Lepton only with multiview off. Changeable in the "
             "game too (F1 > VR); F1 > VR also shows the resolution in use.");
        if (c.dynamicResolution == vr::dynres_vr::Mode::Custom) {
            ImGui::SliderFloat("Lowest scale", &c.dynresMin, 0.5f, 1.0f, "%.3fx");
            ImGui::SliderFloat("Highest scale", &c.dynresMax, 0.5f, 1.0f, "%.3fx");
            if (c.dynresMin > c.dynresMax) c.dynresMin = c.dynresMax;
            ImGui::SliderFloat("GPU target", &c.dynresGpuTarget, 50.0f, 98.0f, "%.0f%% busy");
            ImGui::SliderFloat("Settle time", &c.dynresSettle, 0.5f, 32.0f, "%.1f s");
            help("Scales are of the internal resolution, linear (0.75x is 56 % of the pixels), kept to the steps inside the range "
                 "(1, 0.875, 0.75, 0.625, 0.5). GPU target: how busy the GPU may be before the resolution goes down (up "
                 "again at 10 points under it). Settle time: the wait after a step down before a step up, doubled (up "
                 "to 32 s) when it swings.");
        }
    }
    if (restore_button("upscaling")) {
        const vr::Config d;
        c.upscaler = d.upscaler; c.upscaleSharpness = d.upscaleSharpness; c.fsrSharpness = d.fsrSharpness;
        c.frameSynthesis = d.frameSynthesis;
        c.dynamicResolution = d.dynamicResolution; c.dynresMin = d.dynresMin; c.dynresMax = d.dynresMax;
        c.dynresGpuTarget = d.dynresGpuTarget; c.dynresSettle = d.dynresSettle;
    }
}

// Extra DLLs (dll_loader.h, docs/dll-loader.md): any DLL the game loads from where it is, at every start
extern SDL_Window* g_window;  // (the launcher window, below)
std::mutex g_dllPickMutex;
std::vector<std::string> g_dllPicked;  // the file dialog's answers (it may answer on another thread)
void SDLCALL dll_picked(void*, const char* const* files, int) {
    if (!files) return;
    std::lock_guard<std::mutex> lock(g_dllPickMutex);
    for (int i = 0; files[i]; ++i) g_dllPicked.push_back(files[i]);
}
void tab_graphics_dlls() {
    auto& dlls = S.graphics.dlls;
    {
        std::lock_guard<std::mutex> lock(g_dllPickMutex);
        for (const std::string& p : g_dllPicked)
            if (std::find(dlls.begin(), dlls.end(), p) == dlls.end()) dlls.push_back(p);
        g_dllPicked.clear();
    }
    ImGui::SeparatorText("Extra DLLs");
    if (dlls.empty()) help("None chosen.");
    for (size_t i = 0; i < dlls.size(); ++i) {
        ImGui::PushID(int(i));
        const bool found = std::filesystem::is_regular_file(std::filesystem::u8path(dlls[i]));
        if (ImGui::SmallButton("Remove")) {
            dlls.erase(dlls.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (found) ImGui::TextWrapped("%s", dlls[i].c_str());
        else ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1), "%s (not found)", dlls[i].c_str());
        ImGui::PopID();
    }
    if (ImGui::Button("Add DLL...")) {
        static const SDL_DialogFileFilter filters[] = {{"DLL files (.dll)", "dll"}, {"All files", "*"}};
        const std::string start = dlls.empty() ? std::string() : utf8(std::filesystem::u8path(dlls.back()).parent_path());
        SDL_ShowOpenFileDialog(dll_picked, nullptr, g_window, filters, 2, start.empty() ? nullptr : start.c_str(), true);
    }
    help("Each DLL chosen here is loaded from where it is every time the game starts, before the renderer and VR start "
         "(so a DLL that hooks Vulkan or OpenXR is in place). Save to keep the list. Also in the game: F1 > Graphics > "
         "Extra DLLs. Only choose DLLs you trust: they run with the game's rights. The game logs each one ([dlls] lines).");
}

void tab_graphics() {
    auto& c = S.config;
    ImGui::SeparatorText("Headset");
    bool known, ext;
    float detected;
    {
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        known = g_probe.done && g_probe.instance;
        ext = g_probe.refreshRateExt;
        detected = g_probe.refreshKnown ? g_probe.refreshHz : 0.0f;
    }
    // the refresh rate (one of five) and, from it, the frame-rate choices (vr_config.h frame_rate_choices: Unlocked,
    // capped at the rate, at half of it, Auto); a new rate keeps the frame rate's meaning, its cap follows
    const int hz = int(c.refreshRate);
    if (ImGui::BeginCombo("Refresh rate", vr::refresh_rate_name(hz).c_str())) {
        for (int r : vr::kRefreshRates)
            if (ImGui::Selectable(vr::refresh_rate_name(r).c_str(), r == hz)) vr::choose_refresh_rate(c, r);
        ImGui::EndCombo();
    }
    if (detected > 0 && vr::supported_refresh_rate(detected) != hz)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1), "The headset ran at %.1f Hz when detected (%d Hz chosen)", double(detected), hz);
    help(!known ? "Asked of the headset at the game's start where the runtime lets applications choose (General > Detect "
                  "Headset reads the rate it runs at and selects it)."
         : ext ? "This runtime lets the game choose: the game asks the headset for this rate when it starts (the headset "
                 "decides which rates it offers; the game's settings overlay shows the rate it runs)."
               : "This runtime does not let applications choose: the headset runs at the rate set in the runtime's own "
                 "settings (SteamVR: Settings > Video). Choose that rate here (Detect Headset does).");
    if (slider("Headset resolution", "vr.renderScale", c.renderScale, "%.2fx of the headset's size (the eye pictures)"))
        c.renderScale = vr::snap_headset_resolution(c.renderScale);
    for (size_t i = 0; i < std::size(vr::kHeadsetResolutionPresets); ++i) {
        const float p = vr::kHeadsetResolutionPresets[i];
        char label[24];
        std::snprintf(label, sizeof label, "%.2fx##headsetres", double(p));
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(label, std::fabs(c.renderScale - p) < 0.001f)) c.renderScale = p;
    }
    if (ImGui::BeginCombo("VR frame rate", vr::frame_rate_name(c.frameRate, int(c.refreshRate)).c_str())) {
        for (const auto& r : vr::frame_rate_choices(int(c.refreshRate)))
            if (ImGui::Selectable(r.name.c_str(), r.rate == c.frameRate)) c.frameRate = r.rate;
        ImGui::EndCombo();
    }
    help("Capped: at the refresh rate (90 fps at 90 Hz) or at half of it (45 fps at 90 Hz, 36 at 72 Hz). 72 and 80 fps: "
         "on a streamed headset with Streaming optimal (below), else drawn at 60. Unlocked: as many frames as the headset "
         "shows evenly and this computer draws. Auto Mode: capped at the refresh rate while its frames fit, else at half. "
         "Also in the game: F1 > VR.");
    ImGui::Checkbox("Streaming optimal", &c.streamingOptimal);
    ImGui::BeginDisabled(!c.streamingOptimal);
    ImGui::Checkbox("Depth for Virtual Desktop", &c.vdxrDepth);
    ImGui::EndDisabled();
    help("With Streaming optimal, on Virtual Desktop's own runtime (VDXR): each eye's depth is sent, so Virtual "
         "Desktop's frame smoothing and reprojection follow head movement and depth when a frame is late or "
         "the network stalls. Not sent while text, menus, the GamePad screen or the item wheel show. "
         "Applies at the next start." " Also in the game: F1 > VR.");
    help("For headsets streamed over Wi-Fi or a cable (Virtual Desktop, Air Link, Steam Link, ALVR). At 72 or 80 Hz "
             "the game draws 72 or 80 fps, a frame on every refresh, instead of 60 (a frame missing every sixth refresh, "
             "which the stream shows as lag). At 90 and 120 Hz nothing changes (90 and 120 fps already fit). No effect on "
             "a wired headset. Auto Mode then holds its rate steadier (judged over 6 s, half the rate below 85 %); off, every "
             "frame-rate mode keeps the wired headset's rules. Detect Headset sets it." " Also in the game: F1 > VR.");
    ImGui::Checkbox("Frame interpolation for VR (at the headset's rate: 90 fps at 90 Hz)", &c.autoFrameRate);
    help("Switches interpolation on for the VR session when it is off; the game logic stays at 30 steps per second.");
    ImGui::SeparatorText("Screen effects in the headset");
    {
        bool noBlur = !c.screenBlur, noBloom = !c.bloom;
        if (ImGui::Checkbox("Disable screen blur", &noBlur)) c.screenBlur = !noBlur;
        help("The game's motion blur (the Swift Sail, some attacks and bosses): the last frame laid over the new one, "
             "zoomed. In the headset it follows the world as your head turns; some find its zoom uncomfortable. Also in "
             "the game: F1 > VR.");
        if (ImGui::Checkbox("Disable bloom", &noBloom)) c.bloom = !noBloom;
        help("The glow around bright light (the sun, torches, glowing items). Off in the headset only: the desktop keeps "
             "the game's Bloom strength setting. Also in the game: F1 > VR.");
        bool noHaze = !c.heatHaze;
        if (ImGui::Checkbox("Disable heat haze", &noHaze)) c.heatHaze = !noHaze;
        help("The shimmer over lava and hot ground (Dragon Roost, lava rooms): in the headset it wobbles with your head, "
             "and drawing it costs frames there. Also in the game: F1 > VR.");
        bool noDof = !c.depthOfField;
        if (ImGui::Checkbox("Disable depth of field", &noDof)) c.depthOfField = !noDof;
        help("The game's blur of distant (and very near) scenery. The headset's stereo already shows how far things are, "
             "and the blur stays where the game focuses, not where you look; off, the distance is sharp and it costs less "
             "GPU time. Only the blur: Hyrule's grey and the telescope's edge are kept. Also in the game: F1 > VR.");
        bool noAo = !c.ambientOcclusion;
        if (ImGui::Checkbox("Disable ambient occlusion", &noAo)) c.ambientOcclusion = !noAo;
        help("The soft dark shading under characters and objects and in corners. Off, its pass is skipped (less GPU time "
             "for each eye) and the picture looks flatter; the sun's shadows stay. Also in the game: F1 > VR.");
    }
    ImGui::SeparatorText("Game picture (the game's own graphics settings)");
    int res = 0;
    for (int i = 0; i < 5; ++i)
        if (S.graphics.resScale == kResScales[i]) res = i;
    const char* resNames[] = {"1x (1280x720)", "1.5x (1920x1080)", "2x (2560x1440)", "3x (3840x2160)", "4x (5120x2880)"};
    if (ImGui::Combo("Internal resolution", &res, resNames, 5)) S.graphics.resScale = kResScales[res];
    help("In VR the eye pictures follow Headset resolution (above); this sizes the shadows and the game's other buffers, "
         "and the picture on the desktop outside VR.");
    ImGui::Checkbox("FXAA", &S.graphics.fxaa);
    ImGui::Checkbox("Anisotropic filtering", &S.graphics.aniso);
    ImGui::Checkbox("Prepare shaders at start", &S.graphics.prepareShaders);
    help("Before the game starts, it builds every shader pipeline saved from earlier play while it waits, so first "
         "visits to a place do not stutter while they are built (seconds to a minute or two, longer after a graphics "
         "driver update). On by default. Off: they are built in the background during the title screen. Also in the "
         "game: F1 > Graphics > Shader cache and F1 > VR.");
    tab_graphics_upscaling();
    tab_graphics_dlls();
    ImGui::SeparatorText("Diagnostics");
    ImGui::Checkbox("GamePad window on the desktop too", &c.desktopGamePad);
    help("Off (faster): while VR runs the GamePad screen is shown only in the headset; presenting its desktop window "
         "cost about 8 ms of every frame.");
    ImGui::Checkbox("Performance overlay (in the game window)", &S.graphics.perfOverlay);
    help("The game always logs [vr] timing and eye lines every 10 seconds while VR runs.");
    ImGui::Checkbox("Write a log file", &S.graphics.logFile);
    help("On by default. The game writes everything it logs to captures/wwhd.log (the run before is kept as "
         "wwhd-previous.log; each stops at 64 MiB), from its next start. Send that file with a bug report. The same "
         "setting as F1 > Graphics > Write a log file.");
    if (ImGui::Button("Open the log folder")) {
        // where the game writes captures/: the folder it works in, its release's data folder when it has one (main.cpp
        // enter_data_folder, the same rule), else the game program's folder, where the launcher starts it
        const std::string exe = find_game(S.gameExe);
        if (!exe.empty()) {
            const std::filesystem::path exeDir = std::filesystem::u8path(exe).parent_path();
            const char* install = std::getenv("WWHD_INSTALL_DIR");
            const std::filesystem::path data = start_folder::find(exeDir, "", install ? install : "");
            const std::filesystem::path dir = (data.empty() ? exeDir : data) / "captures";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            SDL_OpenURL(("file:///" + utf8(dir)).c_str());
        }
    }
    help("Opens the captures folder: wwhd.log, wwhd-previous.log and crash logs (crash-<time>.log). Set the game's path "
         "in General > Game first.");
    if (restore_button("graphics")) {
        const vr::Config d;
        c.refreshRate = d.refreshRate; c.renderScale = d.renderScale; c.autoFrameRate = d.autoFrameRate; c.frameRate = d.frameRate;
        c.streamingOptimal = d.streamingOptimal;
        c.vdxrDepth = d.vdxrDepth;
        c.desktopGamePad = d.desktopGamePad;
    }
}

// ---------------------------------------------------------------------------------------------
// Install game (docs/VR_LAUNCHER.md, "Install game"): a disc image (.wud, .wux) or a Cemu archive (.wua) extracted
// by wwhd-extract to a folder, checked, and that folder set as the game's (vr-launcher.ini launcher.gameDir). The
// decisions are game_install.h's, the process extract_runner.h's; this is the window.

SDL_Window* g_window = nullptr;

#ifdef _WIN32
constexpr const char* kExtractorName = "wwhd-extract.exe";
#else
constexpr const char* kExtractorName = "wwhd-extract";
#endif
// wwhd-extract: next to the launcher, else next to the game program
std::string find_extractor() {
#ifdef __ANDROID__
    // the app's native-library folder (VrLauncherActivity passes it), where Android lets an app run a program: there as
    // libwwhd-extract-driver.so (android/build_native.sh VR=1; the name the app's setup uses too)
    if (const char* d = SDL_getenv("WWHD_NATIVE_LIB_DIR")) {
        const std::string p = std::string(d) + "/libwwhd-extract-driver.so";
        if (exists(p)) return p;
    }
    return {};
#endif
    std::vector<std::string> dirs = {host::exe_dir()};
    const std::string exe = find_game(S.gameExe);
    if (!exe.empty()) dirs.push_back(utf8(std::filesystem::u8path(exe).parent_path()));
    for (const auto& d : dirs) {
        const std::string p = d + "/" + kExtractorName;
        if (exists(p)) return p;
    }
    return {};
}

struct Install {
    enum class Step { Idle, Info, Meta, Ready, Extracting, Done, Failed, Cancelled };
    Step step = Step::Idle;
    std::string source, discKey, commonKey, dest;
    bool configure = true;
    // what was detected
    install::SourceInfo info;
    std::string titleId, title, folder, detectError, titleNote;
    std::string metaDir;  // the temporary folder meta/meta.xml was extracted to
    // the destination (checked when it changes)
    std::string checkedDest;
    install::Destination destInfo;
    bool destWasEmpty = true;  // before this extraction: may its folder be removed after a cancel or failure
    // the result
    std::vector<std::string> done;  // what was verified, in order
    std::string result, resultError;
    std::string configPath, backupPath;
    install::Runner runner;
    // a picker's answer (its callback may come on another thread)
    std::mutex pickMutex;
    int pickWhat = -1;
    std::string picked;
    bool confirmOverwrite = false, confirmCleanup = false;
#ifdef __ANDROID__
    // Android: what the system's file picker gives is a content:// link, not a path the extractor can open. It is
    // copied into the app's folder first (import/), on a worker thread, and the copy is used (and removed after the
    // game is extracted)
    struct Import {
        std::thread thread;
        std::atomic<uint64_t> done{0}, total{0};
        std::atomic<bool> running{false}, finished{false}, cancel{false};
        std::string error, path;
        int what = -1;
    } import;
    // several files picked at once (an image with its keys): copied one after another, each to what its name says
    std::vector<std::string> pickedAll;
    std::vector<std::pair<int, std::string>> importQueue;
    bool importedAny = false;
    // the extract location's browser (Android has no folder picker): real folders, the app's storage or (with All
    // files access) the device's shared storage
    bool browse = false;
    std::string browseDir, newFolder;
#endif
} I;

enum Pick { kPickSource, kPickDest, kPickDiscKey, kPickCommonKey };
void SDLCALL picked(void* what, const char* const* files, int) {
    if (!files || !files[0]) return;  // cancelled (or an error: nothing chosen)
    std::lock_guard<std::mutex> lock(I.pickMutex);
    I.pickWhat = int(reinterpret_cast<intptr_t>(what));
    I.picked = files[0];
#ifdef __ANDROID__
    I.pickedAll.clear();
    for (int i = 0; files[i]; ++i) I.pickedAll.push_back(files[i]);
#endif
}
void pick(Pick what) {
    void* tag = reinterpret_cast<void*>(intptr_t(what));
#ifdef __ANDROID__
    // (Android's picker filters by MIME type, which .wud / .wux / .wua have none of: every file is offered)
    // the source: several files at once (the image with its .key file and common.key: each is used for what its name
    // says, and the extractor finds the keys next to the image)
    if (what != kPickDest) SDL_ShowOpenFileDialog(picked, tag, g_window, nullptr, 0, nullptr, what == kPickSource);
    return;
#endif
    if (what == kPickDest) {
        const std::string start = !I.dest.empty() ? I.dest : I.source.empty() ? std::string() :
                                  utf8(std::filesystem::u8path(I.source).parent_path());
        SDL_ShowOpenFolderDialog(picked, tag, g_window, start.empty() ? nullptr : start.c_str(), false);
        return;
    }
    static const SDL_DialogFileFilter games[] = {{"Wii U games (.wud, .wux, .wua)", "wud;wux;wua"}, {"All files", "*"}};
    static const SDL_DialogFileFilter keys[] = {{"Key files (.key, .bin, .txt)", "key;bin;txt"}, {"All files", "*"}};
    const std::string start = I.source.empty() ? std::string() : utf8(std::filesystem::u8path(I.source).parent_path());
    SDL_ShowOpenFileDialog(picked, tag, g_window, what == kPickSource ? games : keys, 2, start.empty() ? nullptr : start.c_str(), false);
}

bool is_archive_source() { return I.info.archive; }
std::vector<std::string> key_args() {
    std::vector<std::string> a;
    if (!I.discKey.empty()) { a.push_back("--disc-key"); a.push_back(I.discKey); }
    if (!I.commonKey.empty()) { a.push_back("--common-key"); a.push_back(I.commonKey); }
    return a;
}
void remove_meta_dir() {
    if (I.metaDir.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(I.metaDir), ec);
    I.metaDir.clear();
}
// a source chosen (or its keys changed): read it again from the start
void detect() {
    I.runner.cancel();
    I.runner.wait();
    remove_meta_dir();
    I.info = {};
    I.titleId.clear(); I.title.clear(); I.folder.clear(); I.detectError.clear(); I.titleNote.clear();
    I.result.clear(); I.resultError.clear(); I.done.clear();
    I.step = Install::Step::Idle;
    if (I.source.empty()) return;
    std::error_code ec;
    const auto src = std::filesystem::u8path(I.source);
#ifdef __ANDROID__
    // keys imported before (they are kept in the app's import folder): the image's own (<name>.key, which the
    // extractor finds next to it by itself) and the console's common key (common.key, or a file named "common")
    if (I.commonKey.empty() && src.parent_path() == std::filesystem::u8path(android_files()) / "import") {
        for (const auto& e : std::filesystem::directory_iterator(src.parent_path(), ec)) {
            const std::string n = install::lower(utf8(e.path().filename()));
            if (n != "common.key" && n.find("common") != std::string::npos && e.is_regular_file(ec)) I.commonKey = utf8(e.path());
        }
    }
#endif
    if (!std::filesystem::exists(src, ec)) { I.detectError = "The selected file does not exist."; I.step = Install::Step::Failed; return; }
    if (std::filesystem::is_directory(src, ec)) {
        I.detectError = "This is a folder. If it is an already extracted game (with code, content and meta in it), set it "
                        "in General > Game files; otherwise choose the .wud, .wux or .wua file.";
        I.step = Install::Step::Failed;
        return;
    }
    const std::string x = find_extractor();
    if (x.empty()) {
        I.detectError = std::string("The extractor (") + kExtractorName + ") was not found next to the launcher or the game.";
        I.step = Install::Step::Failed;
        return;
    }
    std::vector<std::string> args = {x};
    for (auto& k : key_args()) args.push_back(k);
    args.push_back("info");
    args.push_back(I.source);
    I.step = Install::Step::Info;
    if (!I.runner.start(args)) {
        I.detectError = "The extractor could not be started: " + I.runner.snapshot().error;
        I.step = Install::Step::Failed;
    }
}
// after `info`: the title id (and the archive's title); then its meta/meta.xml for the name
#ifdef __ANDROID__
// what the extractor said, for the message: its own error lines, else (a code not its own: it did not run, as when
// Android's linker refused it) its last lines; also to the device log
std::string android_detail(const install::Runner::Snapshot& s) {
    std::string d = install::error_detail(s.output);
    if (d.empty() && (s.exitCode < 2 || s.exitCode > 10)) {
        std::string tail = s.startFailed ? s.error : s.output;
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
        if (tail.size() > 400) tail = "..." + tail.substr(tail.size() - 400);
        d = tail;
    }
    SDL_Log("[launcher] wwhd-extract ended with %d: %s", s.exitCode, s.startFailed ? s.error.c_str() : s.output.c_str());
    return d;
}
#endif
void after_info(const install::Runner::Snapshot& s) {
    I.info = install::parse_info(s.output);
    if (s.exitCode != 0) {
        // an archive with several titles and none chosen also ends here only when info failed otherwise
#ifdef __ANDROID__
        I.detectError = install::extract_error(s.exitCode, android_detail(s), I.info.archive);
#else
        I.detectError = install::extract_error(s.exitCode, install::error_detail(s.output), I.info.archive);
#endif
        I.step = Install::Step::Failed;
        return;
    }
    if (I.info.archive) {
        std::string why;
        const install::ArchiveTitle* t = install::choose_archive_title(I.info.titles, why);
        if (!t) { I.detectError = why; I.step = Install::Step::Failed; return; }
        I.folder = t->folder;
        I.titleId = t->id;
        I.info.files = t->files;
        I.info.bytes = t->bytes;
        I.info.version = t->version;
    } else {
        I.titleId = I.info.titleId;
    }
    if (!install::is_hex16(I.titleId)) { I.detectError = "The extractor did not report a title ID for this file."; I.step = Install::Step::Failed; return; }
    // its meta/meta.xml alone, to a temporary folder
    char name[64];
    std::snprintf(name, sizeof name, "wwhd-install-%llu", (unsigned long long)SDL_GetTicksNS());
    std::error_code ec;
    I.metaDir = utf8(std::filesystem::temp_directory_path(ec) / name);
    std::vector<std::string> args = {find_extractor()};
    for (auto& k : key_args()) args.push_back(k);
    if (!I.folder.empty()) { args.push_back("--title"); args.push_back(I.folder); }
    args.insert(args.end(), {"--only", "meta/meta.xml", "--progress", "extract", I.source, I.metaDir});
    I.step = Install::Step::Meta;
    if (!I.runner.start(args)) { I.step = Install::Step::Ready; I.title.clear(); }
}
void after_meta(const install::Runner::Snapshot& s) {
    const install::Meta m = s.exitCode == 0 ? install::parse_meta(install::read_file(std::filesystem::u8path(I.metaDir) / "meta" / "meta.xml"))
                                            : install::Meta{};
    remove_meta_dir();
    I.title = m.title;
    if (I.title.empty())
        I.titleNote = s.exitCode != 0 ? "Its name could not be read: " + install::extract_error(s.exitCode, install::error_detail(s.output), I.info.archive)
                                      : "Its meta/meta.xml has no English name.";
    const install::KnownTitle* k = install::known_title(I.titleId);
    if (!k) I.titleNote += std::string(I.titleNote.empty() ? "" : " ") + "This title ID is not The Wind Waker HD's.";
    else if (!k->supported) I.titleNote += std::string(I.titleNote.empty() ? "" : " ") + "This is the " + k->region +
                                           " version of The Wind Waker HD, which this port is not built for (USA or Europe).";
    I.step = Install::Step::Ready;
#ifdef __ANDROID__
    // Android: the game's own folder in the app's storage (the game's default "game", where it runs); no folder picker
    if (I.dest.empty()) I.dest = android_files() + "/game";
#endif
    // a suggestion for where it goes: a new folder next to the file, never the launcher's own folder
    if (I.dest.empty()) {
        const auto src = std::filesystem::u8path(I.source);
        I.dest = utf8(src.parent_path() / (utf8(src.stem()) + " (extracted)"));
    }
}

#ifdef __ANDROID__
void remove_imported_source();
#endif
// which version the configured game program was built from (wwhd --guest-build); empty when it cannot say (the
// release's own program, or an older wwhd)
std::string game_build_title(std::string& note) {
#ifdef __ANDROID__
    note = "on Android the game checks this itself when it starts";
    return {};
#endif
    const std::string exe = find_game(S.gameExe);
    if (exe.empty() || !is_game_itself(exe)) { note = "the game program is not wwhd itself, so its version was not asked"; return {}; }
    install::Runner r;
    if (!r.start({exe, "--guest-build"})) { note = "the game program could not be asked for its version"; return {}; }
    const auto s = r.wait();
    std::istringstream in(s.output);
    std::string word, name, id;
    while (in >> word)
        if (word == "build" && in >> name >> id) return install::lower(id);
    note = "the game program did not report its version (an older build)";
    return {};
}
// the extracted (or already present) game checked, then set as the game's folder
void finish(bool extracted) {
    I.done.clear();
    if (extracted) I.done.push_back("Extraction completed.");
    const install::Verify v = install::verify_game(I.dest, I.titleId);
    if (!v.ok) {
        I.step = Install::Step::Failed;
        I.resultError = "The extracted folder is not a complete game:";
        for (auto& p : v.problems) I.resultError += "\n- " + p;
        return;
    }
    I.done.push_back("The game's files are all there (code/cking.rpx, content, meta/meta.xml, title " + install::title_text(I.titleId) + ").");
    I.step = Install::Step::Done;
#ifdef __ANDROID__
    if (extracted && I.source.rfind(android_files() + "/import/", 0) == 0) {
        remove_imported_source();  // (the launcher's own copy of the picked file)
        I.done.push_back("The copy of the source file in the app's folder was removed.");
    }
#endif
    if (!I.configure) {
        I.result = "Not set as the game's folder (you chose not to). To use it, set it in General > Game files.";
        return;
    }
    std::string note;
    const std::string built = game_build_title(note);
    if (!built.empty() && built != I.titleId) {
        I.resultError = "The game's settings were not changed: this game program was built from title " + install::title_text(built) +
                        ", and the extracted game is " + install::title_text(I.titleId) +
                        ". It would refuse to start with it. The extracted files are kept.";
        return;
    }
    if (!built.empty()) I.done.push_back("The game program was built from this same version (title " + install::title_text(built) + ").");
    else I.done.push_back("Not checked: whether the game program was built from this version (" + note + ").");
    if (game_running()) {
        I.resultError = "The game is running: close it, then press \"Use the game already there\" to set this folder.";
        return;
    }
    I.configPath = launcher_path();
    const install::ConfigResult c = install::configure_game_dir(I.configPath, I.dest, replace);
    if (!c.ok) {
        I.resultError = "The game was extracted, but the launcher's settings could not be updated (" + c.error +
                        "). Set the folder by hand in General > Game files:\n" + I.dest;
        return;
    }
    I.backupPath = c.backup;
    S.gameDir = S.savedGameDir = I.dest;  // the launcher's own state, so it is not an unsaved change
    S.launcherFile.load(launcher_path(), note);
    I.done.push_back("Set as the game's folder: launcher.gameDir in " + I.configPath + " (read back and checked)" +
                     (c.backup.empty() ? "." : "; the previous file is kept as " + c.backup + "."));
    I.result = "The game is ready to start from the General tab. (It has not been started yet.)";
}
void start_extract() {
    const auto d = install::check_destination(I.dest, host::exe_dir());
    if (!d.valid) { I.resultError = d.problem; return; }
    I.destWasEmpty = !d.exists || d.empty;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(I.dest), ec);
    if (ec) { I.resultError = "The folder could not be created: " + ec.message(); return; }
    std::vector<std::string> args = {find_extractor()};
    for (auto& k : key_args()) args.push_back(k);
    if (!I.folder.empty()) { args.push_back("--title"); args.push_back(I.folder); }
    args.insert(args.end(), {"--progress", "extract", I.source, I.dest});
    I.result.clear(); I.resultError.clear(); I.done.clear();
    I.step = Install::Step::Extracting;
    if (!I.runner.start(args)) { I.step = Install::Step::Failed; I.resultError = "The extractor could not be started: " + I.runner.snapshot().error; }
    I.checkedDest.clear();
}
#ifdef __ANDROID__
// the file name in a content:// link (its last path segment, %-decoded; documents name it "primary:Download/x.wua")
std::string uri_file_name(const std::string& uri) {
    std::string d;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            d += char(std::strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            d += uri[i];
        }
    }
    const size_t cut = d.find_last_of("/:");
    std::string name = cut == std::string::npos ? d : d.substr(cut + 1);
    for (char& c : name)
        if (c == '\\' || c == '"' || c < 32) c = '_';
    return name.empty() ? std::string("picked-file") : name;
}
// a picked file copied into the app's folder (import/) on a worker thread
void start_import(int what, const std::string& uri) {
    auto& im = I.import;
    if (im.running) return;
    if (im.thread.joinable()) im.thread.join();
    im.what = what;
    im.error.clear();
    im.done = im.total = 0;
    im.finished = false;
    im.cancel = false;
    im.running = true;
    const std::filesystem::path dir = std::filesystem::u8path(android_files()) / "import";
    im.path = utf8(dir / std::filesystem::u8path(uri_file_name(uri)));
    const long long known = android_call_long("contentSize", uri);  // (its provider's size: SDL's may be unknown)
    SDL_Log("[launcher] copying %s into %s (%lld bytes)", uri.c_str(), im.path.c_str(), known);
    im.thread = std::thread([uri, dir, known] {
        auto& m = I.import;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        SDL_IOStream* in = SDL_IOFromFile(uri.c_str(), "rb");
        std::FILE* out = in ? std::fopen(m.path.c_str(), "wb") : nullptr;
        if (!in) m.error = std::string("The file could not be opened: ") + SDL_GetError();
        else if (!out) m.error = "The app's folder could not be written (" + m.path + ").";
        else {
            const Sint64 size = SDL_GetIOSize(in);
            m.total = size > 0 ? uint64_t(size) : known > 0 ? uint64_t(known) : 0;
            std::vector<char> buf(4 << 20);
            for (;;) {
                if (m.cancel) { m.error = "Copying was cancelled."; break; }
                const size_t n = SDL_ReadIO(in, buf.data(), buf.size());
                if (n == 0) {
                    if (SDL_GetIOStatus(in) != SDL_IO_STATUS_EOF) m.error = std::string("Reading the file failed: ") + SDL_GetError();
                    break;
                }
                if (std::fwrite(buf.data(), 1, n, out) != n) { m.error = "The copy could not be written (is the storage full?)."; break; }
                m.done += n;
            }
        }
        if (out && std::fclose(out) != 0 && m.error.empty()) m.error = "The copy could not be written (is the storage full?).";
        if (in) SDL_CloseIO(in);
        if (!m.error.empty()) std::filesystem::remove(std::filesystem::u8path(m.path), ec);
        m.running = false;
        m.finished = true;
    });
}
// the copy of the source, once the game is extracted from it (or another file is chosen): removed
void remove_imported_source() {
    if (I.source.empty()) return;
    const std::filesystem::path src = std::filesystem::u8path(I.source);
    if (src.parent_path() != std::filesystem::u8path(android_files()) / "import") return;  // (only our own copies)
    std::error_code ec;
    std::filesystem::remove(src, ec);
}
#endif

// each frame: a picker's answer, and a finished process
void install_update() {
    {
        std::lock_guard<std::mutex> lock(I.pickMutex);
        if (I.pickWhat >= 0) {
            const int what = I.pickWhat;
            I.pickWhat = -1;
            if (what == kPickDest) { I.dest = I.picked; I.checkedDest.clear(); }
#ifdef __ANDROID__
            else if (I.picked.rfind("content://", 0) == 0) {
                // each picked file to what its name says: a .key file is a key (common.key, or one named "common",
                // the console's common key; any other the disc's), anything else the game source
                I.importQueue.clear();
                bool source = false;
                for (const auto& uri : I.pickedAll) {
                    int as = what;
                    if (what == kPickSource) {
                        const std::string n = install::lower(uri_file_name(uri));
                        const bool key = n.size() > 4 && n.compare(n.size() - 4, 4, ".key") == 0;
                        as = key ? (n.find("common") != std::string::npos ? kPickCommonKey : kPickDiscKey) : kPickSource;
                    }
                    if (as == kPickSource) {
                        if (source) continue;  // (one game source: the first)
                        source = true;
                    }
                    I.importQueue.push_back({as, uri});
                }
                if (source) remove_imported_source();
                I.importedAny = false;
                if (!I.importQueue.empty()) {
                    const auto next = I.importQueue.front();
                    I.importQueue.erase(I.importQueue.begin());
                    start_import(next.first, next.second);
                }
            }
#endif
            else {
                if (what == kPickSource) { I.source = I.picked; I.dest.clear(); }
                if (what == kPickDiscKey) I.discKey = I.picked;
                if (what == kPickCommonKey) I.commonKey = I.picked;
                detect();
            }
        }
    }
#ifdef __ANDROID__
    if (I.import.finished.exchange(false)) {
        if (I.import.thread.joinable()) I.import.thread.join();
        if (!I.import.error.empty()) {
            SDL_Log("[launcher] copy failed: %s", I.import.error.c_str());
            I.importQueue.clear();
            I.detectError = I.import.error;
            I.step = Install::Step::Failed;
        } else {
            if (I.import.what == kPickSource) I.source = I.import.path;
            if (I.import.what == kPickDiscKey) I.discKey = I.import.path;
            if (I.import.what == kPickCommonKey) I.commonKey = I.import.path;
            I.importedAny = true;
            if (!I.importQueue.empty()) {
                const auto next = I.importQueue.front();
                I.importQueue.erase(I.importQueue.begin());
                start_import(next.first, next.second);
            } else {
                detect();
            }
        }
    }
#endif
    const auto s = I.runner.snapshot();
    if (!s.finished || s.running) return;
    if (I.step == Install::Step::Info) after_info(s);
    else if (I.step == Install::Step::Meta) after_meta(s);
    else if (I.step == Install::Step::Extracting) {
        I.checkedDest.clear();
        if (s.cancelled) {
            I.step = Install::Step::Cancelled;
            I.resultError = "Extraction was cancelled. Some files may remain in the destination folder; the game's settings "
                            "were not changed.";
        } else if (s.exitCode != 0 || s.startFailed) {
            I.step = Install::Step::Failed;
#ifdef __ANDROID__
            I.resultError = "Extraction failed. " + install::extract_error(s.exitCode, android_detail(s), I.info.archive) +
#else
            I.resultError = "Extraction failed. " + install::extract_error(s.exitCode, install::error_detail(s.output), I.info.archive) +
#endif
                            "\nSome files may remain in the destination folder; the game's settings were not changed.";
        } else {
            finish(true);
        }
    }
}

#ifdef __ANDROID__
// The extract location on Android: real folders (the extractor writes by path), in the app's own storage (always) or
// the device's shared storage (with All files access, which this asks the system for). The game reads it from the
// same app, so it can use it too (its folder is passed with --game)
void dest_browser() {
    if (I.browse) {
        ImGui::OpenPopup("Extract to##browser");
        I.browse = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.9f, vp->WorkSize.y * 0.85f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Extract to##browser", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    static double checked = -10;
    static bool allFiles = false;
    if (ImGui::GetTime() - checked > 1.0) {  // (back from the system's permission screen)
        allFiles = android_call_bool("hasAllFilesAccess");
        checked = ImGui::GetTime();
    }
    const std::string appRoot = android_files(), deviceRoot = "/storage/emulated/0";
    if (ImGui::Button("App storage")) I.browseDir = appRoot;
    ImGui::SameLine();
    if (allFiles) {
        if (ImGui::Button("Device storage")) I.browseDir = deviceRoot;
    } else if (ImGui::Button("Allow device storage...")) {
        android_call("requestAllFilesAccess", nullptr);
    }
    if (!allFiles) help("Device storage (your Download folder, for example) needs All files access for this app; App storage always works.");
    ImGui::Separator();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path here = fs::u8path(I.browseDir);
    ImGui::TextWrapped("%s", I.browseDir.c_str());
    const bool atRoot = I.browseDir == appRoot || I.browseDir == deviceRoot || here == here.parent_path();
    ImGui::BeginDisabled(atRoot);
    if (ImGui::Button("Up")) I.browseDir = utf8(here.parent_path());
    ImGui::EndDisabled();
    ImGui::SameLine();
    char name[128];
    std::snprintf(name, sizeof name, "%s", I.newFolder.c_str());
    ImGui::SetNextItemWidth(260 * ImGui::GetStyle().FontScaleDpi);
    if (ImGui::InputTextWithHint("##newfolder", "new folder name", name, sizeof name)) I.newFolder = name;
    ImGui::SameLine();
    ImGui::BeginDisabled(I.newFolder.empty() || I.newFolder.find('/') != std::string::npos || I.newFolder == "." || I.newFolder == "..");
    if (ImGui::Button("Create folder")) {
        const fs::path made = here / fs::u8path(I.newFolder);
        if (fs::create_directories(made, ec) || fs::is_directory(made, ec)) {
            I.browseDir = utf8(made);
            I.newFolder.clear();
        }
    }
    ImGui::EndDisabled();
    // its folders, alphabetically (hidden ones left out)
    std::vector<std::string> dirs;
    for (const auto& e : fs::directory_iterator(here, fs::directory_options::skip_permission_denied, ec)) {
        const std::string n = utf8(e.path().filename());
        if (!n.empty() && n[0] != '.' && e.is_directory(ec)) dirs.push_back(n);
    }
    std::sort(dirs.begin(), dirs.end());
    ImGui::BeginChild("##folders", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f), ImGuiChildFlags_Borders);
    if (ec) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "This folder cannot be read (%s).", ec.message().c_str());
    for (const auto& n : dirs)
        if (ImGui::Selectable((n + "/").c_str())) I.browseDir = utf8(here / fs::u8path(n));
    if (dirs.empty() && !ec) ImGui::TextDisabled("(no folders here)");
    ImGui::EndChild();
    if (ImGui::Button("Use this folder")) {
        I.dest = I.browseDir;
        I.checkedDest.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##browser")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
#endif

void tab_install() {
    install_update();
#ifdef __ANDROID__
    if (I.dest.empty()) I.dest = android_files() + "/game";  // (the game's own folder, until another is chosen)
    dest_browser();
#endif
    const bool busy = I.runner.running();
#ifdef __ANDROID__
    help("Extract the game from your own dump of it: a Cemu archive (.wua: already decrypted, no keys needed) or a Wii U "
         "disc image (.wud or .wux: decrypted while it is extracted, with its keys from your own console). For a disc "
         "image, choose the image, its .key file and your common.key together: each is used for what its name says. The "
         "files are copied into the app's folder first (the image is removed again once the game is extracted; the keys "
         "are kept for next time): keep room for the copy.");
    const bool importing = I.import.running.load();
#else
    help("Extract the game from your own dump of it, for the game built from source (wwhd): a Wii U disc image (.wud or "
         ".wux, with its .key file and your console's common key) or a Cemu archive (.wua, no keys needed). The release's "
         "own program does this in its first start instead.");
    constexpr bool importing = false;
#endif

    ImGui::SeparatorText("Game source");
    ImGui::BeginDisabled(busy || importing);
    ImGui::SetNextItemWidth(-110 * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputText("##source", I.source.data(), I.source.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (!I.source.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", I.source.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Browse...##source")) pick(kPickSource);
    if (!I.info.archive && !I.source.empty()) {
        help("Disc images need two keys from your own console. Without a choice here, the extractor looks for the image's "
             "name with .key and for common.key next to the image.");
        auto keyRow = [&](const char* label, std::string& v, Pick what, const char* id) {
            ImGui::Text("%s", label);
            ImGui::SetNextItemWidth(-200 * ImGui::GetStyle().FontScaleDpi);
            std::string shown = v.empty() ? "(automatic)" : v;
            ImGui::InputText(id, shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            ImGui::PushID(id);
            if (ImGui::Button("Browse...")) pick(what);
            ImGui::SameLine();
            if (ImGui::Button("Automatic") && !v.empty()) { v.clear(); detect(); }
            ImGui::PopID();
        };
        keyRow("Disc key file", I.discKey, kPickDiscKey, "##disckey");
        keyRow("Wii U common key file", I.commonKey, kPickCommonKey, "##commonkey");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Detected game");
    if (I.step == Install::Step::Info || I.step == Install::Step::Meta) {
        ImGui::TextDisabled("Reading the game's information...");
    } else if (!I.detectError.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Not usable:");
        ImGui::TextWrapped("%s", I.detectError.c_str());
    } else if (!I.titleId.empty()) {
        ImGui::Text("Title:     %s", I.title.empty() ? "Unknown title" : I.title.c_str());
        const install::KnownTitle* k = install::known_title(I.titleId);
        ImGui::Text("Title ID:  %s%s%s%s", install::title_text(I.titleId).c_str(), k ? " (" : "", k ? k->region : "", k ? ")" : "");
        ImGui::Text("Size:      %s in %llu files%s", install::size_text(I.info.bytes).c_str(), (unsigned long long)I.info.files,
                    I.info.archive ? " (Cemu archive)" : " (disc image)");
        if (!I.titleNote.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.4f, 1), "%s", I.titleNote.c_str());
    } else {
        ImGui::TextDisabled("Choose a file above.");
    }

    ImGui::SeparatorText("Extract to");
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-110 * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputText("##dest", I.dest.data(), I.dest.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (!I.dest.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", I.dest.c_str());
#ifndef __ANDROID__
    ImGui::SameLine();
    if (ImGui::Button("Browse...##dest")) pick(kPickDest);
#else
    // Android has no folder picker: the launcher's own browser of real folders (dest_browser)
    ImGui::SameLine();
    if (ImGui::Button("Choose...##dest")) {
        std::error_code ec;
        std::filesystem::path at = std::filesystem::u8path(I.dest);
        while (!at.empty() && !std::filesystem::is_directory(at, ec) && at != at.parent_path()) at = at.parent_path();
        I.browseDir = std::filesystem::is_directory(at, ec) ? utf8(at) : android_files();
        I.browse = true;
    }
#endif
    ImGui::EndDisabled();
    if (I.checkedDest != I.dest && !busy) {
        I.checkedDest = I.dest;
        I.destInfo = install::check_destination(I.dest, host::exe_dir());
    }
    const install::Destination& d = I.destInfo;
    const bool sameGame = d.hasGame && !I.titleId.empty() && d.gameTitleId == I.titleId;
    if (!I.dest.empty()) {
        if (!d.valid) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", d.problem.c_str());
        else {
            std::string line = !d.exists ? "A new folder will be made." : d.empty ? "The folder is empty." :
                               sameGame ? "This folder already holds this game." :
                               d.hasGame && d.gameTitleId.empty() ? "This folder already holds a game (its version could not be read): extracting replaces its files." :
                               d.hasGame ? "This folder already holds a game (another version): extracting replaces its files." :
                               "The folder is not empty: files with the same names are replaced, nothing else is deleted.";
            if (d.spaceKnown) {
                line += " Free space: " + install::size_text(d.freeBytes);
                if (I.info.bytes) line += d.freeBytes < I.info.bytes ? " (not enough: " + install::size_text(I.info.bytes) + " needed)." : ".";
                else line += ".";
            } else {
                line += " The free space could not be determined.";
            }
            ImGui::TextWrapped("%s", line.c_str());
        }
    }
    ImGui::BeginDisabled(busy);
    ImGui::Checkbox("Configure ZeldaWWHD-VR to use this game", &I.configure);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets the launcher's game folder (launcher.gameDir in %s) once the game is extracted and checked.", launcher_path().c_str());

    ImGui::SeparatorText("Extraction progress");
    const auto s = I.runner.snapshot();
#ifdef __ANDROID__
    if (I.import.running) {
        // the picked file copied into the app's folder first (the extraction's first stage on Android)
        const uint64_t done = I.import.done, total = I.import.total;
        char overlay[80];
        if (total) std::snprintf(overlay, sizeof overlay, "%.0f%%  (%s of %s)", double(done) * 100.0 / double(total),
                                 install::size_text(done).c_str(), install::size_text(total).c_str());
        else std::snprintf(overlay, sizeof overlay, "%s", install::size_text(done).c_str());
        ImGui::ProgressBar(total ? float(double(done) / double(total)) : -1.0f * float(ImGui::GetTime()), ImVec2(-1, 0), overlay);
        ImGui::Text("Copying the file into the app's folder (%zu more after it)...", I.importQueue.size());
    } else if (I.step == Install::Step::Info || I.step == Install::Step::Meta) {
        ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(-1, 0), "Reading the game's information...");
    } else
#endif
    if (I.step == Install::Step::Extracting) {
        const char* phase = s.progress.phase == "verify" ? "Checking the archive (its checksum)..." : "Extracting game files...";
        if (s.progress.total > 0) {
            const float f = float(double(s.progress.done) / double(s.progress.total));
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%.0f%%  (%s of %s)", double(f) * 100.0, install::size_text(s.progress.done).c_str(),
                          install::size_text(s.progress.total).c_str());
            ImGui::ProgressBar(f, ImVec2(-1, 0), overlay);
        } else {
            ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(-1, 0), "Preparing...");  // no total yet: indeterminate
        }
        ImGui::Text("%s", phase);
    } else if (I.step == Install::Step::Done) {
        ImGui::ProgressBar(1.0f, ImVec2(-1, 0), "Done");
    } else {
        ImGui::ProgressBar(0.0f, ImVec2(-1, 0), "");
    }

    const bool ready = !busy && !I.titleId.empty() && I.detectError.empty() && d.valid &&
                       (I.step == Install::Step::Ready || I.step == Install::Step::Done || I.step == Install::Step::Failed ||
                        I.step == Install::Step::Cancelled);
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Extract Game", ImVec2(180 * ImGui::GetStyle().FontScaleDpi, 0))) {
        if (d.exists && !d.empty) I.confirmOverwrite = true;
        else start_extract();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
#ifdef __ANDROID__
    ImGui::BeginDisabled(!busy && !importing);  // (also the copy into the app's folder)
    if (ImGui::Button("Cancel", ImVec2(120 * ImGui::GetStyle().FontScaleDpi, 0))) {
        I.import.cancel = true;
        I.importQueue.clear();
        I.runner.cancel();
    }
    ImGui::EndDisabled();
#else
    ImGui::BeginDisabled(!busy);
    if (ImGui::Button("Cancel", ImVec2(120 * ImGui::GetStyle().FontScaleDpi, 0))) I.runner.cancel();
    ImGui::EndDisabled();
#endif
    if (sameGame && !busy) {
        ImGui::SameLine();
        if (ImGui::Button("Use the game already there")) finish(false);
    }

    // status
    ImGui::Separator();
    if (busy) ImGui::Text("Status: %s", I.step == Install::Step::Extracting ? "extracting" : "reading the source");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.5f, 1));
    for (const auto& line : I.done) ImGui::TextWrapped("%s", line.c_str());
    ImGui::PopStyleColor();
    if (!I.result.empty()) ImGui::TextWrapped("%s", I.result.c_str());
    if (!I.resultError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.4f, 1));
        ImGui::TextWrapped("%s", I.resultError.c_str());
        ImGui::PopStyleColor();
    }
    if (!busy && I.step == Install::Step::Ready && I.result.empty() && I.resultError.empty()) ImGui::Text("Status: Ready");
    if (I.step == Install::Step::Done || ((I.step == Install::Step::Failed || I.step == Install::Step::Cancelled) && exists(I.dest))) {
#ifndef __ANDROID__  // (Android does not open an app's private folders in a file manager by a file:// link)
        if (ImGui::Button("Open the game folder")) SDL_OpenURL(("file:///" + I.dest).c_str());
        if ((I.step == Install::Step::Failed || I.step == Install::Step::Cancelled) && I.destWasEmpty && exists(I.dest)) {
            ImGui::SameLine();
#else
        if ((I.step == Install::Step::Failed || I.step == Install::Step::Cancelled) && I.destWasEmpty && exists(I.dest)) {
#endif
            if (ImGui::Button("Remove the partly extracted files...")) I.confirmCleanup = true;
        }
    }

    if (I.confirmOverwrite) { ImGui::OpenPopup("Extract into this folder?"); I.confirmOverwrite = false; }
    if (ImGui::BeginPopupModal("Extract into this folder?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", I.dest.c_str());
        ImGui::Text("is not empty. Files with the same names as the game's are replaced;\nnothing else in it is deleted.");
        if (ImGui::Button("Extract")) { ImGui::CloseCurrentPopup(); start_extract(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (I.confirmCleanup) { ImGui::OpenPopup("Remove the partly extracted files?"); I.confirmCleanup = false; }
    if (ImGui::BeginPopupModal("Remove the partly extracted files?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("This deletes the folder and everything in it:");
        ImGui::TextWrapped("%s", I.dest.c_str());
        ImGui::Text("It was empty (or not there) before this extraction.");
        if (ImGui::Button("Delete it")) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::u8path(I.dest), ec);
            I.resultError = ec ? "It could not be removed: " + ec.message() : "The partly extracted folder was removed.";
            I.checkedDest.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace

// --check [--probe] [--dlss] [--check-save]: what the window would show, as text, without a window (tests, support):
// the settings file and its unusable values, the active runtime, optionally the headset and Detect PC, the game it
// would start; with --check-save the settings are saved again (a round trip through the writer)
int check(bool probe, bool dlss, bool saveToo) {
    load();
    std::printf("settings: %s%s\n", settings_path().c_str(), S.error.empty() ? "" : (" (" + S.error + ")").c_str());
    for (const auto& p : S.problems) std::printf("unusable (default kept): %s\n", p.c_str());
    std::printf("runtime: %s%s%s\n", S.runtime.manifest.empty() ? "none set" : S.runtime.name.c_str(),
                S.runtime.manifest.empty() ? "" : " - ", S.runtime.manifest.c_str());
    if (probe) {
        probe_headset(find_game(S.gameExe));
        while (g_probe.running) SDL_Delay(20);
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        std::printf("headset: %s%s (runtime %s; refresh rate choice %s)\n", g_probe.headset ? "found: " : "none: ",
                    g_probe.headset ? g_probe.headsetName.c_str() : g_probe.error.c_str(), g_probe.runtime.c_str(),
                    g_probe.refreshRateExt ? "offered" : "not offered");
    }
    if (probe) {
        apply_detected_refresh();
        std::lock_guard<std::mutex> lock(g_probe.mutex);
        if (!g_probe.refreshMessage.empty()) std::printf("refresh: %s\n", g_probe.refreshMessage.c_str());
    }
    if (dlss) {
        probe_pc(find_game(S.gameExe));
        while (g_pc.running) SDL_Delay(20);
        std::lock_guard<std::mutex> lock(g_pc.mutex);
        S.dlss = read_pc_status(g_pc.line);
        std::printf("dlss: %s%s; DLSS 5 %s\n", g_pc.error.empty() ? "" : ("Detect PC failed: " + g_pc.error + "; ").c_str(),
                    S.dlss.present ? S.dlss.status.c_str() : "not checked",
                    S.dlss.neuralRendering ? "can be turned on"
                    : !own_dlss5_copy(find_game(S.gameExe)).empty() ? "can be turned on (own copy of nvngx_dlssnr.dll)"
                                                                     : "stays off");
    }
    const std::string exe = find_game(S.gameExe);
    if (!S.gameDir.empty()) std::printf("game files: %s\n", S.gameDir.c_str());
    std::printf("game: %s%s\n", exe.empty() ? "not found" : exe.c_str(), exe.empty() ? "" : is_game_itself(exe) ? " (the game)" : " (the release's program)");
    std::printf("vr.enabled=%d vr.menuDistance=%.3f vr.menuPlacement=%d vr.refreshRate=%g vr.frameRate=%s\n", int(S.config.enabled),
                double(S.config.menuDistance), int(S.config.menuPlacement), double(S.config.refreshRate),
                vr::frame_rate_name(S.config.frameRate, int(S.config.refreshRate)).c_str());
    if (saveToo) {
        S.saved.menuDistance = -1;  // force a difference: save() writes the current values
        if (!save()) { std::printf("save failed: %s\n", S.error.c_str()); return 1; }
        std::printf("saved\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    bool checkMode = false, probe = false, dlss = false, saveToo = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check")) checkMode = true;
        else if (!std::strcmp(argv[i], "--probe")) probe = true;
        else if (!std::strcmp(argv[i], "--dlss")) dlss = true;
        else if (!std::strcmp(argv[i], "--check-save")) saveToo = true;
        else if (!std::strcmp(argv[i], "--game") && i + 1 < argc) g_argGame = argv[++i];
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) g_argSave = argv[++i];
    }
    if (checkMode) return check(probe, dlss, saveToo);
#ifdef __ANDROID__
    // the game's settings folder on Android (runtime/src/main.cpp does the same at its start): host::config_dir() is
    // $XDG_CONFIG_HOME/wwhd, in the app's external files folder
    if (const char* dir = SDL_GetAndroidExternalStoragePath()) {
        if (chdir(dir) != 0) std::fprintf(stderr, "cannot enter %s\n", dir);
        setenv("XDG_CONFIG_HOME", (std::string(dir) + "/config").c_str(), 1);
    }
#endif
    SDL_SetHint(SDL_HINT_APP_NAME, "Wind Waker HD VR");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wind Waker HD VR", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Wind Waker HD - VR launcher", 860, 760, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wind Waker HD VR", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    g_window = window;  // (the Install game tab's file and folder pickers belong to it)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Tab and the arrow keys move between controls
    ImGui::StyleColorsDark();
    const float scale = SDL_GetWindowDisplayScale(window);
    ImGui::GetStyle().ScaleAllSizes(scale > 0 ? scale : 1);
    ImGui::GetStyle().FontScaleDpi = scale > 0 ? scale : 1;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    load();
    // test aids (captures, docs/VR_LAUNCHER.md "Install game"): WWHD_LAUNCHER_TAB=install opens on that tab;
    // WWHD_LAUNCHER_SOURCE / WWHD_LAUNCHER_DEST preset its file and folder (WWHD_LAUNCHER_EXTRACT=1 then extracts once
    // it is ready); WWHD_LAUNCHER_SHOT=file.bmp saves the window once the tab has nothing running (then 30 more frames)
    // and quits. With WWHD_USER_DIR for a separate settings folder
    const char* testTab = SDL_getenv("WWHD_LAUNCHER_TAB");
    const char* testShot = SDL_getenv("WWHD_LAUNCHER_SHOT");
    bool testExtract = false;
    if (const char* e = SDL_getenv("WWHD_LAUNCHER_DEST")) I.dest = e;
    if (const char* e = SDL_getenv("WWHD_LAUNCHER_SOURCE")) {
        I.source = e;
        detect();
        testExtract = SDL_getenv("WWHD_LAUNCHER_EXTRACT") != nullptr;
    }
    int testFrames = 0;

    bool quit = false, confirmQuit = false, confirmReset = false;
    // (docs/VR_LAUNCHER.md, "Settings from an older build") asked once when the launcher opens
    bool askOldSettings = old_settings();
    const std::string oldBuild = settings_build();
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (changed()) confirmQuit = true;
                else quit = true;
            }
        }
        apply_detected_refresh();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("General")) { tab_general(); ImGui::EndTabItem(); }
            const ImGuiTabItemFlags installFlags =
                testTab && !std::strcmp(testTab, "install") && ImGui::GetFrameCount() < 3 ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Install game", nullptr, installFlags)) { tab_install(); ImGui::EndTabItem(); }
            if (testExtract && I.step == Install::Step::Ready) { testExtract = false; start_extract(); }
            if (ImGui::BeginTabItem("Camera and comfort")) { tab_camera(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Controllers")) { tab_controls(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Graphics")) { tab_graphics(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        // the VR mode's credit (LICENSE-VR.md: GPL-3.0 section 7(b), shown where players see it)
        ImGui::TextDisabled("%s. VR mode by VR-WakerHD (https://github.com/VR-WakerHD/): GPL-3.0 (LICENSE-VR.md); "
                            "the original port: MPL-2.0.", build::version());
        ImGui::Separator();
        ImGui::BeginDisabled(!changed());
        if (ImGui::Button("Save")) save();
        ImGui::SameLine();
        if (ImGui::Button("Undo changes")) {
            S.config = S.saved; S.bindings = S.savedBindings; S.graphics = S.savedGraphics;
            S.gameExe = S.savedGameExe; S.gameDir = S.savedGameDir;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Restore all VR defaults...")) confirmReset = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", changed() ? "unsaved changes; the game reads them at its next start" : S.status.c_str());
        if (!S.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", S.error.c_str());
        if (!S.problems.empty()) {
            std::string p;
            for (const auto& x : S.problems) p += (p.empty() ? "" : ", ") + x;
            ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Unusable values in the settings file kept at their defaults: %s", p.c_str());
        }
        ImGui::End();

        if (confirmReset) { ImGui::OpenPopup("Restore all VR defaults?"); confirmReset = false; }
        if (ImGui::BeginPopupModal("Restore all VR defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("All VR settings and button bindings go back to their defaults (saved when you click Save).\n"
                                   "The game's other settings are not changed.");
            if (ImGui::Button("Restore")) { S.config = vr::Config{}; S.bindings = vr::Bindings{}; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (askOldSettings) { ImGui::OpenPopup("Settings from an older build"); askOldSettings = false; }
        if (ImGui::BeginPopupModal("Settings from an older build", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Your settings file was last saved by %s; this is %s.",
                        oldBuild.empty() ? "an older build" : oldBuild.c_str(), build::version());
            ImGui::TextUnformatted("Settings saved by an older build keep that build's defaults, so changes to the\n"
                                   "defaults since then (fixes, new options) do not reach you.\n\n"
                                   "Start fresh with this build's defaults? Your current file is not deleted: it is\n"
                                   "kept next to the new one as settings.ini.bak-<date>-<time>.");
            if (ImGui::Button("Start fresh (recommended)")) {
                std::string backup;
                fresh_settings(backup);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep my settings")) {
                keep_old_settings();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (confirmQuit) { ImGui::OpenPopup("Unsaved changes"); confirmQuit = false; }
        if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Save the changes before closing?");
            if (ImGui::Button("Save")) { if (save()) quit = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Discard")) quit = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Render();
        // the backend scales clip rects, not vertices: draw in window coordinates on a high-density (scaled) display
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 20, 22, 28, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (testShot && !I.runner.running() && !testExtract && ++testFrames > 30) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(shot, testShot);
                SDL_DestroySurface(shot);
            }
            quit = true;
        }
        SDL_RenderPresent(renderer);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    // a game started from here keeps running (its own process)
    if (g_launch.process) SDL_DestroyProcess(g_launch.process);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
