// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// NVIDIA NGX probe on the game's Vulkan device: see ngx_vk.h and docs/VR_DLSS.md.
#include "ngx_vk.h"

#include <filesystem>

#include "backend.h"
#include "dynres.h"
#include "gfx/renderer.h"
#include "motion_pass.h"
#include "overlay/hostui.h"
#include "platform/host.h"
#include "runtime.h"
#ifdef WWHD_VR
#include "vr/vr.h"
#endif
#include "vr/vr_framesynth.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#if !defined(__ANDROID__) && !defined(__APPLE__) && (defined(_WIN32) || defined(__linux__))
#define WWHD_NGX_PROBE 1
#ifdef _WIN32
#include <windows.h>
#include <wintrust.h>  // (WinVerifyTrust; the crypto API comes with windows.h: wincrypt.h)
#include <softpub.h>   // (WINTRUST_ACTION_GENERIC_VERIFY_V2)
#include <mscat.h>     // (the catalog a driver's file is signed through)
#else
#include <dlfcn.h>
#endif
#endif

namespace gfxvk::ngx_vk {
namespace {

ngx::Caps g_caps;
std::once_flag g_once;
ngx::Mode g_wanted = ngx::Mode::Off;  // the settings' "dlss" and "dlss5" (read at the probe)
bool g_neuralWanted = false;
std::atomic<bool> g_probed{false};    // the probe has decided g_wanted (motion_wanted follows it from then)

#ifdef WWHD_NGX_PROBE
// what the probe found, for the launcher (it has no device of its own): <config dir>/dlss-status.ini
void write_status_file() {
    const std::string path = host::config_dir() + "/" + ngx::kStatusFileName;
    {
        std::error_code ec;  // a first start: the settings folder may not be there yet
        std::filesystem::create_directories(std::filesystem::u8path(host::config_dir()), ec);
    }
    const std::string text = ngx::status_file_text(g_caps, ngx::renderer_inputs());
#ifdef _WIN32
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0)), wchar_t(0));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), int(w.size()));
    std::FILE* f = _wfopen(w.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}
#endif

#ifdef WWHD_NGX_PROBE
// The few NGX entry points the probe needs, as the driver's core library exports them (public NVIDIA DLSS SDK
// 310.9.16, nvsdk_ngx_vk.h; declared here so no NVIDIA header or library is part of the build). x64: one calling
// convention. Results are NVSDK_NGX_Result (Success = 1, failures 0xBAD00000 | code)
struct NgxParameter;  // the driver's parameter object: called by slot (ngx_core.h param_slot)
using PfnInitExt2 = uint32_t (*)(unsigned long long appId, const wchar_t* dataPath, VkInstance, VkPhysicalDevice, VkDevice,
                                 PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int sdkVersion, const NgxParameter*);
using PfnGetCaps = uint32_t (*)(NgxParameter** out);
using PfnDestroyParams = uint32_t (*)(NgxParameter*);
using PfnAllocateParams = uint32_t (*)(NgxParameter** out);
struct NgxHandle;
using PfnCreateFeature1 = uint32_t (*)(VkDevice, VkCommandBuffer, int feature, NgxParameter*, NgxHandle** out);
using PfnEvaluateFeature = uint32_t (*)(VkCommandBuffer, const NgxHandle*, const NgxParameter*, void* progress);
using PfnReleaseFeature = uint32_t (*)(NgxHandle*);
// NVSDK_NGX_Resource_VK (nvsdk_ngx_defs_vk.h), the image variant
struct NgxResourceVk {
    struct {
        VkImageView view;
        VkImage image;
        VkImageSubresourceRange range;
        VkFormat format;
        unsigned width, height;
    } image;
    int type = 0;  // NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW
    bool readWrite = false;
};
// the run's entry points, kept from the probe on the game's device (stage D4)
struct Core {
    PfnAllocateParams allocate = nullptr;
    PfnDestroyParams destroy = nullptr;
    PfnCreateFeature1 create = nullptr;
    PfnEvaluateFeature evaluate = nullptr;
    PfnReleaseFeature release = nullptr;
    VkDevice device = VK_NULL_HANDLE;
} g_core;
// the device the snippet path would start on: the run's own, set where the core's entry points are kept
struct SnipDev {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
} g_snipDev;
constexpr int kSdkVersion = 0x15;                       // NVSDK_NGX_Version_API of SDK 310.9.16
constexpr unsigned long long kAppId = 0x57574844ull;    // "WWHD": an application id of our own (no NVIDIA-issued id)

#ifdef _WIN32
constexpr bool kMsvcLayout = true;
using Lib = HMODULE;
void* sym(Lib l, const char* n) { return reinterpret_cast<void*>(GetProcAddress(l, n)); }

