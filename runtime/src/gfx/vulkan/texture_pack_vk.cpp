// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Replacement texture packs in the Vulkan renderer: which guest textures a pack replaces, the larger images made for
// them, their lifetime and their VRAM budget (docs/superpowers/specs runtime texture adapter, sections 3, 4, 6, 7).
// The pack registry and the thread that reads and checks the DDS files are renderer-independent (gfx/texture_pack.h);
// binding a replacement in place of its guest texture is draw.cpp's (replacement_for).
//
// Detection: upload_surface hashes every level of a texture whose guest bytes may have changed; level 0's hash is
// kept (Surface::level0Hash). After a real upload, a candidate texture (CPU-written, 2D, one slice, at the guest
// size) whose key {format, width, height, level0Hash} is in a pack asks the loader for it; contents that changed
// to anything else drop the replacement made before, and the original draws.
//
// Creation, on the upload thread (where the device has a queue family that copies without drawing and timeline
// semaphores: texture_pack_transfer_family): it takes the loads as they finish (the loader's ready notification),
// checks them, makes each image itself (the device's own calls: the renderer's helpers are the render thread's),
// shared by the graphics and its own family (VK_SHARING_MODE_CONCURRENT: no ownership transfer), copies the levels
// into its staging ring (texture_pack_core.h StagingRing, 64 MiB: its bound on data in flight; a larger load goes alone
// through a buffer of its own) and records, in its own command pool, one copy with a region per level between the
// layout changes UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY. A batch of them is one submission on its queue that
// signals the next value of a timeline semaphore; when the thread sees that value reached, it hands the images to the
// render thread. A load over the VRAM budget, or whose image could not be made, goes back to the render thread
// without one (the budget's releases are the render thread's), which sends it on again when it made room.
// At swap (texture_pack_step) the render thread only attaches the finished images: no copy, allocation or command,
// O(images finished). Before the first is attached it raises transfer_wait.h's value, so every graphics submission
// from then on waits for that value (every stage): the copies are visible to the draws that sample them.
// Without the upload thread (no such family, no timeline semaphores, WWHD_TEXTURE_PACK_RENDER_UPLOADS=1), the render
// thread makes them at swap as before: at most 16 MiB of level data and 1 ms of the render thread a frame
// (texture_pack_core.h may_start_another; the first of a frame always, so a large one goes alone), each through one
// staging block (the texture staging arena when it fits, else one buffer of its own) and one copy command. Loads past
// the limit wait for the next frame. Either way each is given to every guest surface that still has the key (eye 0's
// and eye 1's maps); a load whose texture changed or went meanwhile is dropped. A replacement is owned by its guest
// surfaces (Surface::replacement, as mipChain) and is in no surface map, so no copy, render-target lookup, eye
// sharing, invalidation or alias code ever sees it.
//
// Lifetime: a guest whose image is destroyed (destroy_surface_image: a format change, stereo switched off) drops its
// replacement; the texture budget's eviction of the guest image (WWHD_VK_TEXTURE_BUDGET) does not (its restore
// uploads and hashes the same bytes, and the unchanged key keeps the replacement). A dropped replacement is kept to
// the next texture_pack_step, so a draw of this frame that took it before (another drawing thread) still finds it,
// and destroyed there (its image freed when the submissions that used it have finished).
//
// Budget: vr.texturePackBudgetMB, or at 0 min(budget / 2, budget - usage) of the device-local heap
// (VK_EXT_memory_budget) at least 1 GiB, read at the first step and again once the VR session runs (the compositor's
// memory counted). It is a cap (texture_pack_core.h admits): a finished load that would take the replacements over it
// first releases replacements not bound for 300 frames, least recently bound first (texture_budget::select), down to
// what makes room (release_target; at most one such release a step); if nothing can, the load is refused: no image,
// the original keeps drawing, and the key is asked for again only when it fits, or 300 frames on (may_retry: the
// step's scan of the refused keys every 30 frames, which tries one release; a request for a refused key meanwhile, an
// upload's or a bind's, only notes the address). Over the budget anyway (it was lowered, or read again smaller), the
// step releases the same way. A released replacement is asked for again at its guest's next bind (replacement_for),
// and the original draws meanwhile. An image that cannot be made releases replacements and is tried once more; then
// the entry is marked bad and the original draws.
//
// Threads: everything here runs under the surface lock (the drawing threads' upload_surface and replacement_for
// included), as the surface maps do, except the upload thread's work, which touches no surface map, guest surface or
// renderer state: its own queue, pool, ring and images, the loader's thread-safe calls (take_ready, mark_bad), the
// budget's atomics, and its two queues with the render thread (Uploads::mu). The loader's calls (request, take_ready,
// mark_bad) are thread-safe by themselves. Off (vr.texturePacks off, no "textures" package, or no BC formats on the
// device): one test in upload_surface, destroy_surface_image and texture_pack_step, no extra queue, no thread, no
// submission waits; nothing else.
//
// Loading with the area (the upload thread's path only): when the game opens an archive the index names, the loader
// queues every key of it (gfx/texture_pack.h prefetch, from hle/fs.cpp's file open) and reads it, but keeps it in
// system memory until the game draws the texture (kPrefetchParkBytes, texture_pack.h set_prefetch_park): its request
// is then delivered at once, and only drawn textures are made. The unattached path below is what runs with parking off
// (the renderer smoke's loads): such a load usually finishes before
// any guest texture has its key; the upload thread makes and fills its image as any other, and the render thread keeps
// the finished image by key, attached to no guest ("unattached", g_unattached). When a guest's upload then has that key,
// texture_pack_after_upload attaches it at once (pointer work under the surface lock: no request, no load, no upload).
// Unattached images count against the VRAM budget (g_unattachedBytes, their level bytes, as g_uploading's) and are the
// first released, least recently prefetched first: for a load with a guest that does not fit, and while the held bytes
// are over the budget; a prefetched load without a guest takes only the room of those of earlier archive opens (an
// archive larger than the budget does not churn). One that cannot fit even so is dropped on the upload thread (its
// DDS data freed there; the render thread gets its key alone) and prefetching pauses (texture_pack.h
// set_prefetch_paused: reopened archives read nothing) until that much fits again. One not attached within
// kUnattachedFrames is released. A replacement the renderer keeps (unattached or attached) is held in the loader
// (texture_pack.h hold), so opening its archive again loads nothing. Without the upload thread prefetch stays off.
//
// WWHD_TEXTURE_PACK_LOG=1: also a line per texture made, released, first bound, dropped load, and per near miss (an
// uploaded texture of a shape the packs have, but other contents).
#include "texture_pack_vk.h"

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "deferred_dispatch.h"
#include "draw_threads.h"
#include "gfx/texture_pack.h"
#include "mods/packages.h"
#include "render_prof.h"
#include "runtime.h"
#include "stereo.h"
#include "texture_budget.h"
#include "transfer_wait.h"
#include "vr/vr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gfxvk {

namespace tp = ::gfx::texture_pack;

bool g_texture_packs_on = false;
uint64_t g_stat_texpack_guest_levels = 0;
std::atomic<uint64_t> g_stat_texpack_bind_calls{0}, g_stat_texpack_bind_ns{0};
uint64_t g_stat_texpack_refused = 0;
std::atomic<uint64_t> g_stat_texpack_render_copy_bytes{0}, g_stat_texpack_uploaded{0};
extern uint64_t g_stat_staging_buffers, g_stat_staging_buffer_bytes;  // (surfaces.cpp)
bool vulkan_device_memory_now(uint64_t& budget, uint64_t& usage);  // (backend.cpp)

