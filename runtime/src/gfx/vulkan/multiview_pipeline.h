// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// A guest pipeline's multiview twin (docs/VR_MULTIVIEW.md, stage M4): the same fixed-function state and vertex input,
// view mask 0b11, and for each stage either its own SPIR-V (nothing per view) or its multiview variant's
// (multiview_glsl.h), whose per-view bindings hold two descriptors. Built on a thread of its own; until it is ready
// (or if it fails), the draws that would use it draw each eye on its own as before.
#pragma once
#include "multiview_glsl.h"
#include "pipeline_state.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace gfxvk::vk::multiview {

// what the guest pipeline was made from (kept with it while multiview is on)
struct Base {
    explicit Base(const GraphicsPipelineState& s) : state(s) {}
    GraphicsPipelineState state;
    std::array<std::vector<VkDescriptorSetLayoutBinding>, 2> descriptors;
    VkDescriptorSetLayout sets[2]{};  // the guest pipeline's own set layouts (borrowed: they live as long as it)
};
struct Stage {
    Variant variant;
    std::vector<uint32_t> perViewBindings;      // bindings with two descriptors: view 0's, view 1's
    std::shared_ptr<const std::string> glsl;    // the translated shader, rewritten when anything is per view
    std::vector<uint32_t> words;                // else its own SPIR-V
    bool per_view() const { return !perViewBindings.empty(); }
};
struct Pipeline {
    enum State { Building, Ready, Failed };
    std::atomic<int> state{Building};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sets[2]{};  // a per-view stage's own layout, else the guest pipeline's
    bool ownSet[2]{};
    std::string error;
};
// WWHD_VK_MULTIVIEW_DRAWS=1 with WWHD_VK_MULTIVIEW=1 (and dynamic rendering, and the dynamic indexing the variants need):
// a draw both eyes can share is drawn once, with view mask 0b11, into both eyes' layers of its targets (stage M2b)
bool draws_on();
// queued for the builder thread; its state says when it is ready
std::shared_ptr<Pipeline> build(std::shared_ptr<const Base> base, Stage vs, Stage ps);
// (before the device goes) the builder's current job finishes, no other starts
void stop();
// (diagnostics) pipelines built and failed so far
void counts(uint64_t& built, uint64_t& failed);

}  // namespace gfxvk::vk::multiview
