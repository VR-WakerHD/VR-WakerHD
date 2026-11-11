# DLSS 5 NR (nvngx_dlssnr.dll) compatibility analysis — what can and cannot load and run here

Companion to DLL_LOADER_ANALYSIS.md (the loader's capabilities) and NR_BRIDGE_ANALYSIS.md (the
state-of-the-art third-party approach). A copy of the DLL (158 MB) is now in the repo root:

- SHA-256: `e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a`
- PE32+ x64 DLL (x86-64, Windows 6.00), 7 sections — no architecture problem.

## Static findings (2026-11-04, pefile; no execution)

**Exports (55): the full NGX surface for every backend.** CUDA (13: `NVSDK_NGX_CUDA_Init*`,
`CreateFeature(1)`, `EvaluateFeature`, `ReleaseFeature`, `Shutdown(1)`, `GetScratchBufferSize`,
`GetFeatureRequirements`, `PopulateParameters_Impl`), D3D11 (10, same shape), D3D12 (10, same shape),
and — decisive for this engine — **Vulkan (13)**: `NVSDK_NGX_VULKAN_Init`, `Init_Ext`, `Init_Ext2`,
`CreateFeature`, `CreateFeature1`, `EvaluateFeature`, `ReleaseFeature`, `Shutdown`, `Shutdown1`,
`GetScratchBufferSize`, `GetFeatureRequirements`, `GetFeature{Instance,Device}ExtensionRequirements`,
`PopulateParameters_Impl`. Plus common: `NVSDK_NGX_GetAPIVersion`, `GetApplicationId`,
`GetDriverVersionEx`, `GetGPUArchitecture`, `GetSnippetVersion`, `SetOverrideStatusCallback`,
`SetRuntimeParamsCallback`, `SetTelemetryEvaluateCallback`.