namespace {

constexpr uint64_t kPinFrames = 300;              // a replacement bound within this many frames is never released

// key -> address of a guest surface waiting for it (both eyes' maps are searched there when it arrives: no pointer
// is kept, so a surface that goes meanwhile leaves nothing dangling)
std::unordered_multimap<tp::Key, uint32_t, tp::KeyHash> g_waiting;
std::vector<std::shared_ptr<Surface>> g_retired;  // dropped, destroyed at the next texture_pack_step
// loads taken from the loader and not made yet (the frame's creation limit came first): made first next frame. Moved,
// never copied (a Loaded's level spans point into its own file data)
std::deque<tp::Loaded> g_taken;
std::atomic<uint64_t> g_vram{0};                  // bytes of the replacements guest surfaces hold
std::atomic<uint64_t> g_budget{0};
std::atomic<bool> g_statusOn{false};              // (the overlay's thread)
uint64_t g_settingBytes = 0;                      // vr.texturePackBudgetMB at the start (0: automatic)
bool g_budgetInVr = false;                        // the automatic budget was read while the VR session ran
uint64_t g_hits = 0, g_made = 0, g_released = 0;  // (since the last stats line)
uint64_t g_releaseTriedFrame = ~0ull;             // the step that last released to make room for a load (one a step)
// keys refused for the budget: the frame, the level bytes, the guest addresses that asked (as g_waiting's)
struct Refused { uint64_t frame, need; std::vector<uint32_t> addrs; };
std::unordered_map<tp::Key, Refused, tp::KeyHash> g_refused;
// level bytes of loads the upload thread was given room for (the budget's admits counted them) and that no guest holds
// yet: counted as held by every budget test, so the loads on their way cannot together pass the budget
std::atomic<uint64_t> g_uploading{0};
bool g_renderUploadsForTest = false;              // (texture_pack_set_render_uploads_for_test)
// prefetched loads the loader keeps in system memory until the game draws their texture (texture_pack.h
// set_prefetch_park): an archive holds many more textures than an area draws (Dragon Roost's open: 2714 keys, 4.3 GB,
// about 180 drawn), so made at once they cost the copy queue and VRAM for nothing
constexpr uint64_t kPrefetchParkBytes = 5ull << 30;
// prefetched replacements made before any guest had their key (the file's header: Loading with the area), under the
// surface lock: the image, its level bytes, the timeline value its copies signalled, the frame it was kept, its
// prefetch number (the archive open's), and its place in g_unattachedOrder (least recently prefetched first; a place
// whose seq is no longer its key's entry's is skipped)
constexpr uint64_t kUnattachedFrames = 3600;      // never attached within this many frames: released
struct Unattached { std::shared_ptr<Surface> r; uint64_t need, value, frame, gen, seq; };
std::unordered_map<tp::Key, Unattached, tp::KeyHash> g_unattached;
std::deque<std::pair<tp::Key, uint64_t>> g_unattachedOrder;
uint64_t g_unattachedSeq = 0;
std::atomic<uint64_t> g_unattachedBytes{0};       // their level bytes: counted as held by every budget test
uint64_t g_prefetchHits = 0, g_unattachedReleased = 0;  // (since the last stats line)
// prefetches the budget had no room for (the smoke reads it); the smallest of their level bytes while prefetching is
// paused for them (tp::set_prefetch_paused: the step resumes it once that much fits beside the attached and on-their-way
// bytes, the unattached ones being any later archive open's to take), 0 when not paused
uint64_t g_prefetchFull = 0, g_pausedNeed = 0;
bool verbose();
void pause_prefetch(const tp::Key& key, uint64_t need) {
    if (!g_pausedNeed || need < g_pausedNeed) g_pausedNeed = need;
    tp::set_prefetch_paused(true);
    if (verbose())
        LOG("[texture pack] dropped the prefetch of fmt %X %ux%u hash %016llx: over the VRAM budget; prefetching paused",
            key.format, key.width, key.height, (unsigned long long)key.hash);
}

// the bytes every budget test counts as held: attached replacements, loads on their way, unattached ones
uint64_t held_bytes() {
    return g_vram.load(std::memory_order_relaxed) + g_uploading.load(std::memory_order_relaxed) +
           g_unattachedBytes.load(std::memory_order_relaxed);
}
void destroy_unused(Surface* r);  // (the upload thread's section)

bool verbose() {
    static const bool v = [] { const char* e = std::getenv("WWHD_TEXTURE_PACK_LOG"); return e && e[0] == '1'; }();
    return v;
}

tp::Key key_of(const Surface* s) { return {s->format, s->width, s->height, s->level0Hash}; }

// a texture the CPU wrote at the guest's size, one 2D image of one slice; one the device decodes from BC
// (bc_decode.h) keeps its original
bool candidate(const Surface* s) {
    return !s->isDepth && !s->gpuWritten && !s->bcDecoded && s->imageType == VK_IMAGE_TYPE_2D && s->slices == 1 &&
           s->arrayLayers == 1 && s->sx == 1 && s->sy == 1;
}

struct HostFormat {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    uint32_t blockBytes = 0;  // per 4x4 block, or per texel
    bool compressed = false;
};
// the image format of a DDS's DXGI format (texture_pack_core.h allowed_dxgi lets only these through)
HostFormat host_format(uint32_t dxgi) {
    switch (dxgi) {
    case 98: return {VK_FORMAT_BC7_UNORM_BLOCK, 16, true};
    case 99: return {VK_FORMAT_BC7_SRGB_BLOCK, 16, true};
    case 80: return {VK_FORMAT_BC4_UNORM_BLOCK, 8, true};
    case 81: return {VK_FORMAT_BC4_SNORM_BLOCK, 8, true};
    case 83: return {VK_FORMAT_BC5_UNORM_BLOCK, 16, true};
    case 84: return {VK_FORMAT_BC5_SNORM_BLOCK, 16, true};
    case 28: return {VK_FORMAT_R8G8B8A8_UNORM, 4, false};
    case 29: return {VK_FORMAT_R8G8B8A8_SRGB, 4, false};
    default: return {};
    }
}
// sampled with filtering and copied into, as every guest texture (bc_decode_required's test)
bool usable(const HostFormat& f) {
    if (!f.vk || (f.compressed && !R.enabledFeatures.textureCompressionBC)) return false;
    constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (format_properties(f.vk).optimalTilingFeatures & needed) == needed;
}

// a guest lets go of its replacement; the last one hands it to the next step to be destroyed
void retire(std::shared_ptr<Surface>& r) {
    if (!r) return;
    if (r.use_count() == 1) {
        g_vram.fetch_sub(std::min(g_vram.load(std::memory_order_relaxed), r->allocationBytes), std::memory_order_relaxed);
        tp::unhold(key_of(r.get()));  // (a prefetch of its archive loads it again)
        g_retired.push_back(std::move(r));
    }
    r.reset();
}
void drop(Surface* s) {
    retire(s->replacement);
    s->replacementKey = {};  // (draw.cpp's resolve_texture looks for a replacement only where the key is set)
    s->replacementReleased = false;
    ++g_surface_epoch;  // (what draws cache of a texture's binding)
}

// asks for the key once per waiting address; a key refused for the budget only notes the address (its retry is the
// step's, refused_retry)
void attach_unattached(const Surface* s, const tp::Key& key);
void want(const Surface* s, const tp::Key& key) {
    if (!g_unattached.empty()) [[unlikely]]
        if (g_unattached.contains(key)) { attach_unattached(s, key); return; }
    if (auto it = g_refused.find(key); it != g_refused.end()) [[unlikely]] {
        if (std::find(it->second.addrs.begin(), it->second.addrs.end(), s->addr) == it->second.addrs.end())
            it->second.addrs.push_back(s->addr);
        return;
    }
    bool waiting = false;
    for (auto [it, end] = g_waiting.equal_range(key); it != end; ++it) waiting |= it->second == s->addr;
    if (!waiting) g_waiting.emplace(key, s->addr);
    tp::request(key);
}

// releases replacements not bound within `pin` frames, least recently bound first, while over `budget` (one shared by
// several guests: when the last of them bound it); returns how many
size_t release_over(uint64_t budget, uint64_t pin) {
    struct Held { Surface* replacement; uint64_t lastUsed; std::vector<Surface*> guests; };
    std::vector<Held> held;
    std::unordered_map<Surface*, size_t> at;
    for (int eye = 0; eye < 2; ++eye)
        for (auto& [addr, owned] : surfaces_of(eye)) {
            Surface* s = owned.get();
            if (!s->replacement) [[likely]] continue;
            auto [it, fresh] = at.emplace(s->replacement.get(), held.size());
            if (fresh) held.push_back({s->replacement.get(), 0, {}});
            Held& h = held[it->second];
            h.lastUsed = std::max(h.lastUsed, s->replacementUsedFrame);
            h.guests.push_back(s);
        }
    std::vector<texture_budget::Candidate> candidates;
    uint64_t total = 0;
    for (size_t i = 0; i < held.size(); ++i) {
        candidates.push_back({held[i].lastUsed, held[i].replacement->allocationBytes, i});
        total += held[i].replacement->allocationBytes;
    }
    const auto chosen = texture_budget::select(std::move(candidates), total, R.frame, budget, 0, pin);
    for (size_t i : chosen) {
        if (verbose())
            LOG("[texture pack] released %08X %ux%u fmt %X (%.1f MiB, last bound frame %llu)", held[i].guests[0]->addr,
                held[i].guests[0]->width, held[i].guests[0]->height, held[i].guests[0]->format,
                held[i].replacement->allocationBytes / 1048576.0, (unsigned long long)held[i].lastUsed);
        for (Surface* s : held[i].guests) {
            retire(s->replacement);
            s->replacementReleased = true;  // asked for again at its next bind
        }
        ++g_released;
    }
    if (!chosen.empty()) ++g_surface_epoch;
    return chosen.size();
}

void destroy_retired() {
    for (auto& r : g_retired) destroy_surface_image(r.get());
    g_retired.clear();
}

// the image of `r` at `extent`; on failure (out of device memory) releases replacements and tries once more
bool create_image(Surface* r, VkExtent3D extent, uint64_t need) {
    for (int attempt = 0;; ++attempt) {
        try {
            create_surface_image(r, false, extent);
            return true;
        } catch (const std::exception& e) {
            LOG("[texture pack] image %ux%u (%u levels) could not be made%s: %s", extent.width, extent.height, r->mips,
                attempt ? "" : ", releasing replacements to try again", e.what());
            if (attempt) return false;
            const uint64_t vram = g_vram.load(std::memory_order_relaxed);
            release_over(vram > need ? vram - need : 1, 1);
            destroy_retired();
        }
    }
}

// level offsets in one staging block: each a multiple of 16 (Vulkan's bufferOffset: a multiple of the texel block
// size and of 4; BC blocks are 8 or 16 bytes, RGBA8 texels 4); returns the block's size
uint64_t staging_layout(const tp::Loaded& l, std::vector<uint64_t>& offsets) {
    uint64_t at = 0;
    offsets.clear();
    for (const auto& level : l.levels) {
        at = (at + 15) & ~uint64_t(15);
        offsets.push_back(at);
        at += level.size();
    }
    return at;
}

// all levels of the DDS into the image through ONE staging block (the submission's upload arena when it fits, else one
// buffer of its own: one allocation per replacement, not one per level) and one copy command with a region per
// level, then shader read
void upload_levels(Surface* r, const tp::Loaded& l) {
    std::vector<uint64_t> offsets;
    const uint64_t bytes = staging_layout(l, offsets);
    const UploadSlice ring = texture_staging_slice(r, size_t(bytes));
    Buffer staging{};
    if (!ring.buffer) {
        staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!staging.mapped) { defer_buffer(staging); throw std::runtime_error("texture pack staging allocation is not mapped"); }
        rprof::add_upload(rprof::kUpTexture, bytes);
        ++g_stat_staging_buffers; g_stat_staging_buffer_bytes += bytes;
    }
    uint8_t* mapped = static_cast<uint8_t*>(ring.buffer ? ring.mapped : staging.mapped);
    g_stat_texpack_render_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    std::vector<VkBufferImageCopy> copies(r->mips);
    for (uint32_t level = 0; level < r->mips; ++level) {
        std::memcpy(mapped + offsets[level], l.levels[level].data(), l.levels[level].size());
        VkBufferImageCopy& copy = copies[level];
        copy.bufferOffset = (ring.buffer ? ring.offset : 0) + offsets[level];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copy.imageExtent = {std::max(1u, r->extent.width >> level), std::max(1u, r->extent.height >> level), 1};
    }
    transition_image(r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    record::vkCmdCopyBufferToImage(command_buffer(), ring.buffer ? ring.buffer : staging.buffer, r->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    if (!ring.buffer) defer_buffer(staging);
    transition_image(r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// one finished load into an image for the guests still waiting for it: made, failed after image work (counted in the
// frame's creation limit), or dropped before any (its texture changed or went, or the entry does not fit)
enum class Made { Made, Failed, Dropped, Refused };

uint64_t level_bytes(const tp::Loaded& l) {
    uint64_t need = 0;
    for (const auto& level : l.levels) need += level.size();
    return need;
}
// a replacement's surface for load `l` in host format `f`, at the key's (the guest's) size, without its image; its
// guest's address fields come from guest_fields
std::shared_ptr<Surface> new_replacement(const tp::Loaded& l, const HostFormat& f) {
    auto r = std::make_shared<Surface>();
    r->width = l.key.width; r->height = l.key.height; r->slices = 1;  // the guest's size; the extent is scale times it (sx, sy)
    r->mips = uint32_t(l.levels.size());
    r->format = l.key.format; r->dim = uint32_t(Latte::E_DIM::DIM_2D);
    r->fmt = FormatInfo{};
    r->fmt.pixel = f.vk; r->fmt.bytesPerBlock = r->fmt.hostBytesPerBlock = f.blockBytes; r->fmt.compressed = f.compressed;
    r->fmt.kind = FormatInfo::FLOAT;
    r->lodOffset = tp::lod_offset(l.scale);
    r->level0Hash = l.key.hash;  // (key_of(r) is the key it replaces: retire's unhold)
    r->dirty = false; r->gpuWritten = false;
    r->lastUsedFrame = ~0ull;  // (not bound yet: the first bind logs)
    return r;
}
void guest_fields(Surface* r, const Surface* g) {
    r->addr = g->addr; r->mipAddr = g->mipAddr; r->tileMode = g->tileMode; r->swizzle = g->swizzle;
}

// whether a replacement of `need` level bytes fits the budget (texture_pack_core.h admits), after releasing
// replacements not bound for kPinFrames down to release_target when it does not; at most one release a step (the
// surface maps' scan), so a burst of refused loads costs one
// the least recently prefetched unattached replacement (g_unattachedOrder's first live place) freed: no graphics
// submission ever used it and its copies have finished; false when none is left
bool release_unattached_front(uint64_t before = ~0ull) {
    while (!g_unattachedOrder.empty()) {
        const auto [key, seq] = g_unattachedOrder.front();
        auto it = g_unattached.find(key);
        if (it == g_unattached.end() || it->second.seq != seq) { g_unattachedOrder.pop_front(); continue; }  // (attached or freed)
        if (it->second.gen >= before) return false;
        g_unattachedOrder.pop_front();
        if (verbose())
            LOG("[texture pack] released the unattached replacement of fmt %X %ux%u hash %016llx (%.1f MiB, kept at frame %llu)",
                key.format, key.width, key.height, (unsigned long long)key.hash, it->second.need / 1048576.0,
                (unsigned long long)it->second.frame);
        destroy_unused(it->second.r.get());
        g_unattachedBytes.fetch_sub(it->second.need, std::memory_order_relaxed);
        g_unattached.erase(it);
        tp::unhold(key);
        ++g_unattachedReleased;
        return true;
    }
    return false;
}
// unattached replacements released, least recently prefetched first (only those prefetched before `before`), until
// `need` fits the budget with `extra` more held
void release_unattached_for(uint64_t need, uint64_t budget, uint64_t before = ~0ull, uint64_t extra = 0) {
    while (!tp::admits(held_bytes() + extra, need, budget) && release_unattached_front(before)) {}
}

bool admit(uint64_t need) {
    const uint64_t budget = g_budget.load(std::memory_order_relaxed);
    if (tp::admits(held_bytes(), need, budget)) return true;
    release_unattached_for(need, budget);  // the replacements no guest has asked for go first
    if (tp::admits(held_bytes(), need, budget)) return true;
    // (on their way, and unattached: 0 without the upload thread)
    const uint64_t others = g_uploading.load(std::memory_order_relaxed) + g_unattachedBytes.load(std::memory_order_relaxed);
    const uint64_t target = tp::release_target(budget, need);
    if (!target || target <= others || g_releaseTriedFrame == R.frame) return false;
    g_releaseTriedFrame = R.frame;
    if (!release_over(target - others, kPinFrames)) return false;
    return tp::admits(held_bytes(), need, budget);  // (released ones are freed at the next step)
}

// every 30 frames: the refused keys whose retry came (texture_pack_core.h may_retry: it fits now, or 300 frames on,
// when one release is tried for it) are asked for again for their guests that still have the key; keys none of whose
// guests has it any more are forgotten
void refused_retry() {
    if (g_refused.empty() || R.frame % 30 != 0) [[likely]] return;
    const uint64_t budget = g_budget.load(std::memory_order_relaxed);
    uint64_t asked = 0;  // the level bytes asked for in this scan, counted as held with held_bytes()
    for (auto it = g_refused.begin(); it != g_refused.end();) {
        const tp::Key key = it->first;
        Refused& refused = it->second;
        std::vector<const Surface*> guests;
        for (uint32_t addr : refused.addrs)
            for (int eye = 0; eye < 2; ++eye)
                for (auto [s, end] = surfaces_of(eye).equal_range(addr); s != end; ++s)
                    if (candidate(s->second.get()) && key_of(s->second.get()) == key && !s->second->replacement)
                        guests.push_back(s->second.get());
        if (guests.empty()) { it = g_refused.erase(it); continue; }
        bool fits = tp::admits(held_bytes() + asked, refused.need, budget);
        if (!tp::may_retry(refused.frame, R.frame, kPinFrames, fits)) { ++it; continue; }
        if (!fits) {
            release_unattached_for(refused.need, budget, ~0ull, asked);  // (unattached ones first)
            fits = tp::admits(held_bytes() + asked, refused.need, budget);
        }
        if (!fits) {
            const uint64_t target = tp::release_target(budget, refused.need);
            const uint64_t others = held_bytes() + asked - g_vram.load(std::memory_order_relaxed);
            if (target > others && g_releaseTriedFrame != R.frame) {
                g_releaseTriedFrame = R.frame;
                release_over(target - others, kPinFrames);
                fits = tp::admits(held_bytes() + asked, refused.need, budget);
            }
            if (!fits) { refused.frame = R.frame; ++it; continue; }  // (the next try kPinFrames on)
        }
        asked += refused.need;
        it = g_refused.erase(it);  // (before want, which would only note the address again)
        for (const Surface* s : guests) want(s, key);
    }
}
// the guest surfaces waiting for `key` that still have it and no replacement of it (both eyes' maps); `forget`: the
// key's requesters are forgotten (the load is attached, refused or dropped now)
std::vector<Surface*> waiting_guests(const tp::Key& key, bool forget) {
    std::vector<uint32_t> addrs;
    {
        auto [it, end] = g_waiting.equal_range(key);
        for (; it != end; ++it)
            if (std::find(addrs.begin(), addrs.end(), it->second) == addrs.end()) addrs.push_back(it->second);
        if (forget) g_waiting.erase(key);
    }
    std::vector<Surface*> guests;
    for (uint32_t addr : addrs)
        for (int eye = 0; eye < 2; ++eye)
            for (auto [it, end] = surfaces_of(eye).equal_range(addr); it != end; ++it) {
                Surface* s = it->second.get();
                if (candidate(s) && key_of(s) == key && !(s->replacement && s->replacementKey == key) &&
                    std::find(guests.begin(), guests.end(), s) == guests.end())
                    guests.push_back(s);
            }
    return guests;
}
void log_dropped(const tp::Key& key) {
    if (verbose())
        LOG("[texture pack] dropped the load of fmt %X %ux%u hash %016llx: its texture changed or went", key.format,
            key.width, key.height, (unsigned long long)key.hash);
}
// a load the budget has no room for: remembered for refused_retry, the original draws
void refuse(const tp::Key& key, uint64_t need, const std::vector<Surface*>& guests) {
    Refused& refused = g_refused[key];
    refused.frame = R.frame;
    refused.need = need;
    for (const Surface* s : guests)
        if (std::find(refused.addrs.begin(), refused.addrs.end(), s->addr) == refused.addrs.end()) refused.addrs.push_back(s->addr);
    ++g_stat_texpack_refused;
    const Surface* g = guests[0];
    if (verbose())
        LOG("[texture pack] refused %08X %ux%u fmt %X (%.1f MiB): over the VRAM budget (%.0f / %.0f MiB held), the original draws",
            g->addr, g->width, g->height, g->format, need / 1048576.0, g_vram.load() / 1048576.0, g_budget.load() / 1048576.0);
}
// a made replacement to its guests; `held`: already held in the loader (an unattached one), else held now
void attach(const std::shared_ptr<Surface>& r, const std::vector<Surface*>& guests, const tp::Key& key, bool held = false) {
    r->writeSeq = next_write_seq();
    g_vram.fetch_add(r->allocationBytes, std::memory_order_relaxed);
    if (!held) tp::hold(key);  // (a prefetch of its archive skips it while it is kept: retire's unhold)
    for (Surface* s : guests) {
        if (s->replacement) drop(s);
        s->replacement = r;
        s->replacementKey = key;
        s->replacementUsedFrame = R.frame;  // (just made: pinned as if bound now)
        s->replacementReleased = false;
    }
    ++g_surface_epoch;
    ++g_made;
    const Surface* g = guests[0];
    if (verbose())
        LOG("[texture pack] made %08X %ux%u fmt %X -> %ux%u, %u levels, %.1f MiB, %zu surface%s", g->addr, g->width, g->height,
            g->format, r->extent.width, r->extent.height, r->mips, r->allocationBytes / 1048576.0, guests.size(),
            guests.size() == 1 ? "" : "s");
}
Made make(tp::Loaded& l) {
    const std::vector<Surface*> guests = waiting_guests(l.key, true);
    if (guests.empty()) {
        log_dropped(l.key);
        return Made::Dropped;
    }
    const HostFormat f = host_format(l.dxgi);
    if (!usable(f)) { tp::mark_bad(l.key, "the graphics card cannot sample DXGI format " + std::to_string(l.dxgi)); return Made::Dropped; }
    const Surface* g = guests[0];
    if (l.levels.empty() || l.levels.size() > 16 || uint64_t(g->width) * l.scale != l.width || uint64_t(g->height) * l.scale != l.height ||
        l.width > R.properties.limits.maxImageDimension2D || l.height > R.properties.limits.maxImageDimension2D) {
        tp::mark_bad(l.key, "DDS does not fit the texture or the device");
        return Made::Dropped;
    }
    auto r = new_replacement(l, f);
    guest_fields(r.get(), g);
    const uint64_t need = level_bytes(l);
    if (!admit(need)) {
        refuse(l.key, need, guests);
        return Made::Refused;
    }
    if (!create_image(r.get(), {l.width, l.height, 1}, need)) {
        tp::mark_bad(l.key, "the image could not be made");
        return Made::Failed;
    }
    if (r->bcDecoded) {  // (create_surface_image's decode fallback: the device lacks the guest's BC format)
        destroy_surface_image(r.get());
        tp::mark_bad(l.key, "the graphics card decodes this texture's format");
        return Made::Failed;
    }
    try {
        upload_levels(r.get(), l);
    } catch (const std::exception& e) {
        destroy_surface_image(r.get());
        tp::mark_bad(l.key, std::string("upload failed: ") + e.what());
        return Made::Failed;
    }
    attach(r, guests, l.key);
    return Made::Made;
}

// ---- the upload thread (the file's header: Creation; transfer_wait.h; texture_pack_core.h StagingRing)

PFN_vkWaitSemaphores g_vkWaitSemaphores = nullptr;
PFN_vkGetSemaphoreCounterValue g_vkGetSemaphoreCounterValue = nullptr;

// a finished load on its way through the upload thread
struct Work {
    tp::Loaded l;
    uint64_t need = 0;            // its level bytes
    bool admitted = false;        // `need` is counted in g_uploading
    bool retried = false;         // its image could not be made once; the render thread released replacements since
    std::shared_ptr<Surface> r;   // its image, once made
};
// what the upload thread hands the render thread (texture_pack_step's publish)
struct Done {
    enum What { Uploaded, NeedRoom, CreateFailed } what = Uploaded;
    tp::Key key{};
    uint64_t need = 0;
    uint64_t value = 0;           // Uploaded: the timeline value its copies signalled, which the host has seen reached
    uint64_t prefetch = 0;        // Uploaded: the load's prefetch number (tp::Loaded::prefetch; 0: requested alone)
    std::shared_ptr<Surface> r;   // Uploaded: the filled image, in SHADER_READ_ONLY_OPTIMAL
    tp::Loaded l{};               // NeedRoom, CreateFailed: the load, to be sent on again
    bool retried = false;         // NeedRoom: (a CreateFailed load sent on and over the budget since)
};
// one submission on the upload queue
struct Batch {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t value = 0;           // the timeline value it signals
    uint64_t ringMark = 0;        // the staging ring's mark after its data: free when it finished
    VkBuffer ownBuffer = VK_NULL_HANDLE;    // a load larger than the ring: staging of its own (alone in its batch)
    VkDeviceMemory ownMemory = VK_NULL_HANDLE;
    std::vector<Done> items;
};

struct Uploads {
    // the device's (texture_pack_transfer_family, texture_pack_device_ready, start_uploads: the render thread, before
    // the thread starts)
    bool forTest = false;
    uint32_t family = ~0u;
    VkQueue queue = VK_NULL_HANDLE;
    std::string why;              // why replacements are made on the render thread (texture_pack_start logs it)
    VkPhysicalDeviceMemoryProperties memory{};
    bool usable[100] = {}, linear[100] = {};  // by DXGI format (format_properties is the render thread's)
    VkSemaphore timeline = VK_NULL_HANDLE;    // made at the first start, never destroyed (transfer_wait.h)
    VkCommandPool pool = VK_NULL_HANDLE;      // (the thread's while it runs)
    uint64_t nextValue = 0;       // the last value submitted (the thread's while it runs; kept across restarts)
    bool running = false, startFailed = false;  // (render thread)
    std::thread thread;
    std::mutex mu;                // everything below
    std::condition_variable cv;
    bool stop = false, ready = false;
    std::deque<Work> input;       // render thread -> upload thread: loads sent on after the render thread made room
    std::vector<Done> done;       // upload thread -> render thread
    // (diagnostics since the last stats line: the thread's time working, the render thread's publishing, bytes copied)
    std::atomic<uint64_t> busyNs{0}, publishNs{0}, bytes{0};
};
Uploads& U = *new Uploads;        // leaked on purpose, as the loader's state: no destructor runs under the thread
// held around every host use of U.queue (the upload thread's submit and wait idle) and by vkDeviceWaitIdle's callers
// (texture_pack_queue_lock): the spec's external synchronization of every queue of the device. Held only for those
// calls, never while waiting for anything else, so no lock order is involved
std::mutex& g_queueMutex = *new std::mutex;

uint64_t ns_since(std::chrono::steady_clock::time_point t) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
}
uint32_t memory_type_of(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < U.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (U.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return ~0u;
}
void free_staging(VkBuffer& buffer, VkDeviceMemory& memory) {
    if (buffer) vkDestroyBuffer(R.device, buffer, nullptr);
    if (memory) vkFreeMemory(R.device, memory, nullptr);  // (unmaps it)
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}
// a host-visible, mapped buffer the upload queue copies from (only that queue uses it: exclusive)
bool make_staging(uint64_t bytes, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& mapped) {
    buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; mapped = nullptr;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(R.device, &ci, nullptr, &buffer) != VK_SUCCESS) { buffer = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(R.device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type_of(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* p = nullptr;
    if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(R.device, &ai, nullptr, &memory) != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
        free_staging(buffer, memory);
        return false;
    }
    if (vkBindBufferMemory(R.device, buffer, memory, 0) != VK_SUCCESS || vkMapMemory(R.device, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        free_staging(buffer, memory);
        return false;
    }
    mapped = static_cast<uint8_t*>(p);
    return true;
}
// an image no graphics submission has used (still on its way, or let go before it was attached), its copies finished
// or never submitted: freed at once (an attached one goes through destroy_surface_image's deferred path)
void destroy_unused(Surface* r) {
    if (r->view) vkDestroyImageView(R.device, r->view, nullptr);
    if (r->image) vkDestroyImage(R.device, r->image, nullptr);
    if (r->memory) vkFreeMemory(R.device, r->memory, nullptr);
    r->view = VK_NULL_HANDLE; r->image = VK_NULL_HANDLE; r->memory = VK_NULL_HANDLE;
    r->allocationBytes = 0; r->layout = VK_IMAGE_LAYOUT_UNDEFINED;
}
// the image of a replacement as create_surface_image makes a guest texture's (2D, one layer, its levels, sampled and
// copied, in device-local memory of its own, with its sampling view), but on the upload thread with the device's own
// calls, and shared by the graphics family and the upload family (no ownership transfer). destroy_surface_image frees
// it as it frees create_surface_image's (vkDestroyImage, vkFreeMemory after the submissions that used it).
bool make_image(Surface* r, uint32_t dxgi, VkExtent3D extent, std::string& error) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = r->fmt.pixel;
    ci.extent = extent;
    ci.mipLevels = r->mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const uint32_t families[] = {R.queueFamily, U.family};
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.queueFamilyIndexCount = 2;
    ci.pQueueFamilyIndices = families;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto fail = [&](const char* what, VkResult res) {
        error = std::string(what) + " (" + std::to_string(int(res)) + ")";
        destroy_unused(r);
        return false;
    };
    VkResult res = vkCreateImage(R.device, &ci, nullptr, &r->image);
    if (res != VK_SUCCESS) { r->image = VK_NULL_HANDLE; return fail("create image", res); }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(R.device, r->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type_of(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u) return fail("no device-local memory type", VK_ERROR_FEATURE_NOT_PRESENT);
    res = vkAllocateMemory(R.device, &ai, nullptr, &r->memory);
    if (res != VK_SUCCESS) { r->memory = VK_NULL_HANDLE; return fail("allocate image memory", res); }
    res = vkBindImageMemory(R.device, r->image, r->memory, 0);
    if (res != VK_SUCCESS) return fail("bind image memory", res);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = r->fmt.pixel;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, r->mips, 0, 1};
    res = vkCreateImageView(R.device, &vi, nullptr, &r->view);
    if (res != VK_SUCCESS) { r->view = VK_NULL_HANDLE; return fail("create sampling image view", res); }
    // create_surface_image's fields for a sampled 2D texture of one layer
    r->imageType = VK_IMAGE_TYPE_2D; r->viewType = VK_IMAGE_VIEW_TYPE_2D;
    r->arrayLayers = 1; r->imageLayers = 1; r->baseLayer = 0;
    r->scale = r->dyn = r->ax = r->ay = 1.0f;
    r->extent = extent;
    r->sx = float(extent.width) / r->width; r->sy = float(extent.height) / r->height;
    r->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    r->usage = ci.usage; r->createFlags = ci.flags;
    r->linearFiltering = U.linear[dxgi];
    r->allocationBytes = req.size;
    r->layout = VK_IMAGE_LAYOUT_UNDEFINED; r->use = {};
    return true;
}

// a load's checks before any image work (make()'s, on the load alone); false: marked bad
bool check_load(const tp::Loaded& l) {
    const HostFormat f = host_format(l.dxgi);
    if (!f.vk || l.dxgi >= std::size(U.usable) || !U.usable[l.dxgi]) {
        tp::mark_bad(l.key, "the graphics card cannot sample DXGI format " + std::to_string(l.dxgi));
        return false;
    }
    if (l.levels.empty() || l.levels.size() > 16 || uint64_t(l.key.width) * l.scale != l.width || uint64_t(l.key.height) * l.scale != l.height ||
        l.width > R.properties.limits.maxImageDimension2D || l.height > R.properties.limits.maxImageDimension2D) {
        tp::mark_bad(l.key, "DDS does not fit the texture or the device");
        return false;
    }
    return true;
}

// the upload thread's own state (only it touches these while it runs)
struct UploadThread {
    std::vector<VkCommandBuffer> freeCmds;
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t* ringMapped = nullptr;
    tp::StagingRing staging;      // (capacity 0 when the ring could not be made: each load then goes alone)
    std::deque<Batch> inflight;   // in submission order, so in the order their values are reached
    std::deque<Work> pending;
    std::vector<Done> handBack;   // loads going back to the render thread without an image
};

void let_go(Work& w) {
    if (w.admitted) g_uploading.fetch_sub(w.need, std::memory_order_relaxed);
    w.admitted = false;
}
void hand_back(UploadThread& t, Done::What what, Work& w) {
    Done d;
    d.what = what;
    d.key = w.l.key;
    d.need = w.need;
    d.retried = w.retried;
    d.l = std::move(w.l);
    t.handBack.push_back(std::move(d));
}
// a load that cannot go on (marked bad, or its upload failed): its image and its room let go
void abandon(Work& w, const std::string& why) {
    if (w.r) destroy_unused(w.r.get());
    let_go(w);
    tp::mark_bad(w.l.key, why);
}

// hands the finished batches' images (and the loads going back) to the render thread; `all`: every batch (the queue
// is idle)
void retire_batches(UploadThread& t, bool all) {
    uint64_t reached = ~0ull;
    if (!all && (t.inflight.empty() || g_vkGetSemaphoreCounterValue(R.device, U.timeline, &reached) != VK_SUCCESS)) reached = 0;
    std::vector<Done> finished;
    while (!t.inflight.empty() && t.inflight.front().value <= reached) {
        Batch& b = t.inflight.front();
        t.staging.release_to(b.ringMark);
        free_staging(b.ownBuffer, b.ownMemory);
        t.freeCmds.push_back(b.cmd);
        for (Done& d : b.items) {
            d.value = b.value;
            finished.push_back(std::move(d));
        }
        t.inflight.pop_front();
    }
    if (finished.empty() && t.handBack.empty()) return;
    std::lock_guard lk(U.mu);
    for (Done& d : finished) U.done.push_back(std::move(d));
    for (Done& d : t.handBack) U.done.push_back(std::move(d));
    t.handBack.clear();
}
void wait_oldest(UploadThread& t, uint64_t timeoutNs) {
    if (t.inflight.empty()) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &U.timeline;
    wi.pValues = &t.inflight.front().value;
    const VkResult res = g_vkWaitSemaphores(R.device, &wi, timeoutNs);  // (VK_TIMEOUT: the loop looks again)
    if (res != VK_SUCCESS && res != VK_TIMEOUT) std::this_thread::sleep_for(std::chrono::milliseconds(2));  // (a lost device: no spin)
}

// records `r`'s levels from `src` at `at` (+ `offsets`) between its layout changes; the signal of the batch's
// semaphore value comes after all of it (its memory dependency makes the writes available to the graphics queue's wait)
void record_upload(VkCommandBuffer cmd, VkBuffer src, uint64_t at, const std::vector<uint64_t>& offsets, Surface* r) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;  // (concurrent sharing)
    b.image = r->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, r->mips, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    std::vector<VkBufferImageCopy> copies(r->mips);
    for (uint32_t level = 0; level < r->mips; ++level) {
        VkBufferImageCopy& copy = copies[level];
        copy.bufferOffset = at + offsets[level];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copy.imageExtent = {std::max(1u, r->extent.width >> level), std::max(1u, r->extent.height >> level), 1};
    }
    vkCmdCopyBufferToImage(cmd, src, r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;  // (the graphics queue's semaphore wait makes them visible)
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    r->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkCommandBuffer begin_cmd(UploadThread& t) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!t.freeCmds.empty()) { cmd = t.freeCmds.back(); t.freeCmds.pop_back(); }
    else {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = U.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(R.device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { t.freeCmds.push_back(cmd); return VK_NULL_HANDLE; }  // (the pool resets it at the next begin)
    return cmd;
}

// the next batch from the pending loads: each checked, admitted to the budget, its image made, its levels copied into
// staging and its copies recorded, until the staging ring is full or the loads are used up; then one submission.
// Returns false when the next pending load waits for staging room (a batch in flight must finish first).
bool upload_batch(UploadThread& t) {
    Batch b;
    std::vector<uint64_t> offsets;
    bool blocked = false;
    while (!t.pending.empty()) {
        Work& w = t.pending.front();
        if (!w.r) {
            if (!w.admitted) {
                if (!check_load(w.l)) { t.pending.pop_front(); continue; }
                w.need = level_bytes(w.l);
                // the budget, counting the loads already on their way; no room: the render thread's to release for
                const uint64_t budget = g_budget.load(std::memory_order_relaxed);
                bool fits = tp::admits(held_bytes(), w.need, budget);
                // a prefetched load may take the room of the unattached replacements of earlier archive opens (the
                // render thread releases them when it keeps this one): only attached and on-their-way bytes count
                if (!fits && w.l.prefetch)
                    fits = tp::admits(g_vram.load(std::memory_order_relaxed) + g_uploading.load(std::memory_order_relaxed), w.need, budget);
                if (!fits) {
                    if (w.l.prefetch) {  // (no room even so: its data is freed here, not on the render thread)
                        std::vector<uint8_t>().swap(w.l.file);
                        std::vector<std::span<const uint8_t>>().swap(w.l.levels);
                    }
                    hand_back(t, Done::NeedRoom, w);
                    t.pending.pop_front();
                    continue;
                }
                g_uploading.fetch_add(w.need, std::memory_order_relaxed);
                w.admitted = true;
            }
            auto r = new_replacement(w.l, host_format(w.l.dxgi));
            std::string error;
            if (!make_image(r.get(), w.l.dxgi, {w.l.width, w.l.height, 1}, error)) {
                LOG("[texture pack] image %ux%u (%u levels) could not be made%s: %s", w.l.width, w.l.height, r->mips,
                    w.retried ? "" : ", releasing replacements to try again", error.c_str());
                if (w.retried) abandon(w, "the image could not be made");
                else hand_back(t, Done::CreateFailed, w);  // (its room stays counted: it comes back admitted)
                t.pending.pop_front();
                continue;
            }
            w.r = std::move(r);
        }
        if (!b.cmd && !(b.cmd = begin_cmd(t))) {  // (before any staging is taken for it)
            abandon(w, "no command buffer for its upload");
            t.pending.pop_front();
            break;
        }
        const uint64_t bytes = staging_layout(w.l, offsets);
        VkBuffer src = t.ring;
        uint8_t* mapped = t.ringMapped;
        uint64_t at = t.staging.alloc(bytes, 16);  // (16: a multiple of every texel block's size and of 4)
        if (at == tp::StagingRing::kFull) {
            // larger than the ring (or the ring is missing): alone, through a buffer of its own, once nothing else is
            // in flight; otherwise the batch goes as it is and this load waits for room
            if (!b.items.empty() || !t.inflight.empty() || !t.staging.empty()) { blocked = true; break; }
            if (!make_staging(bytes, b.ownBuffer, b.ownMemory, mapped)) {
                abandon(w, "staging memory for its upload could not be made");
                t.pending.pop_front();
                continue;
            }
            src = b.ownBuffer;
            at = 0;
        }
        for (size_t level = 0; level < w.l.levels.size(); ++level)
            std::memcpy(mapped + at + offsets[level], w.l.levels[level].data(), w.l.levels[level].size());
        record_upload(b.cmd, src, at, offsets, w.r.get());
        U.bytes.fetch_add(bytes, std::memory_order_relaxed);
        Done d;
        d.what = Done::Uploaded;
        d.key = w.l.key;
        d.need = w.need;
        d.prefetch = w.l.prefetch;
        d.r = std::move(w.r);
        b.items.push_back(std::move(d));
        t.pending.pop_front();  // (the load's file data goes now: its levels are in staging)
        if (b.ownBuffer) break;  // (it goes alone)
    }
    if (b.items.empty()) {
        if (b.cmd) { vkEndCommandBuffer(b.cmd); t.freeCmds.push_back(b.cmd); }
        return !blocked;
    }
    b.ringMark = t.staging.mark();
    VkResult res = vkEndCommandBuffer(b.cmd);
    b.value = U.nextValue + 1;
    if (res == VK_SUCCESS) {
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ts.signalSemaphoreValueCount = 1;
        ts.pSignalSemaphoreValues = &b.value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &ts;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &b.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &U.timeline;
        std::lock_guard queueLock(g_queueMutex);  // (vkDeviceWaitIdle's external sync: texture_pack_queue_lock)
        res = vkQueueSubmit(U.queue, 1, &si, VK_NULL_HANDLE);  // (only this thread submits on the queue)
    }
    if (res != VK_SUCCESS) {
        LOG("[texture pack] upload submission failed (%d): %zu replacements not made", int(res), b.items.size());
        for (Done& d : b.items) {
            destroy_unused(d.r.get());
            g_uploading.fetch_sub(d.need, std::memory_order_relaxed);
            tp::mark_bad(d.key, "upload submission failed");
        }
        b.items.clear();
        b.value = U.nextValue;  // (nothing submitted: retired in order behind the batches before it)
    } else {
        U.nextValue = b.value;
    }
    t.inflight.push_back(std::move(b));
    return !blocked;
}

void upload_main() {
    UploadThread t;
    if (make_staging(tp::kStagingRingBytes, t.ring, t.ringMemory, t.ringMapped)) t.staging.capacity = tp::kStagingRingBytes;
    else LOG("[texture pack] the upload staging ring (%llu MiB) could not be made: each replacement uploads alone",
             (unsigned long long)(tp::kStagingRingBytes >> 20));
    for (;;) {
        retire_batches(t, false);
        {
            std::lock_guard lk(U.mu);
            if (U.stop) break;
            // loads sent on by the render thread first, in their order
            for (auto it = U.input.rbegin(); it != U.input.rend(); ++it) t.pending.push_front(std::move(*it));
            U.input.clear();
            U.ready = false;
        }
        if (t.pending.empty())
            for (auto& l : tp::take_ready(tp::kStagingRingBytes)) t.pending.push_back(Work{std::move(l)});
        if (t.pending.empty()) {
            if (t.inflight.empty() && t.handBack.empty()) {
                std::unique_lock lk(U.mu);
                // (the timeout only bounds a missed notification: the loader's and the render thread's both notify)
                U.cv.wait_for(lk, std::chrono::milliseconds(100), [] { return U.stop || U.ready || !U.input.empty(); });
            } else {
                wait_oldest(t, 2000000);  // 2 ms, then new work is looked for again
            }
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        const bool blocked = !upload_batch(t);
        U.busyNs.fetch_add(ns_since(started), std::memory_order_relaxed);
        if (blocked) wait_oldest(t, 2000000);
    }
    // stopping: the queue idle, every batch finished; what was not handed over is the render thread's to free
    // (stop_uploads), what was never submitted is freed here
    {
        std::lock_guard queueLock(g_queueMutex);
        vkQueueWaitIdle(U.queue);
    }
    retire_batches(t, true);
    for (Work& w : t.pending) {
        if (w.r) destroy_unused(w.r.get());
        let_go(w);
    }
    t.pending.clear();
    free_staging(t.ring, t.ringMemory);
}

// (render thread) the upload thread, at the first step with a pack (its queue made at device creation)
bool start_uploads() {
    if (U.running) return true;
    if (!U.queue || U.startFailed || g_renderUploadsForTest) return false;
    auto failed = [](const char* what, VkResult res) {
        LOG("[texture pack] the upload thread could not start (%s, %d): replacements are made on the render thread", what, int(res));
        U.startFailed = true;
        tp::set_prefetch(false);  // (no unattached replacements on the render thread's path)
        return false;
    };
    if (!U.timeline) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &type;
        if (const VkResult res = vkCreateSemaphore(R.device, &ci, nullptr, &U.timeline); res != VK_SUCCESS) {
            U.timeline = VK_NULL_HANDLE;
            return failed("timeline semaphore", res);
        }
        transfer_wait::g_semaphore = U.timeline;  // (before any value is published)
    }
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = U.family;
    if (const VkResult res = vkCreateCommandPool(R.device, &pi, nullptr, &U.pool); res != VK_SUCCESS) {
        U.pool = VK_NULL_HANDLE;
        return failed("command pool", res);
    }
    for (uint32_t dxgi : {28u, 29u, 80u, 81u, 83u, 84u, 98u, 99u}) {
        const HostFormat f = host_format(dxgi);
        U.usable[dxgi] = usable(f);
        U.linear[dxgi] = (format_properties(f.vk).optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    }
    vkGetPhysicalDeviceMemoryProperties(R.physicalDevice, &U.memory);
    {
        std::lock_guard lk(U.mu);
        U.stop = false;
        U.ready = true;  // (loads finished before the notification was set)
    }
    try {
        U.thread = std::thread(upload_main);
    } catch (const std::exception&) {
        vkDestroyCommandPool(R.device, U.pool, nullptr);
        U.pool = VK_NULL_HANDLE;
        return failed("thread", VK_ERROR_UNKNOWN);
    }
    tp::set_ready_notify([] {
        {
            std::lock_guard lk(U.mu);
            U.ready = true;
        }
        U.cv.notify_one();
    });
    U.running = true;
    tp::set_prefetch(true);  // (texture_pack_start turned it on already where the device has the queue; the smoke's load did not)
    return true;
}

// (render thread, after the graphics queue is idle) the upload thread stops; images it had not handed over, and
// those handed over but not attached, are freed (no graphics submission used them)
void stop_uploads() {
    if (!U.running) return;
    tp::set_prefetch(false);
    tp::set_ready_notify(nullptr);
    {
        std::lock_guard lk(U.mu);
        U.stop = true;
    }
    U.cv.notify_all();
    U.thread.join();
    U.running = false;
    std::vector<Done> done;
    {
        std::lock_guard lk(U.mu);
        done.swap(U.done);
        U.input.clear();
        U.stop = false;
        U.ready = false;
    }
    for (Done& d : done)
        if (d.r) destroy_unused(d.r.get());
    g_uploading.store(0, std::memory_order_relaxed);
    vkDestroyCommandPool(R.device, U.pool, nullptr);  // (its command buffers with it; the queue is idle)
    U.pool = VK_NULL_HANDLE;
}

void send_on(Work w) {
    {
        std::lock_guard lk(U.mu);
        U.input.push_back(std::move(w));
    }
    U.cv.notify_one();
}

// (render thread, texture_pack_step, under the surface lock) what the upload thread handed over: finished images
// attached to their guests, loads over the budget admitted (after a release) and sent on, or refused; returns whether
// any was attached
bool publish_uploads() {
    std::vector<Done> done;
    {
        std::lock_guard lk(U.mu);
        if (U.done.empty()) return false;
        done.swap(U.done);
    }
    const auto started = std::chrono::steady_clock::now();
    bool attached = false;
    for (Done& d : done) {
        switch (d.what) {
        case Done::Uploaded: {
            const std::vector<Surface*> guests = waiting_guests(d.key, true);
            if (guests.empty() && d.prefetch && !g_unattached.contains(d.key)) {
                // prefetched, no guest has the key yet: kept for the first that has (attach_unattached), in the room
                // of unattached replacements of earlier archive opens (its bytes are in g_uploading, so in held_bytes)
                const uint64_t budget = g_budget.load(std::memory_order_relaxed);
                while (budget && held_bytes() > budget && release_unattached_front(d.prefetch)) {}
                if (budget && held_bytes() > budget) {
                    g_uploading.fetch_sub(d.need, std::memory_order_relaxed);
                    destroy_unused(d.r.get());  // (its copies finished; no graphics submission saw it)
                    pause_prefetch(d.key, d.need);
                    break;
                }
                const uint64_t seq = ++g_unattachedSeq;
                g_unattached.emplace(d.key, Unattached{std::move(d.r), d.need, d.value, R.frame, d.prefetch, seq});
                g_unattachedOrder.emplace_back(d.key, seq);
                g_unattachedBytes.fetch_add(d.need, std::memory_order_relaxed);  // (before g_uploading is lowered)
                g_uploading.fetch_sub(d.need, std::memory_order_relaxed);
                tp::hold(d.key);
                break;
            }
            if (guests.empty()) {
                g_uploading.fetch_sub(d.need, std::memory_order_relaxed);
                log_dropped(d.key);
                destroy_unused(d.r.get());
                break;
            }
            guest_fields(d.r.get(), guests[0]);
            // before any draw can take it: every graphics submission from now on waits for its copies (transfer_wait.h)
            if (transfer_wait::g_value.load(std::memory_order_relaxed) < d.value)
                transfer_wait::g_value.store(d.value, std::memory_order_release);
            attach(d.r, guests, d.key);  // (g_vram raised before g_uploading is lowered: the upload thread's budget test may count it twice, never not at all)
            g_uploading.fetch_sub(d.need, std::memory_order_relaxed);
            g_stat_texpack_uploaded.fetch_add(1, std::memory_order_relaxed);
            attached = true;
            break;
        }
        case Done::NeedRoom: {
            const std::vector<Surface*> guests = waiting_guests(d.key, false);
            if (d.l.file.empty()) {
                // a prefetch the budget had no room for, its data freed on the upload thread: a guest that has the key
                // by now asks for it again (as any request; its g_waiting entry stays), else prefetching pauses
                ++g_prefetchFull;
                if (!guests.empty()) tp::request(d.key);
                else pause_prefetch(d.key, d.need);
                break;
            }
            if (guests.empty()) {
                g_waiting.erase(d.key);
                log_dropped(d.key);
                break;
            }
            if (!admit(d.need)) {
                g_waiting.erase(d.key);
                refuse(d.key, d.need, guests);
                break;
            }
            g_uploading.fetch_add(d.need, std::memory_order_relaxed);
            send_on(Work{std::move(d.l), d.need, true, d.retried, nullptr});
            break;
        }
        case Done::CreateFailed: {
            if (waiting_guests(d.key, false).empty()) {  // (gone meanwhile: its room let go)
                g_waiting.erase(d.key);
                g_uploading.fetch_sub(d.need, std::memory_order_relaxed);
                log_dropped(d.key);
                break;
            }
            // create_image's second try: replacements released to make room, then once more (its room still counted)
            const uint64_t vram = g_vram.load(std::memory_order_relaxed);
            release_over(vram > d.need ? vram - d.need : 1, 1);
            destroy_retired();
            send_on(Work{std::move(d.l), d.need, true, true, nullptr});
            break;
        }
        }
    }
    U.publishNs.fetch_add(ns_since(started), std::memory_order_relaxed);
    return attached;
}

// (under the surface lock: texture_pack_after_upload's or a bind's want) a guest uploaded with the key of an unattached
// replacement: attached at once to it and to every other guest waiting for the key (both eyes' maps). Pointer work: its
// copies finished when it was kept, so only the graphics submissions' wait value is raised before the attach.
void attach_unattached(const Surface* s, const tp::Key& key) {
    auto it = g_unattached.find(key);
    bool waiting = false;
    for (auto [w, end] = g_waiting.equal_range(key); w != end; ++w) waiting |= w->second == s->addr;
    if (!waiting) g_waiting.emplace(key, s->addr);
    const std::vector<Surface*> guests = waiting_guests(key, true);
    if (guests.empty()) return;  // (not a candidate with that key after all: it stays unattached)
    Unattached u = std::move(it->second);
    g_unattached.erase(it);       // (its place in g_unattachedOrder is skipped from now)
    guest_fields(u.r.get(), guests[0]);
    if (transfer_wait::g_value.load(std::memory_order_relaxed) < u.value)  // (writers: under the surface lock)
        transfer_wait::g_value.store(u.value, std::memory_order_release);
    attach(u.r, guests, key, true);
    g_unattachedBytes.fetch_sub(u.need, std::memory_order_relaxed);  // (after attach raised g_vram)
    ++g_prefetchHits;
}

// (texture_pack_step) unattached replacements not attached within kUnattachedFrames released, then the least recently
// prefetched while the held bytes are over the budget; O(1) a step when none is due
void trim_unattached() {
    while (!g_unattachedOrder.empty()) {
        const auto [key, seq] = g_unattachedOrder.front();
        auto it = g_unattached.find(key);
        if (it == g_unattached.end() || it->second.seq != seq) { g_unattachedOrder.pop_front(); continue; }
        if (R.frame < it->second.frame + kUnattachedFrames) break;
        release_unattached_front();
    }
    const uint64_t budget = g_budget.load(std::memory_order_relaxed);
    while (budget && held_bytes() > budget && release_unattached_front()) {}
}

// the automatic budget, read at the first step and once more after the VR session has started
void update_budget() {
    if (g_settingBytes) return;
    const bool inVr = vr::active();
    if (g_budget.load(std::memory_order_relaxed) && (g_budgetInVr || !inVr)) return;
    uint64_t heapBudget = 0, heapUsage = 0;
    const bool measured = vulkan_device_memory_now(heapBudget, heapUsage);
    const uint64_t bytes = tp::default_budget(heapBudget, heapUsage);
    g_budget.store(bytes, std::memory_order_relaxed);
    g_budgetInVr = inVr;
    LOG("[texture pack] VRAM budget %.0f MiB (automatic%s: device budget %.0f MiB, %.0f MiB in use%s)", bytes / 1048576.0,
        inVr ? ", VR session running" : "", heapBudget / 1048576.0, heapUsage / 1048576.0,
        measured ? "" : "; no VK_EXT_memory_budget: the heap's size");
}

}  // namespace

uint32_t texture_pack_transfer_family(VkPhysicalDevice physical, bool timelineSemaphores, bool bcFormats) {
    U.family = ~0u;
    U.why.clear();
    // only when a pack will be loaded (texture_pack_start's tests): otherwise the device is made as before
    const bool wanted = U.forTest || (bcFormats && vr::config().texturePacks && !mods::packages::texture_roots().empty());
    if (!wanted) return ~0u;
    if (const char* e = std::getenv("WWHD_TEXTURE_PACK_RENDER_UPLOADS"); e && e[0] == '1') {
        U.why = "WWHD_TEXTURE_PACK_RENDER_UPLOADS=1";
        return ~0u;
    }
    if (!timelineSemaphores) {
        U.why = "the device has no timeline semaphores (Vulkan 1.2)";
        return ~0u;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
    std::vector<uint32_t> flags(n), counts(n);
    for (uint32_t i = 0; i < n; ++i) flags[i] = families[i].queueFlags, counts[i] = families[i].queueCount;
    const uint32_t family = tp::transfer_family(flags, counts, R.queueFamily);
    if (family == ~0u) {
        U.why = "the device has no queue family that copies without drawing";
        return ~0u;
    }
    U.family = family;
    return family;
}

void texture_pack_device_ready() {
    if (U.family == ~0u) return;
    auto load = [](const char* name, const char* khr) {
        PFN_vkVoidFunction f = vkGetDeviceProcAddr(R.device, name);
        return f ? f : vkGetDeviceProcAddr(R.device, khr);
    };
    g_vkWaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(load("vkWaitSemaphores", "vkWaitSemaphoresKHR"));
    g_vkGetSemaphoreCounterValue =
        reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(load("vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR"));
    if (!g_vkWaitSemaphores || !g_vkGetSemaphoreCounterValue) {
        U.why = "the driver has no timeline semaphore entry points";
        U.family = ~0u;
        return;
    }
    vkGetDeviceQueue(R.device, U.family, 0, &U.queue);
}

void texture_pack_want_uploads_for_test() { U.forTest = true; }
std::unique_lock<std::mutex> texture_pack_queue_lock() { return std::unique_lock<std::mutex>(g_queueMutex); }
bool texture_pack_uploads_available() { return U.queue != VK_NULL_HANDLE && !U.startFailed; }
void texture_pack_set_render_uploads_for_test(bool on) { g_renderUploadsForTest = on; }

void texture_pack_start() {
    g_texture_packs_on = false;
    const auto roots = mods::packages::texture_roots();
    if (roots.empty()) return;
    const vr::Config c = vr::config();
    if (!c.texturePacks) {
        LOG("[texture pack] %zu textures package%s enabled, replacement off (vr.texturePacks)", roots.size(), roots.size() == 1 ? "" : "s");
        for (const auto& root : roots) mods::packages::note_texture_pack(root, "texture pack: off (vr.texturePacks=0 in settings.ini)");
        return;
    }
    if (!R.enabledFeatures.textureCompressionBC) {
        LOG("[texture pack] replacement off: the graphics card has no BC texture formats");
        for (const auto& root : roots) mods::packages::note_texture_pack(root, "texture pack: off, the graphics card has no BC texture formats");
        return;
    }
    tp::set_status_sink([](const std::filesystem::path& root, const std::string& text) { mods::packages::note_texture_pack(root, text); });
    if (!tp::load(roots, R.properties.limits.maxImageDimension2D)) return;
    g_settingBytes = c.texturePackBudgetMB > 0 ? uint64_t(double(c.texturePackBudgetMB) * 1048576.0) : 0;
    if (g_settingBytes) g_budget.store(g_settingBytes, std::memory_order_relaxed);
    g_texture_packs_on = true;
    g_statusOn.store(true, std::memory_order_relaxed);
    LOG("[texture pack] %llu replacements from %zu package%s; VRAM budget %s", (unsigned long long)tp::stats().entries,
        roots.size(), roots.size() == 1 ? "" : "s",
        g_settingBytes ? (std::to_string(g_settingBytes >> 20) + " MiB (vr.texturePackBudgetMB)").c_str() : "automatic");
    if (U.queue && !U.startFailed && !g_renderUploadsForTest) {
        tp::set_prefetch(true);  // (archives the game opens before the first step queue up in the loader meanwhile)
        tp::set_prefetch_park(kPrefetchParkBytes);
        if (tp::prefetch_on())
            LOG("[texture pack] replacements are read with the archives the game opens (up to %llu MiB kept in memory) "
                "and made when the game first draws them", (unsigned long long)(kPrefetchParkBytes >> 20));
    }
    if (U.queue)
        LOG("[texture pack] replacements are made on their own thread and uploaded on queue family %u (the render thread "
            "only attaches them)", U.family);
    else
        LOG("[texture pack] replacements are made on the render thread at the frame's end (%s)",
            U.why.empty() ? "no upload queue" : U.why.c_str());
}

void texture_pack_shutdown() {
    if (!g_texture_packs_on) return;
    g_texture_packs_on = false;
    g_statusOn.store(false, std::memory_order_relaxed);
    stop_uploads();  // (before the loader: it may be waiting on take_ready's data)
    for (auto& [key, u] : g_unattached) destroy_unused(u.r.get());  // (never used by a graphics submission)
    g_unattached.clear();
    g_unattachedOrder.clear();
    g_unattachedBytes.store(0, std::memory_order_relaxed);
    g_pausedNeed = 0;  // (tp::shutdown clears the pause)
    g_taken.clear();
    g_waiting.clear();
    g_refused.clear();
    tp::shutdown();
}

void texture_pack_after_upload(Surface* s) {
    const tp::Key key = key_of(s);
    if (!candidate(s)) {
        if (s->replacement) drop(s);
        else s->replacementKey = {}, s->replacementReleased = false;  // (a released one: nothing to ask for again)
        return;
    }
    if (s->replacement) {
        if (s->replacementKey == key) return;  // the same contents again (a restore, an invalidation)
        if (verbose())
            LOG("[texture pack] %08X %ux%u fmt %X changed: its replacement goes", s->addr, s->width, s->height, s->format);
        drop(s);
    }
    // no replacement: a key a release left names other contents than these, and would send every bind through the
    // surface lock (resolve_texture tests it); these contents, when in a pack, are asked for below
    s->replacementKey = {};
    s->replacementReleased = false;
    if (!tp::lookup(key)) {
        if (verbose() && tp::has_shape(key.format, key.width, key.height))
            LOG("[texture pack] near miss %08X: fmt %X %ux%u hash %016llx is in no pack (one of that shape is)", s->addr,
                key.format, key.width, key.height, (unsigned long long)key.hash);
        return;
    }
    want(s, key);
}

void texture_pack_drop(Surface* s) { drop(s); }

Surface* replacement_for(Surface* guest) {
    Surface* r = guest->replacement.get();
    if (!r) [[likely]] {
        if (guest->replacementReleased) [[unlikely]] {  // released by the budget: asked for again, the original meanwhile
            draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());
            guest->replacementReleased = false;
            if (candidate(guest) && guest->replacementKey == key_of(guest)) want(guest, guest->replacementKey);
            guest->replacementKey = {};  // (nothing to bind until it is made again, which sets it)
        }
        return nullptr;
    }
    // no longer a texture the CPU wrote (the GPU drew into it since, ...: detection's own test) or other contents now
    if (!candidate(guest) || !(guest->replacementKey == key_of(guest)) || !r->image) return nullptr;
    if (r->lastUsedFrame == ~0ull) [[unlikely]] {
        r->lastUsedFrame = R.frame;
        if (verbose())
            LOG("[texture pack] hit %08X %ux%u fmt %X: drawn with its %ux%u replacement", guest->addr, guest->width,
                guest->height, guest->format, r->extent.width, r->extent.height);
    }
    guest->replacementUsedFrame = R.frame;
    ++g_hits;
    return r;
}

// (without the upload thread) the loads finished since become images here, within the frame's creation limit
// (texture_pack_core.h may_start_another: 16 MiB and 1 ms, the first always): the loader is asked for more only when
// the last take is used up, and for no more than the limit; returns whether any was made
static bool make_on_render_thread() {
    if (g_taken.empty())
        for (auto& l : tp::take_ready(tp::kCreateBytesPerFrame)) g_taken.push_back(std::move(l));
    if (g_taken.empty()) return false;
    bool made = false;
    const auto started = std::chrono::steady_clock::now();
    uint64_t bytes = 0;
    uint32_t worked = 0;  // replacements that went as far as an image this frame
    end_encoder();
    RendererCommands rendererCommands;  // the renderer's stream, never eye 1's held commands
    while (!g_taken.empty()) {
        uint64_t next = 0;
        for (const auto& level : g_taken.front().levels) next += level.size();
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
        if (!tp::may_start_another(bytes, ns, next, worked)) break;
        tp::Loaded l = std::move(g_taken.front());
        g_taken.pop_front();
        const Made m = make(l);
        // (no image work: the texture changed or went, or the budget has no room; a release is in the time)
        if (m == Made::Dropped || m == Made::Refused) continue;
        made |= m == Made::Made;
        bytes += next;
        ++worked;
    }
    return made;
}

void texture_pack_step() {
    if (!g_texture_packs_on) [[likely]] return;
    draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());  // (stage 2.6b: the surface maps are shared)
    destroy_retired();
    update_budget();
    // with the upload thread only its finished images are attached here (and its loads over the budget admitted or
    // refused); without it they are made here
    const bool made = start_uploads() ? publish_uploads() : make_on_render_thread();
    if (!g_unattachedOrder.empty()) [[unlikely]] trim_unattached();
    if (g_pausedNeed) [[unlikely]]
        if (tp::admits(g_vram.load(std::memory_order_relaxed) + g_uploading.load(std::memory_order_relaxed), g_pausedNeed,
                       g_budget.load(std::memory_order_relaxed))) {
            g_pausedNeed = 0;
            tp::set_prefetch_paused(false);
        }
    const uint64_t budget = g_budget.load(std::memory_order_relaxed);
    if (budget && g_vram.load(std::memory_order_relaxed) > budget && (made || R.frame % 30 == 0)) release_over(budget, kPinFrames);
    refused_retry();
}

size_t texture_pack_unattached_for_test() { return g_unattached.size(); }
uint64_t texture_pack_prefetch_full_for_test() { return g_prefetchFull; }

void texture_pack_set_budget_for_test(uint64_t bytes) {
    g_settingBytes = bytes;
    g_budget.store(bytes, std::memory_order_relaxed);
    g_budgetInVr = false;
}

void texture_pack_report() {
    if (!g_texture_packs_on) return;
    draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());  // (the counters are the drawing threads' too)
    const tp::Stats st = tp::stats();
    LOG("[vulkan textures] texpack hits %llu pending %llu VRAM %.0f/%.0f MiB guest-levels %llu refused %llu (%zu keys wait for room)",
        (unsigned long long)g_hits, (unsigned long long)st.pending, g_vram.load() / 1048576.0, g_budget.load() / 1048576.0,
        (unsigned long long)g_stat_texpack_guest_levels, (unsigned long long)g_stat_texpack_refused, g_refused.size());
    {
        const uint64_t calls = g_stat_texpack_bind_calls.exchange(0), ns = g_stat_texpack_bind_ns.exchange(0);
        LOG("[vulkan textures] texpack binds: %llu lookups, %.2f ms in them (lock wait included)%s",
            (unsigned long long)calls, ns / 1e6,
            std::getenv("WWHD_TEXTURE_PACK_NO_BIND") ? "; WWHD_TEXTURE_PACK_NO_BIND: originals bound" : "");
    }
    if (U.running) {
        const uint64_t busy = U.busyNs.exchange(0), publish = U.publishNs.exchange(0), bytes = U.bytes.exchange(0);
        LOG("[vulkan textures] texpack uploads: upload thread %.2f ms, %.1f MiB copied; render thread publishing %.3f ms "
            "(render-thread copies %llu B in all)", busy / 1e6, bytes / 1048576.0, publish / 1e6,
            (unsigned long long)g_stat_texpack_render_copy_bytes.load(std::memory_order_relaxed));
    }
    if (g_made || g_released || st.bad)
        LOG("[vulkan textures] texpack made %llu released %llu; %llu entries bad this session", (unsigned long long)g_made,
            (unsigned long long)g_released, (unsigned long long)st.bad);
    if (st.prefetched || !g_unattached.empty())
        LOG("[vulkan textures] texpack prefetch: %llu keys queued this session; %llu kept in memory (%.0f MiB), %llu drawn "
            "from them, %llu dropped for room; %zu unattached (%.0f MiB), %llu attached from them, %llu released",
            (unsigned long long)st.prefetched, (unsigned long long)st.parked, st.parkedBytes / 1048576.0,
            (unsigned long long)st.parkHits, (unsigned long long)st.parkDropped, g_unattached.size(),
            g_unattachedBytes.load() / 1048576.0, (unsigned long long)g_prefetchHits, (unsigned long long)g_unattachedReleased);
    g_hits = g_made = g_released = g_stat_texpack_guest_levels = g_stat_texpack_refused = 0;
    g_prefetchHits = g_unattachedReleased = 0;
}

std::string texture_pack_status() {
    if (!g_statusOn.load(std::memory_order_relaxed)) return {};
    char line[96];
    std::snprintf(line, sizeof line, "Texture packs: %.0f / %.0f MiB VRAM",
                  (g_vram.load(std::memory_order_relaxed) + g_unattachedBytes.load(std::memory_order_relaxed)) / 1048576.0,
                  g_budget.load(std::memory_order_relaxed) / 1048576.0);
    return line;
}

}  // namespace gfxvk
