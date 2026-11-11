// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// NVIDIA NGX (DLSS) decisions (runtime/src/gfx/vulkan/ngx_core.h): what the settings may offer for each driver
// answer, the status line's reasons, the parameter object's slots, and the per-eye features with a fake backend:
// one feature per eye, made again on size and mode changes, a failed create falling back to native rendering,
// releases at shutdown and none after a lost device.
#include "gfx/vulkan/ngx_core.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace ngx;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

struct FakeBackend {
    struct Made { int eye; uint32_t inW, inH, outW, outH; Mode mode; uint64_t handle; };
    std::vector<Made> made;
    std::vector<uint64_t> released;
    bool failNext = false;
    uint64_t next = 100;
    uint64_t create(int eye, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, Mode mode) {
        if (failNext) {
            failNext = false;
            return 0;
        }
        made.push_back({eye, inW, inH, outW, outH, mode, ++next});
        return next;
    }
    void release(uint64_t h) { released.push_back(h); }
};

static Caps working_caps() {
    Caps c;
    c.built = c.nvidia = c.coreLoaded = c.initialized = true;
    c.gpu = "NVIDIA GeForce RTX 5080";
    c.driver = "621.01";
    c.superResolution.queried = c.superResolution.available = true;
    c.neuralRendering.queried = true;
    return c;
}

