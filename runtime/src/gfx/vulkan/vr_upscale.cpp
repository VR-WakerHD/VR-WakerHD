// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The SGSR1 pass: vr_upscale.h. The fragment shader is Qualcomm's Snapdragon Game Super Resolution 1
// (github.com/SnapdragonStudios/snapdragon-gsr, sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag),
// BSD-3-Clause, Copyright (c) 2025 Qualcomm Innovation Center, Inc. (notice kept with the shader below). Changes for
// this renderer: GLSL 450 for glslang; its constants as push constants (the input's texel size and size, the
// sharpness, the edge threshold; Qualcomm suggests 4/255 for VR); the picture's region scaled into the output's UVs;
// and, into an sRGB swapchain image, the filter run on approximately perceptual values (square root of the linear
// samples, squared back), as its thresholds expect display-encoded values. Edge direction on (UseEdgeDirection).
#include "vr_upscale.h"

#include "pass.h"
#include "pipeline_trace.h"
#include "shaders.h"
#include "runtime.h"
#include "../../exception_report.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace gfxvk::vr_upscale {
namespace {

const char* kVertex = R"glsl(#version 450
layout(location=0) out vec2 uv;
void main() {
 vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
 uv=p;
 gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";

const char* kFragment = R"glsl(#version 450
//============================================================================================================
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list
// of conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided
// with the distribution. 3. Neither the name of the copyright holder nor the names of its contributors may be used to
// endorse or promote products derived from this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//============================================================================================================
precision mediump float;
precision highp int;
layout(set=0,binding=0) uniform mediump sampler2D ps0;
layout(push_constant) uniform Params {
 highp vec4 viewportInfo;  // 1/width, 1/height, width, height of the input
 highp vec2 uvScale;       // the picture's share of the input (its region from the top left)
 float edgeSharpness;
 float edgeThreshold;
 int perceptual;           // 1: linear samples, filtered as sqrt (into an sRGB image)
} pc;
layout(location=0) in highp vec2 uv;
layout(location=0) out vec4 outColor;
#define OperationMode 1
float fastLanczos2(float x) {
 float wA=x-4.0;
 float wB=x*wA-wA;
 wA*=wA;
 return wB*wA;
}
vec2 weightY(float dx,float dy,float c,vec3 data) {
 float std=data.x;
 vec2 dir=data.yz;
 float edgeDis=((dx*dir.y)+(dy*dir.x));
 float x=(((dx*dx)+(dy*dy))+((edgeDis*edgeDis)*((clamp(((c*c)*std),0.0,1.0)*0.7)+-1.0)));
 float w=fastLanczos2(x);
 return vec2(w,w*c);
}
vec2 edgeDirection(vec4 left,vec4 right) {
 vec2 dir;
 float RxLz=(right.x+(-left.z));
 float RwLy=(right.w+(-left.y));
 vec2 delta;
 delta.x=(RxLz+RwLy);
 delta.y=(RxLz+(-RwLy));
 float lengthInv=inversesqrt((delta.x*delta.x+3.075740e-05)+(delta.y*delta.y));
 dir.x=(delta.x*lengthInv);
 dir.y=(delta.y*lengthInv);
 return dir;
}
vec4 enc(vec4 v) { return pc.perceptual!=0?sqrt(max(v,vec4(0.0))):v; }
vec4 gather(highp vec2 at) { return enc(textureGather(ps0,at,OperationMode)); }
void main() {
 highp vec2 tc=uv*pc.uvScale;
 vec4 color;
 color.xyz=enc(vec4(textureLod(ps0,tc,0.0).xyz,0.0)).xyz;
 {
  highp vec2 imgCoord=((tc*pc.viewportInfo.zw)+vec2(-0.5,0.5));
  highp vec2 imgCoordPixel=floor(imgCoord);
  highp vec2 coord=(imgCoordPixel*pc.viewportInfo.xy);
  vec2 pl=(imgCoord+(-imgCoordPixel));
  vec4 left=gather(coord);
  float edgeVote=abs(left.z-left.y)+abs(color[OperationMode]-left.y)+abs(color[OperationMode]-left.z);
  if(edgeVote>pc.edgeThreshold) {
   coord.x+=pc.viewportInfo.x;
   vec4 right=gather(coord+vec2(pc.viewportInfo.x,0.0));
   vec4 upDown;
   upDown.xy=gather(coord+vec2(0.0,-pc.viewportInfo.y)).wz;
   upDown.zw=gather(coord+vec2(0.0,pc.viewportInfo.y)).yx;
   float mean=(left.y+left.z+right.x+right.w)*0.25;
   left=left-vec4(mean);
   right=right-vec4(mean);
   upDown=upDown-vec4(mean);
   color.w=color[OperationMode]-mean;
   float sum=(((((abs(left.x)+abs(left.y))+abs(left.z))+abs(left.w))+(((abs(right.x)+abs(right.y))+abs(right.z))+abs(right.w)))+(((abs(upDown.x)+abs(upDown.y))+abs(upDown.z))+abs(upDown.w)));
   float sumMean=1.014185e+01/sum;
   float std=(sumMean*sumMean);
   vec3 data=vec3(std,edgeDirection(left,right));
   vec2 aWY=weightY(pl.x,pl.y+1.0,upDown.x,data);
   aWY+=weightY(pl.x-1.0,pl.y+1.0,upDown.y,data);
   aWY+=weightY(pl.x-1.0,pl.y-2.0,upDown.z,data);
   aWY+=weightY(pl.x,pl.y-2.0,upDown.w,data);
   aWY+=weightY(pl.x+1.0,pl.y-1.0,left.x,data);
   aWY+=weightY(pl.x,pl.y-1.0,left.y,data);
   aWY+=weightY(pl.x,pl.y,left.z,data);
   aWY+=weightY(pl.x+1.0,pl.y,left.w,data);
   aWY+=weightY(pl.x-1.0,pl.y-1.0,right.x,data);
   aWY+=weightY(pl.x-2.0,pl.y-1.0,right.y,data);
   aWY+=weightY(pl.x-2.0,pl.y,right.z,data);
   aWY+=weightY(pl.x-1.0,pl.y,right.w,data);
   float finalY=aWY.y/aWY.x;
   float maxY=max(max(left.y,left.z),max(right.x,right.w));
   float minY=min(min(left.y,left.z),min(right.x,right.w));
   float deltaY=clamp(pc.edgeSharpness*finalY,minY,maxY)-color.w;
   deltaY=clamp(deltaY,-23.0/255.0,23.0/255.0);
   color.x=clamp((color.x+deltaY),0.0,1.0);
   color.y=clamp((color.y+deltaY),0.0,1.0);
   color.z=clamp((color.z+deltaY),0.0,1.0);
  }
 }
 vec3 c=pc.perceptual!=0?color.xyz*color.xyz:color.xyz;
 outColor=vec4(c,1.0);
}
)glsl";

struct Params {
    float viewportInfo[4];
    float uvScale[2];
    float edgeSharpness;
    float edgeThreshold;
    int32_t perceptual;
};

VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
std::unordered_map<uint32_t, VkPipeline> g_pipelines;  // by target format
std::unordered_map<VkImage, VkImageView> g_views;     // the swapchain images' views
bool g_failed = false;                                // a shader or pipeline failed once: blits from then on

VkShaderModule module(const char* source, bool vertex) {
    std::string error;
    auto words = vk::compile_glsl(source, vertex, &error);
    if (words.empty() || !error.empty()) {
        LOG("[vr upscale] SGSR1 shader failed to compile: %s", error.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(R.device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

bool ensure_layout() {
    if (g_layout) return true;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &g_setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Params)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(R.device, &pl, nullptr, &g_layout) != VK_SUCCESS) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    return vkCreateSampler(R.device, &si, nullptr, &g_sampler) == VK_SUCCESS;
}

VkPipeline pipeline(VkFormat format) {
    if (auto it = g_pipelines.find(uint32_t(format)); it != g_pipelines.end()) return it->second;
    VkShaderModule vs = module(kVertex, true), fs = vs ? module(kFragment, false) : VK_NULL_HANDLE;
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
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bs.attachmentCount = 1;
        bs.pAttachments = &blend;
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_targets(ci, rendering);
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &bs;
        ci.pDynamicState = &ds;
        ci.layout = g_layout;
        if (stutter::create_graphics(R.frame, "vr_upscale_pipeline", R.device, R.pipelineCache, 1, &ci, nullptr, &result) !=
            VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(R.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(R.device, fs, nullptr);
    if (!result) {
        g_failed = true;
        LOG("[vr upscale] SGSR1 pipeline not made (format %u): eye pictures blitted", unsigned(format));
        return VK_NULL_HANDLE;
    }
    g_pipelines.emplace(uint32_t(format), result);
    return result;
}

VkImageView view_of(VkImage image, VkFormat format) {
    VkImageView& view = g_views[image];
    if (!view) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(R.device, &vi, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    }
    return view;
}

bool srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

}  // namespace

bool available(const Surface& source, VkFormat target) {
    if (g_failed || !R.device || R.renderPasses || !source.image || !source.view) return false;
    if (!(source.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return false;
    const auto& src = format_properties(source.fmt.pixel);
    const auto& dst = format_properties(target);
    return (src.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
           (src.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
           (dst.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
}

bool draw(VkCommandBuffer cmd, Surface& source, VkExtent2D shown, VkImage target, VkFormat format, VkExtent2D extent,
          float sharpness) {
    if (!available(source, format) || !ensure_layout()) return false;
    VkPipeline p = pipeline(format);
    VkImageView view = p ? view_of(target, format) : VK_NULL_HANDLE;
    if (!view) return false;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_setLayout;
    if (vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return false;
    transition_image(&source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    VkDescriptorImageInfo info{g_sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(R.device, 1, &write, 0, nullptr);
    // the acquired image is in COLOR_ATTACHMENT_OPTIMAL (OpenXR): its earlier use is the runtime's, ordered by the
    // acquire; only this pass's writes need ordering
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // (every pixel is written)
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    begin_pass(cmd, rendering, [&] { return color_pass(format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); });
    pass_state().rendering = true;
    pass_state().tracked = false;
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    record::vkCmdSetViewport(cmd, 0, 1, &viewport);
    record::vkCmdSetScissor(cmd, 0, 1, &scissor);
    record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, nullptr);
    const float w = float(source.extent.width), h = float(source.extent.height);
    Params params{};
    params.viewportInfo[0] = 1.0f / w;
    params.viewportInfo[1] = 1.0f / h;
    params.viewportInfo[2] = w;
    params.viewportInfo[3] = h;
    params.uvScale[0] = shown.width ? float(shown.width) / w : 1.0f;
    params.uvScale[1] = shown.height ? float(shown.height) / h : 1.0f;
    params.edgeSharpness = sharpness;
    params.edgeThreshold = 4.0f / 255.0f;  // (Qualcomm's suggestion for VR)
    // an sRGB target encodes linear output, so the samples are linear (as the blit copies them)
    params.perceptual = srgb(format) ? 1 : 0;
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    return true;
}

void self_test() {
    // a flat picture: SGSR1 keeps flat areas as they are (no edge vote), so every output pixel is the input colour
    for (const uint32_t gx2Format : {0x1Au, 0x41Au}) {  // RGBA8 UNORM, RGBA8 sRGB (as the headsets' swapchains)
        Surface source, target;
        Buffer readback;
        auto init = [](Surface& s, uint32_t f, uint32_t w, uint32_t h) {
            s.width = s.pitch = w;
            s.height = h;
            s.slices = s.mips = 1;
            s.dim = 1;
            s.format = f;
            s.fmt = format_info(f, false);
            create_surface_image(&s, true, VkExtent3D{w, h, 1});
        };
        try {
            init(source, 0x1A, 8, 6);
            init(target, gx2Format, 20, 14);
            readback = create_readback_buffer(20 * 14 * 4);
            const float in[3] = {0.5f, 0.25f, 0.75f};
            transition_image(&source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue value{{in[0], in[1], in[2], 1}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            record::vkCmdClearColorImage(command_buffer(), source.image, source.layout, &value, 1, &range);
            mark_gpu_written(&source);
            transition_image(&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            if (!available(source, target.fmt.pixel)) {
                LOG("[renderer smoke] SGSR1 pass: not available here (format %u), skipped", unsigned(target.fmt.pixel));
                destroy_surface_image(&source);
                destroy_surface_image(&target);
                defer_buffer(readback);
                continue;
            }
            // the picture's top-left 6x5 region (as an eye picture smaller than its image), upscaled to 20x14
            if (!draw(command_buffer(), source, {6, 5}, target.image, target.fmt.pixel, {20, 14}, 2.0f))
                exception_report::raise("SGSR1 pass recorded nothing");
            transition_image(&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);
            transition_buffer(readback, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {20, 14, 1};
            record::vkCmdCopyImageToBuffer(command_buffer(), target.image, target.layout, readback.buffer, 1, &copy);
            transition_buffer(readback, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            flush();
            wait_idle();
            const auto* px = static_cast<const uint8_t*>(readback.mapped);
            const bool srgbTarget = srgb(target.fmt.pixel);
            int worst = 0;
            for (int i = 0; i < 20 * 14; ++i)
                for (int k = 0; k < 3; ++k) {
                    // the expected byte: the colour as stored (sRGB-encoded in an sRGB image)
                    float v = in[k];
                    if (srgbTarget) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                    const int expected = int(std::lround(v * 255.0f));
                    worst = std::max(worst, std::abs(int(px[i * 4 + k]) - expected));
                }
            if (worst > 2)
                exception_report::raise("SGSR1 pass changed a flat picture (by " + std::to_string(worst) + "/255)");
            LOG("[renderer smoke] SGSR1 pass: flat %s picture 6x5 -> 20x14 unchanged (within %d/255)",
                srgbTarget ? "sRGB" : "UNORM", worst);
        } catch (...) {
            if (source.image) destroy_surface_image(&source);
            if (target.image) destroy_surface_image(&target);
            forget_views();
            throw;
        }
        wait_idle();
        forget_views();
        destroy_surface_image(&source);
        destroy_surface_image(&target);
        defer_buffer(readback);
    }
}

void forget_views() {
    if (!g_views.empty()) note_descriptor_targets_destroyed();
    for (auto& [image, view] : g_views)
        if (view) vkDestroyImageView(R.device, view, nullptr);
    g_views.clear();
}

}  // namespace gfxvk::vr_upscale
