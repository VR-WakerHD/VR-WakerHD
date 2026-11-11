# DLSS 5 NR override — implementation report

This file answers Phase 5/6/7 of the stated task: what was implemented so `nvngx_dlssnr.dll` can load
and initialize even when the driver's NGX core refuses feature 18. The driver-core path is unchanged and
stays primary; what is new is the **snippet path** behind it. The analysis docs in the same folder carry
the reasoning; this file describes what actually ships.

## The two paths, as the engine now runs them

1. **Driver core (primary, unchanged).** `nvngx_dlssnr.dll` next to `wwhd.exe` (the launcher's
   `own_dlss5_copy`), found by NGX's own search, feature 18 created through the driver's core on the
   game's VkDevice. Verified working in the 2026-11-01 flat run (3840x2160 NR + DLAA, no errors, driver
   621.01). Nothing about this path changed.
2. **Snippet path (new, fallback only).** When the core's `CreateFeature1(18)` fails
   (`FeatureNotFound` 0xBAD00004, `OutOfDate` 0xBAD0000C, or any other refusal), the engine loads
   `nvngx.dll_wwhd_nr.dll` (the proxy, built from `tools/nr_snippet_proxy/`) and drives the model DLL
   **directly** on the same device, with the same parameter object. The proxy is named so its path
   contains "nvngx.dll" — the model checks the calling module's file path and answers `PlatformError`
   (0xBAD00002) otherwise; the proxy is the calling module, and volatile results in it prevent the
   compiler from turning a forward into a tail call (a tail call would leave the real caller's return
   address in place, which is what the model inspects). Technique after nr-bridge's forwarder; the user
   supplied the model and explicitly authorized this on 2026-11-04, reversing the earlier
   analysis-only decision.

The snippet path is **fallback only**: if the core initializes and serves feature 18, the proxy is never
loaded. `WWHD_NR_SNIPPET=0` disables the fallback entirely (core path alone, as before).

## What was changed

- **`tools/nr_snippet_proxy/nr_snippet_proxy.c` + `build.sh`** (new): a ~100-line x64 forwarding proxy.
  Exports: `wwnr_snip_load` (loads the model by path), and `wwnr_init_ext2`, `wwnr_create_feature1`,
  `wwnr_evaluate_feature`, `wwnr_release_feature`, `wwnr_shutdown1` (volatile-call forwards of the
  model's `NVSDK_NGX_VULKAN_*` entry points). kernel32 is its only dependency. Built with
  `zig cc -target x86_64-windows-gnu`; the prebuilt DLL is committed so players do not build tools.
  The model DLL itself is **never** committed and **never** modified — the proxy changes nothing in the
  model's bytes.
- **`runtime/src/gfx/vulkan/ngx_vk.cpp`**:
  - `Snippet` state + `snippet_init()`: finds the proxy (program folder, then data folder), loads it,
    resolves its exports, finds the model (program folder, data folder, or the proxy's own folder),
    loads it through the proxy, and calls `Init_Ext2` on the run's device with the same app id
    (`0x57574844`), SDK version (0x15) and NGX data path the core was started with. Tried once per run.
  - `g_snipDev`: the run's instance/physical device/device/proc-addrs, stored where the core's entry
    points are kept (the snippet starts on the same device).
  - `evaluate_nr()`: on a core create failure the snippet path is attempted before giving up; each
    feature handle remembers which path made it (`NrFeature::snippet`) so release and evaluate go back
    to the right module. Size/preset changes re-create through the same path the handle came from.
  - Non-Windows builds compile the same call sites against stubs that can never engage (the proxy is a
    Windows DLL); Linux behavior is unchanged.
- **Nothing else**: `dll_loader` untouched (the proxy rides in the game folder, not through the extra-DLL
  loader), menus untouched, packaging untouched. The model DLL is not signature-checked by the engine —
  the player supplies it, exactly as for the core path.

## The 7 states, as the engine now reports them

| state | where it shows |
|---|---|
| proxy/model DLL found and loaded | `[ngx] DLSS 5 snippet path: …` lines; a missing file is logged, not fatal |
| exports resolved | "the proxy lacks its exports" if resolution fails; otherwise silent success |
| init attempted | `[ngx] DLSS 5 snippet: model …, Init_Ext2 -> 0x%08X` |
| hardware check handled | the model's own result passes through: `FeatureNotSupported` on a too-old GPU is logged as the model's answer, never masked |
| API validated | `Init_Ext2` ran on the game's own VkInstance/VkDevice with the engine's real proc-addrs |
| feature init success | `CreateFeature1 -> 0x%08X` then `eye %d: %ux%u made (through the model, …)` |
| execution success | per-frame evaluate through the model; an evaluate failure logs `DLSS 5's evaluate failed` as before |

A loaded DLL alone never counts as success anywhere in the log; every stage prints its own result code.

## What the snippet path cannot fix

- **No core, no snippet.** The parameter object comes from the core's `AllocateParameters`; without a
  started core (non-NVIDIA GPU, no driver) there is nothing to hand the model, and the failure stays
  honest.
- **GPU tier.** The model's own Ada+ check is inside the model; a Turing card gets the model's own
  `FeatureNotSupported` through the proxy, unchanged.
- **Caller check only.** The proxy defeats the caller-path check. It does not patch the model, spoof
  return addresses, or fake vendor strings.

## Phase 7 — performance and stability

The fallback adds zero cost when unused (one `getenv` and two file-existence checks, once per run, only
after a core create failure). When active, calls go through one extra function frame per NGX call —
noise against a neural render pass. The core path's measured numbers (docs/VR_DLSS.md) are unaffected.

## Deliverable files

- docs/DLL_LOADER_ANALYSIS.md — the extra-DLL loader, line-by-line
- docs/NR_BRIDGE_ANALYSIS.md — the third-party approach; which techniques were adapted
- docs/DLSS_NR_COMPATIBILITY_ANALYSIS.md — the model DLL itself (exports, imports, gating chain)
- docs/DLSS_NR_TEST_RESULTS.md — what was verified here, and the Windows test procedure
