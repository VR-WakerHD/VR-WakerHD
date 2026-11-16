// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
//
// Fixed foveated rendering (2026-11-11; docs/VR_FOVEATION.md): the eye-sized two-eye (multiview) passes shade the edges
// of each eye in blocks of 2x2 (and 4x4 at the far edges) pixels, where the lenses show less, through a shading rate map
// (VK_KHR_fragment_shading_rate by attachment). Shapes and depth are still drawn pixel by pixel; only a block's colour is
// worked out once. The way AstroQuest (a shadPS4 Quest 3 port that also draws the game offscreen) does it: its README
// measured a fragment density map (VK_EXT_fragment_density_map, our first try) putting the Quest driver in its tiled mode,
// slower (and in our first player log it saved nothing, with coarse 256-pixel squares on the clouds and black patches
// at the edges); shading rate maps work in the direct mode.
//
// Only passes at the eye picture's size (vr::picture_size): the game's smaller buffers (bloom, blur, the sky's) are
// shaded in full, a coarse block there would be magnified on the screen. One map per size and level, 16 pixels a texel
// (or the nearest the device allows: 8 on Turnip), two identical layers, one per view: Turnip reads a shading rate
// attachment as layered whatever the view's layerCount (tu7_emit_subpass_shading_rate: .layered = true, array_pitch the
// layer's size), so with one layer the right eye read past the image (Valve: VRS with multiview "will result in a black
// screen"). A map is made and filled on the render thread, in its
// commands; any thread uses it only from the next frame on, when the commands that filled it went to the GPU's queue
// before (a drawing thread's commands of this frame may run before them).
#include "foveation.h"

#include "backend.h"
#include "runtime.h"
#include "deferred_dispatch.h"
#include "draw_threads.h"
#ifdef WWHD_VR
#include "vr/vr.h"
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace gfxvk {
namespace {

struct Map {
    uint32_t width = 0, height = 0;  // the render area it is for
    int level = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t readyFrame = 0;  // usable from this frame on
};
std::mutex g_mutex;
std::vector<Map> g_maps;

// a texel's rate: log2 of the block's width, twice, then of its height (VK_KHR_fragment_shading_rate's encoding)
constexpr uint8_t rate(uint32_t w, uint32_t h) {
    return uint8_t(((w >= 4 ? 2 : w >= 2 ? 1 : 0) << 2) | (h >= 4 ? 2 : h >= 2 ? 1 : 0));
}

// from how far out (a share of the way from the middle to the edge, on both axes, sqrt(x^2 + y^2)) a level shades in
// 2x2 and in 4x4 blocks: AstroQuest's rings, measured on a Quest 3 (17.0 ms of the GPU a frame without, 15.1, 14.3,
// 12.8 ms at its levels 1-3, the edges' blocks hardly seen at 1-2)
struct Rings { float coarse, coarsest; };
constexpr Rings kRings[4] = {
    {2.0f, 2.0f},    // off
    {0.80f, 2.0f},   // low: about half of the picture (its corners are a lot of it) in 2x2
    {0.62f, 2.0f},   // medium: seven tenths in 2x2
    {0.50f, 0.85f},  // high: four fifths in 2x2, the far corners in 4x4
};

// (2026-11-12) the map's layers: two (one per view) on Mesa's Turnip (the Steam Frame), which reads a shading rate
// attachment as layered whatever its layerCount; one elsewhere. Qualcomm's own driver (Quest 3) crashed with two, a
// null read inside vkCmdDrawIndexed after some minutes of play (three Quest crash logs on the builds with two layers;
// the build before, with one, ran without). WWHD_VK_FOVEATION_LAYERS=1/2 chooses
uint32_t map_layers() {
    static const uint32_t n = [] {
        const char* e = std::getenv("WWHD_VK_FOVEATION_LAYERS");
        const uint32_t v = e && (*e == '1' || *e == '2') ? uint32_t(*e - '0') : R.driverID == VK_DRIVER_ID_MESA_TURNIP ? 2u : 1u;
        LOG("[vulkan foveation] shading rate maps of %u layer%s (%s)", v, v == 2 ? "s, one per view" : ", both views",
            e && (*e == '1' || *e == '2') ? "WWHD_VK_FOVEATION_LAYERS" : v == 2 ? "Turnip" : "not Turnip");
        return v;
    }();
    return n;
}

bool make(Map& m) {
    const uint32_t texel = R.fsrTexel;
    const uint32_t w = (m.width + texel - 1) / texel, h = (m.height + texel - 1) / texel;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8_UINT;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    const uint32_t layers = map_layers();
    ii.arrayLayers = layers;  // (map_layers: one per view on Turnip, else one for both)
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(R.device, &ii, nullptr, &m.image) != VK_SUCCESS) return false;
    VkMemoryRequirements needs{};
    vkGetImageMemoryRequirements(R.device, m.image, &needs);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = needs.size;
    ai.memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(R.device, &ai, nullptr, &m.memory) != VK_SUCCESS ||
        vkBindImageMemory(R.device, m.image, m.memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m.image;
    vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};  // (two: a multiview pass's views 0 and 1)
    if (vkCreateImageView(R.device, &vi, nullptr, &m.view) != VK_SUCCESS) return false;

    // its rates, texel by texel (the middle of each texel, measured from the middle of the picture), from a staging
    // buffer in the render thread's commands
    const Rings rings = kRings[m.level < 0 ? 0 : m.level > 3 ? 3 : m.level];
    const uint8_t coarsest = R.fsr4x4 ? rate(4, 4) : rate(2, 2);
    std::vector<uint8_t> texels(size_t(w) * h);
    uint32_t coarse = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = (x + 0.5f) * texel / m.width * 2 - 1, fy = (y + 0.5f) * texel / m.height * 2 - 1;
            const float out = std::sqrt(fx * fx + fy * fy);
            const uint8_t r = out >= rings.coarsest ? coarsest : out >= rings.coarse ? rate(2, 2) : rate(1, 1);
            texels[size_t(y) * w + x] = r;
            coarse += r != rate(1, 1);
        }
    Buffer staging = create_buffer(texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!staging.mapped) {
        defer_buffer(staging);
        return false;
    }
    std::memcpy(staging.mapped, texels.data(), texels.size());
    end_encoder();
    const VkCommandBuffer cmd = command_buffer();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m.image;
    b.subresourceRange = vi.subresourceRange;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
    VkBufferImageCopy copies[2]{};  // (both layers from the same texels)
    VkBufferImageCopy& copy = copies[0];
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    copies[1] = copy;
    copies[1].imageSubresource.baseArrayLayer = 1;
    record::vkCmdCopyBufferToImage(cmd, staging.buffer, m.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, copies);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1,
                                 &b);
    defer_buffer(staging);
    m.readyFrame = R.frame + 1;
    LOG("[vulkan foveation] map for %ux%u (%s): %ux%u texels of %ux%u pixels, %u%% of it shaded in blocks (2x2%s), from "
        "the next frame",
        m.width, m.height, foveation_level_name(m.level), w, h, texel, texel, unsigned(coarse * 100 / (w * h)),
        rings.coarsest < 1.5f ? (R.fsr4x4 ? ", 4x4 at the far edges" : ", 4x4 not offered") : "");
    return true;
}

