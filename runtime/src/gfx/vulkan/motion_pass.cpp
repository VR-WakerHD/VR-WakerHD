// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The motion pass: motion_pass.h.
#include "motion_pass.h"
#include "motion_history.h"

#include "deferred_dispatch.h"
#include "mobile_gpu.h"
#include "motion_pipeline.h"
#include "ngx_vk.h"
#include "pass.h"
#include "present.h"
#include "record_worker.h"
#include "runtime.h"
#include "stereo.h"
#include "vr/vr_framesynth.h"
#include "stereo_switches.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace gfxvk::motion_pass {
namespace {

std::mutex g_mutex;
std::vector<Packet> g_packets;
std::unique_ptr<Surface> g_targets[2];  // each eye's motion target
// (stage D4, WWHD_VK_DLSS_EVAL=1) each eye's DLSS output (a storage image the scan's size) and depth view
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
Image g_outputs[2];
Image g_nrOutputs[2];  // (stage D5) each eye's DLSS 5 output: the render size, Super Resolution's colour then
// DLSS's own views of its inputs (a render target's own views may be made for attachment use only on this device:
// attachment_view_usage.h, and DLSS samples them): per eye, the colour, the depth's depth aspect, the motion
struct View { VkImage image = VK_NULL_HANDLE; uint32_t layer = 0; VkImageView view = VK_NULL_HANDLE; };
View g_views[2][3];
uint64_t g_evaluated = 0, g_skipped = 0;
std::atomic<uint64_t> g_packetNs{0};  // (diagnostics) packet building, all threads
uint64_t g_recordNs = 0, g_dlssNs = 0;  // (diagnostics) the pass's recording, DLSS's (drain included), render thread
// (stage D3) this frame's jitter (applied to the scene's draws while it is drawn) and its place in the sequence
float g_jitter[2] = {0, 0};
uint32_t g_jitterIndex = 0;
float halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    for (; i; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}
// DLSS runs: the settings' DLSS mode is offered (ngx_vk::active_mode), or DLSS 5 is chosen (ngx_vk::neural_wanted: with
// DLAA when no mode is), or WWHD_VK_DLSS_EVAL=1 (tests: DLAA); WWHD_VK_DLSS_EVAL=0 never
bool dlss_eval() {
    static const bool v = [] {
        const char* e = std::getenv("WWHD_VK_DLSS_EVAL");
        if (e) return !std::strcmp(e, "1");
        return ngx_vk::active_mode() != ngx::Mode::Off || ngx_vk::neural_wanted();
    }();
    return v;
}
ngx::Mode dlss_mode() {
    const ngx::Mode m = ngx_vk::active_mode();
    return m == ngx::Mode::Off ? ngx::Mode::Dlaa : m;  // (WWHD_VK_DLSS_EVAL=1 with nothing chosen: DLAA)
}
uint64_t g_frames = 0, g_drawn = 0, g_withPrevious = 0, g_passes = 0, g_otherDepth = 0, g_why[4] = {};

float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v = exp == 0 ? std::ldexp(float(man), -24) : exp == 31 ? (man ? NAN : INFINITY)
                                                                  : std::ldexp(float(man | 1024), int(exp) - 25);
    return sign ? -v : v;
}

}  // namespace
EyeResult g_results[2];  // this frame's (record): the frame synthesis export reads them
namespace {
// each eye's DLSS after both eyes' motion passes (record): what it runs on
struct Later { int eye; Surface* depth; uint32_t slice; Surface* motion; bool flipsY; };

#ifdef WWHD_MOBILE_GPU_PATHS
// (mobile GPUs) both eyes' targets in one two-layer image (two_layer_targets): eye 1's is its second layer. Both go
// together; the image is its owner's (eye 0's)
bool paired_targets() { return g_targets[0] && (g_targets[0]->layerTwin || (g_targets[1] && g_targets[1]->layerOwner)); }
void retire_pair() {
    Surface* t0 = g_targets[0].get();
    Surface* t1 = g_targets[1].get();
    std::vector<VkImageView> views;
    for (Surface* t : {t0, t1}) {
        if (!t) continue;
        for (VkImageView v : t->layerViews) if (v) views.push_back(v);
        if (t->view) views.push_back(t->view);
        for (auto& [key, v] : t->sampledViews) if (v) views.push_back(v);
    }
    defer_surface_image(t0 ? t0->image : VK_NULL_HANDLE, t0 ? t0->memory : VK_NULL_HANDLE, std::move(views));
    g_targets[0].reset();
    g_targets[1].reset();
}
#endif

// the eye's motion target, the size of its scene depth (made again when that changes)
Surface* target(int eye, const Surface& depth) {
    auto& t = g_targets[eye];
    if (t && t->image && t->extent.width == depth.extent.width && t->extent.height == depth.extent.height) return t.get();
#ifdef WWHD_MOBILE_GPU_PATHS
    if (paired_targets()) retire_pair();  // (a pair's image is shared: both go; the other eye's is made again too)
#endif
    if (t && t->image) {
        std::vector<VkImageView> views = t->layerViews;
        if (t->view) views.push_back(t->view);
        defer_surface_image(t->image, t->memory, std::move(views));
    }
    t = std::make_unique<Surface>();
    t->width = depth.extent.width;
    t->height = depth.extent.height;
    t->scale = 1;
    t->format = 1;  // (any non-zero guest format: the Vulkan format is set directly)
    t->fmt.pixel = vk::motion::kMotionFormat;
    create_surface_image(t.get(), true, {depth.extent.width, depth.extent.height, 1});
    return t.get();
}

#ifdef WWHD_MOBILE_GPU_PATHS
// (mobile GPUs: docs/VR_DLSS.md, "Both eyes in one motion pass") both eyes' motion targets in one image of two layers,
// the size of the scene depth: eye 0's surface owns it (layer 0), eye 1's is its second layer (as a two-eye render
// target's: surfaces.cpp link_eye1_layers), so a multiview pass writes both and each eye's readers see their own
bool two_layer_targets(const Surface& depth) {
    Surface* t0 = g_targets[0].get();
    if (paired_targets() && t0->image && t0->extent.width == depth.extent.width && t0->extent.height == depth.extent.height)
        return true;
    if (paired_targets()) retire_pair();
    for (int eye = 0; eye < 2; ++eye) {
        auto& t = g_targets[eye];
        if (t && t->image) {
            std::vector<VkImageView> views = t->layerViews;
            if (t->view) views.push_back(t->view);
            defer_surface_image(t->image, t->memory, std::move(views));
        }
        t = std::make_unique<Surface>();
        t->width = depth.extent.width;
        t->height = depth.extent.height;
        t->scale = 1;
        t->format = 1;  // (any non-zero guest format: the Vulkan format is set directly)
        t->fmt.pixel = vk::motion::kMotionFormat;
        t->extent = {depth.extent.width, depth.extent.height, 1};
        t->sx = t->sy = 1;
        t->mips = 1;
        t->arrayLayers = 1;
        t->imageLayers = 2;
        t->baseLayer = uint32_t(eye);
        t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        t->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        t->createFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->use = {};
    }
    t0 = g_targets[0].get();
    Surface* t1 = g_targets[1].get();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = t0->createFlags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = t0->fmt.pixel;
    ci.extent = t0->extent;
    ci.mipLevels = 1;
    ci.arrayLayers = 2;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = t0->usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto failed = [&] {
        if (t0->image) vkDestroyImage(R.device, t0->image, nullptr);
        if (t0->memory) vkFreeMemory(R.device, t0->memory, nullptr);
        g_targets[0].reset();
        g_targets[1].reset();
        return false;
    };
    if (vkCreateImage(R.device, &ci, nullptr, &t0->image) != VK_SUCCESS) return failed();
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(R.device, t0->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(R.device, &ai, nullptr, &t0->memory) != VK_SUCCESS) return failed();
    t0->allocationBytes = req.size;
    vkBindImageMemory(R.device, t0->image, t0->memory, 0);
    t1->image = t0->image;
    t0->layerTwin = t1;
    t1->layerOwner = t0;
    for (Surface* t : {t0, t1}) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = t->fmt.pixel;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t->baseLayer, 1};
        if (vkCreateImageView(R.device, &vi, nullptr, &t->view) != VK_SUCCESS) {
            retire_pair();
            return false;
        }
    }
    return true;
}
// a depth both eyes' combined packets can be drawn against: eye 0's half of a two-eye render target, eye 1's its twin
bool two_eye_depth(const Surface* d0, uint32_t s0, const Surface* d1, uint32_t s1) {
    return d0 && d1 && d0->image && d0->layerTwin == d1 && d1->layerOwner == d0 && !d0->baseLayer &&
           d0->arrayLayers == 1 && d0->imageLayers == 2 && d0->imageType == VK_IMAGE_TYPE_2D && s0 == 0 && s1 == 0;
}
#endif

