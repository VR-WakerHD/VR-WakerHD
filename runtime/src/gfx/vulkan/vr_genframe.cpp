// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR frame generation: vr_genframe.h. The warp is the per-pixel search of the desktop's generated frames (reproj.cpp
// kWarp, MPL-2.0, from Nycolazs/WindWakerHD-Android-Plus): for each pixel of the in-between picture, a few fixed-point
// steps find the real picture's pixel that lands there; fitted to a headset eye (its own asymmetric field of view, the
// in-between camera a full 6DoF move, depth by the camera projection's own terms), without the desktop's HUD (in VR
// the game's UI is a layer of its own) or the camera's follow rule (first person).
#include "vr_genframe.h"

#ifndef WWHD_VR
namespace gfxvk::vr_genframe {
bool draw(VkCommandBuffer, Surface&, uint32_t, uint32_t, Surface&, uint32_t, const Warp&, VkImage, VkFormat, uint32_t, uint32_t) {
    return false;
}
bool read_target(VkImage, VkFormat, uint32_t, uint32_t, std::vector<uint8_t>&) { return false; }
bool dilate(VkCommandBuffer, Surface&, VkImage, VkFormat, uint32_t, uint32_t, Surface*, Surface*, uint32_t) { return false; }
bool hands_coverage_from_depth() { return false; }
void copy_rows(VkCommandBuffer, VkImage, uint32_t, uint32_t, uint32_t, VkBuffer) {}
void forget_views() {}
bool prepare(VkFormat) { return false; }
}  // namespace gfxvk::vr_genframe
#else

#include "pass.h"
#include "pipeline_trace.h"
#include "shaders.h"
#include "runtime.h"

#include <cstdlib>
#include <string>
#include <unordered_map>