// Verify that a DLL is signed by a chain Windows trusts (WinVerifyTrust, the generic Authenticode policy): its own
// signature, else the catalog Windows signed it through. Driver files are the second kind: the driver's NGX core
// (_nvngx.dll in the DriverStore) carries no signature of its own and is signed by the Microsoft Windows Hardware
// Compatibility Publisher catalog the driver was installed with. Any trusted signer passes (the subject is not checked)
static LONG win_verify(WINTRUST_DATA& data) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    const LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return r;
}
static bool verify_dll_signature(const wchar_t* path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof file;
    file.pcwszFilePath = path;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof data;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    const LONG own = win_verify(data);
    if (own == ERROR_SUCCESS) {
        LOG("[ngx] signature verified for %ls", path);
        return true;
    }
    // no signature of its own: the catalog that holds its hash (a driver's file)
    bool ok = false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HCATADMIN admin = nullptr;
    if (f != INVALID_HANDLE_VALUE && CryptCATAdminAcquireContext2(&admin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        DWORD size = 0;
        CryptCATAdminCalcHashFromFileHandle2(admin, f, &size, nullptr, 0);
        std::vector<BYTE> hash(size);
        if (size && CryptCATAdminCalcHashFromFileHandle2(admin, f, &size, hash.data(), 0)) {
            if (HCATINFO cat = CryptCATAdminEnumCatalogFromHash(admin, hash.data(), size, 0, nullptr)) {
                CATALOG_INFO info{};
                info.cbStruct = sizeof info;
                if (CryptCATCatalogInfoFromContext(cat, &info, 0)) {
                    std::wstring tag;  // the member's tag in the catalog: its hash in hexadecimal
                    for (BYTE byte : hash) {
                        wchar_t hex[3];
                        swprintf(hex, 3, L"%02X", byte);
                        tag += hex;
                    }
                    WINTRUST_CATALOG_INFO member{};
                    member.cbStruct = sizeof member;
                    member.pcwszCatalogFilePath = info.wszCatalogFile;
                    member.pcwszMemberFilePath = path;
                    member.pcwszMemberTag = tag.c_str();
                    member.hMemberFile = f;
                    member.pbCalculatedFileHash = hash.data();
                    member.cbCalculatedFileHash = size;
                    WINTRUST_DATA catalog{};
                    catalog.cbStruct = sizeof catalog;
                    catalog.dwUnionChoice = WTD_CHOICE_CATALOG;
                    catalog.pCatalog = &member;
                    ok = win_verify(catalog) == ERROR_SUCCESS;
                }
                CryptCATAdminReleaseCatalogContext(admin, cat, 0);
            }
        }
        CryptCATAdminReleaseContext(admin, 0);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (ok) LOG("[ngx] signature verified for %ls (through the driver's catalog)", path);
    else LOG("[ngx] no trusted signature on %ls (0x%lx)", path, static_cast<unsigned long>(own));
    return ok;
}

// the driver's NGX core: the folder its installer records (the NGX loader's own order: next to the program, then the
// driver's registry keys), _nvngx.dll there (drivers 471.11 and later), else nvngx.dll
Lib load_core(std::string& where) {
    wchar_t path[MAX_PATH];
    auto try_folder = [&](const wchar_t* dir) -> Lib {
        for (const wchar_t* name : {L"\\_nvngx.dll", L"\\nvngx.dll"}) {
            wchar_t full[MAX_PATH];
            if (wcslen(dir) + wcslen(name) >= MAX_PATH) continue;
            wcscpy(full, dir);
            wcscat(full, name);
            if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) continue;  // (not there: the next place)
            // Verify signature before loading: only accept validly signed DLLs
            if (!verify_dll_signature(full)) {
                LOG("[ngx] signature verification failed, skipping %ls", full);
                continue;
            }
            if (Lib l = LoadLibraryW(full)) {
                char narrow[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, full, -1, narrow, sizeof narrow, nullptr, nullptr);
                where = narrow;
                LOG("[ngx] loaded verified NVIDIA DLL from %s", narrow);
                return l;
            }
        }
        return nullptr;
    };
    // a copy next to the program first (a player's own, modified or not: this code checks no signature)
    {
        const std::string exe = host::exe_dir();
        std::wstring dir(size_t(MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), -1, nullptr, 0)), wchar_t(0));
        MultiByteToWideChar(CP_UTF8, 0, exe.c_str(), -1, dir.data(), int(dir.size()));
        if (Lib l = try_folder(dir.c_str())) return l;
    }
    struct Key { const wchar_t* key; const wchar_t* value; };
    const Key keys[] = {{L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath"},
                        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath"},
                        {L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath"}};
    for (const Key& k : keys) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k.key, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        DWORD type = 0, size = sizeof path;
        const LONG r = RegQueryValueExW(h, k.value, nullptr, &type, reinterpret_cast<BYTE*>(path), &size);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        path[MAX_PATH - 1] = 0;
        if (Lib l = try_folder(path)) return l;
    }
    return nullptr;
}
#else
constexpr bool kMsvcLayout = false;
using Lib = void*;
void* sym(Lib l, const char* n) { return dlsym(l, n); }
Lib load_core(std::string& where) {
    where = "libnvidia-ngx.so.1";
    return dlopen("libnvidia-ngx.so.1", RTLD_NOW | RTLD_LOCAL);
}
#endif

// NVSDK_NGX_Parameter::Get(name, int*) / (unsigned int*), by slot
uint32_t get_int(NgxParameter* p, const char* name, int* out) {
    using Fn = uint32_t (*)(NgxParameter*, const char*, int*);
    void** vtable = *reinterpret_cast<void***>(p);
    return reinterpret_cast<Fn>(vtable[ngx::param_slot(true, ngx::ParamType::Int, kMsvcLayout)])(p, name, out);
}
// NVSDK_NGX_Parameter::Set(name, ...), by slot
void set_ui(NgxParameter* p, const char* name, unsigned v) {
    using Fn = void (*)(NgxParameter*, const char*, unsigned);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[ngx::param_slot(false, ngx::ParamType::UInt, kMsvcLayout)])(p, name, v);
}
void set_i(NgxParameter* p, const char* name, int v) {
    using Fn = void (*)(NgxParameter*, const char*, int);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[ngx::param_slot(false, ngx::ParamType::Int, kMsvcLayout)])(p, name, v);
}
void set_f(NgxParameter* p, const char* name, float v) {
    using Fn = void (*)(NgxParameter*, const char*, float);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[ngx::param_slot(false, ngx::ParamType::Float, kMsvcLayout)])(p, name, v);
}
void set_ptr(NgxParameter* p, const char* name, void* v) {
    using Fn = void (*)(NgxParameter*, const char*, void*);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(p))[ngx::param_slot(false, ngx::ParamType::VoidPtr, kMsvcLayout)])(p, name, v);
}
uint32_t get_uint(NgxParameter* p, const char* name, unsigned* out) {
    using Fn = uint32_t (*)(NgxParameter*, const char*, unsigned*);
    void** vtable = *reinterpret_cast<void***>(p);
    return reinterpret_cast<Fn>(vtable[ngx::param_slot(true, ngx::ParamType::UInt, kMsvcLayout)])(p, name, out);
}

