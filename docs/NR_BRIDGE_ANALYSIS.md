# NR-Bridge analysis (lowpoplabs/nr-bridge, MIT, v0.3.0 — source read in full, nothing built)

nr-bridge is an OpenXR API layer (plus an OpenVR proxy front end) that runs DLSS 5 Neural Rendering on
a VR game's headset images. Purpose here: what it does, which mechanisms could be reused by this
project's own loader, and which cannot. Two other analyses in this folder cover the rest:
DLL_LOADER_ANALYSIS.md and DLSS_NR_COMPATIBILITY_ANALYSIS.md.

## Shape

Two engines, chosen by `mode=` in its cfg:

| mode | per frame | needs |
|---|---|---|
| direct | copies the eye pair to its own D3D12 device, builds rotation-only motion vectors from head poses, runs NGX feature 18 at a configurable work scale, transfers the result back | `nvngx_dlssnr.dll` supplied by the user |
| reshade | presents the eye pair through a hidden DXGI swapchain so ReShade + the RenoDX add-on treat it as a window | ReShade dxgi.dll + renodx-dlss.addon64 |

Direct mode is the interesting engine; the reshade one is legacy glue.

## The forwarder — what it is, and why we are not taking it

`src/nr_forwarder.cpp` builds `nvngx.dll_nr_bridge.dll`, a 57-line DLL whose only content:

- it loads the user-supplied `nvngx_dlssnr.dll` (`bnr_snip_load`, `LOAD_WITH_ALTERED_SEARCH_PATH`),
- GetProcAddress's the four D3D12 entry points (`NVSDK_NGX_D3D12_Init_Ext`, `CreateFeature`, `EvaluateFeature`,
  `ReleaseFeature`),
- forwards each call through a `volatile` result variable to defeat the compiler turning the forward into a
  tail call (a tail call would leave the original caller's return address — which is exactly what the model
  checks),
- exports old names (`..._bonelab_nr.dll` fallback in the loader) for standalone installs.

Its single purpose: the NR model checks the module that calls its entry points and refuses any whose file
path does not contain `nvngx.dll`. Naming the forwarder `nvngx.dll_nr_bridge.dll` passes that check; the
model then refuses nothing and runs for any caller. Author's comment, verbatim from the source:

> the Neural Rendering model (nvngx_dlssnr.dll) checks the module that calls its entry points and refuses
> any whose file path does not contain "nvngx.dll". This DLL is named to pass that check and does nothing
> but forward.

This is the technique this project's standing decision bars: nvngx_philosophy (docs/VR_DLSS.md) forbids
defeating NVIDIA's checks; the memory no-nr-forwarder-bypass records the same decision twice (2026-11-01
and again, in elaborate form, 2026-11-03). It is documented here for completeness and is not implemented.

## The direct engine — mechanisms worth copying (and the ones already here)

`src/nr_direct.h`, ~1200 lines, D3D11 game device + private D3D12 device:

- **Shared textures + cross-API fences** (D3D11⇄D3D12 via `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` +
  `D3D11Device5::OpenSharedFence`, ring of 3 slots, waits in both directions). Clean pattern; this project
  already does the Vulkan equivalent on its own device and gains nothing from a second device.
- **Same-device-only NGX contract** (already established here): driver's `_nvngx.dll` asked first; capability
  parameter block via `NVSDK_NGX_D3D12_GetCapabilityParameters`; params set through vtable slots the code
  *probes at runtime* (`DiscoverFloatSlot`: round-trip a float through candidate setter/getter slots 6/1/14/9
  …, logs "confirmed" vs "ASSUMED"); feature id 18 passed where the public header says `Reserved18`.
- **Driver-core-then-snippet fallback** (`LoadNgx`): try the driver core; if `CreateFeature(18)` returns
  `OutOfDate` (driver wants a newer model) or the core lacks the feature, fall back to driving
  `nvngx_dlssnr.dll` directly — *through the forwarder*, which is the part we exclude. `ngx=` cfg values
  select: 0 auto (core then snippet), 1 core only, 2 snippet only, 3 none (passthrough debug), 4/5 debug
  half-brightness stand-ins.
