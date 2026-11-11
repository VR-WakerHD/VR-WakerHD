// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// NVIDIA NGX on the game's Vulkan device (docs/VR_DLSS.md): finds the driver's NGX core at run time (no NVIDIA
// library is linked or shipped), starts it on the game's device, asks which DLSS features this GPU and driver offer,
// and keeps it started for the later stages. Nothing is evaluated yet; the native picture is unchanged. Desktop
// Windows and Linux.
#pragma once
#include <string>

#include <vulkan/vulkan.h>

#include "ngx_core.h"

namespace gfxvk::ngx_vk {

// NGX is wanted this run (WWHD_NGX=0: no; then neither the device's DLSS extensions nor the probe)
bool wanted();
// DLSS is chosen in the settings file ("dlss" a mode, or "dlss5=1"; no menu offers it) and NGX is wanted, or
// WWHD_NGX=1: only then does the game start NVIDIA's NGX (the device's DLSS extensions, the probe), so NVIDIA's DLSS
// libraries next to the game are never looked at otherwise. Read once
bool chosen();
// a DLSS feature is chosen in the settings ("dlss" not off, or "dlss5") and, once the probe has run, can run on this
// device: the motion-vector work may run (ngx_core.h motion_wanted). Any thread; read once
bool motion_wanted();
// render thread, once the device exists (backend.cpp): the probe
void probe();
// --dlss-probe (main.cpp): the probe on a device of its own, for the VR launcher's Detect PC: writes dlss-status.ini
// and prints "dlss-probe: <status>" (or "dlss-probe: error=<why>"); returns the exit code
int probe_main();
// ---- DLSS Super Resolution / DLAA run (docs/VR_DLSS.md, stage D4)
struct DlssImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};
struct DlssEval {
    int eye = 0;
    ngx::Mode mode = ngx::Mode::Dlaa;
    DlssImage color, depth, motion, output;  // inputs read (SHADER_READ_ONLY / DEPTH_STENCIL_READ_ONLY); output GENERAL
    float jitterX = 0, jitterY = 0;          // pixels of the render size
    float mvScaleX = 1, mvScaleY = 1;        // the motion target's units to pixels of the render size
    bool reset = false, mvJittered = false;
};
// in a recorder action on the real command buffer (DeferredCommands::emit_owned): the eye's feature made when its sizes
// or mode changed, then evaluated into e.output. False (each kind of failure logged once) if it could not
bool evaluate(VkCommandBuffer cmd, const DlssEval& e);
// the run is possible: the core started on the game's device and Super Resolution is available
bool can_evaluate();
// ---- DLSS 5 Neural Rendering (NGX feature 18; docs/VR_DLSS.md, stage D5)
struct NrEval {
    int eye = 0;
    DlssImage color, depth, motion, output;  // all the render size; inputs read, output GENERAL
    float mvScaleX = 1, mvScaleY = 1;        // the motion target's units to pixels
    bool reset = false;
    ngx::NrSettings settings;
};
// DLSS 5 is chosen ("dlss5") and has not failed this run: the motion pass runs it before Super Resolution (with DLAA when
// no DLSS mode is chosen). Any thread after probe
bool neural_wanted();
// in a recorder action on the real command buffer, as evaluate: the eye's feature 18 made when its size changed, then
// evaluated into e.output. False if it could not (logged; a failed make turns DLSS 5 off for the run)
bool evaluate_nr(VkCommandBuffer cmd, const NrEval& e);
// the settings' DLSS 5 values (dlss5.*), clamped
ngx::NrSettings nr_settings();
// (docs/VR_DLSS.md, "Live DLSS 5") the player's DLSS 5 switch and settings while playing (F1 > Graphics). They start as
// the settings file's and apply from the next frame; any thread
struct NrLive {
    bool available = false;  // the driver serves DLSS 5 on this device
    bool runs = false;       // DLSS runs this session (the motion pass was started with it): DLSS 5 can be switched
    bool on = false;         // the player's switch
    bool failed = false;     // the feature could not be made (logged); switching it on again tries once more
    ngx::NrSettings settings;
};
NrLive nr_live();
void set_nr_live(bool on, const ngx::NrSettings& settings);
// the settings DLSS 5 is evaluated with this frame (the live ones)
ngx::NrSettings nr_current();
// the DLSS mode that runs: the settings' choice when it is offered and the run is possible, else Off
ngx::Mode active_mode();
// what the probe found (any thread after probe)
const ngx::Caps& caps();
// "NVIDIA DLSS: ..." for the log, the performance report and the settings
std::string status();

}  // namespace gfxvk::ngx_vk
