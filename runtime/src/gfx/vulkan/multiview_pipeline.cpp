// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A guest pipeline's multiview twin: multiview_pipeline.h.
#include "multiview_pipeline.h"
#include "stereo_switches.h"

#include "backend.h"
#include "runtime.h"
#include "shader_compiler.h"
#include "shaders.h"
#include "spirv_precision.h"
#include "uniform_plan.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace gfxvk::vk::multiview {
namespace {

struct Job {
    std::shared_ptr<const Base> base;
    Stage stages[2];
    std::shared_ptr<Pipeline> out;
};
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_jobs;
std::thread g_thread;
bool g_stopping = false;
std::atomic<uint64_t> g_built{0}, g_failed{0};

bool make(Job& job, std::string& error) {
    Pipeline& out = *job.out;
#ifdef WWHD_MOBILE_GPU_PATHS
    // (QUEST3_IMPLEMENTATION_ROADMAP.md 2.1) the twin's layout must fit the device's dynamic uniform buffer cap too: each
    // per-view block holds two descriptors, so a pipeline near the cap (Adreno: 8) made a twin over it, an invalid layout
    // a driver need not reject. Over it the twin fails, and its draws stay per eye (kMvFailed), as the motion twins do
    {
        uint32_t dynamic = 0;
        for (int st = 0; st < 2; ++st)
            for (const auto& b : job.base->descriptors[st]) {
                if (b.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
                bool perView = false;
                if (job.stages[st].per_view())
                    for (uint32_t v : job.stages[st].perViewBindings) perView = perView || v == b.binding;
                dynamic += perView ? 2 * b.descriptorCount : b.descriptorCount;
            }
        const uint32_t cap = uniform_plan::dynamic_limit(R.properties.limits.maxDescriptorSetUniformBuffersDynamic);
        if (dynamic > cap) {
            error = "over the device's dynamic uniform buffers (" + std::to_string(dynamic) + " of " + std::to_string(cap) + ")";
            return false;
        }
    }
#endif
    VkShaderModule modules[2]{};
    struct Modules {
        VkShaderModule* m;
        ~Modules() { for (int i = 0; i < 2; ++i) if (m[i]) vkDestroyShaderModule(R.device, m[i], nullptr); }
    } cleanup{modules};
    for (int st = 0; st < 2; ++st) {
        Stage& stage = job.stages[st];
        std::vector<uint32_t> words;
        if (stage.per_view()) {
            std::string source;
            if (!rewrite(*stage.glsl, stage.variant, source, &error)) return false;
            // compiled here, on the builder's thread (background = false): never in the game's shader compiler's queue,
            // where a burst of twins kept the game's own new shaders waiting and the render thread with them
            // (diagnostics) WWHD_VK_MULTIVIEW_SHARED_COMPILER=1: through the game's compiler queue, as before
            static const bool shared = std::getenv("WWHD_VK_MULTIVIEW_SHARED_COMPILER") != nullptr;
            auto compiled = compile_shader(source, st == 0 ? ShaderStage::Vertex : ShaderStage::Fragment, ~uint64_t{0}, 0,
                                           {}, shared);
            words = std::move(compiled.words);
            if (words.empty()) {
                error = compiled.error.empty() ? "an empty compile" : compiled.error;
                return false;
            }
            if (full_precision_enabled()) strip_relaxed_precision(words);
            // the per-view bindings hold two descriptors
            auto bindings = job.base->descriptors[st];
            for (uint32_t b : stage.perViewBindings) {
                bool found = false;
                for (auto& binding : bindings)
                    if (binding.binding == b) binding.descriptorCount = 2, found = true;
                if (!found) { error = "a per-view binding the pipeline does not have"; return false; }
            }
            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = uint32_t(bindings.size());
            ci.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &out.sets[st]) != VK_SUCCESS) {
                error = "descriptor set layout";
                return false;
            }
            out.ownSet[st] = true;
        } else {
            words = std::move(stage.words);
            out.sets[st] = job.base->sets[st];
        }
        VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mc.codeSize = words.size() * sizeof(uint32_t);
        mc.pCode = words.data();
        if (vkCreateShaderModule(R.device, &mc, nullptr, &modules[st]) != VK_SUCCESS) {
            error = "shader module";
            return false;
        }
    }
    VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lc.setLayoutCount = 2;
    lc.pSetLayouts = out.sets;
    if (vkCreatePipelineLayout(R.device, &lc, nullptr, &out.layout) != VK_SUCCESS) {
        error = "pipeline layout";
        return false;
    }
    GraphicsPipelineState state = job.base->state;
    state.set_view_mask(0b11);
    for (int st = 0; st < 2; ++st) state.set_stage_module(st, modules[st]);
    VkGraphicsPipelineCreateInfo info = *state.get();
    info.layout = out.layout;
    // (no pipeline cache: the renderer's is not synchronized for another thread)
    const VkResult result = vkCreateGraphicsPipelines(R.device, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline);
    if (result != VK_SUCCESS) {
        error = "vkCreateGraphicsPipelines " + std::to_string(int(result));
        out.pipeline = VK_NULL_HANDLE;
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
            ++g_failed;
            job.out->error = error;
            if (g_failed <= 5) LOG("[vulkan multiview] a multiview pipeline failed: %s", error.c_str());
            job.out->state.store(Pipeline::Failed, std::memory_order_release);
        }
    }
}

}  // namespace

bool draws_on() {
    static const bool on = [] {
        // (=0: off; otherwise on wherever multiview is, also where WWHD_VK_MULTIVIEW=1 turned it on)
        const char* e = std::getenv("WWHD_VK_MULTIVIEW_DRAWS");
        if (!R.multiview || (e && !std::strcmp(e, "0"))) return false;
        if (R.renderPasses || !R.multiviewIndexing) {
            LOG("[vulkan multiview] draws: off (%s)", R.renderPasses ? "render passes, not dynamic rendering"
                                                                    : "no dynamic indexing of uniform or image arrays");
            return false;
        }
        LOG("[vulkan multiview] draws: on (WWHD_VK_MULTIVIEW_DRAWS=0 turns them off): a draw both eyes share is drawn once for both");
        return true;
    }();
    return on;
}

std::shared_ptr<Pipeline> build(std::shared_ptr<const Base> base, Stage vs, Stage ps) {
    auto out = std::make_shared<Pipeline>();
    std::lock_guard lock(g_mutex);
    if (g_stopping) {
        out->state = Pipeline::Failed;
        return out;
    }
    if (!g_thread.joinable()) g_thread = std::thread(run);
    Job job{std::move(base), {std::move(vs), std::move(ps)}, out};
    g_jobs.push_back(std::move(job));
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

}  // namespace gfxvk::vk::multiview