So contrary to what the second-hand sources assumed, the model is not D3D12-fenced: it exposes the
same Vulkan entry points the driver core does, with the same names and ABI. A Vulkan snippet path
(driving the model directly on the game's VkDevice) needs no cross-API translation.

**Imports: only four system DLLs** — KERNEL32, ADVAPI32, USER32, VERSION. No D3D runtime, no
nvapi, no CUDA runtime, no NVIDIA driver library at load time — driver/vendor dispatch is through
the Vulkan loader or dynamic export lookups, whatever the GPU supplies. Nothing here questions the
"no distributed dependencies" expectation.

Statically unanswerable (and the reason the fallback must report the model's own codes rather than
promise anything): how the caller check strings the path (any module whose file path contains
"nvngx.dll" is accepted — nr-bridge's and OptiScaler's observed behavior; the accepted-shape proof
will come from a run), which checks inside init gate on driver/GPU, and whether
`VULKAN_GetFeatureRequirements` reports vendor constraints before init.

## What this engine wants

DLSS 5 Neural Rendering = NGX feature 18 (`NVSDK_NGX_Feature_Reserved18` in public headers; the
Streamline project calls the same feature `kFeatureDLSS_NR = 1004` at its own layer). The model binary
is `nvngx_dlssnr.dll`, which NVIDIA does not ship in any official package:

- DLSS SDK 310.9.16 (GitHub NVIDIA/DLSS): no NR model.
- Streamline 2.14.16: declares the feature id, no NR plugin, header or DLL.
- Driver 621.01 and its downloaded models: `FeatureNotFound` 0xBAD00004 — the driver core answers
  DLSSNR.* capability queries, then says the feature does not exist.

The only sources are RenoDX-Discord-style distributions, i.e. the user must obtain the DLL themselves,
which is what the launcher's "own_dlss5_copy" logic assumes in `tools/vr_launcher/vr_launcher.cpp`
(a `nvngx_dlssnr.dll` next to `wwhd.exe`, "modified or not: NVIDIA publishes none, and nothing here
checks its signature").

## Exports and dependencies

No fresh binary available to dump, but nr-bridge's forwarder calls exactly these D3D12 entry points,
and this project's Vulkan path mirrors them in the Vulkan API:

| API | Entry points the NR model must expose | Vulkan counterpart in ngx_vk.cpp |
|---|---|---|
| D3D12 | `NVSDK_NGX_D3D12_Init_Ext`, `NVSDK_NGX_D3D12_CreateFeature`, `NVSDK_NGX_D3D12_EvaluateFeature`, `NVSDK_NGX_D3D12_ReleaseFeature` | `NVSDK_NGX_VULKAN_AllocateParameters`, `CreateFeature1`, `EvaluateFeature`, `ReleaseFeature` |
| D3D12 (also exists) | `NVSDK_NGX_D3D12_Shutdown1`, `NVSDK_NGX_D3D12_GetCapabilityParameters` | `NVSDK_NGX_VULKAN_GetCapabilityParameters` (via getCaps) |

The DLL is D3D12-first. nr-bridge's Vulkan story is not one: it runs its own D3D12 device inside an
OpenXR layer; nothing in nr-bridge loads the model through Vulkan. The community evidence that the
model *accepts* Vulkan resources on a game's device comes from the OptiScaler_DLSSNR fork's
`DlssNrFeature_Vk.cpp` (GPL-3.0), not from any NVIDIA package. This project's own
runtime/src/gfx/vulkan/ngx_vk.cpp stage-D4 code (motion vectors, both-eyes VR feature, `DLSSNR.*`
parameters and creation through the driver core) is the adaptation path it chose.

## What gates the model

A chain, from the outside in:

1. **Driver core holds the capability answers.** `DLSSNR available 1` requires driver 615.64+; on older
   drivers the core reports not available, and nothing downstream happens. This project had that pass on
   621.01: the queries succeed and the core offers the feature (docs/VR_DLSS.md "Flat run" section).
2. **Then the core must find the model.** Even when the capability query says "available", the core
   answers `FeatureNotFound` (0xBAD00004) when `nvngx_dlssnr.dll` is not in its search path — the game
   folder first, per the launcher's own finding. That is the current state on the test PC: queries succeed,
   creation fails `FeatureNotFound`, because the model is in no NVIDIA package.
3. **Then the model DLL checks its caller.** When loaded directly (the nr-bridge snippet path), the model
   inspects the calling module's path and refuses a caller whose path does not contain "nvngx.dll"
   (`PlatformError`); a forwarder named `nvngx.dll_*.dll` passes. This is the check the proxy in this
   project now passes by construction (the engine calls the model only through
   `nvngx.dll_wwhd_nr.dll`). Consequence for any other plan: a plain
   `LoadLibraryExW("nvngx_dlssnr.dll")` followed by GetProcAddress of the Init/Create/Evaluate entry
   points *will deliver PlatformError at Init_* time* — the DLL loads fine; initialization is what fails.
4. **Then the model checks the GPU.** Refuses non-Ada GPUs with `FeatureNotSupported` (verified on an
   RTX 2070 Super in nr-bridge's README). DLSS-NR is also, per NVIDIA's own Omniverse docs, a
   Blackwell-and-driver-615.64+ feature — the Turing refusal is the same wall at a lower tier.
5. **And the license framework.** NVIDIA's RTX SDK license covers "material additional functionality"
   distribution inside an application; this project is MPL-2.0 and is not an NVIDIA partner, so NVIDIA
   binaries never ship in the repo — the user supplies the model. Running NVIDIA's binary on AMD via
   community re-implementations (DLSS-NR-on-AMD etc.) is explicitly outside this project's scope for both
   technical and license reasons (docs/VR_DLSS.md, DLSS-NR-on-AMD row).

## What the loader-based override can and cannot deliver here

Per DLL_LOADER_ANALYSIS.md: the loader is load-and-forget. `wwhd.exe` will not call
`NVSDK_NGX_VULKAN_Init_Ext2` on a DLL loaded this way by itself; DLLs get their `DllMain` run, nothing else.
The loader is therefore not the carrier: the engine's own NGX code (ngx_vk.cpp) is. The realistic
approaches, and what each requires:

1. **Put the model next to wwhd.exe for the core's model search.** The launcher already recognizes this
   (`own_dlss5_copy`): NGX's own search finds the model for the driver core's `CreateFeature(18)` — no
   caller-check problem, because *the driver core* is the caller, and the driver core is what the model
   expects to be called by. This is the path the engine already uses on Windows.
2. **Call the model DLL directly when the core refuses feature 18** (e.g. `OutOfDate`, or the model being
   of a different build than the core serves). The model refuses a caller whose path lacks "nvngx.dll"
   (`PlatformError`), so the call must be made through a module whose path contains that string: a
   forwarding proxy named `nvngx.dll_wwhd_nr.dll` (nr-bridge's technique). **Authorized by the user
   2026-11-04** (explicit reversal of the earlier analysis-only decision). Implemented: the snippet path
   below. Everything else stays honest: the model's own result codes pass through unchanged, and a
   non-NVIDIA GPU still cannot initialize (no parameter object exists without the core).

So the current Windows build's behavior stands: the user drops NVIDIA's signed model next to `wwhd.exe`,
the driver core finds it, feature 18 initializes and evaluates on the render path (verified working on the
desktop in the flat run of 2026-11-01, DLAA + 3840x2160 NR per frame, no errors), and the same flow is what
the launcher's DLSS 5 switch turns on/off. No change to dll_loader.cpp is needed or useful for this —
the loader cannot call NGX entry points and the one call-chain that works does not pass through it.

## Summarizing table

| Gate | Where | Status on this engine |
|---|---|---|
| driver capability offer | driver core | passes on 621.01 (`DLSSNR available 1`) |
| model DLL presence | search path | user must supply the file next to wwhd.exe; no legal NVIDIA download exists |
| caller-module check | inside the model | passes when the driver core calls; the snippet path passes it through the proxy (its own path contains "nvngx.dll"); no patching of the model |
| GPU tier | inside the model | Ada+ needed; Turing refused (`FeatureNotSupported`); no bypass work here |
| Vulkan path | engine-side | this renderer already drives feature 18 through the core on its own VkDevice |

## Outstanding questions

- ~~A fresh, signed copy of nvngx_dlssnr.dll 310.9.16-era could be statically analyzed…~~ Done
  (2026-11-04): the user supplied the DLL (repo root, hash in the header of this file); the export table,
  imports and version info above come from that copy. What static analysis cannot answer — the
  caller-check's exact string match, and the in-init driver/GPU gates — the Windows run in
  DLSS_NR_TEST_RESULTS.md will.
- NR execution in VR (both eyes per frame) remains what it already was: implemented through the driver
  core on the game's own device, needing the user's model DLL, per docs/VR_DLSS.md.
