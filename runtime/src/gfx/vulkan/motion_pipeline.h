// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// A guest pipeline's motion twin (docs/VR_DLSS.md, stage D2b 2c): its pipeline in the motion pass, which draws the
// scene's depth-writing draws again after the scene, into a motion target only, testing the scene's depth EQUAL
// (GraphicsPipelineState::make_motion_only), with the previous frame's vertex inputs at location and binding + 16
// (add_motion_inputs). Its vertex shader is the guest's motion variant (motion_glsl.h: run on the previous frame's
// data, then the current frame's, which gives the scene draw's own depth: the translator declares gl_Position
// invariant), its vertex-stage uniform bindings with two descriptors (current, previous frame); its pixel shader the
// motion pass's own (motion_only_pixel). The game's passes are never changed. Built on a thread of its own like the
// multiview twins (multiview_pipeline.h); until it is ready, or for a pipeline without depth, the draw has no motion.
#pragma once
#include "mobile_gpu.h"
#include "motion_glsl.h"
#include "multiview_pipeline.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace gfxvk::vk::motion {

constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;

struct Stage {
    Variant variant;                          // (vertex; the pixel stage is the motion pass's own)
    std::vector<uint32_t> perFrameBindings;   // vertex: bindings with two descriptors, current and previous frame
    std::shared_ptr<const std::string> glsl;  // the translated shader
};
struct Pipeline {
    enum State { Building, Ready, Failed };
    std::atomic<int> state{Building};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sets[2]{};  // [0]: the vertex stage's own layout (the motion pass has no pixel-stage set)
    uint32_t motionLocation = 0;      // the motion target's colour attachment (the motion pass's only one)
    bool noDepth = false;             // the guest pipeline has no depth: nothing for the motion pass
    std::string error;
#ifdef WWHD_MOBILE_GPU_PATHS
    // (mobile GPUs: docs/VR_DLSS.md, "Both eyes in one motion pass") the same for both eyes in one draw: view mask 0b11,
    // the vertex stage's per-frame bindings with four descriptors (current eye 0, current eye 1, previous eye 0,
    // previous eye 1: motion_glsl.h Variant.views 2). Null where it could not be made: the eyes' own passes then
    VkPipeline pipeline2 = VK_NULL_HANDLE;
    VkPipelineLayout layout2 = VK_NULL_HANDLE;
    VkDescriptorSetLayout sets2[1]{};
#endif
};
// queued for the builder thread; its state says when it is ready. base: what the guest pipeline was made from (the
// multiview twins' Base: state, descriptor bindings, set layouts)
std::shared_ptr<Pipeline> build(std::shared_ptr<const multiview::Base> base, Stage vs, Stage ps);
// (before the device goes) the builder's current job finishes, no other starts
void stop();
// (diagnostics) pipelines built and failed so far
void counts(uint64_t& built, uint64_t& failed);

}  // namespace gfxvk::vk::motion
