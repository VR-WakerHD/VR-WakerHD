// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The CAS pass: vr_cas.h. Contrast adaptive sharpening with upscaling, after AMD FidelityFX CAS (MIT; the method:
// each output pixel blends the four input pixels round it, each sharpened against its four neighbours by a weight that
// falls where the local contrast is already high, so edges are not haloed and flat areas stay flat), as AstroQuest
// sharpens its pictures once a frame instead of the compositor's Super Resolution at every refresh. Written for this
// renderer: one pass from the eye picture's shown region into the swapchain image, 12 texel reads, reads clamped to
// the shown region; into an sRGB swapchain image the filter runs on approximately perceptual values (square root of
// the linear samples, squared back), as the SGSR1 pass does.
#include "vr_cas.h"

#include "pass.h"
#include "pipeline_trace.h"
#include "shaders.h"
#include "runtime.h"
#include "../../exception_report.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace gfxvk::vr_cas {
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
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D src;
layout(push_constant) uniform Params {
 vec2 inScale;   // input pixels per output pixel (the shown region over the output)
 ivec2 shownMax; // the shown region's last pixel
 float peak;     // -1 / mix(8, 5, sharpness)
 int perceptual;
 float edge;     // (a narrowed field of view) the rounded fade to black at the picture's edges, a share of it; 0: none
 int fxaa;       // (2026-11-12) edge smoothing before the sharpening: 1 on
} pc;
vec3 conv(vec3 c) { return pc.perceptual!=0?sqrt(max(c,vec3(0.0))):c; }
float luma(vec3 c) { return dot(c,vec3(0.299,0.587,0.114)); }
// a bilinear read of the source at p (source pixels), kept inside the shown region (past it the image holds an older,
// larger picture: the right and bottom edges smeared, scan_picture_extent)
vec3 bilin(vec2 p) {
 vec2 q=clamp(p,vec2(0.5),vec2(pc.shownMax)+0.5);
 return conv(textureLod(src,q/vec2(textureSize(src,0)),0.0).rgb);
}
vec3 tap(ivec2 p) { return conv(texelFetch(src,clamp(p,ivec2(0),pc.shownMax),0).rgb); }
// one input pixel sharpened against its cross (n, w, e, s): CAS's adaptive weight
vec3 cas(vec3 c,vec3 n,vec3 w,vec3 e,vec3 s) {
 vec3 mn=min(c,min(min(n,w),min(e,s)));
 vec3 mx=max(c,max(max(n,w),max(e,s)));
 vec3 amp=sqrt(clamp(min(mn,2.0-mx)/max(mx,vec3(1.0/65536.0)),0.0,1.0));
 vec3 wt=amp*pc.peak;
 return clamp((c+(n+w+e+s)*wt)/(1.0+4.0*wt),0.0,1.0);
}
void main() {
 vec2 ip=gl_FragCoord.xy*pc.inScale-0.5;
 ivec2 b=ivec2(floor(ip));
 vec2 f=ip-vec2(b);
 // the 4x4 neighbourhood less its corners: 12 reads
 vec3 c10=tap(b+ivec2(0,-1)),c20=tap(b+ivec2(1,-1));
 vec3 c01=tap(b+ivec2(-1,0)),c11=tap(b),c21=tap(b+ivec2(1,0)),c31=tap(b+ivec2(2,0));
 vec3 c02=tap(b+ivec2(-1,1)),c12=tap(b+ivec2(0,1)),c22=tap(b+ivec2(1,1)),c32=tap(b+ivec2(2,1));
 vec3 c13=tap(b+ivec2(0,2)),c23=tap(b+ivec2(1,2));
 vec3 s11=cas(c11,c10,c01,c21,c12);
 vec3 s21=cas(c21,c20,c11,c31,c22);
 vec3 s12=cas(c12,c11,c02,c22,c13);
 vec3 s22=cas(c22,c21,c12,c32,c23);
 vec3 color=mix(mix(s11,s21,f.x),mix(s12,s22,f.x),f.y);
 if(pc.fxaa!=0){
  // FXAA (the classic directional form): the edge's direction from the lumas round the output pixel, two to four
  // reads along it averaged; then CAS's sharpening added back except on the edge it smoothed, so the steps it took out
  // do not come back. In source pixels, at the output pixel's position in the source
  vec2 pos=gl_FragCoord.xy*pc.inScale;
  vec3 bil=mix(mix(c11,c21,f.x),mix(c12,c22,f.x),f.y);
  float lNW=luma(bilin(pos+vec2(-0.5,-0.5))),lNE=luma(bilin(pos+vec2(0.5,-0.5)));
  float lSW=luma(bilin(pos+vec2(-0.5,0.5))),lSE=luma(bilin(pos+vec2(0.5,0.5)));
  float lM=luma(bil);
  float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE))),lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));
  float range=lMax-lMin,threshold=max(0.0312,lMax*0.125);
  if(range>threshold){
   vec2 dir=vec2(-((lNW+lNE)-(lSW+lSE)),(lNW+lSW)-(lNE+lSE));
   float reduce=max((lNW+lNE+lSW+lSE)*0.03125,1.0/128.0);
   dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-8.0),vec2(8.0));
   vec3 a=0.5*(bilin(pos+dir*(1.0/3.0-0.5))+bilin(pos+dir*(2.0/3.0-0.5)));
   vec3 b4=a*0.5+0.25*(bilin(pos-dir*0.5)+bilin(pos+dir*0.5));
   float lB=luma(b4);
   vec3 smoothed=(lB<lMin||lB>lMax)?a:b4;
   float e=clamp((range-threshold)/(threshold*3.0),0.0,1.0);  // how much of an edge: no sharpening across it
   color=smoothed+(color-bil)*(1.0-e);
  }
 }
 color=pc.perceptual!=0?color*color:color;
 if(pc.edge>0.0){
  // a squircle (|x|^4 + |y|^4) round the picture: the view's edge fades instead of ending in a hard rectangle
  vec2 q=abs(gl_FragCoord.xy*pc.inScale/vec2(pc.shownMax+1)*2.0-1.0);
  float d=pow(pow(q.x,4.0)+pow(q.y,4.0),0.25);
  color*=1.0-smoothstep(1.0-pc.edge,1.0,d);
 }
 outColor=vec4(color,1.0);
}
)glsl";