// the pass is the eye picture's size (the scene), not one of the game's smaller buffers
bool scene_size(uint32_t width, uint32_t height) {
#ifdef WWHD_VR
    uint32_t pw = 0, ph = 0;
    if (!vr::picture_size(pw, ph) || !pw || !ph) return false;
    // (not "near": a macro of the Windows headers)
    const auto within = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= b / 32; };
    return within(width, pw) && within(height, ph);
#else
    (void)width;
    (void)height;
    return false;
#endif
}

}  // namespace

const char* foveation_level_name(int level) {
    static const char* const names[] = {"off", "low", "medium", "high"};
    return names[level < 0 ? 0 : level > 3 ? 3 : level];
}

int foveation_level() {
    if (!R.fsr) return 0;
    static const int forced = [] {
        const char* e = std::getenv("WWHD_VK_FOVEATION_LEVEL");
        return e && *e >= '0' && *e <= '3' ? *e - '0' : -1;
    }();
    if (forced >= 0) return forced;
#ifdef WWHD_VR
    return vr::active() ? vr::config().foveation : 0;
#else
    return 0;
#endif
}

VkImageView foveation_map(uint32_t width, uint32_t height) {
    const int level = foveation_level();
    if (level <= 0 || !width || !height || !scene_size(width, height)) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Map& m : g_maps)
        if (m.width == width && m.height == height && m.level == level)
            return R.frame >= m.readyFrame ? m.view : VK_NULL_HANDLE;
    // made on the render thread only (its commands fill it); a drawing thread draws the pass at full detail meanwhile
    if (draw_threads::on_draw_thread() || g_maps.size() >= 64) return VK_NULL_HANDLE;
    Map m;
    m.width = width;
    m.height = height;
    m.level = level;
    if (!make(m)) {
        if (m.view) vkDestroyImageView(R.device, m.view, nullptr);
        if (m.image) vkDestroyImage(R.device, m.image, nullptr);
        if (m.memory) vkFreeMemory(R.device, m.memory, nullptr);
        LOG("[vulkan foveation] map for %ux%u could not be made", width, height);
        m = {};
        m.width = width;
        m.height = height;
        m.level = level;
        m.readyFrame = ~0ull;  // (not tried again)
    }
    g_maps.push_back(m);
    return VK_NULL_HANDLE;  // (from the next frame)
}

}  // namespace gfxvk
