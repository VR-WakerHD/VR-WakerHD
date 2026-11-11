// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// NVIDIA NGX (DLSS) decisions without NGX or Vulkan (docs/VR_DLSS.md): which DLSS choices may be shown, the
// internal resolution of each, the status line, the per-eye feature set and the parameter object's virtual slots.
// The runtime (ngx_vk.cpp) fills Caps from the driver; tests drive everything here with a fake backend.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ngx {

// DLSS Super Resolution's quality modes, and DLAA (Super Resolution at 1:1)
enum class Mode : uint8_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance };
constexpr Mode kModes[] = {Mode::Off, Mode::Dlaa, Mode::Quality, Mode::Balanced, Mode::Performance, Mode::UltraPerformance};

inline const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Off: return "Off";
        case Mode::Dlaa: return "DLAA";
        case Mode::Quality: return "DLSS Quality";
        case Mode::Balanced: return "DLSS Balanced";
        case Mode::Performance: return "DLSS Performance";
        case Mode::UltraPerformance: return "DLSS Ultra Performance";
    }
    return "?";
}

// the internal resolution a mode draws at in this renderer (docs/VR_DLSS.md, stage D4): the dynamic resolution's
// levels (dynres_core.h kLevels) nearest NVIDIA's ratios, kept apart so each offered mode differs; Ultra Performance
// (a third) has no level of its own (the lowest is a half), so it is not offered
inline float render_factor(Mode m) {
    switch (m) {
        case Mode::Quality: return 0.75f;
        case Mode::Balanced: return 0.625f;
        case Mode::Performance: return 0.5f;
        default: return 1.0f;
    }
}
inline bool upscaling_mode(Mode m) { return m == Mode::Quality || m == Mode::Balanced || m == Mode::Performance; }
// the internal resolution as a share of the output, per axis (NVIDIA's published ratios: the DLSS programming guide)
inline float render_scale(Mode m) {
    switch (m) {
        case Mode::Off: case Mode::Dlaa: return 1.0f;
        case Mode::Quality: return 2.0f / 3.0f;
        case Mode::Balanced: return 0.58f;
        case Mode::Performance: return 0.5f;
        case Mode::UltraPerformance: return 1.0f / 3.0f;
    }
    return 1.0f;
}

// NVIDIA's own value for each choice, given to the feature's create as "PerfQualityValue" (NVSDK_NGX_PerfQuality_Value,
// nvsdk_ngx_defs.h of SDK 310.9.16: MaxPerf 0, Balanced 1, MaxQuality 2, UltraPerformance 3, UltraQuality 4, DLAA 5;
// Ultra Quality is not offered: the driver gives it no resolution). Off: -1, no feature
inline int perf_quality_value(Mode m) {
    switch (m) {
        case Mode::Off: return -1;
        case Mode::Performance: return 0;
        case Mode::Balanced: return 1;
        case Mode::Quality: return 2;
        case Mode::UltraPerformance: return 3;
        case Mode::Dlaa: return 5;
    }
    return -1;
}

// the model preset NVIDIA makes the default for each choice (NVSDK_NGX_DLSS_Hint_Render_Preset, the same SDK: K for
// DLAA, Quality and Balanced, M for Performance, L for Ultra Performance); 0 for Off
inline int default_render_preset(Mode m) {
    switch (m) {
        case Mode::Off: return 0;
        case Mode::Dlaa: case Mode::Quality: case Mode::Balanced: return 11;  // K
        case Mode::Performance: return 13;                                     // M
        case Mode::UltraPerformance: return 12;                                // L
    }
    return 0;
}

// one NGX feature's answer to the capability query (NVSDK_NGX_VULKAN_GetCapabilityParameters): "<Name>.Available",
// ".NeedsUpdatedDriver", ".MinDriverVersionMajor/Minor", ".FeatureInitResult"
struct FeatureCaps {
    bool queried = false;      // the driver answered for this feature
    bool available = false;
    bool needsDriver = false;  // a newer driver is needed
    int minMajor = 0, minMinor = 0;
    uint32_t initResult = 0;   // NVSDK_NGX_Result of the feature's own initialisation (0: not reported)
};