- **Mapping and gating the leftovers are the reusable part:** the NR model's parameter names are not in any
  NVIDIA header; nr-bridge uses the same set OptiScaler DLSSNR reverse-engineered:
  `DLSSNR.{Color,Depth,MVec,Output,Width,Height,Reset,Enabled,Style,Preset,Intensity,LocalStructureStrength,
  LocalToneStrength,LocalStructureStrength,SkinStructureStrength,UseAutoMask,UICorrection,.Hint.Render.Preset,
  MVecScaleX/Y,.*SubrectBase/X/Y/W/H}`, plus `CreationNodeMask`, `VisibilityNodeMask` at create. This project's
  own code (runtime/src/gfx/vulkan/ngx_vk.cpp, `g_nrParams` set-up) already mirrors this exact list — identical
  names, so the two implement the same contract.
- **Error-code table** (`ResultName`): `FeatureNotSupported`/`PlatformError`/`FeatureNotFound`/`OutOfDate`/
  `Denied` etc. with `0xBAD0000x` codes — same table this renderer logs.
- **Feature lifecycle**: features are created lazily per pass (multi-pass = another instance fed the previous
  pass's output), released on reconfigure, `Shutdown1` on the core at shutdown; evaluate failure → disable the
  layer for the session rather than crash the game. A worth-copying robustness idea for any evaluate-driven
  path.
- **Caller module and GPU gating are handled by the operating ambient environment, not by this code** — the
  forwarder handles the caller check; the model itself refuses a Turing GPU with `FeatureNotSupported`
  (README, verified on an RTX 2070 Super), which the bridging code simply reports. nr-bridge does **not** patch
  or spoof vendor detection inside the model — it only avoids it by being an NVIDIA-blessed caller-shaped module
  and by running on hardware the model accepts anyway.

## Dependencies and limits

- Needs the user to supply NVIDIA's `nvngx_dlssnr.dll` (~158 MB, DLSS 310.8/Streamline 2.13-era; the GitHub
  repo never ships it and explicitly says never to).
- Needs an NVIDIA GPU the model accepts (verified Ada/Blackwell; Turing refused `FeatureNotSupported`).
- D3D12 only. There is no Vulkan snippet path in nr-bridge at all — direct mode is D3D12 end to end.
- The OpenVR front end renames the real `openvr_api.dll` and drops in a forwarding proxy — its own forwarding
  plumbing, specific to that loader, not adaptable to ours.

## What we take, what we leave

| nr-bridge mechanism | Take? |
|---|---|
| Caller-path forwarder named `nvngx.dll_*.dll` | **No** — standing decision (`no-nr-forwarder-bypass`), twice confirmed; defeats NVIDIA's caller check |
| Rotation-only motion vectors from head poses | No (this project already has real 6DoF motion vectors; pose-only loses translation) |
| Second D3D12 device + cross-API fences | No (our engine runs the model on the game's own VkDevice) |
| Parameter block probing / vtable slot discovery for the D3D12 capability block | Not applicable (we are on Vulkan; our `ngx_vk.cpp` already implements the Vulkan analogue) |
| Driver-core-first-then-model-DLL fallback *ordering* | Already present here (`ngx_vk.cpp` init → capability read → feature create), minus the snippet fallback we exclude |
| Per-frame error → disable-for-session robustness | Already present (`g_runFailed`/stage D4 pattern in `ngx_vk.cpp`) |
| DLSSNR.* parameter names and NGX feature id 18 | Already mirrored in `ngx_vk.cpp` |

Net: nr-bridge's one uniquely valuable trick for a loader-based integration is the forwarder, and that is the
part this project has decided not to do. Everything else it does, this renderer either already does on Vulkan
or has evaluated and rejected for cause (second device, pose-only motion vectors, ReShade detour).
