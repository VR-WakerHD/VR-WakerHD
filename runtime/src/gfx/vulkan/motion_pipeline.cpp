// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A guest pipeline's motion twin: motion_pipeline.h.
#include "motion_pipeline.h"

#include "backend.h"
#include "runtime.h"
#include "shader_compiler.h"
#include "shaders.h"
#include "spirv_precision.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace gfxvk::vk::motion {
namespace {

struct Job {
    std::shared_ptr<const multiview::Base> base;
    Stage stages[2];
    std::shared_ptr<Pipeline> out;
};
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_jobs;
std::thread g_thread;
bool g_stopping = false;
std::atomic<uint64_t> g_built{0}, g_failed{0}, g_noDepth{0};

// the motion pass's pixel shader, compiled once (the same for every pipeline)
const std::vector<uint32_t>& motion_pixel_words(std::string& error) {
    static std::mutex m;
    static std::vector<uint32_t> words;
    static std::string failed;
    std::lock_guard lock(m);
    if (words.empty() && failed.empty()) {
        auto compiled = compile_shader(motion_only_pixel(), ShaderStage::Fragment, ~uint64_t{0}, 0, {}, false);
        words = std::move(compiled.words);
        if (words.empty()) failed = compiled.error.empty() ? "an empty compile" : compiled.error;
    }
    if (words.empty()) error = "the motion pixel shader: " + failed;
    return words;
}

// views 1: the motion twin (pipeline, layout, sets); views 2 (mobile GPUs): its both-eyes form (pipeline2, ...)
bool make_views(Job& job, uint32_t views, std::string& error, VkPipeline& pipelineOut, VkPipelineLayout& layoutOut,
                VkDescriptorSetLayout& setOut);
bool make(Job& job, std::string& error) {
    Pipeline& out = *job.out;
    if (!make_views(job, 1, error, out.pipeline, out.layout, out.sets[0])) return false;
#ifdef WWHD_MOBILE_GPU_PATHS
    // (a failure here only keeps the eyes' own passes for this pipeline's draws)
    std::string error2;
    if (!make_views(job, 2, error2, out.pipeline2, out.layout2, out.sets2[0])) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) LOG("[vulkan motion] a both-eyes motion pipeline was not made: %s", error2.c_str());
    }
