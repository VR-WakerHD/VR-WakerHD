# VR-WakerHD development: code layout, tests and the upstream project

For people who want to work on **VR-WakerHD**, the open-source VR mode for *The Legend of Zelda: The Wind Waker HD*
built on ZeldaWWHDRecomp. To build it, see [BUILDING.md](BUILDING.md). The design is described in
[VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).

## Where the VR code is

| Path | What it holds |
|---|---|
| `runtime/src/vr/` | The game side: `vr_game.cpp` (camera, body, arms, items, input), `openxr_session.cpp` (OpenXR), and the pure, tested logic in the `vr_*.h` headers |
| `runtime/src/gfx/vulkan/vr_vulkan.cpp` | The renderer side: the eyes, the layers, frame timing |
| `tools/vr_launcher/` | The VR launcher |
| `android/` | The Android and Quest app |
| `LICENSES/VR-FILES.txt` | Every file of the VR mode (GPL-3.0-or-later with the credit term); every other file is MPL-2.0 |

## Tests

- All tests: `ctest --test-dir <build folder> --output-on-failure`.
- The VR logic's unit tests are `vr_test` (`runtime/tools/vr_test.cpp`). They need no headset.
- Scripted test aids also need no headset, for example `WWHD_VR_TEST_WHEEL` and `WWHD_VR_TEST_BODY`. Each is described
  in the doc it belongs to. The in-game test area is [VR_TEST_ENVIRONMENT.md](VR_TEST_ENVIRONMENT.md), and the checks
  are in [VR_TESTING.md](VR_TESTING.md).
- A build without the game, for tests only: generate placeholder code with `python3 tools/recomp/stubgen.py
  build/gen-stub` and configure with `-DGEN_DIR=$PWD/build/gen-stub`.

## The upstream project

VR-WakerHD builds on [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) and keeps its full Git
history; the VR work is a series of commits on top of it. To bring in the original's changes:

```bash
git remote add upstream https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp.git
```

```bash
git fetch upstream
```

```bash
git merge upstream/main
```

The original's README (macOS and Android builds, flat-screen controls, mods, save states) is kept as
[UPSTREAM_README.md](UPSTREAM_README.md). Development notes, audits and benchmarks are in [diagnosis/](diagnosis/README.md).

## Contributing

Open an [issue](https://github.com/VR-WakerHD/VR-WakerHD/issues) for a bug or an idea before a large change. Code under
`LICENSES/VR-FILES.txt` is GPL-3.0-or-later with the credit term ([LICENSE-VR.md](../LICENSE-VR.md)); changes to the
original port's files stay MPL-2.0. Never commit game files, recompiled code (`build/gen/`), keys or shader caches.

See also: [BUILDING.md](BUILDING.md) · [FEATURES.md](FEATURES.md) · [back to the README](../README.md)