// "<prefix>.Available" and the rest for one feature
ngx::FeatureCaps read_feature(NgxParameter* p, const char* prefix) {
    ngx::FeatureCaps f;
    auto key = [&](const char* field) { return std::string(prefix) + "." + field; };
    int v = 0;
    if (ngx::ok(get_int(p, key("Available").c_str(), &v))) {
        f.queried = true;
        f.available = v != 0;
    }
    if (ngx::ok(get_int(p, key("NeedsUpdatedDriver").c_str(), &v))) {
        f.queried = true;
        f.needsDriver = v != 0;
    }
    unsigned u = 0;
    if (ngx::ok(get_uint(p, key("MinDriverVersionMajor").c_str(), &u))) f.minMajor = int(u);
    if (ngx::ok(get_uint(p, key("MinDriverVersionMinor").c_str(), &u))) f.minMinor = int(u);
    if (ngx::ok(get_int(p, key("FeatureInitResult").c_str(), &v))) f.initResult = uint32_t(v);
    return f;
}

// the device NGX starts on: the game's (probe) or the PC check's own (probe_main)
struct Device {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    const VkPhysicalDeviceProperties& properties;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
};

void run_probe(const Device& d) {
    ngx::Caps& c = g_caps;
    c.built = true;
    c.gpu = d.properties.deviceName;
    c.nvidia = d.properties.vendorID == 0x10DE;
    if (c.nvidia) {
        const uint32_t v = d.properties.driverVersion;  // NVIDIA's packing: 10 bits major, 8 minor
        char b[32];
        snprintf(b, sizeof b, "%u.%02u", (v >> 22) & 0x3FF, (v >> 14) & 0xFF);
        c.driver = b;
    }
    if (!wanted()) {
        c.failure = "WWHD_NGX=0";
        return;
    }
    if (!c.nvidia) return;
    std::string where;
    Lib lib = load_core(where);
    if (!lib) {
        c.failure = "the driver's NGX core library was not found";
        return;
    }
    c.coreLoaded = true;
    // the core stays loaded for the rest of the run (the driver's own library; unloading buys nothing)
    auto init = reinterpret_cast<PfnInitExt2>(sym(lib, "NVSDK_NGX_VULKAN_Init_Ext2"));
    auto getCaps = reinterpret_cast<PfnGetCaps>(sym(lib, "NVSDK_NGX_VULKAN_GetCapabilityParameters"));
    auto destroy = reinterpret_cast<PfnDestroyParams>(sym(lib, "NVSDK_NGX_VULKAN_DestroyParameters"));
    if (!init || !getCaps || !destroy) {
        c.failure = "the driver's NGX core lacks the Vulkan entry points";
        return;
    }
    // NGX writes its logs and downloaded models under the data path: the user folder's own subfolder
    const std::string dir = host::config_dir() + "/ngx";
#ifdef _WIN32
    std::wstring wdir(size_t(MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0)), wchar_t(0));
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir.data(), int(wdir.size()));
#else
    std::wstring wdir(dir.begin(), dir.end());
#endif
    c.initResult = init(kAppId, wdir.c_str(), d.instance, d.physicalDevice, d.device, d.gipa, d.gdpa, kSdkVersion, nullptr);
    if (!ngx::ok(c.initResult)) {
        char b[96];
        snprintf(b, sizeof b, "NGX did not start on this device (0x%08X)", unsigned(c.initResult));
        c.failure = b;
        return;
    }
    c.initialized = true;
    NgxParameter* params = nullptr;
    const uint32_t r = getCaps(&params);
    if (ngx::ok(r) && params) {
        c.superResolution = read_feature(params, "SuperSampling");
        c.neuralRendering = read_feature(params, "DLSSNR");
        destroy(params);
    } else {
        char b[96];
        snprintf(b, sizeof b, "the capability query failed (0x%08X)", unsigned(r));
        c.failure = b;
    }
    // the run's entry points (stage D4), kept while the game's own device is the probe's
    if (d.device == R.device && c.superResolution.available) {
        g_core.allocate = reinterpret_cast<PfnAllocateParams>(sym(lib, "NVSDK_NGX_VULKAN_AllocateParameters"));
        g_core.destroy = destroy;
        g_core.create = reinterpret_cast<PfnCreateFeature1>(sym(lib, "NVSDK_NGX_VULKAN_CreateFeature1"));
        g_core.evaluate = reinterpret_cast<PfnEvaluateFeature>(sym(lib, "NVSDK_NGX_VULKAN_EvaluateFeature"));
        g_core.release = reinterpret_cast<PfnReleaseFeature>(sym(lib, "NVSDK_NGX_VULKAN_ReleaseFeature"));
        g_core.device = d.device;
        g_snipDev = {d.instance, d.physicalDevice, d.device, d.gipa, d.gdpa};
    }
    // NGX stays started for the run: right after its initialisation it checks for model updates on a thread of its
    // own, and a shutdown at once crashed in the driver (a write through a null pointer in _nvngx.dll, seen once).
    // The later stages create their features on it (docs/VR_DLSS.md)
    LOG("[ngx] core %s; init 0x%08X; SuperSampling available %d (needs driver %d, %d.%d, init 0x%08X); DLSSNR available %d "
        "(needs driver %d, %d.%d, init 0x%08X)",
        where.c_str(), unsigned(c.initResult), int(c.superResolution.available), int(c.superResolution.needsDriver),
        c.superResolution.minMajor, c.superResolution.minMinor, unsigned(c.superResolution.initResult),
        int(c.neuralRendering.available), int(c.neuralRendering.needsDriver), c.neuralRendering.minMajor,
        c.neuralRendering.minMinor, unsigned(c.neuralRendering.initResult));
}
#endif

}  // namespace

