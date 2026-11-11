// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// FSR 1: vr_fsr1.h. The shaders are AMD FidelityFX Super Resolution 1's EASU and RCAS (ffx_fsr1.h of the FidelityFX
// SDK, as in github.com/GPUOpen-Effects/FidelityFX-FSR2 src/ffx-fsr2-api/shaders/ffx_fsr1.h), MIT, Copyright (c) 2022
// Advanced Micro Devices, Inc. (notice kept with the shaders below). Ported to standalone GLSL 450 for glslang: the
// 32-bit float paths (ffxFsrEasuFloat, FsrRcasF) with their callbacks (gathers of the picture; texel loads of the
// intermediate), the approximate reciprocals as exact ones, and the constants (ffxFsrPopulateEasuConstants,
// FsrRcasCon) computed on the CPU as floats. Into an sRGB swapchain the filters run on approximately perceptual values
// (square root of the linear samples, squared back at the end), as FSR 1 expects perceptual input. Upscaling only.
#include "vr_fsr1.h"

#include "pass.h"
#include "pipeline_trace.h"
#include "shaders.h"
#include "runtime.h"

#include <cmath>
#include <string>
#include <unordered_map>

namespace gfxvk::vr_fsr1 {
namespace {

const char* kVertex = R"glsl(#version 450
void main() {
 vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
 gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";

#define FSR1_NOTICE R"glsl(
// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
)glsl"

// EASU: the picture (perceptual or as stored) upscaled into the intermediate image
const char* kEasu = "#version 450\n" FSR1_NOTICE R"glsl(
layout(set=0,binding=0) uniform sampler2D src;
layout(push_constant) uniform Params { vec4 con0; vec4 con1; vec4 con2; vec4 con3; int perceptual; } pc;
layout(location=0) out vec4 result;
vec4 enc(vec4 v) { return pc.perceptual!=0?sqrt(max(v,vec4(0.0))):v; }
vec4 FsrEasuRF(vec2 p) { return enc(textureGather(src,p,0)); }
vec4 FsrEasuGF(vec2 p) { return enc(textureGather(src,p,1)); }
vec4 FsrEasuBF(vec2 p) { return enc(textureGather(src,p,2)); }
void fsrEasuTapFloat(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
 vec2 v;
 v.x=(off.x*(dir.x))+(off.y*dir.y);
 v.y=(off.x*(-dir.y))+(off.y*dir.x);
 v*=len;
 float d2=v.x*v.x+v.y*v.y;
 d2=min(d2,clp);
 float wB=float(2.0/5.0)*d2+float(-1.0);
 float wA=lob*d2+float(-1.0);
 wB*=wB;
 wA*=wA;
 wB=float(25.0/16.0)*wB+float(-(25.0/16.0-1.0));
 float w=wB*wA;
 aC+=c*w;
 aW+=w;
}
void fsrEasuSetFloat(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV,
                     float lA, float lB, float lC, float lD, float lE) {
 float w=0.0;
 if(biS)w=(1.0-pp.x)*(1.0-pp.y);
 if(biT)w=pp.x*(1.0-pp.y);
 if(biU)w=(1.0-pp.x)*pp.y;
 if(biV)w=pp.x*pp.y;
 float dc=lD-lC;
 float cb=lC-lB;
 float lenX=max(abs(dc),abs(cb));
 lenX=1.0/lenX;
 float dirX=lD-lB;
 dir.x+=dirX*w;
 lenX=clamp(abs(dirX)*lenX,0.0,1.0);
 lenX*=lenX;
 len+=lenX*w;
 float ec=lE-lC;
 float ca=lC-lA;
 float lenY=max(abs(ec),abs(ca));
 lenY=1.0/lenY;
 float dirY=lE-lA;
 dir.y+=dirY*w;
 lenY=clamp(abs(dirY)*lenY,0.0,1.0);
 lenY*=lenY;
 len+=lenY*w;
}
void main() {
 uvec2 ip=uvec2(gl_FragCoord.xy);
 vec2 pp=vec2(ip)*pc.con0.xy+pc.con0.zw;
 vec2 fp=floor(pp);
 pp-=fp;
 vec2 p0=fp*pc.con1.xy+pc.con1.zw;
 vec2 p1=p0+pc.con2.xy;
 vec2 p2=p0+pc.con2.zw;
 vec2 p3=p0+pc.con3.xy;
 vec4 bczzR=FsrEasuRF(p0);
 vec4 bczzG=FsrEasuGF(p0);
 vec4 bczzB=FsrEasuBF(p0);
 vec4 ijfeR=FsrEasuRF(p1);
 vec4 ijfeG=FsrEasuGF(p1);
 vec4 ijfeB=FsrEasuBF(p1);
 vec4 klhgR=FsrEasuRF(p2);
 vec4 klhgG=FsrEasuGF(p2);
 vec4 klhgB=FsrEasuBF(p2);
 vec4 zzonR=FsrEasuRF(p3);
 vec4 zzonG=FsrEasuGF(p3);
 vec4 zzonB=FsrEasuBF(p3);
 vec4 bczzL=bczzB*vec4(0.5)+(bczzR*vec4(0.5)+bczzG);
 vec4 ijfeL=ijfeB*vec4(0.5)+(ijfeR*vec4(0.5)+ijfeG);
 vec4 klhgL=klhgB*vec4(0.5)+(klhgR*vec4(0.5)+klhgG);
 vec4 zzonL=zzonB*vec4(0.5)+(zzonR*vec4(0.5)+zzonG);
 float bL=bczzL.x; float cL=bczzL.y;
 float iL=ijfeL.x; float jL=ijfeL.y; float fL=ijfeL.z; float eL=ijfeL.w;
 float kL=klhgL.x; float lL=klhgL.y; float hL=klhgL.z; float gL=klhgL.w;
 float oL=zzonL.z; float nL=zzonL.w;
 vec2 dir=vec2(0.0);
 float len=0.0;
 fsrEasuSetFloat(dir,len,pp,true,false,false,false,bL,eL,fL,gL,jL);
 fsrEasuSetFloat(dir,len,pp,false,true,false,false,cL,fL,gL,hL,kL);
 fsrEasuSetFloat(dir,len,pp,false,false,true,false,fL,iL,jL,kL,nL);
 fsrEasuSetFloat(dir,len,pp,false,false,false,true,gL,jL,kL,lL,oL);
 vec2 dir2=dir*dir;
 float dirR=dir2.x+dir2.y;
 bool zro=dirR<float(1.0/32768.0);
 dirR=inversesqrt(dirR);
 dirR=zro?1.0:dirR;
 dir.x=zro?1.0:dir.x;
 dir*=vec2(dirR);
 len=len*0.5;
 len*=len;
 float stretch=(dir.x*dir.x+dir.y*dir.y)/(max(abs(dir.x),abs(dir.y)));
 vec2 len2=vec2(1.0+(stretch-1.0)*len,1.0+(-0.5)*len);
 float lob=0.5+float((1.0/4.0-0.04)-0.5)*len;
 float clp=1.0/lob;
 vec3 min4=min(min(min(vec3(ijfeR.z,ijfeG.z,ijfeB.z),vec3(klhgR.w,klhgG.w,klhgB.w)),vec3(ijfeR.y,ijfeG.y,ijfeB.y)),
               vec3(klhgR.x,klhgG.x,klhgB.x));
 vec3 max4=max(max(max(vec3(ijfeR.z,ijfeG.z,ijfeB.z),vec3(klhgR.w,klhgG.w,klhgB.w)),vec3(ijfeR.y,ijfeG.y,ijfeB.y)),
               vec3(klhgR.x,klhgG.x,klhgB.x));
 vec3 aC=vec3(0.0);
 float aW=0.0;
 fsrEasuTapFloat(aC,aW,vec2(0.0,-1.0)-pp,dir,len2,lob,clp,vec3(bczzR.x,bczzG.x,bczzB.x));
 fsrEasuTapFloat(aC,aW,vec2(1.0,-1.0)-pp,dir,len2,lob,clp,vec3(bczzR.y,bczzG.y,bczzB.y));
 fsrEasuTapFloat(aC,aW,vec2(-1.0,1.0)-pp,dir,len2,lob,clp,vec3(ijfeR.x,ijfeG.x,ijfeB.x));
 fsrEasuTapFloat(aC,aW,vec2(0.0,1.0)-pp,dir,len2,lob,clp,vec3(ijfeR.y,ijfeG.y,ijfeB.y));
 fsrEasuTapFloat(aC,aW,vec2(0.0,0.0)-pp,dir,len2,lob,clp,vec3(ijfeR.z,ijfeG.z,ijfeB.z));
 fsrEasuTapFloat(aC,aW,vec2(-1.0,0.0)-pp,dir,len2,lob,clp,vec3(ijfeR.w,ijfeG.w,ijfeB.w));
 fsrEasuTapFloat(aC,aW,vec2(1.0,1.0)-pp,dir,len2,lob,clp,vec3(klhgR.x,klhgG.x,klhgB.x));
 fsrEasuTapFloat(aC,aW,vec2(2.0,1.0)-pp,dir,len2,lob,clp,vec3(klhgR.y,klhgG.y,klhgB.y));
 fsrEasuTapFloat(aC,aW,vec2(2.0,0.0)-pp,dir,len2,lob,clp,vec3(klhgR.z,klhgG.z,klhgB.z));
 fsrEasuTapFloat(aC,aW,vec2(1.0,0.0)-pp,dir,len2,lob,clp,vec3(klhgR.w,klhgG.w,klhgB.w));
 fsrEasuTapFloat(aC,aW,vec2(1.0,2.0)-pp,dir,len2,lob,clp,vec3(zzonR.z,zzonG.z,zzonB.z));
 fsrEasuTapFloat(aC,aW,vec2(0.0,2.0)-pp,dir,len2,lob,clp,vec3(zzonR.w,zzonG.w,zzonB.w));
 vec3 pix=min(max4,max(min4,aC*vec3(1.0/aW)));
 result=vec4(pix,1.0);
}
)glsl";

// RCAS: the intermediate image sharpened into the swapchain image (squared back from perceptual into an sRGB image)
const char* kRcas = "#version 450\n" FSR1_NOTICE R"glsl(
layout(set=0,binding=0) uniform sampler2D src;
layout(push_constant) uniform Params { vec4 con0; vec4 con1; vec4 con2; vec4 con3; int perceptual; } pc;
layout(location=0) out vec4 result;
#define FSR_RCAS_LIMIT (0.25-(1.0/16.0))
vec4 FsrRcasLoadF(ivec2 p) { return texelFetch(src,clamp(p,ivec2(0),textureSize(src,0)-1),0); }
void main() {
 ivec2 sp=ivec2(gl_FragCoord.xy);
 vec3 b=FsrRcasLoadF(sp+ivec2(0,-1)).rgb;
 vec3 d=FsrRcasLoadF(sp+ivec2(-1,0)).rgb;
 vec3 e=FsrRcasLoadF(sp).rgb;
 vec3 f=FsrRcasLoadF(sp+ivec2(1,0)).rgb;
 vec3 h=FsrRcasLoadF(sp+ivec2(0,1)).rgb;
 float bR=b.r; float bG=b.g; float bB=b.b;
 float dR=d.r; float dG=d.g; float dB=d.b;
 float eR=e.r; float eG=e.g; float eB=e.b;
 float fR=f.r; float fG=f.g; float fB=f.b;
 float hR=h.r; float hG=h.g; float hB=h.b;
 float mn4R=min(min(min(bR,dR),fR),hR);
 float mn4G=min(min(min(bG,dG),fG),hG);
 float mn4B=min(min(min(bB,dB),fB),hB);
 float mx4R=max(max(max(bR,dR),fR),hR);
 float mx4G=max(max(max(bG,dG),fG),hG);
 float mx4B=max(max(max(bB,dB),fB),hB);
 vec2 peakC=vec2(1.0,-1.0*4.0);
 float hitMinR=mn4R*(1.0/(4.0*mx4R));
 float hitMinG=mn4G*(1.0/(4.0*mx4G));
 float hitMinB=mn4B*(1.0/(4.0*mx4B));
 float hitMaxR=(peakC.x-mx4R)*(1.0/(4.0*mn4R+peakC.y));
 float hitMaxG=(peakC.x-mx4G)*(1.0/(4.0*mn4G+peakC.y));
 float hitMaxB=(peakC.x-mx4B)*(1.0/(4.0*mn4B+peakC.y));
 float lobeR=max(-hitMinR,hitMaxR);
 float lobeG=max(-hitMinG,hitMaxG);
 float lobeB=max(-hitMinB,hitMaxB);
 float lobe=max(float(-FSR_RCAS_LIMIT),min(max(max(lobeR,lobeG),lobeB),0.0))*pc.con0.x;
 float rcpL=1.0/(4.0*lobe+1.0);
 vec3 pix;
 pix.r=(lobe*bR+lobe*dR+lobe*hR+lobe*fR+eR)*rcpL;
 pix.g=(lobe*bG+lobe*dG+lobe*hG+lobe*fG+eG)*rcpL;
 pix.b=(lobe*bB+lobe*dB+lobe*hB+lobe*fB+eB)*rcpL;
 pix=clamp(pix,0.0,1.0);
 if(any(isnan(pix)))pix=e;
 result=vec4(pc.perceptual!=0?pix*pix:pix,1.0);
}
)glsl";