#endif
    return true;
}
bool make_views(Job& job, uint32_t views, std::string& error, VkPipeline& pipelineOut, VkPipelineLayout& layoutOut,
                VkDescriptorSetLayout& setOut) {
    Pipeline& out = *job.out;
    GraphicsPipelineState state = job.base->state;
    // the motion pass's: the motion target only, the scene's depth tested equal (pipeline_state.h make_motion_only)
    try {
        state.make_motion_only(kMotionFormat);
    } catch (const std::runtime_error& e) {
        error = e.what();
        out.noDepth = true;
        return false;
    }
    state.add_motion_inputs(kPrevAttributeOffset, R.properties.limits.maxVertexInputAttributes,
                            R.properties.limits.maxVertexInputBindings);
    if (views == 1) out.motionLocation = 0;
    // the vertex stage's per-frame bindings hold two descriptors (current, previous frame); its dynamic uniform
    // buffers must fit the device's limit (the motion pass has no pixel-stage set)
    auto vertexBindings = job.base->descriptors[0];
    for (uint32_t b : job.stages[0].perFrameBindings) {
        bool found = false;
        for (auto& binding : vertexBindings)
            if (binding.binding == b) binding.descriptorCount = 2 * views, found = true;
        if (!found) {
            error = "a per-frame binding the pipeline does not have";
            return false;
        }
    }
    uint32_t dynamic = 0;
    for (const auto& b : vertexBindings)
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) dynamic += b.descriptorCount;
    if (dynamic > R.properties.limits.maxDescriptorSetUniformBuffersDynamic) {
        error = "over the device's dynamic uniform buffers (" + std::to_string(dynamic) + ")";
        return false;
    }
    VkShaderModule modules[2]{};
    struct Modules {
        VkShaderModule* m;
        ~Modules() { for (int i = 0; i < 2; ++i) if (m[i]) vkDestroyShaderModule(R.device, m[i], nullptr); }
    } cleanup{modules};
    for (int st = 0; st < 2; ++st) {
        std::vector<uint32_t> words;
        if (st == 0) {
            std::string source;
            Variant variant = job.stages[0].variant;
            variant.views = views;
            if (!rewrite(*job.stages[0].glsl, variant, source, &error)) return false;
            // compiled on the builder's thread, never in the game's shader compiler's queue (as the multiview twins)
            auto compiled = compile_shader(source, ShaderStage::Vertex, ~uint64_t{0}, 0, {}, false);
            words = std::move(compiled.words);
            if (words.empty()) {
                error = compiled.error.empty() ? "an empty compile" : compiled.error;
                return false;
            }
            if (full_precision_enabled()) strip_relaxed_precision(words);
        } else {
            words = motion_pixel_words(error);
            if (words.empty()) return false;
        }
        VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mc.codeSize = words.size() * sizeof(uint32_t);
        mc.pCode = words.data();
        if (vkCreateShaderModule(R.device, &mc, nullptr, &modules[st]) != VK_SUCCESS) {
            error = "shader module";
            return false;
        }
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = uint32_t(vertexBindings.size());
    ci.pBindings = vertexBindings.data();
    if (vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &setOut) != VK_SUCCESS) {
        error = "descriptor set layout";
        return false;
    }
    VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lc.setLayoutCount = 1;  // (the vertex stage's; the motion pixel shader reads nothing)
    lc.pSetLayouts = &setOut;
    if (vkCreatePipelineLayout(R.device, &lc, nullptr, &layoutOut) != VK_SUCCESS) {
        error = "pipeline layout";
        return false;
    }
    if (views == 2) state.set_view_mask(0b11);  // (after make_motion_only, which clears it)
    for (int st = 0; st < 2; ++st) state.set_stage_module(st, modules[st]);
    VkGraphicsPipelineCreateInfo info = *state.get();
    info.layout = layoutOut;
    // (no pipeline cache: the renderer's is not synchronized for another thread)
    const VkResult result = vkCreateGraphicsPipelines(R.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipelineOut);
    if (result != VK_SUCCESS) {
        error = "vkCreateGraphicsPipelines " + std::to_string(int(result));
        pipelineOut = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock, [] { return g_stopping || !g_jobs.empty(); });
            if (g_stopping) return;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        std::string error;
        bool ok = false;
        try { ok = make(job, error); }
        catch (const std::exception& e) { error = e.what(); }
        if (ok) {
            ++g_built;
            job.out->state.store(Pipeline::Ready, std::memory_order_release);
        } else {
            // a pipeline without depth draws nothing the motion pass needs: not a failure
            ++(job.out->noDepth ? g_noDepth : g_failed);
            job.out->error = error;
            if (!job.out->noDepth && g_failed <= 8) LOG("[vulkan motion] a motion pipeline failed: %s", error.c_str());
            job.out->state.store(Pipeline::Failed, std::memory_order_release);
        }
        if ((g_built + g_failed + g_noDepth) % 100 == 0)
            LOG("[vulkan motion] motion pipelines: %llu built, %llu failed, %llu without depth (no motion needed)",
                (unsigned long long)g_built.load(), (unsigned long long)g_failed.load(),
                (unsigned long long)g_noDepth.load());
    }
}

}  // namespace

std::shared_ptr<Pipeline> build(std::shared_ptr<const multiview::Base> base, Stage vs, Stage ps) {
    auto out = std::make_shared<Pipeline>();
    std::lock_guard lock(g_mutex);
    if (g_stopping) {
        out->state = Pipeline::Failed;
        return out;
    }
    if (!g_thread.joinable()) g_thread = std::thread(run);
    g_jobs.push_back(Job{std::move(base), {std::move(vs), std::move(ps)}, out});
    g_wake.notify_one();
    return out;
}

void stop() {
    {
        std::lock_guard lock(g_mutex);
        g_stopping = true;
        g_jobs.clear();
    }
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void counts(uint64_t& built, uint64_t& failed) {
    built = g_built;
    failed = g_failed;
}

}  // namespace gfxvk::vk::motion
