// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR upscaling (docs/VR_UPSCALING.md): which upscaler draws each eye's picture into the headset's image. Each eye is
// drawn at the internal resolution (on a Quest 0.75x of it, dynres.cpp fixed_factor) and was scaled to the swapchain's
// size by a linear blit. The choices, by what the device offers (runtime capability checks, not GPU models):
//   - Quest Super Resolution: the Meta compositor's own SGSR-based upscale and sharpen, asked for with
//     XR_FB_composition_layer_settings' quality sharpening on the projection layer (openxr_session.cpp). Free for the
//     app's frame; costs the compositor a little GPU. Only where the runtime offers the extension (Meta's runtime);
//   - SGSR1: Qualcomm's Snapdragon Game Super Resolution 1 (BSD-3), one fragment pass per eye in place of the blit
//     (gfx/vulkan/vr_upscale.cpp): a 12-tap Lanczos-like edge-adaptive upscale with built-in sharpening;
//   - DLSS (NVIDIA RTX on a PC, docs/VR_DLSS.md): the renderer's own DLSS 4 Super Resolution / DLAA in the scene, with
//     motion vectors; decided at the start (gfx/vulkan/ngx_vk.cpp probe: DLAA when it is offered and no "dlss" mode
//     is set), so a change to or from it applies at the next start; the eye copy is then the blit;
//   - SGSR2: Qualcomm's temporal Snapdragon Game Super Resolution 2 (BSD-3), its 2-pass fragment variant per eye with
//     its own history (gfx/vulkan/vr_sgsr2.cpp), fed by the motion pass's motion vectors and the scene depth. By choice
//     only (never auto): it needs the motion pass (a second draw of the scene's geometry); an eye without motion this
//     frame takes SGSR1;
//   - FSR 1: AMD FidelityFX Super Resolution 1 (MIT): EASU then RCAS per eye (gfx/vulkan/vr_fsr1.cpp). By choice only;
//     upscaling only (no frame generation);
//   - none: the blit, as before.
// Auto picks the best the device offers: Quest Super Resolution on a Quest (Meta's runtime), DLSS on a PC whose
// NVIDIA GPU, driver and DLSS library offer it, else SGSR1 wherever the eye picture is upscaled (the Steam Frame, a PC
// on AMD or Intel), else the blit. Never two upscalers: each sharpens. Pure logic here (vr_test checks it).
#pragma once
#include <mutex>
#include <string>

namespace vr::upscale {

// vr.upscaler: auto, off, sgsr1, superResolution, dlss, sgsr2, fsr1
enum class Mode : int { Auto = 0, Off = 1, Sgsr1 = 2, SuperResolution = 3, Dlss = 4, Sgsr2 = 5, Fsr1 = 6 };

struct Caps {
    bool superResolution = false;  // the runtime offers XR_FB_composition_layer_settings (Meta's compositor)
    bool shaderPass = false;       // the renderer can draw the SGSR1 pass into the headset's images
    bool standalone = false;       // the headset's own GPU renders (Android, or an arm64 headset such as the Frame)
    bool dlss = false;             // DLSS runs this session (an NVIDIA GPU, driver and library offered it at the start)
    bool temporal = false;         // the motion pass runs (SGSR2's motion vectors and depth)
};

struct Choice {
    bool sgsr1 = false;
    bool superResolution = false;
    bool dlss = false;  // (DLSS itself was started at the start; this says it is the upscaler in use)
    bool sgsr2 = false;  // (with sgsr1 also set: the fallback for an eye without motion this frame)
    bool fsr1 = false;
    const char* why = "";  // for the log and the settings' status line
};

// scale: the headset image's width over the eye picture's (1 = no upscale)
inline Choice choose(Mode mode, const Caps& caps, float scale) {
    Choice c;
    switch (mode) {
    case Mode::Off:
        c.why = "off (vr.upscaler)";
        return c;
    case Mode::SuperResolution:
        if (caps.superResolution) { c.superResolution = true; c.why = "Quest Super Resolution (chosen)"; }
        else c.why = "Quest Super Resolution chosen, but this runtime does not offer it: off";
        return c;
    case Mode::Sgsr1:
        if (caps.shaderPass) { c.sgsr1 = true; c.why = "SGSR1 (chosen)"; }
        else c.why = "SGSR1 chosen, but the renderer cannot draw its pass here: off";
        return c;
    case Mode::Sgsr2:
        if (caps.temporal && caps.shaderPass) {
            c.sgsr2 = c.sgsr1 = true;
            c.why = "SGSR2 (chosen; temporal, the motion pass's motion vectors)";
        } else if (caps.shaderPass) {
            c.sgsr1 = true;
            c.why = "SGSR2 chosen, but there are no motion vectors here (the motion pass needs dynamic rendering, and "
                    "starts with the game: a change applies at the next start): SGSR1";
        } else c.why = "SGSR2 chosen, but the renderer cannot draw its passes here: off";
        return c;
    case Mode::Fsr1:
        if (caps.shaderPass) { c.fsr1 = true; c.why = "FSR 1 (chosen; AMD FidelityFX EASU + RCAS, upscaling only)"; }
        else c.why = "FSR 1 chosen, but the renderer cannot draw its passes here: off";
        return c;
    case Mode::Dlss:  // (no longer offered: a saved "dlss" runs as Auto)
    case Mode::Auto:
    default:
        // the compositor's upscale first (no cost to the app's frame); DLSS where the settings file turned it on (no
        // menu offers it; never two upscalers); else SGSR1 wherever the eye picture is upscaled (the Steam Frame, a
        // PC); else the blit
        if (caps.superResolution) { c.superResolution = true; c.why = "Quest Super Resolution (auto: Meta's runtime)"; }
        else if (caps.dlss) { c.dlss = true; c.why = "DLSS (auto: turned on in the settings file)"; }
        else if (caps.shaderPass && scale > 1.05f) {
            c.sgsr1 = true;
            c.why = caps.standalone ? "SGSR1 (auto: standalone headset)" : "SGSR1 (auto: no DLSS on this PC)";
        } else c.why = "off (auto: the eye picture is not upscaled)";
        return c;
    }
}

// the choice in use and why, for the settings' status line (set by the renderer each frame it changes)
inline std::mutex& status_mutex() { static std::mutex m; return m; }
inline std::string& status_text() { static std::string s = "not in VR yet"; return s; }
inline void set_status(const std::string& s) { std::lock_guard<std::mutex> l(status_mutex()); status_text() = s; }
inline std::string status() { std::lock_guard<std::mutex> l(status_mutex()); return status_text(); }

}  // namespace vr::upscale
