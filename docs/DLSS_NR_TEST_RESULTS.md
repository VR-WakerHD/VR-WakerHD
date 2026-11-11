# DLSS 5 NR override — test results (what was verified here, and what the Windows run must show)

## Verified in this environment (Linux, no Windows, no NVIDIA GPU; the game was not launched)

| check | result |
|---|---|
| model DLL static analysis | done on the user's copy at the repo root: SHA-256 `e67dee…81c989a`, PE32+ x64, 165,840,496 bytes; **55 exports** — the full NGX surface for CUDA, D3D11, D3D12 **and Vulkan** (`NVSDK_NGX_VULKAN_Init_Ext2`, `CreateFeature1`, `EvaluateFeature`, `ReleaseFeature`, `Shutdown1`, …); imports only KERNEL32/ADVAPI32/USER32/VERSION (details in DLSS_NR_COMPATIBILITY_ANALYSIS.md) |
| proxy builds | `tools/nr_snippet_proxy/build.sh` (zig cc, x86_64-windows-gnu): `nvngx.dll_wwhd_nr.dll` produced; export table verified — all six `wwnr_*` exports present |
| engine builds | `cmake --build build --target wwhd` on Linux: clean (the snippet call sites compile against the non-Windows stubs; Windows-only machinery is `#ifdef _WIN32`) |
| engine code review | the fallback engages only on a core create failure; each feature handle records its path (`snippet` flag) so release/evaluate return to the right module; `WWHD_NR_SNIPPET=0` disables; no path reports success on load alone |
| game run | not run (house rule: the player runs it) |

## What still needs the Windows machine (RTX 5080, driver 621.01)

1. **Build or take the proxy.** `tools/nr_snippet_proxy/nvngx.dll_wwhd_nr.dll` is committed prebuilt; if
   rebuilding: run `tools/nr_snippet_proxy/build.sh` from a WSL/clang setup, or compile
   `nr_snippet_proxy.c` on Windows as x64 `/LD` with the same six exports.
2. **Place the files** next to `wwhd.exe`:
   - `nvngx_dlssnr.dll` (the model; the same copy hash-verified above)
   - `nvngx.dll_wwhd_nr.dll` (the proxy)
3. **Core-path control run first** (no proxy needed): start with `dlss5=1`. Expected log:
   `[ngx] core …; DLSSNR available 1 …` and `eye … made (through the driver core, …)`. This is the known-good
   2026-11-01 behavior.
4. **Snippet-path run**: rename/move the model out of the core's search (or leave it — the core path wins
   when it works) and check the fallback by forcing the core to refuse: if the log shows
   `the core would not make the feature (0x…)` followed by
   `[ngx] DLSS 5 snippet: model …, Init_Ext2 -> 0x…` and `eye … made (through the model, …)`, the proxy
   path engaged and the model accepted the proxy as caller. `Init_Ext2 -> 0xBAD00002` would mean the
   caller check still fired (proxy not the calling module — report it); `0xBAD00007` would mean the proxy
   could not resolve the model's exports.
5. **Negative test**: `WWHD_NR_SNIPPET=0` must restore the old behavior exactly (refusal logged, DLSS 5
   off, Super Resolution unaffected).
6. **Non-NVIDIA GPU** (if available): core init fails → no parameter object → no snippet attempt → the
   same honest failure as before. The proxy must never appear in the log.

## Not verifiable here, stated plainly

- Whether the model accepts the proxy as caller (the caller-check pass) — needs one Windows run.
- Whether the model's Vulkan `Init_Ext2` accepts a parameter object created by the core for a *different*
  module context (nr-bridge's snippet path does exactly this and works, but that is D3D12 evidence, not
  Vulkan evidence).
- Frame-level picture correctness and performance through the snippet path.

## Prior results that still stand

- 2026-11-01 flat run (docs/VR_DLSS.md): DLSS 5 NR + DLAA every frame at 3840x2160 through the driver
  core, picture correct at frame 4200, no evaluate errors, 81–84 fps (render-thread-bound).
- The loader's own behaviors (missing deps → error 126 logged; wrong arch → 193; dedup) are unchanged and
  were verified in the loader analysis.

## Outstanding

1. The one Windows session above (control run, snippet run, negative run).
2. If the snippet path works: measure VR fps with NR through the model vs through the core (Phase 7's
   real question), and record which path engaged in `dlss-status.ini` if the player wants it visible.