// what the probe found (ngx_vk.cpp), once per device
struct Caps {
    bool built = false;        // this build has the NGX module (desktop Windows and Linux)
    bool nvidia = false;       // the chosen GPU is NVIDIA's
    bool coreLoaded = false;   // the driver's NGX core library was found and loaded
    bool initialized = false;  // NVSDK_NGX_VULKAN_Init* succeeded on the game's device
    uint32_t initResult = 0;
    std::string gpu, driver, failure;  // failure: why it stopped, empty if it did not
    FeatureCaps superResolution;       // DLSS Super Resolution and DLAA (NGX feature 1)
    FeatureCaps neuralRendering;       // DLSS 5 Neural Rendering ("DLSSNR", NGX feature 18: reserved in the public SDK)
};

// the inputs the renderer gives each eye today; DLSS needs all three for a correct picture
struct Inputs {
    bool depth = false;          // each eye's depth at the internal resolution
    bool motionVectors = false;  // per-pixel motion of everything drawn (not the camera's alone)
    bool jitter = false;         // a sub-pixel projection offset per frame, known to the evaluate call
    bool upscaling = false;      // a lower internal resolution, DLSS's output shown at the eye's full size
};
// what the renderer provides now (docs/VR_DLSS.md): depth, the motion pass's motion vectors (D2), the viewport jitter
// (D3), and upscaling (D4: the scene drawn at the mode's internal resolution, DLSS writing the full-size picture)
inline Inputs renderer_inputs() { return Inputs{true, true, true, true}; }

inline bool ok(uint32_t result) { return (result & 0xFFF00000u) != 0xBAD00000u && result != 0; }

// the choices the settings may show: Off always; the others only when the driver has the feature and the renderer
// gives what it needs (an option that would draw ghosts or fail is not shown)
inline std::vector<Mode> exposed_modes(const Caps& c, const Inputs& in) {
    std::vector<Mode> out{Mode::Off};
    const bool sr = c.initialized && c.superResolution.available && in.depth && in.motionVectors && in.jitter;
    if (!sr) return out;
    for (Mode m : kModes)
        if (m == Mode::Dlaa || (in.upscaling && upscaling_mode(m))) out.push_back(m);
    return out;
}
// DLSS 5 Neural Rendering: a separate toggle, never one of the modes. Shown only when the driver reports the
// feature available and the renderer has its inputs; its API is not public (docs/VR_DLSS.md), so no build creates it
inline bool neural_rendering_exposed(const Caps& c, const Inputs& in) {
    return c.initialized && c.neuralRendering.available && in.depth && in.motionVectors;
}

// motion vectors are made (and their diagnostics run) only while a DLSS feature is chosen: Super Resolution, DLAA and
// DLSS 5 all need them; with both choices off nothing of it runs, whatever the test switches say
inline bool motion_wanted(Mode chosen, bool neuralChosen) { return chosen != Mode::Off || neuralChosen; }

