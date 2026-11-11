// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// SGSR2: vr_sgsr2.h. The shaders are Qualcomm's Snapdragon Game Super Resolution 2, 2-pass fragment variant
// (github.com/SnapdragonStudios/snapdragon-gsr, sgsr/v2/include/glsl_2_pass_fs: sgsr2_convert.fs, sgsr2_upscale.fs),
// BSD-3-Clause, Copyright (c) 2024 Qualcomm Innovation Center, Inc. (notice kept with the shaders below). Changes for
// this renderer:
//   - GLSL 450 for glslang; the parameters as push constants (clipToPrevClip and bSameCamera dropped: unused here);
//   - the convert pass reads the motion pass's motion target directly (its (previous - current) * 0.5 in the game's
//     clip space, turned into current - previous Vulkan NDC by the push constant's scale, as the frame synthesis
//     export does: vr/vr_framesynth.h motion_scale), so the camera reconstruction branch is not needed (every pixel
//     has motion: the background draws give the sky its own);
//   - the non-y-up NDC path (Vulkan): PrevUV = uv - 0.5 * motion;
//   - the scene is not jittered (jitterOffset 0): the jitter's sign against the scene's viewport jitter is not yet
//     checked in a headset, so SGSR2 upscales and stabilizes with its history without sub-pixel jitter for now.
// Parameters as Qualcomm's Vulkan sample sets them (adreno-gpu-vulkan-code-sample-framework, samples/sgsr2):
// scaleRatio = (output width / render width, min(20, (output area / render area)^3)), minLerpContribution 0 (a head is
// never still), cameraFovAngleHor = tan(horizontal fov / 2) (1.2 here: about the headsets' eye fov).
#include "vr_sgsr2.h"

#include "motion_pass.h"
#include "pass.h"
#include "pipeline_trace.h"
#include "shaders.h"
#include "runtime.h"
#include "vr/vr_framesynth.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace gfxvk::vr_sgsr2 {
namespace {

const char* kVertex = R"glsl(#version 450
layout(location=0) out highp vec2 texCoord;
void main() {
 vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
 texCoord=p;
 gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";

#define SGSR2_NOTICE R"glsl(
//============================================================================================================
//                  Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
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
)glsl"

#define SGSR2_PARAMS R"glsl(
layout(push_constant) uniform Params {
 highp vec2 renderSize;
 highp vec2 outputSize;
 highp vec2 renderSizeRcp;
 highp vec2 outputSizeRcp;
 highp vec2 jitterOffset;
 highp vec2 scaleRatio;
 highp vec2 motionScale;   // the motion pass's value to current - previous Vulkan NDC
 highp float cameraFovAngleHor;
 highp float minLerpContribution;
 highp float reset;
} params;
)glsl"

const char* kConvert = "#version 450\n" SGSR2_NOTICE R"glsl(
precision highp float;
precision highp int;
layout(location=0) out vec4 MotionDepthClipAlphaBuffer;
layout(location=0) in highp vec2 texCoord;
layout(set=0,binding=0) uniform mediump sampler2D InputDepth;
layout(set=0,binding=1) uniform mediump sampler2D InputVelocity;
)glsl" SGSR2_PARAMS R"glsl(
void main() {
 uvec2 InputPos=uvec2(texCoord*params.renderSize);
 vec2 gatherCoord=texCoord-vec2(0.5)*params.renderSizeRcp;
 vec4 btmLeft=textureGather(InputDepth,gatherCoord,0);
 vec2 v10=vec2(params.renderSizeRcp.x*2.0,0.0);
 vec4 btmRight=textureGather(InputDepth,(gatherCoord+v10),0);
 vec2 v12=vec2(0.0,params.renderSizeRcp.y*2.0);
 vec4 topLeft=textureGather(InputDepth,(gatherCoord+v12),0);
 vec2 v14=vec2(params.renderSizeRcp.x*2.0,params.renderSizeRcp.y*2.0);
 vec4 topRight=textureGather(InputDepth,(gatherCoord+v14),0);
 float maxC=min(min(min(btmLeft.z,btmRight.w),topLeft.y),topRight.x);
 float depthclip=0.0;
 if(maxC<1.0-1.0e-05) {
  float btmLeft4=min(min(min(btmLeft.y,btmLeft.x),btmLeft.z),btmLeft.w);
  float btmRight4=min(min(min(btmRight.y,btmRight.x),btmRight.z),btmRight.w);
  float topLeft4=min(min(min(topLeft.y,topLeft.x),topLeft.z),topLeft.w);
  float topRight4=min(min(min(topRight.y,topRight.x),topRight.z),topRight.w);
  float Wdepth=0.0;
  float Ksep=1.37e-05;
  float Kfov=params.cameraFovAngleHor;
  float diagonal_length=length(params.renderSize);
  float Ksep_Kfov_diagonal=Ksep*Kfov*diagonal_length;
  float Depthsep=Ksep_Kfov_diagonal*(1.0-maxC);
  float EPSILON=1.19e-07;
  Wdepth+=clamp((Depthsep/(abs(maxC-btmLeft4)+EPSILON)),0.0,1.0);
  Wdepth+=clamp((Depthsep/(abs(maxC-btmRight4)+EPSILON)),0.0,1.0);
  Wdepth+=clamp((Depthsep/(abs(maxC-topLeft4)+EPSILON)),0.0,1.0);
  Wdepth+=clamp((Depthsep/(abs(maxC-topRight4)+EPSILON)),0.0,1.0);
  depthclip=clamp(1.0-Wdepth*0.25,0.0,1.0);
 }
 // (this renderer) the motion pass's motion, as current - previous Vulkan NDC
 vec2 motion=texelFetch(InputVelocity,ivec2(InputPos),0).rg*params.motionScale;
 if(any(isnan(motion))||any(isinf(motion)))motion=vec2(0.0);
 MotionDepthClipAlphaBuffer=vec4(motion,depthclip,0.0);
}
)glsl";