struct Params {
    float con0[4], con1[4], con2[4], con3[4];
    int32_t perceptual;
};
static_assert(sizeof(Params) <= 128, "push constants: the guaranteed 128 bytes");

constexpr uint32_t kFloat16x4 = 0x820;  // GX2 FLOAT R16G16B16A16 (formats.cpp: VK_FORMAT_R16G16B16A16_SFLOAT)

Surface g_intermediate[2];  // EASU's output per eye, the headset image's size
VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkSampler g_linear = VK_NULL_HANDLE;
VkPipeline g_easu = VK_NULL_HANDLE;
std::unordered_map<uint32_t, VkPipeline> g_rcas;     // by swapchain format
std::unordered_map<VkImage, VkImageView> g_views;    // the swapchain images' views
bool g_failed = false;

VkShaderModule module(const char* source, bool vertex) {
    std::string error;
    auto words = vk::compile_glsl(source, vertex, &error);
    if (words.empty() || !error.empty()) {
        LOG("[vr fsr1] a shader failed to compile: %s", error.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(R.device, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkPipeline make_pipeline(const char* fragment, VkFormat format) {
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
        if (stutter::create_graphics(R.frame, "vr_fsr1_pipeline", R.device, R.pipelineCache, 1, &ci, nullptr, &result) !=
            VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(R.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(R.device, fs, nullptr);
    return result;
}

bool ensure_objects(VkFormat target) {
    if (g_failed) return false;
    if (!g_layout) {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &b;
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
    if (!g_easu) g_easu = make_pipeline(kEasu, VK_FORMAT_R16G16B16A16_SFLOAT);
    VkPipeline& rcas = g_rcas[uint32_t(target)];
    if (!rcas && g_easu) rcas = make_pipeline(kRcas, target);
    if (!g_easu || !rcas) {
        g_failed = true;
        LOG("[vr fsr1] its pipelines were not made (format %u): SGSR1 or the blit from now on", unsigned(target));
        return false;
    }
    return true;
}

bool srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

VkImageView target_view(VkImage image, VkFormat format) {
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

// one full-screen pass of `p` sampling `source` into `view` (an image in COLOR_ATTACHMENT_OPTIMAL of `format`)
bool pass(VkCommandBuffer cmd, VkPipeline p, VkImageView source, VkImageView view, VkFormat format, VkExtent2D extent,
          const Params& params) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_setLayout;
    if (!source || !view || vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo info{g_linear, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(R.device, 1, &write, 0, nullptr);
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
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    return true;
}

}  // namespace

bool draw(VkCommandBuffer cmd, int eye, Surface& picture, VkExtent2D shown, VkImage target, VkFormat format,
          VkExtent2D extent, float sharpnessStops) {
    if (eye < 0 || eye > 1 || R.renderPasses || !picture.view || !(picture.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !ensure_objects(format))
        return false;
    const auto& features = format_properties(picture.fmt.pixel).optimalTilingFeatures;
    if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    // the intermediate image (EASU's output), the headset image's size
    Surface& mid = g_intermediate[eye];
    if (!mid.image || mid.extent.width != extent.width || mid.extent.height != extent.height) {
        if (mid.image) destroy_surface_image(&mid);
        mid.width = mid.pitch = extent.width;
        mid.height = extent.height;
        mid.slices = mid.mips = 1;
        mid.dim = 1;
        mid.format = kFloat16x4;
        mid.fmt = format_info(kFloat16x4, false);
        create_surface_image(&mid, true, VkExtent3D{extent.width, extent.height, 1});
        if (!mid.image) return false;
    }
    // EASU's constants (ffxFsrPopulateEasuConstants): the picture's region (viewport) in its image (size)
    const float vw = float(shown.width ? shown.width : picture.extent.width);
    const float vh = float(shown.height ? shown.height : picture.extent.height);
    const float iw = float(picture.extent.width), ih = float(picture.extent.height);
    const float ow = float(extent.width), oh = float(extent.height);
    Params p{};
    p.con0[0] = vw / ow;
    p.con0[1] = vh / oh;
    p.con0[2] = 0.5f * vw / ow - 0.5f;
    p.con0[3] = 0.5f * vh / oh - 0.5f;
    p.con1[0] = 1.0f / iw;
    p.con1[1] = 1.0f / ih;
    p.con1[2] = 1.0f / iw;
    p.con1[3] = -1.0f / ih;
    p.con2[0] = -1.0f / iw;
    p.con2[1] = 2.0f / ih;
    p.con2[2] = 1.0f / iw;
    p.con2[3] = 2.0f / ih;
    p.con3[0] = 0.0f;
    p.con3[1] = 4.0f / ih;
    p.perceptual = srgb(format) ? 1 : 0;  // (an sRGB swapchain: the samples are linear, the blit's meaning)
    end_encoder();
    transition_image(&picture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    transition_image(&mid, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (!pass(cmd, g_easu, picture.view, layer_view(&mid, 0), mid.fmt.pixel, extent, p)) return false;
    mark_gpu_written(&mid);
    // RCAS's constant (FsrRcasCon): the sharpness from stops to a linear amount; into the swapchain image
    Params r{};
    r.con0[0] = std::exp2(-std::max(0.0f, sharpnessStops));
    r.perceptual = p.perceptual;
    transition_image(&mid, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // (acquired so: OpenXR)
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    return pass(cmd, g_rcas[uint32_t(format)], mid.view, target_view(target, format), format, extent, r);
}

void forget() {
    for (Surface& s : g_intermediate)
        if (s.image) destroy_surface_image(&s);
    if (!g_views.empty()) note_descriptor_targets_destroyed();
    for (auto& [image, view] : g_views)
        if (view) vkDestroyImageView(R.device, view, nullptr);
    g_views.clear();
}

}  // namespace gfxvk::vr_fsr1