#ifdef WWHD_NGX_PROBE
namespace {
// each eye's Super Resolution feature (its own history) and the parameter object the run fills
struct EyeFeature {
    NgxHandle* handle = nullptr;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0, flags = 0;
    ngx::Mode mode = ngx::Mode::Off;
};
std::mutex g_runMutex;
EyeFeature g_features[2];
NgxParameter* g_params = nullptr;
bool g_runFailed = false;
// NVSDK_NGX_PerfQuality_Value
int perf_quality(ngx::Mode m) {
    switch (m) {
        case ngx::Mode::Performance: return 0;
        case ngx::Mode::Balanced: return 1;
        case ngx::Mode::Quality: return 2;
        case ngx::Mode::UltraPerformance: return 3;
        case ngx::Mode::Dlaa: return 5;
        default: return 5;
    }
}
NgxResourceVk resource(const DlssImage& i, bool readWrite) {
    NgxResourceVk r{};
    r.image = {i.view, i.image, {i.aspect, 0, 1, 0, 1}, i.format, i.width, i.height};
    r.readWrite = readWrite;
    return r;
}
void log_once(int kind, const char* what, uint32_t result) {
    static bool logged[8] = {};
    if (logged[kind & 7]) return;
    logged[kind & 7] = true;
    LOG("[ngx] DLSS run: %s (0x%08X)", what, unsigned(result));
}
}  // namespace
#endif

bool can_evaluate() {
#ifdef WWHD_NGX_PROBE
    return g_core.create && g_core.evaluate && g_core.allocate && !g_runFailed;
#else
    return false;
#endif
}

bool evaluate(VkCommandBuffer cmd, const DlssEval& e) {
#ifndef WWHD_NGX_PROBE
    (void)cmd; (void)e;
    return false;
#else
    std::lock_guard lock(g_runMutex);
    if (!can_evaluate() || e.eye < 0 || e.eye > 1) return false;
    if (!g_params) {
        const uint32_t r = g_core.allocate(&g_params);
        if (!ngx::ok(r) || !g_params) {
            log_once(0, "the parameter object could not be made", r);
            g_runFailed = true;
            return false;
        }
    }
    // NVSDK_NGX_DLSS_Feature_Flags: MVLowRes (motion at the render size), MVJittered
    const uint32_t flags = (1u << 1) | (e.mvJittered ? (1u << 2) : 0u);
    EyeFeature& f = g_features[e.eye];
    if (!f.handle || f.inW != e.color.width || f.inH != e.color.height || f.outW != e.output.width ||
        f.outH != e.output.height || f.mode != e.mode || f.flags != flags) {
        if (f.handle) g_core.release(f.handle);
        f = {};
        set_ui(g_params, "CreationNodeMask", 1);
        set_ui(g_params, "VisibilityNodeMask", 1);
        set_ui(g_params, "Width", e.color.width);
        set_ui(g_params, "Height", e.color.height);
        set_ui(g_params, "OutWidth", e.output.width);
        set_ui(g_params, "OutHeight", e.output.height);
        set_i(g_params, "PerfQualityValue", perf_quality(e.mode));
        set_i(g_params, "DLSS.Feature.Create.Flags", int(flags));
        set_i(g_params, "DLSS.Enable.Output.Subrects", 0);
        // (only Super Resolution / DLAA here, and feature 18 below: DLSS Frame Generation, NVSDK_NGX_Feature_FrameGeneration
        // in nvngx_dlssg, is never created. It interpolates between two finished frames, adding latency, and NVIDIA does
        // not support it in VR; the runtime's frame synthesis or SteamVR's Motion Smoothing makes VR's in-between frames:
        // docs/VR_FRAME_SYNTHESIS.md)
        const uint32_t r = g_core.create(g_core.device, cmd, 1 /* NVSDK_NGX_Feature_SuperSampling */, g_params, &f.handle);
        if (!ngx::ok(r) || !f.handle) {
            log_once(1, "the feature could not be made", r);
            f = {};
            g_runFailed = true;
            return false;
        }
        f.inW = e.color.width;
        f.inH = e.color.height;
        f.outW = e.output.width;
        f.outH = e.output.height;
        f.mode = e.mode;
        f.flags = flags;
        LOG("[ngx] DLSS eye %d: %s %ux%u to %ux%u made", e.eye, ngx::mode_name(e.mode), f.inW, f.inH, f.outW, f.outH);
    }
    NgxResourceVk color = resource(e.color, false), depth = resource(e.depth, false), motion = resource(e.motion, false),
                  output = resource(e.output, true);
    set_ptr(g_params, "Color", &color);
    set_ptr(g_params, "Output", &output);
    set_ptr(g_params, "Depth", &depth);
    set_ptr(g_params, "MotionVectors", &motion);
    set_f(g_params, "Jitter.Offset.X", e.jitterX);
    set_f(g_params, "Jitter.Offset.Y", e.jitterY);
    set_f(g_params, "Sharpness", 0.0f);
    set_i(g_params, "Reset", e.reset ? 1 : 0);
    set_f(g_params, "MV.Scale.X", e.mvScaleX);
    set_f(g_params, "MV.Scale.Y", e.mvScaleY);
    set_ui(g_params, "DLSS.Render.Subrect.Dimensions.Width", e.color.width);
    set_ui(g_params, "DLSS.Render.Subrect.Dimensions.Height", e.color.height);
    set_f(g_params, "DLSS.Pre.Exposure", 1.0f);
    set_f(g_params, "DLSS.Exposure.Scale", 1.0f);
    const uint32_t r = g_core.evaluate(cmd, f.handle, g_params, nullptr);
    if (!ngx::ok(r)) {
        log_once(2, "the evaluate failed", r);
        return false;
    }
    return true;
#endif
}