namespace gfxvk::vr_genframe {
namespace {

const char* kVertex = R"glsl(#version 450
layout(location=0) out vec2 uv;
void main() {
 vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
 uv=p;
 gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";

// ax, ay, az: the in-between camera's axes in the real camera's space, .w: its position there (x, y, z); tan: left,
// right, up, down; proj: p[10], p[11] (metres), halfZ, unused; rect: the shown part of the colour image (uv scale x, y),
// the depth's (uv scale x, y)
const char* kWarp = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D colorImg;
layout(set=0,binding=1) uniform sampler2D depthImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 ax; vec4 ay; vec4 az; vec4 tan; vec4 proj; vec4 rect; vec4 nq; vec4 no; } p;
// the view distance (metres) of the nearest of the 2x2 depth texels around q: outlines lie on the background's depth and
// would otherwise move with it (plain fetches: a gather on a depth-stencil image froze some drivers, reproj.cpp)
float viewDepth(vec2 q) {
 vec2 dq = q * p.rect.zw;
 ivec2 sz = textureSize(depthImg, 0), b = ivec2(floor(dq * vec2(sz) - 0.5));
 ivec2 lo = ivec2(0), hi = sz - 1;
 float d = min(min(texelFetch(depthImg, clamp(b, lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 0), lo, hi), 0).r),
               min(texelFetch(depthImg, clamp(b + ivec2(0, 1), lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 1), lo, hi), 0).r));
 float ndc = p.proj.z > 0.5 ? d : d * 2.0 - 1.0;
 return p.proj.y / min(ndc + p.proj.x, -1e-6);
}
// where the real picture's pixel q (uv, y down), at view distance z, lands in the in-between picture
vec2 toPicture(vec2 q, vec3 g) {
 if (g.z > -1e-4) return q;
 vec2 t = g.xy / -g.z;
 return vec2((t.x - p.tan.x) / (p.tan.y - p.tan.x), (t.y - p.tan.z) / (p.tan.w - p.tan.z));
}
vec3 viewPoint(vec2 q, float z) { return vec3(mix(p.tan.x, p.tan.y, q.x), mix(p.tan.z, p.tan.w, q.y), -1.0) * z; }
// the world's camera (the head, the stick's turn and the walk)
vec2 toGen(vec2 q, float z) {
 vec3 d = viewPoint(q, z) - vec3(p.ax.w, p.ay.w, p.az.w);
 return toPicture(q, vec3(dot(p.ax.xyz, d), dot(p.ay.xyz, d), dot(p.az.xyz, d)));
}
// the near field's camera (the head's own move only): its rotation's inverse applied (conjugate quaternion)
vec2 toNear(vec2 q, float z) {
 vec3 d = viewPoint(q, z) - p.no.xyz;
 vec3 u = -p.nq.xyz;
 vec3 t = 2.0 * cross(u, d);
 return toPicture(q, d + p.nq.w * t + cross(u, t));
}
float nearM() { return abs(p.no.w); }
// (Turn smoothing's frames: the world not warped, its turn in the submitted pose; nearMetres sent negative)
bool worldStill() { return p.no.w < 0.0; }
bool isNear(float z) { return z < nearM(); }
void main() {
 ivec2 csz = textureSize(colorImg, 0);
 float px2 = 4.0 / dot(vec2(csz), vec2(csz));  // (two pixels, squared, in uv)
 // (the world still, Turn smoothing, 2026-11-12: a headset run had the far view worse with this pass than without it,
 // its pixels the same: the pass's cost per frame. Most pixels are far and nothing near lands on them: two depth reads
 // decide it, the picture's own pixel then shown; the near search only where a near pixel is or may land)
 bool searchNear = nearM() > 0.0;
 if (worldStill() && searchNear) {
  float zu = viewDepth(uv);
  vec2 from = clamp(uv - (toNear(uv, nearM() * 0.5) - uv), vec2(0.0), vec2(1.0));
  if (!isNear(zu) && !isNear(viewDepth(from))) {
   result = vec4(texelFetch(colorImg, clamp(ivec2(uv * p.rect.xy * vec2(csz)), ivec2(0), csz - 1), 0).rgb, 1.0);
   if (p.proj.w > 2.5) result.rgb = vec3(clamp(log(max(zu, 0.1) / 0.1) / log(1000.0), 0.0, 1.0));
   return;
  }
 }
 // the near field first (in front of the world): the real picture's near pixel that the head's own move brings here
 if (searchNear) {
  vec2 n = uv;
  for (int i = 0; i < 4; i++) n = clamp(uv - (toNear(n, viewDepth(n)) - n), vec2(0.0), vec2(1.0));
  float zn = viewDepth(n);
  vec2 nm = toNear(n, zn) - uv;
  if (isNear(zn) && dot(nm, nm) <= px2) {
   result = vec4(texelFetch(colorImg, clamp(ivec2(n * p.rect.xy * vec2(csz)), ivec2(0), csz - 1), 0).rgb, 1.0);
   if (p.proj.w > 0.5 && p.proj.w < 1.5) result.rgb *= vec3(1.0, 0.55, 0.55);
   if (p.proj.w > 1.5 && p.proj.w < 2.5) result.rgb = mix(result.rgb, vec3(0.0, 1.0, 0.0), 0.6);
   if (p.proj.w > 2.5) result.rgb = vec3(clamp(log(max(zn, 0.1) / 0.1) / log(1000.0), 0.0, 1.0));
   return;
  }
 }
 // the world: near pixels searched through as if 10 m away (behind them is the world; their own distance would throw
 // the search off)
 vec2 q = uv;
 if (!worldStill())
  for (int i = 0; i < 4; i++) {
   float z = viewDepth(q);
   if (nearM() > 0.0 && isNear(z)) z = 10.0;
   q = clamp(uv - (toGen(q, z) - q), vec2(0.0), vec2(1.0));
  }
 // the world a near object uncovered (its pixel was never drawn: the search lands on the near object): the nearest
 // world pixel beside it, sideways first (the stick turns sideways), at growing distances
 if (nearM() > 0.0 && isNear(viewDepth(q))) {
  vec2 step = 1.0 / vec2(csz);
  vec2 found = q;
  bool got = false;
  for (int k = 1; k <= 7 && !got; k++) {
   float r = float(1 << k);  // 2 to 128 pixels
   vec2 c[4] = vec2[4](q + vec2(r, 0.0) * step, q - vec2(r, 0.0) * step, q + vec2(0.0, r) * step, q - vec2(0.0, r) * step);
   for (int j = 0; j < 4 && !got; j++) {
    vec2 cc = clamp(c[j], vec2(0.0), vec2(1.0));
    if (!isNear(viewDepth(cc))) { found = cc; got = true; }
   }
  }
  q = found;
 }
 // stage 4, the edges: where the search lands on a near pixel whose own move does not bring it here (the background
 // a near object uncovers: its pixel was never drawn), the farther of the two pixels either side along the move
 float z = viewDepth(q);
 vec2 miss = toGen(q, z) - uv;
 if (dot(miss, miss) > px2 && !(nearM() > 0.0 && isNear(z))) {
  vec2 dir = normalize(miss + vec2(1e-6));
  vec2 px = 3.0 / vec2(textureSize(colorImg, 0));
  vec2 a = clamp(q + dir * px, vec2(0.0), vec2(1.0)), b = clamp(q - dir * px, vec2(0.0), vec2(1.0));
  q = viewDepth(a) > viewDepth(b) ? a : b;
 }
 ivec2 sz = textureSize(colorImg, 0);
 result = vec4(texelFetch(colorImg, clamp(ivec2(q * p.rect.xy * vec2(sz)), ivec2(0), sz - 1), 0).rgb, 1.0);
 if (p.proj.w > 0.5 && p.proj.w < 1.5) result.rgb *= vec3(1.0, 0.55, 0.55);  // (WWHD_VR_GENFRAME_TINT=1: tinted red)
 // (WWHD_VR_GENFRAME_DEBUG=1: what lies within 1 m, the near field, green; =2: the view distance, grey from 0.1 m
 // (black) to 100 m (white))
 if (p.proj.w > 1.5 && p.proj.w < 2.5 && viewDepth(q) < 1.0) result.rgb = mix(result.rgb, vec3(0.0, 1.0, 0.0), 0.6);
 if (p.proj.w > 2.5) result.rgb = vec3(clamp(log(max(viewDepth(q), 0.1) / 0.1) / log(1000.0), 0.0, 1.0));
}
)glsl";

