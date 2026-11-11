# DLSS 5 NR snippet path via proxy — implementation plan

Status (2026-11-04): **done**. Steps 1–4 complete; step 5 is the Windows test procedure in
docs/DLSS_NR_TEST_RESULTS.md, which needs the user's machine.

User authorized the caller-check proxy explicitly (2026-11-04), reversing the prior analysis-only
decision. The driver-core path stays primary; the snippet path is the fallback it already logs
"the driver does not serve it" for and then gives up on — this work gives that case a real path.

## Design

- **Proxy**: `nvngx.dll_wwhd_nr.dll` (name passes the model's caller check: caller path must contain
  "nvngx.dll"; same technique and shape as nr-bridge's proven forwarder). Pure x64 C, kernel32 only,
  zig-cc cross-compiled. It finds `nvngx_dlssnr.dll` (proxy dir → exe dir → cwd), loads it with
  LOAD_WITH_ALTERED_SEARCH_PATH, resolves `NVSDK_NGX_VULKAN_{Init_Ext2,CreateFeature1,EvaluateFeature,
  ReleaseFeature}`, forwards through volatile calls (no tail-call), and returns the model's own result
  codes unchanged.
- **Engine**: `ngx_vk.cpp` loads the proxy (exe dir, then data dir) only when a feature-18 create through
  the driver core fails; reuses the core's parameter object (the nr-bridge contract); per-eye feature
  handles know their path (core vs snippet); every stage logged with the model's real codes.
  `WWHD_NR_SNIPPET=0` disables. Non-NVIDIA GPUs: core never initializes → no parameter object → snippet
  path impossible → honest failure (unchanged).
- **No changes**: dll_loader, menus, packaging, signature checking (`verify_dll_signature` stays for the
  core; the model DLL itself was never signature-checked).

## Steps

1. tools/nr_snippet_proxy/nr_snippet_proxy.c + build (zig cc, x86_64-windows-gnu) — inspect exports.
2. ngx_vk.cpp: Snippet loader + feature-18 fallback + path-aware create/evaluate/release + logging.
3. Linux build of the project (type-checks the shared code; _WIN32-only code excluded).
4. Static-analysis findings (caller-check strings, version info) into the docs.
5. Rewrite DLSS_NR_IMPLEMENTATION.md / DLSS_NR_TEST_RESULTS.md for reality; changelog entry.