const char* kUpscale = "#version 450\n" SGSR2_NOTICE R"glsl(
precision mediump float;
precision highp int;
float FastLanczos(float base) {
 float y=base-1.0;
 float y2=y*y;
 float y_temp=0.75*y+y2;
 return y_temp*y2;
}
layout(location=0) out mediump vec4 Output;
layout(location=0) in highp vec2 texCoord;
layout(set=0,binding=0) uniform mediump sampler2D PrevOutput;
layout(set=0,binding=1) uniform mediump sampler2D MotionDepthClipAlphaBuffer;
layout(set=0,binding=2) uniform mediump sampler2D InputColor;
)glsl" SGSR2_PARAMS R"glsl(
void tap(vec3 samplecolor, vec2 baseoffset, float kernelbias2, float curvebias, inout vec4 Upsampledcw,
         inout vec3 rectboxmin, inout vec3 rectboxmax, inout vec3 rectboxcenter, inout vec3 rectboxvar,
         inout float rectboxweight, bool first) {
 float baseoffset_dot=dot(baseoffset,baseoffset);
 float base=clamp(baseoffset_dot*kernelbias2,0.0,1.0);
 float weight=FastLanczos(base);
 Upsampledcw+=vec4(samplecolor*weight,weight);
 float boxweight=exp(baseoffset_dot*curvebias);
 if(first) { rectboxmin=samplecolor; rectboxmax=samplecolor; }
 else { rectboxmin=min(rectboxmin,samplecolor); rectboxmax=max(rectboxmax,samplecolor); }
 vec3 wsample=samplecolor*boxweight;
 rectboxcenter+=wsample;
 rectboxvar+=(samplecolor*wsample);
 rectboxweight+=boxweight;
}
void main() {
 float Biasmax_viewportXScale=params.scaleRatio.x;
 float scalefactor=params.scaleRatio.y;
 highp vec2 Hruv=texCoord;
 highp vec2 Jitteruv;
 Jitteruv.x=clamp(Hruv.x+(params.jitterOffset.x*params.renderSizeRcp.x),0.0,1.0);
 Jitteruv.y=clamp(Hruv.y+(params.jitterOffset.y*params.renderSizeRcp.y),0.0,1.0);
 highp ivec2 InputPos=ivec2(Jitteruv*params.renderSize);
 highp vec3 mda=textureLod(MotionDepthClipAlphaBuffer,Jitteruv,0.0).xyz;
 highp vec2 Motion=mda.xy;
 highp vec2 PrevUV;
 PrevUV.x=clamp(-0.5*Motion.x+Hruv.x,0.0,1.0);
 PrevUV.y=clamp(-0.5*Motion.y+Hruv.y,0.0,1.0);
 float depthfactor=mda.z;
 vec3 HistoryColor=textureLod(PrevOutput,PrevUV,0.0).xyz;
 vec4 Upsampledcw=vec4(0.0);
 float biasmax=Biasmax_viewportXScale;
 float biasmin=max(1.0,0.3+0.3*biasmax);
 float biasfactor=0.25*depthfactor;
 float kernelbias=mix(biasmax,biasmin,biasfactor);
 float motion_viewport_len=length(Motion*params.outputSize);
 float curvebias=mix(-2.0,-3.0,clamp(motion_viewport_len*0.02,0.0,1.0));
 vec3 rectboxcenter=vec3(0.0);
 vec3 rectboxvar=vec3(0.0);
 float rectboxweight=0.0;
 highp vec2 srcpos=vec2(InputPos)+vec2(0.5)-params.jitterOffset;
 kernelbias*=0.5;
 float kernelbias2=kernelbias*kernelbias;
 vec2 srcpos_srcOutputPos=srcpos-Hruv*params.renderSize;
 vec3 rectboxmin=vec3(0.0);
 vec3 rectboxmax=vec3(0.0);
 tap(texelFetch(InputColor,InputPos+ivec2(0,1),0).xyz,srcpos_srcOutputPos+vec2(0.0,1.0),kernelbias2,curvebias,
     Upsampledcw,rectboxmin,rectboxmax,rectboxcenter,rectboxvar,rectboxweight,true);
 tap(texelFetch(InputColor,InputPos+ivec2(1,0),0).xyz,srcpos_srcOutputPos+vec2(1.0,0.0),kernelbias2,curvebias,
     Upsampledcw,rectboxmin,rectboxmax,rectboxcenter,rectboxvar,rectboxweight,false);
 tap(texelFetch(InputColor,InputPos+ivec2(-1,0),0).xyz,srcpos_srcOutputPos+vec2(-1.0,0.0),kernelbias2,curvebias,
     Upsampledcw,rectboxmin,rectboxmax,rectboxcenter,rectboxvar,rectboxweight,false);
 tap(texelFetch(InputColor,InputPos,0).xyz,srcpos_srcOutputPos,kernelbias2,curvebias,
     Upsampledcw,rectboxmin,rectboxmax,rectboxcenter,rectboxvar,rectboxweight,false);
 tap(texelFetch(InputColor,InputPos+ivec2(0,-1),0).xyz,srcpos_srcOutputPos+vec2(0.0,-1.0),kernelbias2,curvebias,
     Upsampledcw,rectboxmin,rectboxmax,rectboxcenter,rectboxvar,rectboxweight,false);
 rectboxweight=1.0/rectboxweight;
 rectboxcenter*=rectboxweight;
 rectboxvar*=rectboxweight;
 rectboxvar=sqrt(abs(rectboxvar-rectboxcenter*rectboxcenter));
 Upsampledcw.xyz=clamp(Upsampledcw.xyz/Upsampledcw.w,rectboxmin-vec3(0.075),rectboxmax+vec3(0.075));
 Upsampledcw.w=Upsampledcw.w*(1.0/3.0);
 float baseupdate=1.0-depthfactor;
 baseupdate=min(baseupdate,mix(baseupdate,Upsampledcw.w*10.0,clamp(10.0*motion_viewport_len,0.0,1.0)));
 baseupdate=min(baseupdate,mix(baseupdate,Upsampledcw.w,clamp(motion_viewport_len*0.05,0.0,1.0)));
 float basealpha=baseupdate;
 const float EPSILON=1.192e-07;
 float boxscale=max(depthfactor,clamp(motion_viewport_len*0.05,0.0,1.0));
 float boxsize=mix(scalefactor,1.0,boxscale);
 vec3 sboxvar=rectboxvar*boxsize;
 vec3 boxmin=rectboxcenter-sboxvar;
 vec3 boxmax=rectboxcenter+sboxvar;
 rectboxmax=min(rectboxmax,boxmax);
 rectboxmin=max(rectboxmin,boxmin);
 vec3 clampedcolor=clamp(HistoryColor,rectboxmin,rectboxmax);
 float startLerpValue=params.minLerpContribution;
 if((abs(mda.x)+abs(mda.y))>0.000001)startLerpValue=0.0;
 float lerpcontribution=(any(greaterThan(rectboxmin,HistoryColor))||any(greaterThan(HistoryColor,rectboxmax)))?startLerpValue:1.0;
 HistoryColor=mix(clampedcolor,HistoryColor,clamp(lerpcontribution,0.0,1.0));
 float basemin=min(basealpha,0.1);
 basealpha=mix(basemin,basealpha,clamp(lerpcontribution,0.0,1.0));
 float alphasum=max(EPSILON,basealpha+Upsampledcw.w);
 float alpha=clamp(Upsampledcw.w/alphasum+params.reset,0.0,1.0);
 Upsampledcw.xyz=mix(HistoryColor,Upsampledcw.xyz,alpha);
 Output=vec4(Upsampledcw.xyz,1.0);
}
)glsl";