// DLSS 5 Neural Rendering's own settings, not Super Resolution's modes: NVIDIA's, as its Omniverse renderer exposes
// them (the OmniRtxPostDlssNrAPI_1 schema, "DLSS 5 3D-Guided Neural Rendering"; no public NGX API names them).
// NR runs at the output resolution. settings.ini keys "dlss5.<name>" (kNrKeys)
struct NrSettings {
    float intensity = 1;       // overall strength of the effect, 0..1
    float globalTone = 0;      // tone of the whole picture, 0..1
    float localTone = 0;       // spatially adaptive tone, 0..1
    float localStructure = 0;  // local detail and structure, 0..1
    float skinStructure = 0;   // structure of skin, 0..1
    bool autoMask = false;     // the model's own mask of where it acts
    int style = 0;             // 0..2
    int preset = 0;            // NVIDIA's DLSS 5 presets: 0 the SDK's default, 1..7 presets A..G
    int iterations = 1;        // evaluations chained, 1..4 (experimental in NVIDIA's renderer)
    bool operator==(const NrSettings&) const = default;
};
constexpr int kNrStyles = 3, kNrPresets = 8, kNrMaxIterations = 4;
// NVIDIA numbers the styles 0..2 only; these names are the ones players report the games showing for them
inline const char* nr_style_name(int s) {
    switch (s) {
        case 0: return "Default";
        case 1: return "Natural";
        case 2: return "Cinematic";
    }
    return "?";
}
inline NrSettings clamped(NrSettings n) {
    auto unit = [](float& v) { v = v < 0 ? 0 : v > 1 ? 1 : v; };
    unit(n.intensity); unit(n.globalTone); unit(n.localTone); unit(n.localStructure); unit(n.skinStructure);
    n.style = n.style < 0 ? 0 : n.style >= kNrStyles ? kNrStyles - 1 : n.style;
    n.preset = n.preset < 0 ? 0 : n.preset >= kNrPresets ? kNrPresets - 1 : n.preset;
    n.iterations = n.iterations < 1 ? 1 : n.iterations > kNrMaxIterations ? kNrMaxIterations : n.iterations;
    return n;
}
// the settings' keys and values, in one order (reading and writing share it)
inline std::vector<std::pair<std::string, std::string>> nr_to_kv(const NrSettings& n) {
    auto f = [](float v) { char b[32]; snprintf(b, sizeof b, "%g", double(v)); return std::string(b); };
    return {{"dlss5.intensity", f(n.intensity)}, {"dlss5.globalTone", f(n.globalTone)},
            {"dlss5.localTone", f(n.localTone)}, {"dlss5.localStructure", f(n.localStructure)},
            {"dlss5.skinStructure", f(n.skinStructure)}, {"dlss5.autoMask", n.autoMask ? "1" : "0"},
            {"dlss5.style", std::to_string(n.style)}, {"dlss5.preset", std::to_string(n.preset)},
            {"dlss5.iterations", std::to_string(n.iterations)}};
}
// from the settings: `get(key, value)` returns whether the key is there; missing or unreadable values keep defaults
template <class Get>
inline NrSettings nr_from_kv(Get get) {
    NrSettings n;
    std::string v;
    auto num = [&](const char* k, float& out) {
        if (!get(k, v)) return;
        char* end = nullptr;
        const float x = strtof(v.c_str(), &end);
        if (end != v.c_str()) out = x;
    };
    auto whole = [&](const char* k, int& out) {
        float x = float(out);
        num(k, x);
        out = int(x);
    };
    num("dlss5.intensity", n.intensity);
    num("dlss5.globalTone", n.globalTone);
    num("dlss5.localTone", n.localTone);
    num("dlss5.localStructure", n.localStructure);
    num("dlss5.skinStructure", n.skinStructure);
    if (get("dlss5.autoMask", v)) n.autoMask = v == "1";
    whole("dlss5.style", n.style);
    whole("dlss5.preset", n.preset);
    whole("dlss5.iterations", n.iterations);
    return clamped(n);
}

// a mode the settings asked for, checked against what is offered: anything not offered runs as Off
inline Mode effective_mode(Mode wanted, const Caps& c, const Inputs& in) {
    for (Mode m : exposed_modes(c, in))
        if (m == wanted) return m;
    return Mode::Off;
}
// after the probe: motion vectors only for a chosen feature that can run here (a choice the driver does not offer, such
// as DLSS 5 without its library, runs as Off: its motion work would cost every scene draw for nothing)
inline bool motion_wanted(Mode chosen, bool neuralChosen, const Caps& c, const Inputs& in) {
    return effective_mode(chosen, c, in) != Mode::Off || (neuralChosen && neural_rendering_exposed(c, in));
}

// library: what to say when the feature's own NVIDIA library, which NGX looks for next to the game, is not there
// (0xBAD00004, FeatureNotFound: the driver ships only NGX's core)
inline std::string feature_line(const char* name, const FeatureCaps& f, const char* library = nullptr) {
    char b[400];
    if (!f.queried) {
        snprintf(b, sizeof b, "%s: not reported by the driver", name);
    } else if (f.available) {
        snprintf(b, sizeof b, "%s: available", name);
    } else if (f.needsDriver) {
        snprintf(b, sizeof b, "%s: needs driver %d.%d or newer", name, f.minMajor, f.minMinor);
    } else if (f.initResult == 0xBAD00004u) {
        snprintf(b, sizeof b, "%s: not available: %s", name, library ? library : "its NVIDIA library is not next to the game");
    } else if (f.initResult && !ok(f.initResult)) {
        snprintf(b, sizeof b, "%s: not available (0x%08X)", name, unsigned(f.initResult));
    } else {
        snprintf(b, sizeof b, "%s: not available", name);
    }
    return b;
}

// what is missing on the renderer's side for Super Resolution, empty if nothing
inline std::string missing_inputs(const Inputs& in) {
    std::string s;
    auto add = [&](const char* what) { s += s.empty() ? what : std::string(", ") + what; };
    if (!in.depth) add("depth");
    if (!in.motionVectors) add("motion vectors");
    if (!in.jitter) add("projection jitter");
    if (!in.upscaling) add("upscaling (DLAA only)");
    return s;
}