// (the hands layer, dilate) its covered pixels kept, the transparent ones near them given their colour (alpha 0)
// (2026-11-13, "Link's sword clips through objects instead of hitting them") his layer is shown over the whole world, so
// what the world draws after his lists (pots, grass, enemies) never hid him: his pixels where the depth at the frame's
// end (the world's) is nearer than his own are left out, as the game's depth test would have hidden them
const char* kDilate = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D colorImg;
layout(set=0,binding=1) uniform sampler2D linkDepth;
layout(set=0,binding=2) uniform sampler2D worldDepth;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 a; } p;
ivec2 sz;
vec4 covered(ivec2 q) {
 vec4 c = texelFetch(colorImg, q, 0);
 if (c.a > 0.0 && p.a.x > 0.5) {
  ivec2 dz = textureSize(linkDepth, 0);
  ivec2 d = clamp(ivec2((vec2(q) + 0.5) * vec2(dz) / vec2(sz)), ivec2(0), dz - 1);
  float him = texelFetch(linkDepth, d, 0).r, world = texelFetch(worldDepth, d, 0).r;
  if (world < him - 1e-6) c.a = 0.0;
 }
 return c;
}
void main() {
 sz = textureSize(colorImg, 0);
 ivec2 q = clamp(ivec2(uv * vec2(sz)), ivec2(0), sz - 1);
 // (coverage from depth, premultiplied: his pixels where his depth is nearer than the world's are opaque; the rest of
 // his image (his glows over the empty background) is added onto the world; the image's own alpha is the game's)
 if (p.a.z > 0.5) {
  vec4 t = texelFetch(colorImg, q, 0);
  ivec2 dz = textureSize(linkDepth, 0);
  ivec2 d = clamp(ivec2((vec2(q) + 0.5) * vec2(dz) / vec2(sz)), ivec2(0), dz - 1);
  float him = texelFetch(linkDepth, d, 0).r, world = texelFetch(worldDepth, d, 0).r;
  if (him < world - 1e-6) { result = vec4(t.rgb, 1.0); return; }
  if (him < 1.0 && him > world + 1e-6) { result = vec4(0.0); return; }  // (behind what the world drew after him)
  result = vec4(t.rgb, 0.0);
  return;
 }
 vec4 c = covered(q);
 if (p.a.y > 0.5) { result = vec4(c.rgb * c.a, c.a); return; }  // (premultiplied: no edge fill, WWHD_VR_HANDS_SUBMIT=premult)
 if (c.a > 0.0) { result = c; return; }
 for (int r = 1; r <= 3; r++)
  for (int dy = -r; dy <= r; dy++)
   for (int dx = -r; dx <= r; dx++) {
    if (max(abs(dx), abs(dy)) != r) continue;
    vec4 n = covered(clamp(q + ivec2(dx, dy), ivec2(0), sz - 1));
    if (n.a > 0.0) { result = vec4(n.rgb, 0.0); return; }
   }
 result = vec4(0.0);
}
)glsl";
std::unordered_map<uint32_t, VkPipeline> g_dilatePipelines;  // by format

VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
std::unordered_map<uint32_t, VkPipeline> g_pipelines;  // by format
std::unordered_map<VkImage, VkImageView> g_targets;   // the swapchain images' views
struct SourceView { VkImage image = VK_NULL_HANDLE; uint32_t layer = 0; VkImageView view = VK_NULL_HANDLE; };
SourceView g_sources[3];  // [0 colour, 1 depth, 2 a second depth (the hands layer)]
bool g_failed = false;

VkShaderModule module(const char* source, bool vertex) {
    std::string error;
    auto words = vk::compile_glsl(source, vertex, &error);
    if (words.empty() || !error.empty()) {
        LOG("[vr frame generation] a shader failed to compile: %s", error.c_str());
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
    // (binding 2: the hands layer's second depth, dilate; the warp leaves it unwritten and unused)
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 3;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &g_setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};  // (128: every Vulkan device's least)
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(R.device, &pl, nullptr, &g_layout) != VK_SUCCESS) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    return vkCreateSampler(R.device, &si, nullptr, &g_sampler) == VK_SUCCESS;
}

VkPipeline pipeline(VkFormat format, bool dilatePass = false) {
    auto& cache = dilatePass ? g_dilatePipelines : g_pipelines;
    if (auto it = cache.find(uint32_t(format)); it != cache.end()) return it->second;
    VkShaderModule vs = module(kVertex, true);
    VkShaderModule fs = vs ? module(dilatePass ? kDilate : kWarp, false) : VK_NULL_HANDLE;
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
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &bs;
        ci.pDynamicState = &dy;
        ci.layout = g_layout;
        if (stutter::create_graphics(R.frame, "vr_genframe_pipeline", R.device, R.pipelineCache, 1, &ci, nullptr, &result) !=
            VK_SUCCESS)
            result = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(R.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(R.device, fs, nullptr);
    if (!result) {
        if (!dilatePass) g_failed = true;
        LOG("[vr frame generation] a pipeline was not made (format %u, %s)", unsigned(format), dilatePass ? "the hands' edges" : "generated frames off for the run");
        return VK_NULL_HANDLE;
    }
    cache.emplace(uint32_t(format), result);
    return result;
}

VkImageView target_view(VkImage image, VkFormat format) {
    VkImageView& view = g_targets[image];
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

// a sampled view of a surface's layer, made again when the surface's image changes (the old one destroyed once the GPU is
// past it), as vr_framesynth.cpp source_view
VkImageView source_view(int role, Surface& s, uint32_t slice) {
    SourceView& d = g_sources[role];
    const uint32_t layer = s.baseLayer + slice;
    if (d.view && d.image == s.image && d.layer == layer) return d.view;
    if (d.view) defer_surface_image(VK_NULL_HANDLE, VK_NULL_HANDLE, {d.view});
    d = {};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s.fmt.pixel;
    vi.subresourceRange = {role >= 1 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, layer, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.view) != VK_SUCCESS) return VK_NULL_HANDLE;
    d.image = s.image;
    d.layer = layer;
    return d.view;
}

// 0 normal; 1 tinted red (WWHD_VR_GENFRAME_TINT=1); 2 the near field green, 3 the depth grey (WWHD_VR_GENFRAME_DEBUG=1, 2)
float debug_mode() {
    static const float mode = [] {
        const char* d = std::getenv("WWHD_VR_GENFRAME_DEBUG");
        if (d && (*d == '1' || *d == '2')) return *d == '1' ? 2.0f : 3.0f;
        const char* t = std::getenv("WWHD_VR_GENFRAME_TINT");
        return t && *t == '1' ? 1.0f : 0.0f;
    }();
    return mode;
}

}  // namespace