#ifdef WWHD_NGX_PROBE
namespace {
// DLSS 5 Neural Rendering: NGX feature 18 (Reserved18 in the public SDK) through the driver's core, as Super Resolution
// is: one feature per eye (its own history), its own parameter object. The driver decides whether it serves the
// feature (its own model, or a copy next to the program that the core accepts); nothing here loads or checks the model.
// Its parameter names are not in any NVIDIA header: these are the ones other integrations use (nr-bridge, section 10)
struct NrFeature {
    NgxHandle* handle = nullptr;
    uint32_t w = 0, h = 0;
    int preset = -1;  // the preset it was made with (a create-time hint: another preset makes it again)
    bool snippet = false;  // the handle is the model's own (the snippet path), not the driver core's
};
NrFeature g_nrFeatures[2];
NgxParameter* g_nrParams = nullptr;
bool g_nrFailed = false;
bool g_nrRestart[2] = {};  // (g_runMutex) switched on again: the eye's next evaluate starts a new history

// --- the snippet path: the model itself, when the driver core refuses feature 18 (Windows only) ----------
// nvngx.dll_wwhd_nr.dll (tools/nr_snippet_proxy) forwards NVSDK_NGX_VULKAN_* to nvngx_dlssnr.dll, and is
// named so the model's caller check passes (the calling module's path must contain "nvngx.dll", else the
// model answers PlatformError). The user supplied the model and asked for this explicitly (2026-11-04);
// the driver-core path stays primary and this runs only after the core's create refused the feature. The
// parameter object is still the core's: the model's own parameter contract is not public, so a parameter
// object can only come from the core (nr-bridge and the OptiScaler fork do the same). No core, no snippet
// path — which is what keeps a non-NVIDIA GPU an honest failure.
// Optional switch: WWHD_NR_SNIPPET=0 turns the fallback off (the core path alone, as before).
#ifdef _WIN32
using PfnSnipLoad = int (__cdecl *)(const wchar_t *path);
using PfnSnipInitExt2 = uint32_t (__cdecl *)(unsigned long long, const wchar_t *, VkInstance, VkPhysicalDevice,
                                             VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                             int, const NgxParameter *);
using PfnSnipCreateFeature1 = uint32_t (__cdecl *)(VkDevice, VkCommandBuffer, int, NgxParameter *, NgxHandle **);
using PfnSnipEvaluateFeature = uint32_t (__cdecl *)(VkCommandBuffer, const NgxHandle *, const NgxParameter *, void *);
using PfnSnipReleaseFeature = uint32_t (__cdecl *)(NgxHandle *);
using PfnSnipShutdown1 = uint32_t (__cdecl *)(VkDevice);
struct Snippet {
    HMODULE lib = nullptr;
    PfnSnipLoad load = nullptr;
    PfnSnipInitExt2 init = nullptr;
    PfnSnipCreateFeature1 create = nullptr;
    PfnSnipEvaluateFeature evaluate = nullptr;
    PfnSnipReleaseFeature release = nullptr;
    PfnSnipShutdown1 shutdown1 = nullptr;
    bool tried = false;   // looked for once per run (a missing proxy or model is not looked for again)
    bool inited = false;  // Init_Ext2 answered Success on the run's device
} g_snippet;

// `name` found as a regular file in one of the folders (in order): its full path as utf-8, else ""
std::string find_file(const char* const* dirs, size_t n, const char* name) {
    for (size_t i = 0; i < n; ++i) {
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(dirs[i]) / name;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            const auto u8 = candidate.u8string();  // (C++20: std::u8string; its bytes are the utf-8 path)
            return std::string(u8.begin(), u8.end());
        }
    }
    return std::string();
}

std::wstring utf8_to_wide(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), wchar_t(0));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    return w;
}

// the proxy loaded, its exports resolved, the model loaded through it, Init_Ext2 answered Success
bool snippet_init() {
    if (g_snippet.tried) return g_snippet.inited;
    g_snippet.tried = true;
    if (const char* off = getenv("WWHD_NR_SNIPPET"); off && !strcmp(off, "0")) {
        LOG("[ngx] DLSS 5 snippet path off (WWHD_NR_SNIPPET=0)");
        return false;
    }
    const std::string exeDir = host::exe_dir();
    const char* dirs[] = {exeDir.c_str(), "."};
    const std::string proxyPath = find_file(dirs, 2, "nvngx.dll_wwhd_nr.dll");
    std::string modelPath = find_file(dirs, 2, "nvngx_dlssnr.dll");
    if (proxyPath.empty()) {
        LOG("[ngx] DLSS 5 snippet path: nvngx.dll_wwhd_nr.dll not found (the program folder or the data folder)");
        return false;
    }
    g_snippet.lib = LoadLibraryW(utf8_to_wide(proxyPath).c_str());
    if (!g_snippet.lib) {
        LOG("[ngx] DLSS 5 snippet path: the proxy could not be loaded (error %lu)", GetLastError());
        return false;
    }
    auto sym = [&](const char* n) { return GetProcAddress(g_snippet.lib, n); };
    g_snippet.load = reinterpret_cast<PfnSnipLoad>(sym("wwnr_snip_load"));
    g_snippet.init = reinterpret_cast<PfnSnipInitExt2>(sym("wwnr_init_ext2"));
    g_snippet.create = reinterpret_cast<PfnSnipCreateFeature1>(sym("wwnr_create_feature1"));
    g_snippet.evaluate = reinterpret_cast<PfnSnipEvaluateFeature>(sym("wwnr_evaluate_feature"));
    g_snippet.release = reinterpret_cast<PfnSnipReleaseFeature>(sym("wwnr_release_feature"));
    g_snippet.shutdown1 = reinterpret_cast<PfnSnipShutdown1>(sym("wwnr_shutdown1"));
    if (!g_snippet.load || !g_snippet.init || !g_snippet.create || !g_snippet.evaluate || !g_snippet.release) {
        LOG("[ngx] DLSS 5 snippet path: the proxy lacks its exports");
        g_snippet.lib = nullptr;
        return false;
    }
    if (modelPath.empty()) {
        // also next to the proxy itself
        wchar_t wself[1024] = L"";
        GetModuleFileNameW(g_snippet.lib, wself, 1024);
        char self[1024 * 3] = "";
        WideCharToMultiByte(CP_UTF8, 0, wself, -1, self, sizeof self, nullptr, nullptr);
        const std::string selfStr = self;
        const size_t slash = selfStr.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string proxyDir = selfStr.substr(0, slash);
            const char* proxyDirs[] = {proxyDir.c_str()};
            modelPath = find_file(proxyDirs, 1, "nvngx_dlssnr.dll");
        }
    }
    if (modelPath.empty()) {
        LOG("[ngx] DLSS 5 snippet path: nvngx_dlssnr.dll not found (the program folder, the data folder or the proxy's folder)");
        g_snippet.lib = nullptr;
        return false;
    }
    const int lr = g_snippet.load(utf8_to_wide(modelPath).c_str());
    if (lr <= 0) {
        LOG("[ngx] DLSS 5 snippet path: the model could not be loaded (wwnr_snip_load=%d)", lr);
        g_snippet.lib = nullptr;
        return false;
    }
    // the same NGX data path the core was started with (its logs and model cache)
    const std::string dataDir = host::config_dir() + "/ngx";
    const uint32_t ir = g_snippet.init(kAppId, utf8_to_wide(dataDir).c_str(), g_snipDev.instance,
                                       g_snipDev.physicalDevice, g_snipDev.device, g_snipDev.gipa, g_snipDev.gdpa,
                                       kSdkVersion, nullptr);
    LOG("[ngx] DLSS 5 snippet: model %s, Init_Ext2 -> 0x%08X", modelPath.c_str(), unsigned(ir));
    if (!ngx::ok(ir)) {
        g_snippet.lib = nullptr;  // (the model stays loaded in the proxy; it is refused for this run)
        return false;
    }
    g_snippet.inited = true;
    return true;
}

