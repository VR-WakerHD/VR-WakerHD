// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The motion pass (docs/VR_DLSS.md, stage D2b 2d): per eye, after the scene, the scene's depth-writing draws drawn again
// with their motion twins (motion_pipeline.h) into a motion target (R16G16 float, the eye's depth size), the eye's
// scene depth tested EQUAL and never written. The game's own passes are not changed. The draws are recorded as packets
// while the scene is drawn (draw.cpp: any thread); the pass is recorded at the frame's end (backend.cpp swap), before
// the store turns over. On only with WWHD_VK_MOTION_PASS=1 and a DLSS feature chosen; dynamic rendering only.
#pragma once
#include <cstdint>
#include <string>

#include "backend.h"
#include "mobile_gpu.h"

namespace gfxvk::motion_pass {

struct Packet {
    int eye = 0;  // (mobile GPUs: 2, a multiview draw's both eyes in one packet: draw.cpp motion_both)
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // the vertex stage's, both frames' elements
#ifdef WWHD_MOBILE_GPU_PATHS
    uint32_t offsets[68]{};                // its dynamic offsets, by binding and element (both eyes': four a binding)
#else
    uint32_t offsets[34]{};                // its dynamic offsets, by binding and element
#endif
    uint32_t offsetCount = 0;
    struct Bind { uint32_t binding; VkBuffer buffer; VkDeviceSize offset; };
    Bind binds[32]{};                      // current frame's at their bindings, previous frame's at + 16
    uint32_t bindCount = 0;
    bool indexed = false;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    uint32_t count = 0, instances = 1, baseVertex = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    Surface* depth = nullptr;  // the scene depth it was tested against
    uint32_t depthSlice = 0;
    bool previous = false;     // it had the previous frame's data (else both frames' are the current: zero motion)
    // (docs/VR_DLSS.md, "Background motion") a draw that tests depth without writing it (the sky, the sea, effects),
    // its depth pinned to the cleared value: drawn after the geometry, in draw order, where no geometry wrote depth
    bool background = false;
    uint32_t order = 0;        // its draw's number in the frame (the background's order)
    // (diagnostics) why not: 1 no previous draw matched, 2 a cached mesh moved address, 3 a uniform block missing
    uint8_t why = 0;
};

// WWHD_VK_MOTION_PASS=1 with a DLSS feature chosen (and dynamic rendering)
bool on();
// (VR, 2026-11-03) whether anything uses the motion vectors this frame: SGSR2 in use, the runtime's frame synthesis, or
// DLSS. While nothing does, the draws build no packets and record() draws nothing (the motion store keeps going, so
// the draws still find their previous frame); from not needed to needed, the histories start again (a cut). The VR
// renderer sets it each headset frame; true otherwise (the flat game's DLSS). Any thread
void set_needed(bool needed);
bool active();  // on() and needed: the draws' packets
// (docs/VR_FRAME_SYNTHESIS.md) an eye's motion pass of this frame: its motion target (SHADER_READ_ONLY after record),
// the scene depth it was tested against, and whether the game's viewports flip y. False when the eye had none
struct EyeResult {
    bool valid = false;
    Surface* depth = nullptr;
    uint32_t slice = 0;
    Surface* motion = nullptr;
    bool flipsY = false;
    uint64_t frame = 0;
    bool reset = false;  // this frame starts new temporal histories (a view cut, a new scene: both eyes)
};
bool eye_result(int eye, EyeResult& out);
// any thread, while the scene is drawn: a draw for the motion pass
void add(const Packet& p);
// (diagnostics) time spent building packets (draw.cpp motion_packet), any thread
void note_packet_ns(uint64_t ns);
// (diagnostics) a draw left out of the pass: nothing about it changed since the previous frame (its motion is zero)
void note_still();
// (docs/VR_DLSS.md, "History resets") the frame whose swap carries this starts new temporal histories in both eyes (a
// camera cut, a recentre, a snap turn: render::note_view_cut); render thread, at that swap, before the pass is recorded
void note_cut();
// WWHD_VK_MOTION_BACKGROUND=0 leaves the background draws out (their pixels' motion zero, as before)
bool background_on();
// render thread, at the frame's end (no pass open): the motion pass of each eye with packets; the packets cleared
void record();
// (stage D3) DLSS runs (WWHD_VK_DLSS_EVAL=1): the scene's draws are jittered by this frame's sub-pixel offset (pixels
// of the target, -0.5..0.5, a Halton(2,3) sequence), added to their viewport (draw.cpp); false: no jitter
bool jitter(float& x, float& y);
// DLSS writes the TV's and eyes' scans at the frame's end (the mod HUD overlays then follow it: backend.cpp)
bool dlss_runs();
// (diagnostics, WWHD_VK_MOTION_DUMP=<frame>) that frame's motion targets written to motion_<frame>_eye<n>.png
// (|motion| scaled, red = x, green = y), and logged
std::string stats_line();

}  // namespace gfxvk::motion_pass