bool draw(VkCommandBuffer cmd, Surface& color, uint32_t shownWidth, uint32_t shownHeight, Surface& depth, uint32_t slice,
          const Warp& w, VkImage target, VkFormat format, uint32_t width, uint32_t height) {
    if (g_failed || !ensure_layout() || !color.image || !depth.image || !depth.fmt.depth ||
        !(depth.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !color.extent.width || !color.extent.height)
        return false;
    VkPipeline p = pipeline(format);
    VkImageView view = p ? target_view(target, format) : VK_NULL_HANDLE;
    transition_image(&color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    transition_image(&depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    VkImageView colorView = source_view(0, color, 0), depthView = source_view(1, depth, slice);
    if (!view || !colorView || !depthView) return false;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_setLayout;
    if (vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo info[2] = {{g_sampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {g_sampler, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(R.device, 2, writes, 0, nullptr);
    // acquired images are in COLOR_ATTACHMENT_OPTIMAL (OpenXR) and must be again at release: only this pass's writes
    // need ordering
    const VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = layout;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view;
    attachment.imageLayout = layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // (every pixel is written)
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {width, height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    begin_pass(cmd, rendering, [&] { return color_pass(format, layout); });
    pass_state().rendering = true;
    pass_state().tracked = false;
    const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    const VkRect2D scissor{{0, 0}, {width, height}};
    record::vkCmdSetViewport(cmd, 0, 1, &viewport);
    record::vkCmdSetScissor(cmd, 0, 1, &scissor);
    record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, nullptr);
    const float params[32] = {
        w.axisX[0], w.axisX[1], w.axisX[2], w.origin[0],
        w.axisY[0], w.axisY[1], w.axisY[2], w.origin[1],
        w.axisZ[0], w.axisZ[1], w.axisZ[2], w.origin[2],
        w.tanLeft, w.tanRight, w.tanUp, w.tanDown,
        w.projA, w.projB, w.halfZ ? 1.0f : 0.0f, debug_mode(),
        float(shownWidth) / float(color.extent.width), float(shownHeight) / float(color.extent.height), 1.0f, 1.0f,
        w.nearRotation[0], w.nearRotation[1], w.nearRotation[2], w.nearRotation[3],
        w.nearOrigin[0], w.nearOrigin[1], w.nearOrigin[2], w.nearMetres,
    };
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof params, params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    return true;
}

bool prepare(VkFormat format) { return !g_failed && ensure_layout() && pipeline(format); }

bool hands_coverage_from_depth() {
    static const bool on = [] {
        const char* e = std::getenv("WWHD_VR_HANDS_COVERAGE");
        const bool v = !(e && *e == '0');
        LOG("[vr hands] his layer's coverage: %s", v ? "from depth, sent premultiplied (the game's alpha kept)"
                                                     : "his image's alpha, sent unpremultiplied (WWHD_VR_HANDS_COVERAGE=0)");
        return v;
    }();
    return on;
}

bool dilate(VkCommandBuffer cmd, Surface& src, VkImage target, VkFormat format, uint32_t width, uint32_t height,
            Surface* linkDepth, Surface* worldDepth, uint32_t dstX) {
    if (!ensure_layout() || !src.image || !width || !height) return false;
    VkPipeline p = pipeline(format, true);
    VkImageView view = p ? target_view(target, format) : VK_NULL_HANDLE;
    if (!view) return false;
    transition_image(&src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkImageView srcView = source_view(0, src, 0);
    if (!srcView) return false;
    const bool occlude = linkDepth && worldDepth && linkDepth->image && worldDepth->image;
    VkImageView linkView = srcView, worldView = srcView;
    if (occlude) {
        transition_image(linkDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        transition_image(worldDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        linkView = source_view(1, *linkDepth, 0);
        worldView = source_view(2, *worldDepth, 0);
        if (!linkView || !worldView) return false;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_setLayout;
    if (vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return false;
    // (no depths: colour views stand in for them, unread with the push constant 0; their layout is the colour's)
    VkDescriptorImageInfo info[3] = {{g_sampler, srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {g_sampler, linkView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {g_sampler, worldView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(R.device, 3, writes, 0, nullptr);
    const VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = layout;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view;
    attachment.imageLayout = layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.offset = {int32_t(dstX), 0};
    rendering.renderArea.extent = {width, height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    begin_pass(cmd, rendering, [&] { return color_pass(format, layout); });
    pass_state().rendering = true;
    pass_state().tracked = false;
    const VkViewport viewport{float(dstX), 0, float(width), float(height), 0, 1};
    const VkRect2D scissor{{int32_t(dstX), 0}, {width, height}};
    record::vkCmdSetViewport(cmd, 0, 1, &viewport);
    record::vkCmdSetScissor(cmd, 0, 1, &scissor);
    record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, nullptr);
    float params[32] = {};
    params[0] = occlude ? 1.0f : 0.0f;
    static const bool premult = [] { const char* e = std::getenv("WWHD_VR_HANDS_SUBMIT"); return e && !std::strcmp(e, "premult"); }();
    // (2026-11-14, a PS VR2 run at 45 fps in the Tower of the Gods with a video: blinking, rainbow hands, the rope
    // flickering) with coverage from depth the layer is sent premultiplied (set_hands_premultiplied); a frame without his
    // depth (occlude false) fell back to the alpha path's unpremultiplied colour under that flag: its edges and glows came
    // out brighter or another colour for that frame. Such a frame is premultiplied by its alpha now, as the flag says
    params[1] = premult || (!occlude && hands_coverage_from_depth()) ? 1.0f : 0.0f;
    params[2] = occlude && hands_coverage_from_depth() ? 1.0f : 0.0f;
    if (!occlude && hands_coverage_from_depth()) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1) < 5) LOG("[vr hands] a frame without his depth: his layer from its alpha, premultiplied");
    }
    record::vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof params, params);
    record::vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass(cmd);
    pass_state().rendering = false;
    pass_state().tracked = false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG("[vr hands] his layer's pixels behind what the world drew after him left out: %s", occlude ? "on" : "off (no depths)");
    }
    return true;
}

void copy_rows(VkCommandBuffer cmd, VkImage target, uint32_t width, uint32_t y0, uint32_t rows, VkBuffer buffer) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, int32_t(y0), 0};
    region.imageExtent = {width, rows, 1};
    record::vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
}
bool read_target(VkImage target, VkFormat format, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba) {
    const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!(bgra || format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM) || !width || !height) return false;
    const size_t count = size_t(width) * height;
    Buffer buffer = create_readback_buffer(count * 4);
    if (!buffer.mapped) {
        defer_buffer(buffer);
        return false;
    }
    end_encoder();
    auto cmd = command_buffer();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    record::vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
    // back to the layout OpenXR takes it in at release
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
    derive_dependency(buffer.use, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    transition_buffer(buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    flush();
    const auto* raw = static_cast<const uint8_t*>(buffer.mapped);
    rgba.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = raw + i * 4;
        rgba[i * 4 + 0] = bgra ? p[2] : p[0];
        rgba[i * 4 + 1] = p[1];
        rgba[i * 4 + 2] = bgra ? p[0] : p[2];
        rgba[i * 4 + 3] = 255;
    }
    defer_buffer(buffer);
    return true;
}

void forget_views() {
    for (auto& [image, view] : g_targets)
        if (view) vkDestroyImageView(R.device, view, nullptr);
    g_targets.clear();
}

}  // namespace gfxvk::vr_genframe
#endif