// the snippet call sites, shaped so the non-Windows build has the same names (it never reaches them)
uint32_t snip_create(VkCommandBuffer cmd, NgxHandle** out) {
    return g_snippet.create(g_snipDev.device, cmd, 18 /* DLSS 5 Neural Rendering */, g_nrParams, out);
}
uint32_t snip_evaluate(VkCommandBuffer cmd, const NgxHandle* handle) {
    return g_snippet.evaluate(cmd, handle, g_nrParams, nullptr);
}
uint32_t snip_release(NgxHandle* handle) { return g_snippet.release(handle); }
#else
// no snippet path off Windows: the proxy is a Windows DLL, so the core path alone (as before)
struct Snippet {
    bool tried = false, inited = false;
} g_snippet;
inline bool snippet_init() { return false; }
inline uint32_t snip_create(VkCommandBuffer, NgxHandle**) { return 0xBAD00007u; }
inline uint32_t snip_evaluate(VkCommandBuffer, const NgxHandle*) { return 0xBAD00007u; }
inline uint32_t snip_release(NgxHandle*) { return 0xBAD00007u; }
#endif  // _WIN32

}  // namespace
#endif

// (live) the player's switch, and the settings DLSS 5 runs with; both start as the settings file's (probe)
std::atomic<bool> g_nrOn{false};
std::mutex g_nrLiveMutex;
ngx::NrSettings g_nrLive;

bool neural_wanted() {
#ifdef WWHD_NGX_PROBE
    std::lock_guard lock(g_runMutex);
    return g_nrOn.load(std::memory_order_relaxed) && can_evaluate() && !g_nrFailed;
#else
    return false;
#endif
}

