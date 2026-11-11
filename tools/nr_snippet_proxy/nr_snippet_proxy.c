// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// nvngx.dll_wwhd_nr.dll — a forwarding proxy for the DLSS 5 Neural Rendering model
// (nvngx_dlssnr.dll), Windows x64 only.
//
// The model checks the module that calls its entry points and refuses any whose file path does not
// contain "nvngx.dll" (NVSDK_NGX_Result_PlatformError otherwise). This proxy is named so its path
// passes that check and forwards the Vulkan NGX surface to the model unchanged. Technique and shape
// after nr-bridge's forwarder (MIT, LowPopLabs), adapted to this project's Vulkan engine; the user
// asked for it explicitly (2026-11-04), reversing the earlier analysis-only decision.
//
// The engine (runtime/src/gfx/vulkan/ngx_vk.cpp) loads this DLL and calls wwnr_snip_load with the
// model's path. Everything returns the model's own result codes: nothing here reports success that
// the model did not report. If the model refuses the GPU (FeatureNotSupported) or the parameter
// object given to it, that failure is returned as-is.
//
// Built without any NVIDIA header: the signatures are the engine's own declarations of the NGX
// Vulkan entry points (ngx_vk.cpp). Volatile results, so the compiler cannot turn a forward into a
// tail call — a tail call would leave the original caller's return address in place, which is what
// the model inspects.
//
// build: zig cc -target x86_64-windows-gnu -shared -O2 (see build.sh)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned int (__cdecl *PfnInitExt2)(unsigned long long app_id, const wchar_t *data_path,
                                            void *vk_instance, void *vk_physical_device, void *vk_device,
                                            void *vk_get_instance_proc_addr, void *vk_get_device_proc_addr,
                                            int sdk_version, const void *params);
typedef unsigned int (__cdecl *PfnCreateFeature1)(void *vk_device, void *vk_command_buffer, int feature,
                                                  void *params, void **out_handle);
typedef unsigned int (__cdecl *PfnEvaluateFeature)(void *vk_command_buffer, const void *handle,
                                                   const void *params, void *progress);
typedef unsigned int (__cdecl *PfnReleaseFeature)(void *handle);
typedef unsigned int (__cdecl *PfnShutdown1)(void *vk_device);

static HMODULE g_mod = NULL;
static PfnInitExt2 g_init = NULL;
static PfnCreateFeature1 g_create = NULL;
static PfnEvaluateFeature g_eval = NULL;
static PfnReleaseFeature g_release = NULL;
static PfnShutdown1 g_shutdown1 = NULL;

__declspec(dllexport) int __cdecl wwnr_snip_load(const wchar_t *path)
{
    if (g_mod) return g_create != NULL;
    if (!path || !path[0]) return 0;
    g_mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_mod) return -(int)GetLastError();
    g_init = (PfnInitExt2)(void (*)(void))GetProcAddress(g_mod, "NVSDK_NGX_VULKAN_Init_Ext2");
    g_create = (PfnCreateFeature1)(void (*)(void))GetProcAddress(g_mod, "NVSDK_NGX_VULKAN_CreateFeature1");
    g_eval = (PfnEvaluateFeature)(void (*)(void))GetProcAddress(g_mod, "NVSDK_NGX_VULKAN_EvaluateFeature");
    g_release = (PfnReleaseFeature)(void (*)(void))GetProcAddress(g_mod, "NVSDK_NGX_VULKAN_ReleaseFeature");
    g_shutdown1 = (PfnShutdown1)(void (*)(void))GetProcAddress(g_mod, "NVSDK_NGX_VULKAN_Shutdown1");
    return (g_init && g_create && g_eval && g_release) ? 1 : 0;
}

__declspec(dllexport) unsigned int __cdecl wwnr_init_ext2(unsigned long long app_id, const wchar_t *data_path,
                                                          void *instance, void *physical_device, void *device,
                                                          void *gipa, void *gdpa, int sdk_version,
                                                          const void *params)
{
    if (!g_init) return 0xBAD00007u;  // NVSDK_NGX_Result_NotInitialized
    volatile unsigned int r = g_init(app_id, data_path, instance, physical_device, device, gipa, gdpa, sdk_version, params);
    return r;
}

__declspec(dllexport) unsigned int __cdecl wwnr_create_feature1(void *device, void *cmd, int feature,
                                                                void *params, void **out_handle)
{
    if (!g_create) return 0xBAD00007u;
    volatile unsigned int r = g_create(device, cmd, feature, params, out_handle);
    return r;
}

__declspec(dllexport) unsigned int __cdecl wwnr_evaluate_feature(void *cmd, const void *handle,
                                                                 const void *params, void *progress)
{
    if (!g_eval) return 0xBAD00007u;
    volatile unsigned int r = g_eval(cmd, handle, params, progress);
    return r;
}

__declspec(dllexport) unsigned int __cdecl wwnr_release_feature(void *handle)
{
    if (!g_release) return 0xBAD00007u;
    volatile unsigned int r = g_release(handle);
    return r;
}

__declspec(dllexport) unsigned int __cdecl wwnr_shutdown1(void *device)
{
    if (!g_shutdown1) return 0xBAD00007u;
    volatile unsigned int r = g_shutdown1(device);
    return r;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