// the status for the log, the performance report and the settings ("NVIDIA DLSS: ..."); `wanted`: the settings' choice
inline std::string status(const Caps& c, const Inputs& in, Mode wanted = Mode::Off, bool neuralWanted = false) {
    if (!c.built) return "NVIDIA DLSS: not in this build";
    if (!c.nvidia) return "NVIDIA DLSS: not available (" + (c.gpu.empty() ? std::string("no NVIDIA GPU") : c.gpu + " is not NVIDIA's") + "; DLSS needs an NVIDIA RTX GPU)";
    std::string s = "NVIDIA DLSS on " + c.gpu + (c.driver.empty() ? "" : " (driver " + c.driver + ")") + ": ";
    if (!c.coreLoaded || !c.initialized) return s + "not available (" + (c.failure.empty() ? "NGX did not start" : c.failure) + ")";
#ifdef _WIN32
    const char* srLibrary = "nvngx_dlss.dll is not next to the game (copy it from NVIDIA's DLSS SDK, or a game that ships it)";
#else
    const char* srLibrary = "libnvidia-ngx-dlss.so.310.x is not next to the game (copy it from NVIDIA's DLSS SDK)";
#endif
    s += feature_line("Super Resolution / DLAA", c.superResolution, srLibrary) + "; " +
         feature_line("DLSS 5 Neural Rendering", c.neuralRendering, "NVIDIA publishes no library for it yet");
    const std::string missing = missing_inputs(in);
    if (!missing.empty()) s += "; not offered yet: the renderer has no " + missing;
    // the chosen mode runs when it is offered (docs/VR_DLSS.md, stage D4); anything else runs as Off
    const Mode active = effective_mode(wanted, c, in);
    s += "; active: " + std::string(mode_name(active));
    if (wanted != active) s += std::string(" (chosen: ") + mode_name(wanted) + ")";
    if (neuralWanted) s += neural_rendering_exposed(c, in) ? "; DLSS 5 chosen" : "; DLSS 5 chosen, not available";
    return s;
}

// ---- the settings and the launcher (tools/vr_launcher): "dlss" = a mode's key, "dlss5" = 0/1 in settings.ini; the
// game writes what its last probe found to dlss-status.ini next to it, which the launcher reads (it has no device)
inline const char* mode_key(Mode m) {
    switch (m) {
        case Mode::Off: return "off";
        case Mode::Dlaa: return "dlaa";
        case Mode::Quality: return "quality";
        case Mode::Balanced: return "balanced";
        case Mode::Performance: return "performance";
        case Mode::UltraPerformance: return "ultra";
    }
    return "off";
}
inline Mode parse_mode(const std::string& s) {
    for (Mode m : kModes)
        if (s == mode_key(m)) return m;
    return Mode::Off;
}
constexpr const char* kStatusFileName = "dlss-status.ini";

// the status file's text: one key=value a line
inline std::string status_file_text(const Caps& c, const Inputs& in) {
    std::string modes;
    for (Mode m : exposed_modes(c, in)) modes += std::string(modes.empty() ? "" : ",") + mode_key(m);
    std::string s;
    s += "gpu=" + c.gpu + "\n";
    s += "driver=" + c.driver + "\n";
    s += std::string("superResolution=") + (c.initialized && c.superResolution.available ? "1" : "0") + "\n";
    s += std::string("neuralRendering=") + (c.initialized && c.neuralRendering.available ? "1" : "0") + "\n";
    s += "offered=" + modes + "\n";
    s += std::string("neuralOffered=") + (neural_rendering_exposed(c, in) ? "1" : "0") + "\n";
    std::string st = status(c, in);
    for (char& ch : st)
        if (ch == '\n' || ch == '\r') ch = ' ';
    s += "status=" + st + "\n";
    return s;
}
struct StatusFile {
    bool present = false;
    std::string gpu, driver, status;
    bool superResolution = false, neuralRendering = false, neuralOffered = false;
    std::vector<Mode> offered;  // always holds Off
    bool offers(Mode m) const {
        for (Mode o : offered)
            if (o == m) return true;
        return m == Mode::Off;
    }
};
inline StatusFile parse_status_file(const std::string& text) {
    StatusFile f;
    f.offered = {Mode::Off};
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        f.present = true;
        if (k == "gpu") f.gpu = v;
        else if (k == "driver") f.driver = v;
        else if (k == "status") f.status = v;
        else if (k == "superResolution") f.superResolution = v == "1";
        else if (k == "neuralRendering") f.neuralRendering = v == "1";
        else if (k == "neuralOffered") f.neuralOffered = v == "1";
        else if (k == "offered") {
            size_t p = 0;
            while (p <= v.size()) {
                size_t comma = v.find(',', p);
                if (comma == std::string::npos) comma = v.size();
                const std::string name = v.substr(p, comma - p);
                const Mode m = parse_mode(name);
                if (m != Mode::Off && name == mode_key(m)) f.offered.push_back(m);
                p = comma + 1;
            }
        }
    }
    return f;
}