bool evaluate_nr(VkCommandBuffer cmd, const NrEval& e) {
#ifndef WWHD_NGX_PROBE
    (void)cmd; (void)e;
    return false;
#else
    std::lock_guard lock(g_runMutex);
    if (!g_nrOn.load(std::memory_order_relaxed) || g_nrFailed || !can_evaluate() || e.eye < 0 || e.eye > 1) return false;
    if (!g_nrParams) {
        const uint32_t r = g_core.allocate(&g_nrParams);
        if (!ngx::ok(r) || !g_nrParams) {
            LOG("[ngx] DLSS 5: its parameter object could not be made (0x%08X); DLSS 5 off for this run", unsigned(r));
            g_nrFailed = true;
            return false;
        }
    }
    NrFeature& f = g_nrFeatures[e.eye];
    if (!f.handle || f.w != e.color.width || f.h != e.color.height || f.preset != e.settings.preset) {
        if (f.handle) {
            if (f.snippet) snip_release(f.handle);
            else g_core.release(f.handle);
        }
        f = {};
        set_ui(g_nrParams, "CreationNodeMask", 1);
        set_ui(g_nrParams, "VisibilityNodeMask", 1);
        for (const char* k : {"Width", "DLSSNR.Width"}) set_ui(g_nrParams, k, e.color.width);
        for (const char* k : {"Height", "DLSSNR.Height"}) set_ui(g_nrParams, k, e.color.height);
        set_ui(g_nrParams, "OutWidth", e.output.width);
        set_ui(g_nrParams, "OutHeight", e.output.height);
        set_ui(g_nrParams, "DLSSNR.Hint.Render.Preset", unsigned(e.settings.preset));
        const uint32_t r = g_core.create(g_core.device, cmd, 18 /* DLSS 5 Neural Rendering */, g_nrParams, &f.handle);
        if (!ngx::ok(r) || !f.handle) {
            LOG("[ngx] DLSS 5: the core would not make the feature (0x%08X%s)", unsigned(r),
                r == 0xBAD00004u ? ": the driver does not serve it" : r == 0xBAD0000Cu ? ": out of date" : "");
            // the fallback: the model itself, through the proxy (the snippet path; tried once per run)
            if (!snippet_init()) {
                LOG("[ngx] DLSS 5: no snippet path either; DLSS 5 off for this run, DLSS Super Resolution goes on "
                    "without it");
                f = {};
                g_nrFailed = true;
                return false;
            }
            f = {};
            const uint32_t sr = snip_create(cmd, &f.handle);
            LOG("[ngx] DLSS 5 snippet: CreateFeature1 -> 0x%08X", unsigned(sr));
            if (!ngx::ok(sr) || !f.handle) {
                LOG("[ngx] DLSS 5: the model refused the feature too (0x%08X); DLSS 5 off for this run, DLSS Super "
                    "Resolution goes on without it",
                    unsigned(sr));
                f = {};
                g_nrFailed = true;
                return false;
            }
            f.snippet = true;
        } else {
            f.snippet = false;
        }
        f.w = e.color.width;
        f.h = e.color.height;
        f.preset = e.settings.preset;
        LOG("[ngx] DLSS 5 eye %d: %ux%u made (%s, preset %d, style %s)", e.eye, f.w, f.h,
            f.snippet ? "through the model" : "through the driver core", e.settings.preset,
            ngx::nr_style_name(e.settings.style));
    }
    NgxResourceVk color = resource(e.color, false), depth = resource(e.depth, false), motion = resource(e.motion, false),
                  output = resource(e.output, true);
    for (const char* k : {"Color", "DLSSNR.Color"}) set_ptr(g_nrParams, k, &color);
    for (const char* k : {"Depth", "DLSSNR.Depth"}) set_ptr(g_nrParams, k, &depth);
    for (const char* k : {"MotionVectors", "DLSSNR.MVec"}) set_ptr(g_nrParams, k, &motion);
    for (const char* k : {"Output", "DLSSNR.Output"}) set_ptr(g_nrParams, k, &output);
    const bool reset = e.reset || std::exchange(g_nrRestart[e.eye], false);  // (switched on again: a new history)
    for (const char* k : {"Reset", "DLSSNR.Reset"}) set_i(g_nrParams, k, reset ? 1 : 0);
    for (const char* k : {"MV.Scale.X", "DLSSNR.MVecScale.X"}) set_f(g_nrParams, k, e.mvScaleX);
    for (const char* k : {"MV.Scale.Y", "DLSSNR.MVecScale.Y"}) set_f(g_nrParams, k, e.mvScaleY);
    const ngx::NrSettings& s = e.settings;
    set_f(g_nrParams, "DLSSNR.Intensity", s.intensity);
    set_f(g_nrParams, "DLSSNR.GlobalToneStrength", s.globalTone);
    set_f(g_nrParams, "DLSSNR.LocalToneStrength", s.localTone);
    set_f(g_nrParams, "DLSSNR.LocalStructureStrength", s.localStructure);
    set_f(g_nrParams, "DLSSNR.SkinStructureStrength", s.skinStructure);
    set_i(g_nrParams, "DLSSNR.UseAutoMask", s.autoMask ? 1 : 0);
    set_i(g_nrParams, "DLSSNR.Style", s.style);
    set_ui(g_nrParams, "DLSSNR.Hint.Render.Preset", unsigned(s.preset));
    set_i(g_nrParams, "DLSSNR.Iterations", s.iterations);
    const uint32_t r = f.snippet ? snip_evaluate(cmd, f.handle) : g_core.evaluate(cmd, f.handle, g_nrParams, nullptr);
    if (!ngx::ok(r)) {
        log_once(3, "DLSS 5's evaluate failed", r);
        return false;
    }
    return true;
#endif
}

ngx::NrSettings nr_settings() {
    return ngx::clamped(ngx::nr_from_kv([](const char* k, std::string& v) { return hostui::get(k, v); }));
}

NrLive nr_live() {
    NrLive l;
#ifdef WWHD_NGX_PROBE
    {
        std::lock_guard lock(g_runMutex);
        l.available = can_evaluate() && g_caps.neuralRendering.available;
        l.failed = g_nrFailed;
    }
    l.runs = l.available && motion_pass::dlss_runs();
    l.on = g_nrOn.load(std::memory_order_relaxed);
    std::lock_guard lock(g_nrLiveMutex);
    l.settings = g_nrLive;
#endif
    return l;
}

void set_nr_live(bool on, const ngx::NrSettings& settings) {
#ifdef WWHD_NGX_PROBE
    {
        std::lock_guard lock(g_nrLiveMutex);
        g_nrLive = ngx::clamped(settings);
    }
    std::lock_guard lock(g_runMutex);
    if (on && !g_nrOn.load(std::memory_order_relaxed)) {
        g_nrRestart[0] = g_nrRestart[1] = true;  // its history stopped while it was off
        g_nrFailed = false;                      // (a feature that could not be made is tried once more)
    }
    g_nrOn.store(on, std::memory_order_relaxed);
#else
    (void)on; (void)settings;
#endif
}

ngx::NrSettings nr_current() {
#ifdef WWHD_NGX_PROBE
    std::lock_guard lock(g_nrLiveMutex);
    return g_nrLive;
#else
    return {};
#endif
}

ngx::Mode active_mode() {
    if (!can_evaluate()) return ngx::Mode::Off;
    return ngx::effective_mode(g_wanted, g_caps, ngx::renderer_inputs());
}

bool motion_wanted() {
    // after the probe: whether a DLSS feature was chosen in the settings file (no menu offers DLSS)
    // (and for the runtime's frame synthesis: vr/vr_framesynth.h, set by the OpenXR session before the device)
    if (vr::framesynth::motion_wanted()) return true;
    if (g_probed.load(std::memory_order_acquire)) return ngx::motion_wanted(g_wanted, g_neuralWanted, g_caps, ngx::renderer_inputs());
    static const bool on = [] {
        std::string v;
        ngx::Mode m = ngx::Mode::Off;
        bool neural = false;
        if (hostui::get("dlss", v)) m = ngx::parse_mode(v);
        if (hostui::get("dlss5", v)) neural = v == "1";
        return ngx::motion_wanted(m, neural);
    }();
    return on;
}

bool wanted() {
    const char* e = getenv("WWHD_NGX");
    return !(e && !strcmp(e, "0"));
}

