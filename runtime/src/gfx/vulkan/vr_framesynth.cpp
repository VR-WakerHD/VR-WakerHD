// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Frame synthesis and depth for the OpenXR runtime: vr_framesynth.h, docs/VR_FRAME_SYNTHESIS.md.
#include "vr_framesynth.h"

#ifndef WWHD_VR
namespace gfxvk::vr_framesynth {
void set_active(bool) {}
bool active() { return false; }
void note_camera(int, const uint32_t*, const unsigned char*, size_t) {}
void record(VkCommandBuffer) {}
void release_images() {}
void forget_views() {}
const char* status() { return "off"; }
bool carrying() { return false; }
}  // namespace gfxvk::vr_framesynth
#else

#include "motion_pass.h"
#include "pass.h"
#include "pipeline_trace.h"
#include "shaders.h"
#include "runtime.h"
#include "gfx/stereo_projection.h"
#include "vr/vr.h"
#include "vr/vr_framesynth.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gfxvk::vr_framesynth {
namespace {

const char* kVertex = R"glsl(#version 450
layout(location=0) out vec2 uv;
void main() {
 vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
 uv=p;
 gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";
// the scene depth (0 near .. 1 far, as drawn) into the runtime's depth image
const char* kDepthFragment = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D src;
layout(location=0) in vec2 uv;
void main() { gl_FragDepth=textureLod(src,uv,0.0).r; }
)glsl";
// the motion pass's (previous - current) * 0.5 in the game's clip space to current - previous in Vulkan NDC
// (vr::framesynth::motion_scale); z (the depth's change) is not known: 0
const char* kMotionFragment = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D src;
layout(push_constant) uniform Params { vec2 scale; } pc;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
void main() {
 vec2 m=textureLod(src,uv,0.0).rg*pc.scale;
 result=vec4(any(isnan(m))||any(isinf(m))?vec2(0.0):m,0.0,0.0);
}
)glsl";

std::atomic<bool> g_active{false};
std::mutex g_noteMutex;
struct Noted {
    std::map<std::pair<Surface*, uint32_t>, uint32_t> depths;  // the depth targets the eye's camera draws drew into
    float nearZ = 0, farZ = 0;                                  // the planes of the last camera draw (projection units)
    bool planes = false;
};
Noted g_noted[2];

VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
std::unordered_map<uint64_t, VkPipeline> g_pipelines;  // by kind << 32 | format
std::unordered_map<VkImage, VkImageView> g_views;      // the swapchain images' views
struct SourceView { VkImage image = VK_NULL_HANDLE; uint32_t layer = 0; VkImageView view = VK_NULL_HANDLE; };
SourceView g_sources[2][2];                            // [eye][0 depth, 1 motion]
bool g_acquired[2][2] = {};                            // [eye][0 depth, 1 motion]
bool g_failed = false;
std::string g_status = "not in VR yet";
std::atomic<bool> g_carrying{false};

bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

