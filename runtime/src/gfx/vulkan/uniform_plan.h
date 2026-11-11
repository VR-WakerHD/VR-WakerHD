// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Which of a pipeline's uniform blocks are dynamic uniform buffers (QUEST3_IMPLEMENTATION_ROADMAP.md, 2.1; mobile GPUs
// only: mobile_gpu.h). Vulkan caps a pipeline layout's dynamic uniform buffers (maxDescriptorSetUniformBuffersDynamic);
// Adreno reports 8 where NVIDIA reports 15, and the game's shaders bind up to 16 blocks and a support block a stage.
// Everywhere else a pipeline over the cap binds every block as a plain uniform buffer; then eye 1 cannot replay eye 0's
// draw with its own copy of the eye-shifted blocks at another dynamic offset, and the draw cannot be a multiview draw
// (draw.cpp kMvNotDynamic): on a Quest log 3,192 of about 3,400 draws a frame were drawn per eye.
//
// Here a pipeline over the cap makes some of its blocks dynamic instead of none: the vertex stage's first (the support
// block, then the blocks in slot order: the camera's projection, the rows eye 1 shifts, is a vertex-stage block), then
// the pixel stage's, up to the cap less kMultiviewReserve (a multiview twin doubles each per-view block's descriptor:
// multiview_pipeline.cpp checks the twin's own total). The rest stay plain buffers. A draw whose eye-shifted blocks all
// landed on dynamic ones replays and draws multiview as on any other GPU (draw.cpp: replay needs each eye-shifted
// block's dynamic offset, else the draw is per eye as before). Pure logic: no device state.
//
// Also without multiview: eye 1 then replays such a draw (its own copy of the eye-shifted blocks at another offset)
// instead of resolving and binding it again. WWHD_VK_PARTIAL_DYNAMIC=0 turns it off (every block plain, as before).
//
// WWHD_VK_TEST_DYNAMIC_UBO_LIMIT=n (a test build with WWHD_TEST_MOBILE_GPU_PATHS, or a headset): the cap taken as at most
// n, so a desktop GPU makes the Quest's choices (n = 8: Adreno's)
#pragma once
#include "mobile_gpu.h"
#ifdef WWHD_MOBILE_GPU_PATHS
#include <cstdint>
#include <cstdlib>

namespace gfxvk::uniform_plan {

constexpr uint32_t kSupportSlot = 16;      // the support block's logical slot (bind_stage's logicalSlot 16)
constexpr uint32_t kMultiviewReserve = 2;  // room left for a multiview twin's per-view copies of the blocks
constexpr uint32_t kAllSlots = 0x1FFFFu;   // blocks 0..15 and the support block

// the cap: the device's, or a lower test cap
inline uint32_t dynamic_limit(uint32_t device) {
    static const long test = [] {
        const char* e = std::getenv("WWHD_VK_TEST_DYNAMIC_UBO_LIMIT");
        return e && *e ? std::strtol(e, nullptr, 10) : -1L;
    }();
    return test >= 0 && uint32_t(test) < device ? uint32_t(test) : device;
}

struct Plan {
    bool all = false;           // every block dynamic (the layout fits the cap): as on every other GPU
    uint32_t mask[2] = {0, 0};  // per stage (0 vertex, 1 pixel): the logical slots bound as dynamic buffers
    bool partial() const { return !all && (mask[0] | mask[1]); }
    bool dynamic(int stage, uint32_t slot) const { return all || ((mask[stage] >> slot) & 1u); }
};

// Mapping: the shaders' resource mapping (uniformVarsBufferBindingPoint, uniformBuffersBindingPoint[16])
template <class Mapping>
inline Plan plan(const Mapping& vs, const Mapping& ps, uint32_t limit) {
    const Mapping* stages[2] = {&vs, &ps};
    uint32_t total = 0;
    for (const Mapping* m : stages) {
        total += m->uniformVarsBufferBindingPoint >= 0;
        for (int b : m->uniformBuffersBindingPoint) total += b >= 0;
    }
    Plan p;
    if (total <= limit) {
        p.all = true;
        p.mask[0] = p.mask[1] = kAllSlots;
        return p;
    }
    // WWHD_VK_PARTIAL_DYNAMIC=0: every block of a pipeline over the cap plain, as before
    static const bool partialOn = [] {
        const char* e = std::getenv("WWHD_VK_PARTIAL_DYNAMIC");
        return !(e && e[0] == '0');
    }();
    if (!partialOn) return p;
    uint32_t budget = limit > kMultiviewReserve ? limit - kMultiviewReserve : 0;
    for (int st = 0; st < 2 && budget; ++st) {
        const Mapping& m = *stages[st];
        if (m.uniformVarsBufferBindingPoint >= 0 && budget) p.mask[st] |= 1u << kSupportSlot, --budget;
        for (uint32_t slot = 0; slot < 16 && budget; ++slot)
            if (m.uniformBuffersBindingPoint[slot] >= 0) p.mask[st] |= 1u << slot, --budget;
    }
    return p;
}

}  // namespace gfxvk::uniform_plan
#endif