// ---- NVSDK_NGX_Parameter's virtual functions
// The driver's parameter object is a C++ class with overloaded pure virtual Set/Get (nvsdk_ngx_params.h, in
// declaration order: unsigned long long, float, double, unsigned int, int, ID3D11Resource*, ID3D12Resource*, void*;
// then Reset). On Windows the driver is built with MSVC, which groups overloads and lists each group in reverse
// declaration order; this build (clang, Itanium ABI) would call the wrong slot through the class, so it calls by
// slot. On Linux both sides use the Itanium order (declaration order).
enum class ParamType : uint8_t { ULL, Float, Double, UInt, Int, D3D11, D3D12, VoidPtr };
inline int param_slot(bool get, ParamType t, bool msvcLayout) {
    const int i = int(t);  // declaration index within the Set or Get group (8 each)
    const int inGroup = msvcLayout ? 7 - i : i;
    return (get ? 8 : 0) + inGroup;
}
constexpr int kParamResetSlot = 16;

// ---- per-eye features
// Each eye has its own NGX feature (its own temporal history): the eyes are never one image, never share history.
// A feature is made again when its eye's input or output size or the mode changes; all are released at shutdown,
// at a lost device and when DLSS is turned off. Backend: create(eye, inW, inH, outW, outH, mode) -> handle (0 =
// failed), release(handle).
struct EyeSpec {
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    Mode mode = Mode::Off;
    bool operator==(const EyeSpec& o) const {
        return inW == o.inW && inH == o.inH && outW == o.outW && outH == o.outH && mode == o.mode;
    }
};

template <class Backend>
struct EyeFeatures {
    Backend* backend = nullptr;
    uint64_t handle[2] = {};
    EyeSpec spec[2];
    bool resetHistory[2] = {};  // the next evaluate of that eye starts a new history
    bool failed = false;        // a create failed: native rendering until settings change or a new device

    // before an eye's evaluate: its feature for `want`, made (again) if needed; false = draw natively this frame
    bool prepare(int eye, const EyeSpec& want) {
        if (want.mode == Mode::Off || failed) {
            release_eye(eye);
            return false;
        }
        if (handle[eye] && spec[eye] == want) return true;
        release_eye(eye);
        handle[eye] = backend->create(eye, want.inW, want.inH, want.outW, want.outH, want.mode);
        if (!handle[eye]) {
            failed = true;
            release_all();
            return false;
        }
        spec[eye] = want;
        resetHistory[eye] = true;
        return true;
    }
    // a camera cut, a stage change, a teleport: both eyes start new histories (the eyes stay independent)
    void cut() { resetHistory[0] = resetHistory[1] = true; }
    // the evaluate of `eye` is recorded: returns whether it had to reset its history
    bool take_reset(int eye) {
        const bool r = resetHistory[eye];
        resetHistory[eye] = false;
        return r;
    }
    void release_eye(int eye) {
        if (handle[eye]) backend->release(handle[eye]);
        handle[eye] = 0;
        spec[eye] = EyeSpec{};
        resetHistory[eye] = false;
    }
    void release_all() {
        release_eye(0);
        release_eye(1);
    }
    // settings changed (the player chose again): a failed create may be tried once more
    void settings_changed() { failed = false; }
    // the device was lost or NGX shut down: handles are gone with it, nothing is released through the backend
    void forget() {
        handle[0] = handle[1] = 0;
        spec[0] = spec[1] = EyeSpec{};
        resetHistory[0] = resetHistory[1] = false;
    }
};

}  // namespace ngx