VkShaderModule module(const char* source, bool vertex) {
    std::string error;
    auto words = vk::compile_glsl(source, vertex, &error);
    if (words.empty() || !error.empty()) {
        LOG("[vr frame synthesis] a shader failed to compile: %s", error.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(R.device, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

bool ensure_layout() {
    if (g_layout) return true;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &g_setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(R.device, &pl, nullptr, &g_layout) != VK_SUCCESS) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;  // (motion and depth are not blended across edges)
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    return vkCreateSampler(R.device, &si, nullptr, &g_sampler) == VK_SUCCESS;
}

// kind 0: depth into a depth image of `format`; 1: motion into an RGBA16F image
VkPipeline pipeline(int kind, VkFormat format) {
    const uint64_t key = uint64_t(kind) << 32 | uint32_t(format);
    if (auto it = g_pipelines.find(key); it != g_pipelines.end()) return it->second;
    VkShaderModule vs = module(kVertex, true);
    VkShaderModule fs = vs ? module(kind ? kMotionFragment : kDepthFragment, false) : VK_NULL_HANDLE;
    VkPipeline result = VK_NULL_HANDLE;
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = i ? fs : vs;
            stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = kind == 0;
        ds.depthWriteEnable = kind == 0;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bs.attachmentCount = kind ? 1 : 0;
        bs.pAttachments = kind ? &blend : nullptr;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        if (kind) {
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &format;
        } else {
            rendering.depthAttachmentFormat = format;
            rendering.stencilAttachmentFormat = has_stencil(format) ? format : VK_FORMAT_UNDEFINED;
        }
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_targets(ci, rendering);
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &bs;
        ci.pDynamicState = &dy;
        ci.layout = g_layout;
        if (stutter::create_graphics(R.frame, "vr_framesynth_pipeline", R.device, R.pipelineCache, 1, &ci, nullptr,
                                     &result) != VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(R.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(R.device, fs, nullptr);
    if (!result) {
        g_failed = true;
        LOG("[vr frame synthesis] its %s pipeline was not made (format %u): frame synthesis off for the run",
            kind ? "motion" : "depth", unsigned(format));
        return VK_NULL_HANDLE;
    }
    g_pipelines.emplace(key, result);
    return result;
}

VkImageView target_view(VkImage image, VkFormat format, bool depth) {
    VkImageView& view = g_views[image];
    if (!view) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        const VkImageAspectFlags aspect = depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) |
                                                      (has_stencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                                : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        if (vkCreateImageView(R.device, &vi, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    }
    return view;
}

// a sampled view of a surface's layer (the depth aspect of a depth surface), made again when the surface's image
// changes (the old one destroyed once the GPU is past it), as the DLSS inputs (motion_pass.cpp input_view)
VkImageView source_view(int eye, int role, Surface* s, uint32_t slice) {
    SourceView& d = g_sources[eye][role];
    const uint32_t layer = s->baseLayer + slice;
    if (d.view && d.image == s->image && d.layer == layer) return d.view;
    if (d.view) defer_surface_image(VK_NULL_HANDLE, VK_NULL_HANDLE, {d.view});
    d = {};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s->fmt.pixel;
    vi.subresourceRange = {role == 0 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, layer, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.view) != VK_SUCCESS) return VK_NULL_HANDLE;
    d.image = s->image;
    d.layer = layer;
    return d.view;
}

// one full-screen pass of `kind` sampling `source` into the acquired swapchain image `target`
bool draw_pass(VkCommandBuffer cmd, int kind, VkImageView source, VkImage target, VkFormat format, uint32_t width,
               uint32_t height, const float* push) {
    VkPipeline p = pipeline(kind, format);
    VkImageView view = p ? target_view(target, format, kind == 0) : VK_NULL_HANDLE;
    if (!view || !source) return false;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_setLayout;
    if (vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo info{g_sampler, source, kind ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(R.device, 1, &write, 0, nullptr);
    // acquired images are in COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL (OpenXR) and must be again at
    // release: only this pass's writes need ordering
    const VkImageLayout layout = kind ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkPipelineStageFlags stage = kind ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                            : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = layout;
    b.dstAccessMask = kind ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {kind ? VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT)
                               : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) | (has_stencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                          0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, stage, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view;
    attachment.imageLayout = layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // (every pixel is written)
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {width, height};
    rendering.layerCount = 1;
    if (kind) {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
    } else {
        rendering.pDepthAttachment = &attachment;
        rendering.pStencilAttachment = has_stencil(format) ? &attachment : nullptr;
    }
    begin_pass(cmd, rendering, [&] {
        if (kind) return color_pass(format, layout);
        PassBegin pass;
        pass.formats.depth = format;
        pass.formats.stencil = has_stencil(format);
        pass.layouts[8] = layout;
        return pass;
    });
    pass_state().rendering = true;
    pass_state().tracked = false;
    const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    const VkRect2D scissor{{0, 0}, {width, height}};
    record::vkCmdSetViewport(cmd, 0, 1, &viewport);
    record::vkCmdSetScissor(cmd, 0, 1, &scissor);
    record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, nullptr);
    float params[4] = {push ? push[0] : 0, push ? push[1] : 0, 0, 0};
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof params, params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    return true;
}

bool sampleable_depth(const Surface* s) {
    return s && s->image && s->fmt.depth && (s->usage & VK_IMAGE_USAGE_SAMPLED_BIT);
}

}  // namespace

void set_active(bool on) { g_active.store(on, std::memory_order_relaxed); }
bool active() { return g_active.load(std::memory_order_relaxed) && !g_failed; }

void note_camera(int eye, const uint32_t* regs, const unsigned char* block, size_t size) {
    if (!active() || eye < 0 || eye > 1 || !block) return;
    // one camera draw in kNoteEvery per drawing thread (2026-11-05, asked for: depth must not add to the drawing threads'
    // time): every scene draw with the camera's projection came here, 600 and more a frame across the scene's and the
    // atlas's threads, each searching its block and taking g_noteMutex. What is noted is the same for every draw (the
    // camera's planes) or a count (the eye's most-drawn depth target), so a sample keeps both: about 20 a frame
    constexpr unsigned kNoteEvery = 32;
    static thread_local unsigned sampled = 0;
    if (sampled++ % kNoteEvery != 0) return;
    float p[16];
    if (!stereo::find_camera_projection(block, size, p)) return;
    Latte::LATTE_PA_CL_CLIP_CNTL clip;
    std::memcpy(&clip, regs + Latte::REGADDR::PA_CL_CLIP_CNTL, 4);
    float nearZ = 0, farZ = 0;
    const bool planes = vr::framesynth::planes(p, clip.get_DX_CLIP_SPACE_DEF(), nearZ, farZ);
    const PassState& pass = pass_state();
    std::lock_guard<std::mutex> l(g_noteMutex);
    Noted& n = g_noted[eye];
    if (pass.depth) ++n.depths[{pass.depth, pass.depthSlice}];
    if (planes) n.nearZ = nearZ, n.farZ = farZ, n.planes = true;
}

void record(VkCommandBuffer cmd) {
    Noted noted[2];
    {
        std::lock_guard<std::mutex> l(g_noteMutex);
        noted[0] = std::move(g_noted[0]);
        noted[1] = std::move(g_noted[1]);
        g_noted[0] = {};
        g_noted[1] = {};
    }
    g_carrying.store(false, std::memory_order_relaxed);
    if (!active() || R.renderPasses || !ensure_layout()) return;
    auto& xs = vr::session();
    const bool synthesis = xs.has_synthesis_swapchains();
    if (!synthesis && !xs.depth_submission()) return;
    // the planes: the camera's (game units) in metres
    const Noted& np = noted[0].planes ? noted[0] : noted[1];
    const float scale = vr::world_scale();
    vr::xr::Session::SynthFrame frame;
    if (np.planes && scale > 0) {
        frame.nearZ = np.nearZ / scale;
        frame.farZ = np.farZ / scale;  // (infinity stays infinite)
    }
    end_encoder();
    int written = 0, motions = 0;
    for (int eye = 0; eye < 2; ++eye) {
        // the eye's scene depth: the motion pass's (its motion was tested against it), else the depth target most of
        // its camera draws drew into; a multiview eye 1 has no draws of its own: eye 0's target's layer twin
        motion_pass::EyeResult mr;
        const bool motion = synthesis && motion_pass::eye_result(eye, mr) && mr.motion && mr.motion->image;
        Surface* depth = motion ? mr.depth : nullptr;
        uint32_t slice = motion ? mr.slice : 0;
        if (!depth) {
            auto pick = [](const Noted& n, Surface*& s, uint32_t& sl) {
                uint32_t best = 0;
                for (const auto& [k, count] : n.depths)
                    if (count > best) best = count, s = k.first, sl = k.second;
            };
            pick(noted[eye], depth, slice);
            if (!depth && eye == 1) {
                Surface* d0 = nullptr;
                uint32_t s0 = 0;
                pick(noted[0], d0, s0);
                if (d0 && d0->layerTwin) depth = d0->layerTwin, slice = s0;
            }
        }
        if (!sampleable_depth(depth) || !(frame.nearZ > 0)) continue;
        // depth into the runtime's depth image
        const int depthIndex = vr::xr::Session::kDepth + eye;
        const auto& ds = xs.swapchain(depthIndex);
        VkImage depthImage = xs.acquire(depthIndex);
        if (!depthImage) continue;
        g_acquired[eye][0] = true;
        transition_image(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT);
        frame.depth[eye] = draw_pass(cmd, 0, source_view(eye, 0, depth, slice), depthImage, VkFormat(ds.format), ds.width,
                                     ds.height, nullptr);
        written += frame.depth[eye];
        // motion into the runtime's motion vector image (frame synthesis only, where the motion pass made it)
        if (motion && frame.depth[eye]) {
            const int motionIndex = vr::xr::Session::kMotion + eye;
            const auto& ms = xs.swapchain(motionIndex);
            VkImage motionImage = xs.acquire(motionIndex);
            if (motionImage) {
                g_acquired[eye][1] = true;
                float push[2];
                vr::framesynth::motion_scale(mr.flipsY, push[0], push[1]);
                transition_image(mr.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
                frame.motion[eye] = draw_pass(cmd, 1, source_view(eye, 1, mr.motion, 0), motionImage, VkFormat(ms.format),
                                              ms.width, ms.height, push);
                motions += frame.motion[eye];
            }
        }
    }
    xs.set_synthesis_frame(frame);
    g_carrying.store(synthesis && motions == 2, std::memory_order_relaxed);
    // the status (logged when it changes)
    const char* now = !written                 ? "nothing this frame (no scene depth or planes: menus, loading)"
                      : synthesis && motions == 2 ? "frame synthesis: motion vectors and depth"
                      : synthesis                  ? "depth only this frame (no motion pass result)"
                                                   : "depth for reprojection";
    if (g_status != now) {
        g_status = now;
        LOG("[vr frame synthesis] %s (near %.3f m, far %.3g m)", now, double(frame.nearZ), double(frame.farZ));
        vr::framesynth::set_status(now);
    }
}

void release_images() {
    auto& xs = vr::session();
    for (int eye = 0; eye < 2; ++eye) {
        if (g_acquired[eye][0]) xs.release(vr::xr::Session::kDepth + eye);
        if (g_acquired[eye][1]) xs.release(vr::xr::Session::kMotion + eye);
        g_acquired[eye][0] = g_acquired[eye][1] = false;
    }
}

void forget_views() {
    if (!g_views.empty()) note_descriptor_targets_destroyed();
    for (auto& [image, view] : g_views)
        if (view) vkDestroyImageView(R.device, view, nullptr);
    g_views.clear();
}

const char* status() { return g_status.c_str(); }
bool carrying() { return g_carrying.load(std::memory_order_relaxed); }

}  // namespace gfxvk::vr_framesynth
#endif  // WWHD_VR