struct Params {
    float inScale[2];
    int32_t shownMax[2];
    float peak;
    int32_t perceptual;
    float edge;
    int32_t fxaa;
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
        LOG("[vr cas] CAS shader failed to compile: %s", error.c_str());
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
        if (stutter::create_graphics(R.frame, "vr_cas_pipeline", R.device, R.pipelineCache, 1, &ci, nullptr, &result) !=
            VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(R.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(R.device, fs, nullptr);
    if (!result) {
        g_failed = true;
        LOG("[vr cas] CAS pipeline not made (format %u): eye pictures blitted", unsigned(format));
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
          float sharpness, float edge, bool fxaa) {
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
    const uint32_t sw = shown.width ? shown.width : source.extent.width;
    const uint32_t sh = shown.height ? shown.height : source.extent.height;
    Params params{};
    params.inScale[0] = float(sw) / float(extent.width);
    params.inScale[1] = float(sh) / float(extent.height);
    params.shownMax[0] = int32_t(sw) - 1;
    params.shownMax[1] = int32_t(sh) - 1;
    const float sharp = std::clamp(sharpness, 0.0f, 1.0f);
    params.peak = -1.0f / (8.0f + (5.0f - 8.0f) * sharp);
    // an sRGB target encodes linear output, so the samples are linear (as the blit copies them)
    params.perceptual = srgb(format) ? 1 : 0;
    params.edge = std::clamp(edge, 0.0f, 0.5f);
    params.fxaa = fxaa ? 1 : 0;
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    return true;
}

void forget_views() {
    if (!g_views.empty()) note_descriptor_targets_destroyed();
    for (auto& [image, view] : g_views)
        if (view) vkDestroyImageView(R.device, view, nullptr);
    g_views.clear();
}

}  // namespace gfxvk::vr_cas