int main() {
    const Inputs all{true, true, true, true};
    const Inputs today = renderer_inputs();
    const Inputs noMotion{true, false, false, false};

    // render scales: NVIDIA's ratios, DLAA and Off at 1:1
    CHECK(render_scale(Mode::Off) == 1.0f && render_scale(Mode::Dlaa) == 1.0f);
    CHECK(render_scale(Mode::Performance) == 0.5f);
    CHECK(render_scale(Mode::Quality) > render_scale(Mode::Balanced));
    CHECK(render_scale(Mode::Balanced) > render_scale(Mode::Performance));
    CHECK(render_scale(Mode::Performance) > render_scale(Mode::UltraPerformance));
    // DLSS 5's own settings: NVIDIA's defaults, a round trip through the keys, out-of-range values clamped
    {
        const NrSettings def;
        CHECK(def.intensity == 1 && def.globalTone == 0 && def.style == 0 && def.preset == 0 && def.iterations == 1);
        NrSettings n;
        n.intensity = 0.5f; n.localStructure = 0.25f; n.autoMask = true; n.style = 2; n.preset = 7; n.iterations = 3;
        const auto kv = nr_to_kv(n);
        auto get = [&](const char* k, std::string& v) {
            for (const auto& [key, value] : kv)
                if (key == k) { v = value; return true; }
            return false;
        };
        CHECK(nr_from_kv(get) == n);
        auto bad = [](const char* k, std::string& v) {
            const std::string key = k;
            v = key == "dlss5.intensity" ? "4" : key == "dlss5.style" ? "9" : key == "dlss5.preset" ? "-2"
              : key == "dlss5.iterations" ? "0" : "x";
            return true;
        };
        const NrSettings c = nr_from_kv(bad);
        CHECK(c.intensity == 1 && c.style == 2 && c.preset == 0 && c.iterations == 1 && c.globalTone == 0);
        CHECK(std::string(nr_style_name(1)) == "Natural");
    }
    // NVIDIA's NVSDK_NGX_PerfQuality_Value and default presets (SDK 310.9.16)
    CHECK(perf_quality_value(Mode::Off) == -1 && perf_quality_value(Mode::Performance) == 0);
    CHECK(perf_quality_value(Mode::Balanced) == 1 && perf_quality_value(Mode::Quality) == 2);
    CHECK(perf_quality_value(Mode::UltraPerformance) == 3 && perf_quality_value(Mode::Dlaa) == 5);
    CHECK(default_render_preset(Mode::Dlaa) == 11 && default_render_preset(Mode::Quality) == 11);
    CHECK(default_render_preset(Mode::Performance) == 13 && default_render_preset(Mode::UltraPerformance) == 12);

    // 1. DLSS unavailable (not in the build): only Off, said so
    {
        Caps c;
        CHECK(exposed_modes(c, all) == std::vector<Mode>{Mode::Off});
        CHECK(status(c, all) == "NVIDIA DLSS: not in this build");
    }
    // 2. unsupported GPU (AMD): only Off; the native renderer is untouched
    {
        Caps c;
        c.built = true;
        c.gpu = "Radeon RX 480";
        CHECK(exposed_modes(c, all).size() == 1);
        CHECK(contains(status(c, all), "is not NVIDIA's"));
        CHECK(!neural_rendering_exposed(c, all));
    }
    // 3. unsupported driver: Super Resolution needs a newer one, with the version in the status
    {
        Caps c = working_caps();
        c.superResolution.available = false;
        c.superResolution.needsDriver = true;
        c.superResolution.minMajor = 999;
        c.superResolution.minMinor = 1;
        CHECK(exposed_modes(c, all).size() == 1);
        CHECK(contains(status(c, all), "needs driver 999.1"));
    }
    // 4. initialisation failure: no choices, the reason shown
    {
        Caps c = working_caps();
        c.initialized = false;
        c.failure = "NGX did not start on this device (0xBAD00002)";
        CHECK(exposed_modes(c, all).size() == 1);
        CHECK(contains(status(c, all), "0xBAD00002"));
        CHECK(effective_mode(Mode::Quality, c, all) == Mode::Off);
    }
    // without motion vectors or jitter: no Super Resolution choice even when available, and the status names what is
    // missing
    {
        Caps c = working_caps();
        CHECK(exposed_modes(c, noMotion).size() == 1);
        CHECK(effective_mode(Mode::Dlaa, c, noMotion) == Mode::Off);
        const std::string s = status(c, noMotion);
        CHECK(contains(s, "Super Resolution / DLAA: available"));
        CHECK(contains(s, "motion vectors") && contains(s, "projection jitter"));
        CHECK(contains(s, "active: Off"));
    }
    // the renderer's inputs today (motion vectors, jitter, upscaling): Off, DLAA, Quality, Balanced, Performance offered;
    // Ultra Performance has no internal resolution of its own (render_factor) and runs as Off, named
    {
        Caps c = working_caps();
        CHECK(today.depth && today.motionVectors && today.jitter && today.upscaling);
        const auto m = exposed_modes(c, today);
        CHECK(m.size() == 5 && m[0] == Mode::Off && m[1] == Mode::Dlaa && m[2] == Mode::Quality &&
              m[3] == Mode::Balanced && m[4] == Mode::Performance);
        CHECK(effective_mode(Mode::Dlaa, c, today) == Mode::Dlaa);
        CHECK(effective_mode(Mode::Quality, c, today) == Mode::Quality);
        CHECK(effective_mode(Mode::UltraPerformance, c, today) == Mode::Off);
        CHECK(contains(status(c, today, Mode::Dlaa), "active: DLAA"));
        CHECK(contains(status(c, today, Mode::Balanced), "active: DLSS Balanced"));
        CHECK(contains(status(c, today, Mode::UltraPerformance), "active: Off (chosen: DLSS Ultra Performance)"));
        CHECK(!contains(status(c, today), "not offered yet"));
        // the internal resolutions: the dynamic resolution's levels, each mode its own, DLAA and Off at full
        CHECK(render_factor(Mode::Quality) == 0.75f && render_factor(Mode::Balanced) == 0.625f &&
              render_factor(Mode::Performance) == 0.5f && render_factor(Mode::Dlaa) == 1.0f);
        CHECK(upscaling_mode(Mode::Quality) && !upscaling_mode(Mode::Dlaa) && !upscaling_mode(Mode::UltraPerformance));
    }
    // with every input: all six choices, in order; an offered mode runs as asked
    {
        Caps c = working_caps();
        const auto m = exposed_modes(c, all);
        CHECK(m.size() == 5 && m[0] == Mode::Off && m[1] == Mode::Dlaa && m[4] == Mode::Performance);
        CHECK(effective_mode(Mode::Balanced, c, all) == Mode::Balanced);
    }
    // DLSS 5 Neural Rendering: its own toggle, only when the driver says available; never one of the modes
    {
        Caps c = working_caps();
        CHECK(!neural_rendering_exposed(c, all));  // reported, not available
        CHECK(contains(status(c, all), "DLSS 5 Neural Rendering: not available"));
        c.neuralRendering.available = true;
        CHECK(neural_rendering_exposed(c, all));
        CHECK(!neural_rendering_exposed(c, noMotion));  // no motion vectors
        CHECK(exposed_modes(c, all).size() == 5);  // the toggle adds no mode
        c.neuralRendering = FeatureCaps{};
        CHECK(contains(status(c, all), "DLSS 5 Neural Rendering: not reported by the driver"));
    }
    // the settings' keys and the launcher's status file (written by the game, read by the launcher)
    {
        for (Mode m : kModes) CHECK(parse_mode(mode_key(m)) == m);
        CHECK(parse_mode("nonsense") == Mode::Off && parse_mode("") == Mode::Off);
        Caps c = working_caps();
        StatusFile f = parse_status_file(status_file_text(c, noMotion));
        CHECK(f.present && f.gpu == "NVIDIA GeForce RTX 5080" && f.driver == "621.01");
        CHECK(f.superResolution && !f.neuralRendering && !f.neuralOffered);
        CHECK(f.offered.size() == 1 && f.offers(Mode::Off) && !f.offers(Mode::Dlaa));  // no motion: only Off
        const StatusFile now = parse_status_file(status_file_text(c, today));
        CHECK(now.offered.size() == 5 && now.offers(Mode::Quality) && !now.offers(Mode::UltraPerformance));
        CHECK(contains(f.status, "motion vectors"));
        c.neuralRendering.available = true;
        f = parse_status_file(status_file_text(c, all));
        CHECK(f.offered.size() == 5 && f.offers(Mode::Performance) && !f.offers(Mode::UltraPerformance) &&
              f.neuralRendering && f.neuralOffered);
        CHECK(!parse_status_file("").present);
        f = parse_status_file("offered=off,bogus,dlaa\r\nstatus=x\r\n");  // unknown names dropped, CRLF read
        CHECK(f.offered.size() == 2 && f.offers(Mode::Dlaa) && f.status == "x");
        // the chosen mode in the status: not offered, so Off runs and the choice is named
        CHECK(contains(status(working_caps(), today, Mode::UltraPerformance, true),
                       "active: Off (chosen: DLSS Ultra Performance)"));
        CHECK(contains(status(working_caps(), today, Mode::Off, true), "DLSS 5 chosen, not available"));
    }
    // motion vectors only while a DLSS feature is chosen
    CHECK(!motion_wanted(Mode::Off, false));
    CHECK(motion_wanted(Mode::Off, true) && motion_wanted(Mode::Dlaa, false) && motion_wanted(Mode::Quality, true));
    // after the probe: only a feature this device can run (DLSS 5 without its library, the settings' choice here: none)
    {
        Caps c = working_caps();
        CHECK(!motion_wanted(Mode::Off, true, c, today) && motion_wanted(Mode::Dlaa, false, c, today));
        c.neuralRendering.available = true;
        CHECK(motion_wanted(Mode::Off, true, c, today));
        CHECK(!motion_wanted(Mode::Quality, true, Caps{}, today));   // (no NGX: nothing runs)
    }
    // result codes
    CHECK(ok(1) && !ok(0) && !ok(0xBAD00000u) && !ok(0xBAD00005u));

    // the parameter object's slots: Itanium (declaration order) and MSVC (each overload group reversed);
    // OptiScaler's measurement on the Windows driver: the float Set at slot 6
    CHECK(param_slot(false, ParamType::ULL, false) == 0 && param_slot(false, ParamType::VoidPtr, false) == 7);
    CHECK(param_slot(true, ParamType::ULL, false) == 8 && param_slot(true, ParamType::Int, false) == 12);
    CHECK(param_slot(false, ParamType::Float, true) == 6);
    CHECK(param_slot(false, ParamType::VoidPtr, true) == 0);
    CHECK(param_slot(true, ParamType::Int, true) == 11 && param_slot(true, ParamType::UInt, true) == 12);
    CHECK(param_slot(true, ParamType::ULL, true) == 15 && kParamResetSlot == 16);

    // 6./7. per-eye resources: one feature per eye, each with its own history
    {
        FakeBackend b;
        EyeFeatures<FakeBackend> f;
        f.backend = &b;
        const EyeSpec left{1368, 1395, 2736, 2790, Mode::Performance};
        const EyeSpec right = left;
        CHECK(f.prepare(0, left) && f.prepare(1, right));
        CHECK(b.made.size() == 2 && b.made[0].eye == 0 && b.made[1].eye == 1);
        CHECK(f.handle[0] != f.handle[1]);
        CHECK(f.take_reset(0) && f.take_reset(1));      // first evaluates start histories
        CHECK(!f.take_reset(0) && !f.take_reset(1));
        CHECK(f.prepare(0, left) && b.made.size() == 2);  // unchanged: kept, history continues
        CHECK(!f.take_reset(0));
        // 11. a cut resets both eyes, separately
        f.cut();
        CHECK(f.take_reset(0) && f.take_reset(1));
        // 12. a resolution change of one eye remakes only that eye
        EyeSpec bigger = left;
        bigger.inW = 1824;
        bigger.inH = 1860;
        CHECK(f.prepare(1, bigger));
        CHECK(b.made.size() == 3 && b.made[2].eye == 1 && b.released.size() == 1);
        CHECK(f.take_reset(1) && !f.take_reset(0));
        // 13. a mode change remakes
        EyeSpec quality = left;
        quality.mode = Mode::Quality;
        CHECK(f.prepare(0, quality) && b.made.size() == 4 && b.released.size() == 2);
        // Off releases
        EyeSpec off = left;
        off.mode = Mode::Off;
        CHECK(!f.prepare(0, off) && f.handle[0] == 0 && b.released.size() == 3);
        // 14./15. shutdown releases what is left
        f.release_all();
        CHECK(f.handle[0] == 0 && f.handle[1] == 0 && b.released.size() == 4);
    }
    // 5./17. a create failure: native rendering for both eyes until the settings change
    {
        FakeBackend b;
        EyeFeatures<FakeBackend> f;
        f.backend = &b;
        const EyeSpec s{1368, 1395, 2736, 2790, Mode::Dlaa};
        CHECK(f.prepare(0, s));
        b.failNext = true;
        CHECK(!f.prepare(1, s));
        CHECK(f.failed && f.handle[0] == 0 && f.handle[1] == 0);  // the made eye released too: no lone eye
        CHECK(!f.prepare(0, s) && b.made.size() == 1);            // no retry every frame
        f.settings_changed();
        CHECK(f.prepare(0, s) && f.prepare(1, s) && b.made.size() == 3);
    }
    // 16. a lost device: handles forgotten, none released through the dead device
    {
        FakeBackend b;
        EyeFeatures<FakeBackend> f;
        f.backend = &b;
        const EyeSpec s{2736, 2790, 2736, 2790, Mode::Dlaa};
        CHECK(f.prepare(0, s) && f.prepare(1, s));
        f.forget();
        CHECK(f.handle[0] == 0 && f.handle[1] == 0 && b.released.empty());
        CHECK(f.prepare(0, s) && b.made.size() == 3);  // made again on the new device
    }

    if (failures) {
        std::fprintf(stderr, "ngx_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ngx_test: ok\n");
    return 0;
}