// (diagnostics) a motion target read back: written as a PNG (|x| red, |y| green, scaled so 1% of the picture is
// full), and its largest and mean motion logged
void dump(Surface* t, int eye, uint64_t frame) {
    const uint32_t w = t->extent.width, h = t->extent.height;
    Buffer buffer = create_readback_buffer(size_t(w) * h * 4);
    transition_image(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->baseLayer, 1};  // (a pair's eye 1: its layer)
    region.imageExtent = {w, h, 1};
    record::vkCmdCopyImageToBuffer(command_buffer(), t->image, t->layout, buffer.buffer, 1, &region);
    derive_dependency(buffer.use, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    transition_buffer(buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    flush();
    const auto* raw = static_cast<const uint16_t*>(buffer.mapped);
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    double sum = 0;
    float largest = 0;
    uint64_t moving = 0;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const float x = half_to_float(raw[i * 2]), y = half_to_float(raw[i * 2 + 1]);
        const float m = std::sqrt(x * x + y * y);
        if (std::isfinite(m)) {
            sum += m;
            largest = std::max(largest, m);
            moving += m > 1e-5f;
        }
        rgba[i * 4 + 0] = uint8_t(std::min(255.0f, std::fabs(x) * 25500.0f));
        rgba[i * 4 + 1] = uint8_t(std::min(255.0f, std::fabs(y) * 25500.0f));
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255;
    }
    defer_buffer(buffer);
    const std::string path = "motion_" + std::to_string(frame) + "_eye" + std::to_string(eye) + ".png";
    try {
        write_rgba_png(path, w, h, rgba);
    } catch (const std::exception& e) {
        LOG("[motion] dump failed: %s", e.what());
    }
    LOG("[motion] frame %llu eye %d: %ux%u, %.1f%% of pixels moving, mean %.5f, largest %.4f (of the picture); %s",
        (unsigned long long)frame, eye, w, h, 100.0 * double(moving) / double(size_t(w) * h), sum / double(size_t(w) * h),
        double(largest), path.c_str());
}

// the scan's format as a storage image can hold it (sRGB formats are not storage formats: their linear twin, the same bytes)
VkFormat storage_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}
bool output_image(Image& o, VkFormat format, uint32_t w, uint32_t h) {
    if (o.image && o.format == format && o.width == w && o.height == h) return true;
    if (o.image) defer_surface_image(o.image, o.memory, {o.view});
    o = {};
    if (!(format_properties(format).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(R.device, &ci, nullptr, &o.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(R.device, o.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(R.device, &ai, nullptr, &o.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(R.device, o.image, o.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &o.view) != VK_SUCCESS) return false;
    o.format = format;
    o.width = w;
    o.height = h;
    return true;
}
// a plain 2D view of one layer of a surface (role: 0 colour, 1 depth's depth aspect, 2 motion)
VkImageView input_view(int eye, int role, Surface* s, uint32_t slice) {
    View& d = g_views[eye][role];
    const uint32_t layer = s->baseLayer + slice;
    if (d.view && d.image == s->image && d.layer == layer) return d.view;
    if (d.view) defer_surface_image(VK_NULL_HANDLE, VK_NULL_HANDLE, {d.view});
    d = {};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s->fmt.pixel;
    vi.subresourceRange = {role == 1 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, 1, layer, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.view) != VK_SUCCESS) return VK_NULL_HANDLE;
    d.image = s->image;
    d.layer = layer;
    return d.view;
}
void image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(command_buffer(), srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}
// (stage D4, WWHD_VK_DLSS_EVAL=1) DLAA on the eye's picture: its scan (colour after the game's post-processing), the
// scene depth and this frame's motion in, a storage image out, copied back into the scan (the picture shown)
void run_dlss(int eye, Surface* depth, uint32_t slice, Surface* motion, bool reset, bool flipsY) {
    Screen& screen = eye ? stereoRight : R.tv;
    // this frame's picture: the scan being built (swap() makes it the shown one right after this), else the shown one;
    // DLSS reads the colour buffer it was resampled from (the internal size, like the depth and the motion) and writes
    // the scan at its full size (an upscaling mode: larger; DLAA: the same)
    const bool building = screen.buildingScanDirty && screen.buildingScan;
    Surface* scan = building ? screen.buildingScan.get() : screen.scan.get();
    Surface* source = building && screen.scanSource && screen.scanSource->image ? screen.scanSource : scan;
    if (!scan || !scan->image || !source || source->extent.width != motion->extent.width ||
        source->extent.height != motion->extent.height || source->fmt.depth) {
        ++g_skipped;
        return;
    }
    const uint32_t w = source->extent.width, h = source->extent.height;              // render size
    const uint32_t ow = scan->extent.width, oh = scan->extent.height;                // output size
    const VkFormat outFormat = storage_format(scan->fmt.pixel);
    const VkImageView cview = input_view(eye, 0, source, 0), dview = input_view(eye, 1, depth, slice),
                      mview = input_view(eye, 2, motion, 0);
    if (!cview || !dview || !mview || !output_image(g_outputs[eye], outFormat, ow, oh)) {
        ++g_skipped;
        return;
    }
    Image& o = g_outputs[eye];
    transition_image(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    transition_image(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    transition_image(motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    image_barrier(o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
    ngx_vk::DlssEval e;
    e.eye = eye;
    e.mode = dlss_mode();
    e.color = {source->image, cview, source->fmt.pixel, w, h};
    e.depth = {depth->image, dview, depth->fmt.pixel, w, h, VK_IMAGE_ASPECT_DEPTH_BIT};
    e.motion = {motion->image, mview, motion->fmt.pixel, w, h};
    e.output = {o.image, o.view, o.format, ow, oh};
    e.mvScaleX = float(w);  // (the motion target holds a share of the picture along clip space's axes)
    e.mvScaleY = motion::motion_scale_y(h, flipsY);  // (y down in the picture where the viewports flip it)
    static bool loggedFlip[2] = {};
    if (!loggedFlip[eye]) {
        loggedFlip[eye] = true;
        LOG("[motion] eye %d: the scene's viewports %s y: DLSS's motion scale %.0f x %.0f", eye,
            flipsY ? "flip" : "keep", double(e.mvScaleX), double(e.mvScaleY));
    }
    // the jitter this frame's scene was drawn with. The motion vectors hold none of it: they are the difference of two
    // clip positions in the vertex shader, and the jitter is the viewport's, applied after it (so not MVJittered)
    e.jitterX = g_jitter[0];
    e.jitterY = g_jitter[1];
    e.mvJittered = false;
    // a new history: this frame's view does not follow from the previous one (record: a cut, a new scene)
    e.reset = reset;
    // (stage D5) DLSS 5 first, on the eye's picture at the render size (the same size as its depth and motion): its
    // output is then Super Resolution's colour. When it does not run (it failed, or the driver does not serve it) Super
    // Resolution reads the picture itself
    struct Both { ngx_vk::DlssEval sr; ngx_vk::NrEval nr; bool neural; } both{e, {}, false};
    if (ngx_vk::neural_wanted() && output_image(g_nrOutputs[eye], storage_format(source->fmt.pixel), w, h)) {
        Image& n = g_nrOutputs[eye];
        image_barrier(n.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
        const ngx::NrSettings settings = ngx_vk::nr_current();  // (live: F1 > Graphics)
        both.nr.eye = eye;
        both.nr.color = e.color;
        both.nr.depth = e.depth;
        both.nr.motion = e.motion;
        both.nr.output = {n.image, n.view, n.format, w, h};
        both.nr.mvScaleX = e.mvScaleX;
        both.nr.mvScaleY = e.mvScaleY;
        both.nr.reset = reset;
        both.nr.settings = settings;
        both.neural = true;
    }
    const VkCommandBuffer cmd = command_buffer();
    // NGX records into the command buffer itself: a call in the command stream, run in order by the thread that
    // records the command buffer (the record worker when it runs: a DLSS call beside it, from this thread, crashed in
    // the driver; draining the worker first cost 0.8 ms a frame)
    static_assert(std::is_trivially_copyable_v<Both>);
    record::call([](VkCommandBuffer c, const void* bytes) {
        Both copy;
        std::memcpy(&copy, bytes, sizeof copy);
        if (copy.neural && ngx_vk::evaluate_nr(c, copy.nr)) {
            // DLSS 5's writes before Super Resolution reads them
            VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                                 0, nullptr, 0, nullptr);
            copy.sr.color = copy.nr.output;
        }
        ngx_vk::evaluate(c, copy.sr);
    }, cmd, &both, uint32_t(sizeof both));
    image_barrier(o.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    transition_image(scan, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, scan->baseLayer, 1};
    copy.extent = {ow, oh, 1};
    record::vkCmdCopyImage(cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scan->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // the mod HUD overlays (climb stamina, boost bar) over DLSS's picture: copy_to_scan leaves them out while DLSS runs
    if (building) draw_mod_overlay(*scan, screen.buildingScanDisplayExtent);
    ++g_evaluated;
}

}  // namespace


bool eye_result(int eye, EyeResult& out) {
    if (eye < 0 || eye > 1 || !g_results[eye].valid) return false;  // (reset at each record: this frame's)
    out = g_results[eye];
    return true;
}

bool on() {
    static const bool v = [] {
        // on with WWHD_VK_MOTION_PASS=1, or whenever DLSS runs (=0: never); only with a DLSS feature chosen
        const char* e = std::getenv("WWHD_VK_MOTION_PASS");
        // (or for the runtime's frame synthesis: docs/VR_FRAME_SYNTHESIS.md, vr/vr_framesynth.h)
        const bool asked = e ? !std::strcmp(e, "1") : dlss_eval() || vr::framesynth::motion_wanted();
        if (!asked || !ngx_vk::motion_wanted()) return false;
        if (R.renderPasses) {
            LOG("[motion] pass: off (render passes, not dynamic rendering)");
            return false;
        }
        LOG("[motion] pass: on (the scene's depth-writing draws again into each eye's motion target)");
        return true;
    }();
    return v;
}

void note_packet_ns(uint64_t ns) { g_packetNs.fetch_add(ns, std::memory_order_relaxed); }
std::atomic<uint64_t> g_still{0};
void note_still() { g_still.fetch_add(1, std::memory_order_relaxed); }
std::atomic<bool> g_cut{false};
uint64_t g_cuts = 0, g_sceneCuts = 0, g_background = 0;  // (diagnostics) render thread
#ifdef WWHD_MOBILE_GPU_PATHS
uint64_t g_bothEyeFrames = 0;  // (mobile GPUs, diagnostics) frames record_both_eyes drew
#endif
void note_cut() { g_cut.store(true, std::memory_order_relaxed); }
std::atomic<bool> g_needed{true};
void set_needed(bool needed) {
    const bool was = g_needed.exchange(needed, std::memory_order_relaxed);
    if (was == needed) return;
    if (needed) note_cut();  // (its last frame's motion is not this frame's previous one)
    LOG("[motion] pass %s (SGSR2, frame synthesis or DLSS %s)", needed ? "resumed" : "paused", needed ? "uses it again" : "does not use it now");
}
bool active() { return on() && g_needed.load(std::memory_order_relaxed); }
bool background_on() {
    static const bool on = [] {
        const char* e = std::getenv("WWHD_VK_MOTION_BACKGROUND");
        return !(e && e[0] == '0');
    }();
    return on;
}

void add(const Packet& p) {
    std::lock_guard lock(g_mutex);
    g_packets.push_back(p);
}

#ifdef WWHD_MOBILE_GPU_PATHS
// (mobile GPUs: docs/VR_DLSS.md, "Both eyes in one motion pass") a frame with both-eyes packets (eye 2: a multiview
// draw's, one draw for both eyes): both eyes' targets in one two-layer image, drawn in runs. Each eye's draws keep the
// order they have in the eyes' own passes (the geometry in packet order, then the background in draw order); a run of
// both-eyes packets goes in one multiview pass, a run of one eye's in that eye's own. So each eye's motion target is the
// one its own pass would have drawn. False (nothing recorded): the eyes' own passes, which leave both-eyes packets out
bool record_both_eyes(const std::vector<Packet>& packets, bool& reset, Later later[2], int& later_n, uint64_t dumpFrame) {
    bool both = false;
    for (const Packet& p : packets) both |= p.eye == 2;
    if (!both) return false;
    // each eye's scene depth: the depth most of its packets (its own and the both-eyes ones) were drawn against
    std::map<std::pair<Surface*, uint32_t>, uint32_t> depths[2];
    for (const Packet& p : packets) {
        if (!p.depth) continue;
        if (p.eye == 0 || p.eye == 2) ++depths[0][{p.depth, p.depthSlice}];
        if (p.eye == 1) ++depths[1][{p.depth, p.depthSlice}];
        if (p.eye == 2 && p.depth->layerTwin) ++depths[1][{p.depth->layerTwin, p.depthSlice}];
    }
    Surface* depth[2] = {};
    uint32_t slice[2] = {};
    for (int eye = 0; eye < 2; ++eye) {
        if (depths[eye].empty()) return false;
        auto best = depths[eye].begin();
        for (auto it = depths[eye].begin(); it != depths[eye].end(); ++it)
            if (it->second > best->second) best = it;
        depth[eye] = best->first.first;
        slice[eye] = best->first.second;
    }
    if (!two_eye_depth(depth[0], slice[0], depth[1], slice[1]) || !two_layer_targets(*depth[0])) return false;
    // the geometry: the both-eyes packets in one multiview pass, then each eye's own in its pass (each where its depth
    // is the scene's: the order does not matter, as in the eyes' own passes). The background: each eye's in draw order
    std::vector<const Packet*> both2, own[2], order[2];
    uint64_t otherDepth = 0;
    for (int eye = 0; eye < 2; ++eye) {
        std::vector<const Packet*>& background = order[eye];
        for (const Packet& p : packets) {
            if (p.eye != eye && p.eye != 2) continue;
            const bool mine = p.eye == 2 ? p.depth == depth[0] && p.depthSlice == slice[0]
                                         : p.depth == depth[eye] && p.depthSlice == slice[eye];
            if (!mine) {
                ++otherDepth;
                continue;
            }
            if (p.background) background.push_back(&p);
            else if (p.eye != 2) own[eye].push_back(&p);
            else if (eye == 0) both2.push_back(&p);
        }
        std::stable_sort(background.begin(), background.end(),
                         [](const Packet* a, const Packet* b) { return a->order < b->order; });
    }
    // the runs: the geometry's (above), then the background's: a both-eyes packet is drawn when it is next for both
    // eyes, the eyes' own packets before it each in its eye's pass. The two orders hold the both-eyes packets in the same
    // order (draw order), so the walk meets each one in both; if not, the eyes' own passes (nothing recorded yet)
    struct Run { int kind; std::vector<const Packet*> draws; };  // kind: 0, 1 one eye; 2 both
    std::vector<Run> runs;
    auto put = [&](int kind, const Packet* p) {
        if (runs.empty() || runs.back().kind != kind) runs.push_back({kind, {}});
        runs.back().draws.push_back(p);
    };
    for (const Packet* p : both2) put(2, p);
    for (int eye = 0; eye < 2; ++eye)
        for (const Packet* p : own[eye]) put(eye, p);
    size_t at[2] = {0, 0};
    while (at[0] < order[0].size() || at[1] < order[1].size()) {
        const Packet* next[2] = {at[0] < order[0].size() ? order[0][at[0]] : nullptr,
                                 at[1] < order[1].size() ? order[1][at[1]] : nullptr};
        if (next[0] && next[0]->eye != 2) { put(0, next[0]); ++at[0]; continue; }
        if (next[1] && next[1]->eye != 2) { put(1, next[1]); ++at[1]; continue; }
        if (!next[0] || !next[1] || next[0] != next[1]) return false;
        put(2, next[0]);
        ++at[0];
        ++at[1];
    }
    if (runs.empty()) return false;
    g_otherDepth += otherDepth;
    Surface* t[2] = {g_targets[0].get(), g_targets[1].get()};
    for (int eye = 0; eye < 2; ++eye) {
        transition_image(depth[eye], VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        transition_image(t[eye], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    VkCommandBuffer cmd = command_buffer();
    // a pass: both layers (multiview; the first one clears both), or one eye's layer (after the first: loaded)
    bool first = true;
    auto begin = [&](int kind) {
        if (!first) {
            // the last pass's motion writes before this one loads and writes the image again (another render pass)
            VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = kind == 2 ? multiview_view(t[0]) : layer_view(t[kind], 0);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0, 0, 0, 0}};
        Surface* d = kind == 2 ? depth[0] : depth[kind];
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = kind == 2 ? multiview_view(depth[0]) : layer_view(d, slice[kind]);
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {t[0]->extent.width, t[0]->extent.height};
        ri.layerCount = 1;
        ri.viewMask = kind == 2 ? 0b11 : 0;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = d->fmt.stencil ? &da : nullptr;
        record::vkCmdBeginRendering(cmd, &ri);
        const float blend[4] = {0, 0, 0, 0};
        record::vkCmdSetBlendConstants(cmd, blend);
        record::vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        first = false;
        ++g_passes;
    };
    // (the first pass clears both layers: a multiview one, empty when the frame starts with one eye's draws)
    if (runs.front().kind != 2) {
        begin(2);
        record::vkCmdEndRendering(cmd);
    }
    uint32_t drawn[2] = {}, unmatched[2] = {}, flipped[2] = {};
    for (const Run& run : runs) {
        begin(run.kind);
        for (const Packet* p : run.draws) {
            record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
            record::vkCmdSetViewport(cmd, 0, 1, &p->viewport);
            record::vkCmdSetScissor(cmd, 0, 1, &p->scissor);
            record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->set,
                                            p->offsetCount, p->offsets);
            for (uint32_t i = 0; i < p->bindCount; ++i)
                record::vkCmdBindVertexBuffers(cmd, p->binds[i].binding, 1, &p->binds[i].buffer, &p->binds[i].offset);
            if (p->indexed) {
                record::vkCmdBindIndexBuffer(cmd, p->indexBuffer, p->indexOffset, p->indexType);
                record::vkCmdDrawIndexed(cmd, p->count, p->instances, 0, int32_t(p->baseVertex), 0);
            } else {
                record::vkCmdDraw(cmd, p->count, p->instances, p->baseVertex, 0);
            }
            // (counted per eye, as the eyes' own passes count them)
            for (int eye = 0; eye < 2; ++eye) {
                if (run.kind != 2 && run.kind != eye) continue;
                ++g_drawn;
                g_withPrevious += p->previous;
                ++g_why[p->why & 3];
                ++drawn[eye];
                unmatched[eye] += p->why == 1;
                flipped[eye] += p->viewport.height < 0;
                g_background += p->background;
            }
        }
        record::vkCmdEndRendering(cmd);
    }
    for (int eye = 0; eye < 2; ++eye) {
        if (motion::scene_changed(drawn[eye], unmatched[eye]) && !reset) {
            reset = true;
            ++g_sceneCuts;
        }
        transition_image(t[eye], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (dumpFrame && R.frame == dumpFrame) dump(t[eye], eye, R.frame);
        const bool flipsY = motion::viewport_flips_y(drawn[eye], flipped[eye]);
        g_results[eye] = {true, depth[eye], slice[eye], t[eye], flipsY, R.frame, false};
        if (dlss_eval() && ngx_vk::can_evaluate()) later[later_n++] = {eye, depth[eye], slice[eye], t[eye], flipsY};
    }
    ++g_bothEyeFrames;
    return true;
}
#endif

void record() {
    const auto started = std::chrono::steady_clock::now();
    g_results[0] = g_results[1] = {};
    std::vector<Packet> packets;
    {
        std::lock_guard lock(g_mutex);
        packets.swap(g_packets);
    }
    if (!g_needed.load(std::memory_order_relaxed)) return;  // (paused: set_needed; a frame's packets left over dropped)
    ++g_frames;
    static const uint64_t dumpFrame = [] {
        const char* e = std::getenv("WWHD_VK_MOTION_DUMP");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    if (!packets.empty()) end_encoder();
    // each eye's DLSS after both eyes' motion passes: the record worker is then drained once, not once an eye
    Later later[2];
    int later_n = 0;
    // a cut this frame (its swap's): both eyes' histories start again; a new scene, found below, does the same
    bool reset = g_cut.exchange(false, std::memory_order_relaxed);
    g_cuts += reset;
    bool recorded = false;
#ifdef WWHD_MOBILE_GPU_PATHS
    recorded = record_both_eyes(packets, reset, later, later_n, dumpFrame);
#endif
    for (int eye = 0; eye < 2 && !recorded; ++eye) {
        // the eye's scene depth: the depth most of its packets were drawn against
        std::map<std::pair<Surface*, uint32_t>, uint32_t> depths;
        for (const Packet& p : packets)
            if (p.eye == eye && p.depth) ++depths[{p.depth, p.depthSlice}];
        if (depths.empty()) continue;
        auto best = depths.begin();
        for (auto it = depths.begin(); it != depths.end(); ++it)
            if (it->second > best->second) best = it;
        Surface* depth = best->first.first;
        const uint32_t slice = best->first.second;
        if (!depth->image) continue;
        Surface* t = target(eye, *depth);
        transition_image(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        transition_image(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkCommandBuffer cmd = command_buffer();
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = layer_view(t, 0);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0, 0, 0, 0}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = layer_view(depth, slice);
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {t->extent.width, t->extent.height};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = depth->fmt.stencil ? &da : nullptr;
        record::vkCmdBeginRendering(cmd, &ri);
        const float blend[4] = {0, 0, 0, 0};
        record::vkCmdSetBlendConstants(cmd, blend);
        record::vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        // the geometry first (each where its depth is the scene's: the order does not matter), then the background in
        // the frame's draw order (each where no geometry wrote depth: a later one covers an earlier, as drawn)
        std::vector<const Packet*> background;
        uint32_t eyeDrawn = 0, eyeUnmatched = 0, eyeFlipped = 0;
        auto draw = [&](const Packet& p) {
            record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
            record::vkCmdSetViewport(cmd, 0, 1, &p.viewport);
            record::vkCmdSetScissor(cmd, 0, 1, &p.scissor);
            record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.set, p.offsetCount,
                                            p.offsets);
            for (uint32_t i = 0; i < p.bindCount; ++i)
                record::vkCmdBindVertexBuffers(cmd, p.binds[i].binding, 1, &p.binds[i].buffer, &p.binds[i].offset);
            if (p.indexed) {
                record::vkCmdBindIndexBuffer(cmd, p.indexBuffer, p.indexOffset, p.indexType);
                record::vkCmdDrawIndexed(cmd, p.count, p.instances, 0, int32_t(p.baseVertex), 0);
            } else {
                record::vkCmdDraw(cmd, p.count, p.instances, p.baseVertex, 0);
            }
            ++g_drawn;
            g_withPrevious += p.previous;
            ++g_why[p.why & 3];
            ++eyeDrawn;
            eyeUnmatched += p.why == 1;
            eyeFlipped += p.viewport.height < 0;
        };
        for (const Packet& p : packets) {
            if (p.eye != eye) continue;
            if (p.depth != depth || p.depthSlice != slice) {
                ++g_otherDepth;
                continue;
            }
            if (p.background) background.push_back(&p);
            else draw(p);
        }
        std::stable_sort(background.begin(), background.end(),
                         [](const Packet* a, const Packet* b) { return a->order < b->order; });
        for (const Packet* p : background) draw(*p);
        g_background += background.size();
        if (motion::scene_changed(eyeDrawn, eyeUnmatched) && !reset) {
            reset = true;
            ++g_sceneCuts;
        }
        record::vkCmdEndRendering(cmd);
        ++g_passes;
        transition_image(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (dumpFrame && R.frame == dumpFrame) dump(t, eye, R.frame);
        g_results[eye] = {true, depth, slice, t, motion::viewport_flips_y(eyeDrawn, eyeFlipped), R.frame, false};
        if (dlss_eval() && ngx_vk::can_evaluate())
            later[later_n++] = {eye, depth, slice, t, motion::viewport_flips_y(eyeDrawn, eyeFlipped)};
    }
    g_results[0].reset = g_results[1].reset = reset;  // (a cut or a new scene found above: both eyes)
    if (later_n) {
        const auto d0 = std::chrono::steady_clock::now();
        for (int i = 0; i < later_n; ++i) run_dlss(later[i].eye, later[i].depth, later[i].slice, later[i].motion, reset, later[i].flipsY);
        g_dlssNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - d0).count());
    }
    g_recordNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    // the next frame's jitter (Halton 2, 3 over 16 frames, centred)
    if (dlss_eval()) {
        g_jitterIndex = g_jitterIndex % 16 + 1;
        g_jitter[0] = halton(g_jitterIndex, 2) - 0.5f;
        g_jitter[1] = halton(g_jitterIndex, 3) - 0.5f;
    }
    if (g_frames % 600 == 0) LOG("[motion] %s", stats_line().c_str());
}

bool dlss_runs() { return dlss_eval() && on() && ngx_vk::can_evaluate(); }

bool jitter(float& x, float& y) {
    if (!dlss_eval() || !on() || !ngx_vk::can_evaluate()) return false;
    x = g_jitter[0];
    y = g_jitter[1];
    return true;
}

std::string stats_line() {
    char b[320];
    const double f = g_frames ? double(g_frames) : 1.0;
    LOG("[motion] still draws left out a frame (both eyes): %.0f; background draws %.1f a frame (both eyes)",
        g_still.exchange(0) / f, g_background / f);
    if (g_cuts || g_sceneCuts)
        LOG("[motion] histories started again: %llu view cuts, %llu new scenes", (unsigned long long)g_cuts,
            (unsigned long long)g_sceneCuts);
    g_background = g_cuts = g_sceneCuts = 0;
#ifdef WWHD_MOBILE_GPU_PATHS
    LOG("[motion] both eyes in one pass (mobile GPUs): %.2f of the frames (WWHD_VK_MOTION_BOTH_EYES=0: the eyes' own passes)",
        g_bothEyeFrames / f);
    g_bothEyeFrames = 0;
#endif
    LOG("[motion] cost ms a frame: packets built %.3f (drawing threads and render thread together), pass recorded %.3f "
        "(of which DLSS with the worker's drain %.3f)", g_packetNs.exchange(0) / f / 1e6, g_recordNs / f / 1e6, g_dlssNs / f / 1e6);
    g_recordNs = g_dlssNs = 0;
    if (g_evaluated || g_skipped)
        LOG("[motion] DLSS: %.2f evaluated a frame, %.2f left out (the scan and motion sizes differ, or no output image)",
            g_evaluated / f, g_skipped / f);
    g_evaluated = g_skipped = 0;
    snprintf(b, sizeof b, "pass: %.2f passes a frame, %.0f draws (%.0f with the previous frame's data; without: %.0f "
             "no previous draw, %.0f a cached mesh moved, %.0f a block missing), %.1f against another depth (left out)",
             g_passes / f, g_drawn / f, g_withPrevious / f, g_why[1] / f, g_why[2] / f, g_why[3] / f, g_otherDepth / f);
    g_frames = g_drawn = g_withPrevious = g_passes = g_otherDepth = 0;
    g_why[0] = g_why[1] = g_why[2] = g_why[3] = 0;
    return b;
}

}  // namespace gfxvk::motion_pass