struct Params {
    float renderSize[2], outputSize[2], renderSizeRcp[2], outputSizeRcp[2], jitterOffset[2], scaleRatio[2], motionScale[2];
    float cameraFovAngleHor, minLerpContribution, reset;
};
static_assert(sizeof(Params) <= 128, "push constants: the guaranteed 128 bytes");

constexpr uint32_t kFloat16x4 = 0x820;  // GX2 FLOAT R16G16B16A16 (formats.cpp: VK_FORMAT_R16G16B16A16_SFLOAT)

struct Eye {
    Surface motionDepth;  // the convert pass's output, the render size
    Surface history[2];   // the upscaled pictures, the headset image's size: this frame's and the last
    int current = 0;
    uint64_t lastFrame = 0;  // the frame it was last drawn (a gap starts the history again)
    VkExtent2D render{}, output{};
    struct View { VkImage image = VK_NULL_HANDLE; uint32_t layer = 0; VkImageView view = VK_NULL_HANDLE; } depth, motion;
};
Eye g_eyes[2];
VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkSampler g_linear = VK_NULL_HANDLE;
VkPipeline g_convert = VK_NULL_HANDLE, g_upscale = VK_NULL_HANDLE;
bool g_failed = false;

VkShaderModule module(const char* source, bool vertex) {
    std::string error;
    auto words = vk::compile_glsl(source, vertex, &error);
    if (words.empty() || !error.empty()) {
        LOG("[vr sgsr2] a shader failed to compile: %s", error.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(R.device, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkPipeline make_pipeline(const char* fragment) {
    VkShaderModule vs = module(kVertex, true), fs = vs ? module(fragment, false) : VK_NULL_HANDLE;
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
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = states;
        const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
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
        ci.pDynamicState = &dy;
        ci.layout = g_layout;
        if (stutter::create_graphics(R.frame, "vr_sgsr2_pipeline", R.device, R.pipelineCache, 1, &ci, nullptr, &result) !=
            VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(R.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(R.device, fs, nullptr);
    return result;
}

bool ensure_objects() {
    if (g_convert && g_upscale) return true;
    if (g_failed) return false;
    if (!g_layout) {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 3;
        dl.pBindings = b;
        VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Params)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &g_setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &range;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        if (vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &g_setLayout) != VK_SUCCESS ||
            vkCreatePipelineLayout(R.device, &pl, nullptr, &g_layout) != VK_SUCCESS ||
            vkCreateSampler(R.device, &si, nullptr, &g_linear) != VK_SUCCESS) {
            g_failed = true;
            return false;
        }
    }
    g_convert = make_pipeline(kConvert);
    g_upscale = g_convert ? make_pipeline(kUpscale) : VK_NULL_HANDLE;
    if (!g_convert || !g_upscale) {
        g_failed = true;
        LOG("[vr sgsr2] its pipelines were not made: SGSR1 from now on");
        return false;
    }
    return true;
}

void make_surface(Surface& s, uint32_t w, uint32_t h) {
    if (s.image && s.extent.width == w && s.extent.height == h) return;
    if (s.image) destroy_surface_image(&s);
    s.width = s.pitch = w;
    s.height = h;
    s.slices = s.mips = 1;
    s.dim = 1;
    s.format = kFloat16x4;
    s.fmt = format_info(kFloat16x4, false);
    create_surface_image(&s, true, VkExtent3D{w, h, 1});
}

// a sampled view of a surface's layer (the depth aspect of a depth surface), made again when its image changes
VkImageView source_view(Eye::View& d, Surface* s, uint32_t slice, bool depth) {
    const uint32_t layer = s->baseLayer + slice;
    if (d.view && d.image == s->image && d.layer == layer) return d.view;
    if (d.view) defer_surface_image(VK_NULL_HANDLE, VK_NULL_HANDLE, {d.view});
    d = {};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s->fmt.pixel;
    vi.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, layer, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.view) != VK_SUCCESS) return VK_NULL_HANDLE;
    d.image = s->image;
    d.layer = layer;
    return d.view;
}

// one full-screen pass into `target` with the given sampled inputs (binding order)
bool pass(VkCommandBuffer cmd, VkPipeline p, Surface& target, std::initializer_list<std::pair<VkImageView, VkImageLayout>> inputs,
          const Params& params) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_setLayout;
    if (vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo infos[3]{};
    VkWriteDescriptorSet writes[3]{};
    uint32_t n = 0;
    for (const auto& [view, layout] : inputs) {
        if (!view) return false;
        infos[n] = {g_linear, view, layout};
        writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[n].dstSet = set;
        writes[n].dstBinding = n;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[n].pImageInfo = &infos[n];
        ++n;
    }
    vkUpdateDescriptorSets(R.device, n, writes, 0, nullptr);
    transition_image(&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = layer_view(&target, 0);
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // (every pixel is written)
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {target.extent.width, target.extent.height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    begin_pass(cmd, rendering, [&] { return color_pass(target.fmt.pixel, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); });
    pass_state().rendering = true;
    pass_state().tracked = false;
    const VkViewport viewport{0, 0, float(target.extent.width), float(target.extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {target.extent.width, target.extent.height}};
    record::vkCmdSetViewport(cmd, 0, 1, &viewport);
    record::vkCmdSetScissor(cmd, 0, 1, &scissor);
    record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, nullptr);
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    mark_gpu_written(&target);
    return true;
}

}  // namespace

bool draw(VkCommandBuffer cmd, int eye, Surface& picture, VkExtent2D shown, VkImage target, VkFormat format,
          VkExtent2D extent) {
    if (eye < 0 || eye > 1 || R.renderPasses || !ensure_objects()) return false;
    motion_pass::EyeResult mr;
    if (!motion_pass::eye_result(eye, mr) || !mr.motion || !mr.motion->image || !mr.depth || !mr.depth->image)
        return false;
    // the picture, its depth and its motion at one size (the scene's; the Quest's fixed scale keeps them so)
    const uint32_t rw = shown.width ? shown.width : picture.extent.width;
    const uint32_t rh = shown.height ? shown.height : picture.extent.height;
    if (mr.depth->extent.width != rw || mr.depth->extent.height != rh || mr.motion->extent.width != rw ||
        mr.motion->extent.height != rh || !(picture.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !(mr.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return false;
    Eye& e = g_eyes[eye];
    const bool resized = e.render.width != rw || e.render.height != rh || e.output.width != extent.width ||
                         e.output.height != extent.height;
    make_surface(e.motionDepth, rw, rh);
    make_surface(e.history[0], extent.width, extent.height);
    make_surface(e.history[1], extent.width, extent.height);
    if (!e.motionDepth.image || !e.history[0].image || !e.history[1].image) return false;
    // a new history: a view cut or a new scene (the motion pass), new sizes, or a frame (or more) without SGSR2
    const bool reset = resized || mr.reset || e.lastFrame + 1 != R.frame;
    e.render = {rw, rh};
    e.output = extent;
    e.lastFrame = R.frame;
    Params params{};
    params.renderSize[0] = float(rw), params.renderSize[1] = float(rh);
    params.outputSize[0] = float(extent.width), params.outputSize[1] = float(extent.height);
    params.renderSizeRcp[0] = 1.0f / float(rw), params.renderSizeRcp[1] = 1.0f / float(rh);
    params.outputSizeRcp[0] = 1.0f / float(extent.width), params.outputSizeRcp[1] = 1.0f / float(extent.height);
    params.scaleRatio[0] = float(extent.width) / float(rw);
    params.scaleRatio[1] = std::min(20.0f, std::pow(float(extent.width) * float(extent.height) / (float(rw) * float(rh)), 3.0f));
    vr::framesynth::motion_scale(mr.flipsY, params.motionScale[0], params.motionScale[1]);
    params.cameraFovAngleHor = 1.2f;
    params.minLerpContribution = 0.0f;
    params.reset = reset ? 1.0f : 0.0f;
    end_encoder();
    // convert: motion and the depth clip at the render size
    transition_image(mr.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    transition_image(mr.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    if (!pass(cmd, g_convert, e.motionDepth,
              {{source_view(e.depth, mr.depth, mr.slice, true), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
               {source_view(e.motion, mr.motion, 0, false), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
              params))
        return false;
    // upscale: the last history, this frame's motion and depth clip, and the picture into this frame's history
    Surface& previous = e.history[e.current];
    Surface& next = e.history[1 - e.current];
    transition_image(&previous, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    transition_image(&e.motionDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    transition_image(&picture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    if (!pass(cmd, g_upscale, next,
              {{previous.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
               {e.motionDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
               {picture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}},
              params))
        return false;
    e.current = 1 - e.current;
    // into the headset's image (the same size: the blit converts float to the swapchain's format, sRGB encoding included)
    transition_image(&next, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
    record::vkCmdBlitImage(cmd, next.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_NEAREST);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
    (void)format;
    return true;
}

void forget() {
    for (Eye& e : g_eyes) {
        for (Surface* s : {&e.motionDepth, &e.history[0], &e.history[1]})
            if (s->image) destroy_surface_image(s);
        for (Eye::View* v : {&e.depth, &e.motion})
            if (v->view) defer_surface_image(VK_NULL_HANDLE, VK_NULL_HANDLE, {v->view});
        e = Eye{};
    }
}

}  // namespace gfxvk::vr_sgsr2
