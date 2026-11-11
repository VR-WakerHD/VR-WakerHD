# DLL loader analysis (runtime/src/dll_loader.{h,cpp}, docs/dll-loader.md)

What the game's extra-DLL mechanism can and cannot do, checked line by line. Prepared for the
DLSS 5 NR override question (DLSS_NR_COMPATIBILITY_ANALYSIS.md); no loader code was changed.

## What it is

A start-time loader, not an interception layer. `dll_loader::load_all()` (main.cpp calls it after the log
file starts, before renderer/VR/game) collects paths from five sources and calls `LoadLibraryExW`
(Windows) or `dlopen` (Linux) on each:

1. every `*.dll`/`*.so` in a `dlls` folder next to the executable, then in the data folder (file-name order;
   no subfolders);
2. the Graphics > Extra DLLs list (`settings.ini` key `extraDlls`, '|' separated), from where each was chosen;
3. `WWHD_LOAD_DLLS` (';' separated, relative to the data folder);
4. `--load-dll <path>`, repeatedly;
5. deduplicated by canonical path, case-insensitive key on Windows.

`--no-dlls` / `WWHD_NO_DLLS=1` skips everything. A failure is logged (`[dlls] ... cannot load: ...`) and
skipped, never fatal.

## Answers to the architecture questions

| Question | Answer |
|---|---|
| How are overrides resolved? | They are not leaked DLLs — resolution is by explicit path only. No name-based override table, no `LoadLibrary` hooking. |
| Does it intercept `LoadLibrary`/`LoadLibraryEx`? | No. The game's own imports and every later dynamic load are untouched. |
| Proxy DLLs or forwarding? | Not supported by the loader itself. A proxy DLL can simply be placed in `dlls\` or listed — it is loaded like any other DLL — but the loader provides no forwarding plumbing. |
| Custom DLL search paths? | Only `LOAD_WITH_ALTERED_SEARCH_PATH` on Windows: the loaded DLL's own dependencies are found next to it first, then the usual places. Linux: `RTLD_LOCAL`, no path augmentation. |
| Exports / function pointers? | Nothing is looked up or called. The game "calls nothing else in them" (docs/dll-loader.md); `DllMain`/`_init` is the only entry that runs. |
| Initialization hooks? | None beyond `DllMain` on the loader's thread. The doc warns: `DllMain` runs under the loader lock — spawn a thread for anything longer. |
| Static imports vs dynamic loads? | Static imports of an extra DLL resolve normally through the altered search path; dynamically loaded DLLs elsewhere in the process are unaffected (no hooks). |

## Limits relevant to nvngx_dlssnr.dll

- Error 126 (`The specified module could not be found`) means a missing dependency of the DLL itself — its own
  folder is searched first, so helper DLLs must sit beside it.
- Error 193: a 32-bit DLL; the game is 64-bit.
- The loader will happily `LoadLibraryEx` `nvngx_dlssnr.dll`, and on success the log prints
  `[dlls] loaded ...`. That is *all* it means: no export is called, so no NR feature could ever initialize
  through this mechanism alone. Anything that wants to *use* the NR model must reach it through NGX's own
  loading path or a purpose-built proxy of its own — outside this loader's scope.
- There is no unload, no per-DLL config, no version negotiation, no diagnostic beyond the one line per DLL.

## Conclusion

The existing architecture is a delivery mechanism for third-party DLLs, by design "load and forget". It can
carry a proxy DLL into the process, but it implements none of: LoadLibrary interception, export forwarding,
load-time or init-time hooks, search-path control, or per-DLL options. Documented here so the NR work's
requirements are judged against what actually exists rather than an assumed interception framework.