bool chosen() {
    static const bool v = [] {
        const char* e = getenv("WWHD_NGX");
        if (e && !strcmp(e, "1")) return true;
        if (!wanted()) return false;
        std::string s;
        ngx::Mode m = ngx::Mode::Off;
        bool neural = false;
        if (hostui::get("dlss", s)) m = ngx::parse_mode(s);
        if (hostui::get("dlss5", s)) neural = s == "1";
        return m != ngx::Mode::Off || neural;
    }();
    return v;
}

void probe() {
    std::call_once(g_once, [] {
        std::string v;
        if (hostui::get("dlss", v)) g_wanted = ngx::parse_mode(v);
        if (hostui::get("dlss5", v)) g_neuralWanted = v == "1";
#ifdef WWHD_NGX_PROBE
        g_nrOn.store(g_neuralWanted, std::memory_order_relaxed);  // (live: F1 > Graphics changes these while playing)
        {
            std::lock_guard lock(g_nrLiveMutex);
            g_nrLive = nr_settings();
        }
#endif
#ifdef WWHD_NGX_PROBE
        if (!chosen()) {
            // DLSS is not chosen: NGX is not started, and NVIDIA's DLSS libraries are not looked at
            g_probed.store(true, std::memory_order_release);
            LOG("[ngx] NVIDIA DLSS off (neither \"dlss\" nor \"dlss5\" in the settings): NGX not started");
            return;
        }
        run_probe({R.instance, R.physicalDevice, R.device, R.properties, vkGetInstanceProcAddr, vkGetDeviceProcAddr});
        write_status_file();
        // an upscaling mode: the scene drawn at its internal resolution from the first frame (dynres.h fixed factor;
        // the renderer reads it before its first frame), DLSS writing the full-size picture
        if (const ngx::Mode m = active_mode(); ngx::upscaling_mode(m)) {
            dynres::set_dlss(ngx::render_factor(m));
            LOG("[ngx] %s: the scene drawn at %.3gx of the internal resolution, DLSS writes it at full size",
                ngx::mode_name(m), double(dynres::fixed_factor()));
        }
#endif
        g_probed.store(true, std::memory_order_release);
        LOG("[ngx] %s", status().c_str());
        // the game looks for view cuts (DLSS's history resets) only while DLSS runs, in any mode
        ::render::g_viewCutsWanted.store(motion_pass::dlss_runs(), std::memory_order_relaxed);
    });
}

int probe_main() {
    auto answer = [](const std::string& line) {
        std::printf("dlss-probe: %s\n", line.c_str());
        std::fflush(stdout);
        return 0;
    };
#ifndef WWHD_NGX_PROBE
    return answer("NVIDIA DLSS: not in this build");
#else
    // SDL loads the Vulkan loader (as the game does: loader.h) once its video part is started; no window is opened
    if (!SDL_Init(SDL_INIT_VIDEO)) return answer(std::string("error=SDL could not start (") + SDL_GetError() + ")");
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return answer(std::string("error=Vulkan could not be loaded (") + SDL_GetError() + ")");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    auto createInstance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    if (!createInstance) return answer("error=Vulkan could not be loaded");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WWHD DLSS check";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance(&ici, nullptr, &instance) != VK_SUCCESS) return answer("error=no Vulkan 1.2 driver answered");
    auto fn = [&](const char* n) { return gipa(instance, n); };
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fn("vkEnumeratePhysicalDevices"));
    auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(fn("vkGetPhysicalDeviceProperties"));
    auto getQueues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(fn("vkGetPhysicalDeviceQueueFamilyProperties"));
    auto getExts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(fn("vkEnumerateDeviceExtensionProperties"));
    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(fn("vkGetPhysicalDeviceFeatures2"));
    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(fn("vkCreateDevice"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(fn("vkGetDeviceProcAddr"));
    if (!enumerate || !getProps || !getQueues || !getExts || !getFeatures2 || !createDevice || !gdpa)
        return answer("error=the Vulkan driver lacks basic entry points");
    uint32_t n = 0;
    enumerate(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    enumerate(instance, &n, gpus.data());
    if (gpus.empty()) return answer("error=no GPU with Vulkan was found");
    // an NVIDIA GPU if there is one (a laptop's integrated GPU may come first), else the first
    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceProperties props{};
    getProps(gpu, &props);
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties p{};
        getProps(g, &p);
        if (p.vendorID == 0x10DE) { gpu = g; props = p; break; }
    }
    // the device the game creates for NGX (backend.cpp): one queue, DLSS's extensions and buffer device addresses
    uint32_t qn = 0;
    getQueues(gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> families(qn);
    getQueues(gpu, &qn, families.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < qn; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    uint32_t en = 0;
    getExts(gpu, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> available(en);
    getExts(gpu, nullptr, &en, available.data());
    std::vector<const char*> extensions;
    for (const char* e : {"VK_NVX_binary_import", "VK_NVX_image_view_handle", VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME})
        for (const auto& a : available)
            if (!std::strcmp(a.extensionName, e)) { extensions.push_back(e); break; }
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have12};
    getFeatures2(gpu, &have);
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.bufferDeviceAddress = have12.bufferDeviceAddress;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    VkDevice device = VK_NULL_HANDLE;
    if (createDevice(gpu, &dci, nullptr, &device) != VK_SUCCESS)
        return answer(std::string("error=a Vulkan device could not be created on ") + props.deviceName);
    run_probe({instance, gpu, device, props, gipa, gdpa});
    write_status_file();
    // the device and instance end with the process (NGX's own thread may still use them: see run_probe)
    return answer(ngx::status(g_caps, ngx::renderer_inputs()));
#endif
}

const ngx::Caps& caps() { return g_caps; }

std::string status() { return ngx::status(g_caps, ngx::renderer_inputs(), g_wanted, g_neuralWanted); }

}  // namespace gfxvk::ngx_vk
