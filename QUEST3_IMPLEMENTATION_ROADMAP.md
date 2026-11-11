# Quest 3 implementation roadmap (2026-11-03)

Ranked by the measured bottleneck: the Quest 3 is CPU-bound at 24-30 fps with the GPU 50-60 % busy (logs in
[QUEST3_PERFORMANCE_AUDIT.md](QUEST3_PERFORMANCE_AUDIT.md)). Project rules: graphics exact, 30 Hz logic unchanged,
multiview decisions asked for, headset tests asked for first, PC VR unaffected. **Every item needs a Quest 3 for its
validation; this PC cannot build the Android target.**

## Priority 1: immediate

| # | Item | Benefit | Complexity / risk | Files | Test | Quest needed |
|---|---|---|---|---|---|---|
| 1.1 | **Done:** declare the game, render and end-frame threads (`XR_KHR_android_thread_settings`) | unknown until measured: better core placement and priority for the CPU-bound threads [est] | low / low (runtime-detected; logged) | `openxr_session.*`, `vr_vulkan.cpp` | log `[vr] XR thread scheduling: ... declared`; A/B fps and render-thread p99 | yes |
| 1.2 | **Done:** memory log every 10 s | memory pressure visible | low / low | `vr_vulkan.cpp` | log `[vr] memory: ...` in Outset and a busy stage | yes |
| 1.3 | **Baseline run** with today's build | the numbers everything else is compared to | — | — | benchmark scenes (QUEST3_BENCHMARK_RESULTS.md), 3 min each | yes |
| 1.4 | Read the BC line in the start-up log | decides whether texture transcoding is P2 or not needed | — | — | `device has no BC texture support` present or not | yes |
| 1.5 | **On by default for Quest VR builds (the user, 2026-11-03):** ThinLTO and `CPU=quest3` in `android/build_native.sh` (`-march=armv8.2-a -mtune=cortex-a78`; `LTO=0` / `CPU=generic` off) — still to measure | CPU time of the game thread (OoT VR's biggest win was compiler optimization; we are at -O3 already) | low / medium (the game code keeps `-ffp-contract=off`; an `-mcpu` must not enable FP contraction) | `android/build_native.sh`, `CMakeLists.txt` | game pass time p50/p99 in the log, same scenes | yes |
| 1.6 | End-frame thread's effect on Quest | confirms the 13-17 ms serial wait is gone | — | — | `end-frame thread: the render thread waited ...` line | yes |

## Priority 2: major standalone work

| # | Item | Benefit | Complexity / risk | Files | Test |
|---|---|---|---|---|---|
| 2.1 | **Built (mobile GPU builds only; not run); multiview and the guard on by default on a standalone Quest (the user, 2026-11-03):** some blocks dynamic for over-limit pipelines (`uniform_plan.h`), the multiview twin kept within the cap, and no mixed passes on two-eye images (`kMultiviewNoMix`); was: **Multiview for over-limit pipelines** (Adreno's `maxDescriptorSetUniformBuffersDynamic`): pack a draw's blocks into one dynamic buffer, or plain buffers with per-eye sets | the largest CPU saving available (each draw recorded once for both eyes); removes the mixed passes the black squares came from | high / high | `draw.cpp` (pipeline layout, `bind_stage`, `kMvNotDynamic`), `multiview_pipeline.cpp`, `stereo.cpp` | desktop first with the limit forced to 8 (picture comparison against today's), then Quest: the multiview share in the log, fps, squares |
| 2.2 | Lock contention on Quest (`WWHD_VK_LOCK_COUNT=1` sites) | 1.5-3 ms a frame on the atlas thread | medium / medium | `buffer_cache.cpp`, `shaders.cpp`, `surfaces.cpp`, `draw.cpp` | lock counts before/after |
| 2.3 | BC -> ASTC transcoding on upload, if 1.4 shows no BC | 2-4x less texture memory and bandwidth than RGBA8 | medium / medium (quality per format) | the BC decoder path (`bc_decode.cpp`, `bc_shader.h`) | memory log; picture checks of text, icons, sea |
| 2.4 | ASTC texture packs for standalone | pack memory (OoT VR: 4x less) | medium / low | texture pack tools, `texture_pack_vk.cpp` | memory log with a pack |
| 2.5 | **Built (not run):** `XR_EXT_performance_settings` Boost while the game is stalled, Sustained High after | shorter load freezes | low / low | `openxr_session.cpp` | load times; thermal |
| 2.6 | **Default now off on Quest** (the user, 2026-11-03); the A/B (Auto vs off) still to run | smoothness at 36 fps vs artifacts | — | settings | smearing on static geometry, sword swings |

## Priority 3: advanced

| # | Item | Notes |
|---|---|---|
| 3.1 | App-side fixed foveation (FDM on the eye passes, per layer) | GPU only; after 2.1 |
| 3.2 | Dynamic resolution with multiview on the Qualcomm driver | needs the rescale crash understood |
| 3.3 | Half-resolution post passes on standalone | only if GPU timestamps show them |
| 3.4 | Refresh rates from the runtime's list | Quest 3: 72/80/90/120 |
| 3.5 | 72/80 fps full-rate schedules | parked until the Frame or Quest holds 72 |

## Priority 4: experience

Haptic strength and block debounce; comfort vignette; theatre screen for cutscenes; reach-to-holster shortcuts; fade on
scene changes (QUEST3_VR_EXPERIENCE_AUDIT.md section 6). Each opt-in, each a headset check.

## Order

1.3 baseline -> 1.4 / 1.6 read from it -> 1.1 A/B -> 1.5 -> 2.1 (desktop work first, picture-checked) -> 2.2 -> 2.3/2.4
by the memory numbers -> 2.6 -> P3 -> P4.
