#include "platform/quest_theater.h"
#include "../gamepad_backdrop.h"
#include "gamepad_backdrop.h"
#include "gamepad_backdrop_blend.h"
#include "gamepad_backdrop_replay.h"
#include "gx2/gx2_regs.h"
#include "vertex_cpu_policy.h"
#include <set>
#include <bit>
#include <map>
#include <tuple>
#include "pass.h"
#include "../../exception_report.h"
#include "../../render_objects.h"
#include "aspect.h"
#include "aspect_panes.h"
#include "../renderer.h"
// Vulkan draw submission. Guest state conventions follow Cemu (MPL-2.0).
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "backend.h"
#include "dynres.h"
#include "stereo.h"
#include "texture_pack_vk.h"
#ifdef WWHD_VR
#include "../../vr/vr.h"
#endif
#include "../stereo_projection.h"
#include "../sampler_lod_bias.h"
#include "../artistic_mips.h"
#include "sampler_filtering.h"
#include "deferred_dispatch.h"
#include "buffer_cache.h"
#include "gx2/state_groups.h"
#include "vertex_cpu_decode.h"
#include "render_prof.h"
#include "background_shaders.h"
#include "runtime.h"
#include "gx2/gx2.h"
#include "gx2/gx2_cmd.h"
#include "shaders.h"
#include "shader_binary.h"
#include "stutter_trace.h"
#include "../../dsweep.h"
#include "../../pipeline_groups.h"
#include "pipeline_state.h"
#include "pipeline_worker.h"
#include "pipeline_demand.h"
#include "pipeline_failure.h"
#include "settings.h"
#include "overlay/hostui.h"
#include "vertex_formats.h"
#include "uniform_snapshot.h"
#include "batch_glsl.h"
#include "vr_framesynth.h"
#include "shader_compiler.h"
#include "../draw_batch.h"
#include "index_conversion.h"
#include "vertex_history.h"
#include "vertex_snapshot_history.h"
#include "motion_match.h"
#include "motion_store.h"
#include "motion_pipeline.h"
#include "motion_pass.h"
#include "motion_history.h"
#include "ngx_vk.h"
#include "hud_coverage.h"
#include "reproj.h"
#include "../../framegen.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <fstream>
#include <memory>
#include <thread>
#include "thread_check.h"
#include "draw_threads.h"
#include "atlas_overlap.h"
#include "scene_stats.h"
#include "gx2/replay_machine.h"
#include "atlas_thread.h"
#include "eye_thread.h"
#include "draw_thread.h"
#include "platform/thread_sampler.h"
#include "platform/host.h"
#include "platform/core_groups.h"
#include "mobile_gpu.h"
#include "uniform_plan.h"
#include "fov_diag.h"
#include "../../aspect.h"
#include "per_thread.h"
#include "multiview_pipeline.h"
extern "C" uint64_t g_shader_state_gen;  // (the render thread's; the draw path reads gx2::shader_state_gen(): stage 2.9)
using namespace Latte;
namespace gfxvk {
namespace {
inline bool multiview_draws() { return vk::multiview::draws_on(); }  // (docs/VR_MULTIVIEW.md, stage M4)
bool preparation_stats_enabled() {
  static const bool enabled=std::getenv("WWHD_VK_STATS")!=nullptr;
  return enabled;
}
// Bounded overlap: the default on every platform (measured on macOS and Windows); an explicit
// zero/invalid WWHD_VK_DRAW_BATCH disables it.
// (docs/VR_DLSS.md, stage D2b 2b) each draw's previous frame's data, while a DLSS feature is chosen
// (ngx_vk::motion_wanted): the render thread keys each scene draw by its number (scene_split_before_draw's), the thread
// that records it copies the bytes it uploads into the draw's slot (per eye), and the store turns over at the scene's
// frame end. Nothing is bound from it yet
static motion::Store& motion_store() { static motion::Store s; return s; }
static bool motion_on() { static const bool on = ngx_vk::motion_wanted(); return on; }
static uint32_t current_draw_index();  // (t_drawIndex, below)
// the calling thread draws numbered scene draws (the render thread, the scene's drawing threads): only those record
// into the store (the sun atlas's and eye 1's threads carry no draw number of their own: they would write another
// draw's slot beside its own thread)
static bool motion_records() {
  return motion_on() && (!draw_threads::on_draw_thread() || scene_thread::on_scene_thread() || scene_thread::on_scene_b());
}
uint32_t parse_draw_batch(const char* text) {
  if (!text) return 2048;
  if (!*text) return 0;
  uint32_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return 0;
    uint32_t digit = uint32_t(*p - '0');
    if (value > (1048576u - digit) / 10) return 0;
    value = value * 10 + digit;
  }
  return value;
}
uint32_t parse_draw_batch_cap(const char* text) {
  if (!text) return 3;
  // Explicit invalid caps retain the original conservative fallback.
  const uint32_t value = parse_draw_batch(text);
  return value >= 1 && value <= 3 ? value : 2;
}
struct DrawBatchState {
  uint64_t frame = ~uint64_t{0};
  uint32_t draws = 0, submissions = 0;
  // weight: the draws it stands for (a multiview draw is both eyes', docs/VR_MULTIVIEW.md stage M4)
  bool after_draw(uint64_t currentFrame, uint32_t batch, uint32_t cap, uint32_t weight = 1) {
    if (frame != currentFrame) {
      frame = currentFrame;
      draws = submissions = 0;
    }
    if (!batch || submissions >= cap) return false;
    if ((draws += weight) < batch) return false;
    draws = 0;
    ++submissions;
    return true;
  }
};
DrawBatchState drawBatchState;
uint64_t drawBatchSubmissions = 0;
// (per thread, docs/VR_EYE_THREAD.md: eye 1's thread replays its own AO; a plain global let one thread's replay redirect
// the other's draw into its private AO images)
PER_THREAD(bool, aoPrivateReplay, false)
struct AoPrivate {
  uint32_t source = 0;
  uint64_t frame = ~0ull;
  Surface color, depth;
};
std::array<AoPrivate, 2> aoPrivateEyes;  // stereo: each eye replays into its own images
AoPrivate& ao_private() { return aoPrivateEyes[stereoEye()]; }
// dynamic resolution (dynres.h): private AO images of other sizes, kept for a change back (private_ao_surface)
static std::vector<Surface>& ao_kept() { PER_THREAD_STATIC(kept, std::vector<Surface>); return kept; }  // (per thread)
// ... and made ahead for the sizes the target's kept images (the neighbouring levels) need, one a frame
static void prepare_ao_neighbours(const Surface& dst, const Surface* like) {
  if (!dynres::on()) return;
  PER_THREAD_STATIC(madeFrameSlot, std::array<uint64_t, 1>);  // (per thread; 0: none yet)
  uint64_t& madeFrame = madeFrameSlot[0];
  if (madeFrame == R.frame) return;
  auto& kept = ao_kept();
  auto needed = [&](const Surface& e) {  // a size of this target's levels (current or kept), or another format's
    if (e.fmt.pixel != like->fmt.pixel || e.width != dst.width || e.height != dst.height) return true;
    if (e.extent.width == dst.extent.width && e.extent.height == dst.extent.height) return true;
    for (auto& k : like->spares)
      if (k->image && e.extent.width == k->extent.width * 3 / 2 && e.extent.height == k->extent.height * 3 / 2) return true;
    return false;
  };
  for (size_t i = 0; i < kept.size();)
    if (!needed(kept[i])) { destroy_surface_image(&kept[i]); kept.erase(kept.begin() + long(i)); } else ++i;
  for (auto& k : like->spares) {
    if (!k->image) continue;
    const VkExtent3D extent{k->extent.width * 3 / 2, k->extent.height * 3 / 2, 1};
    bool have = false;
    for (auto& e : kept)
      if (e.image && e.width == dst.width && e.height == dst.height && e.extent.width == extent.width &&
          e.extent.height == extent.height && e.fmt.pixel == like->fmt.pixel) have = true;
    if (have || !extent.width || !extent.height || extent.width > R.properties.limits.maxImageDimension2D ||
        extent.height > R.properties.limits.maxImageDimension2D) continue;
    Surface n = *like;
    n.own_image_only();  // (stage M2b: like may be eye 1's half of a two-eye image)
    n.spares.clear(); n.mipChain.reset(); n.forget_replacement();
    n.image = VK_NULL_HANDLE; n.memory = VK_NULL_HANDLE; n.view = VK_NULL_HANDLE;
    n.layerViews.clear(); n.sampledViews.clear(); n.guestLayout.reset();
    n.addr = n.mipAddr = 0;
    n.width = dst.width; n.height = dst.height; n.slices = n.mips = 1;
    n.dim = uint32_t(Latte::E_DIM::DIM_2D);
    n.gpuWritten = true; n.dirty = false;
    create_surface_image(&n, false, extent);
    // (not cleared ahead as the other images made ahead: this runs between draws, and a clear would end the pass)
    layer_view(&n, 0);  // the views the private image is used through, so its first use creates none
    for (auto& [key, view] : dst.sampledViews) make_sampled_view(&n, key);
    kept.push_back(n);
    madeFrame = R.frame;
    while (kept.size() > 8) { destroy_surface_image(&kept.front()); kept.erase(kept.begin()); }  // a safety bound
    return;
  }
}
Surface* private_ao_surface(Surface& dst, const Surface* like) {
  const uint64_t logicalWidth = uint64_t(like->width) * 3 / 2;
  const uint64_t logicalHeight = uint64_t(like->height) * 3 / 2;
  // Full-size torchlight doubles the depth copy (target_scale): the private image keeps the AO pass's size
  const uint64_t light = like->sizeClaim.size == gfx::TargetSize::Light && render::full_light() ? 2 : 1;
  const uint64_t physicalWidth = uint64_t(like->extent.width) * 3 / (2 * light);
  const uint64_t physicalHeight = uint64_t(like->extent.height) * 3 / (2 * light);
  const auto limit = R.properties.limits.maxImageDimension2D;
  if (!logicalWidth || !logicalHeight || logicalWidth > UINT32_MAX ||
      logicalHeight > UINT32_MAX || !physicalWidth || !physicalHeight ||
      physicalWidth > limit || physicalHeight > limit)
    exception_report::raise("private AO image dimensions exceed device limits");
  const uint32_t width = uint32_t(logicalWidth), height = uint32_t(logicalHeight);
  const VkExtent3D extent{uint32_t(physicalWidth), uint32_t(physicalHeight), 1};
  if (!dst.image || dst.width != width || dst.height != height ||
      dst.extent.width != extent.width || dst.extent.height != extent.height ||
      dst.fmt.pixel != like->fmt.pixel) {
    end_encoder();
    // dynamic resolution (dynres.h): the private images of other sizes are kept (a few), and one of the
    // needed size is taken back instead of creating it (its contents are drawn again each frame)
    auto& kept = ao_kept();
    if (!dynres::on()) {
      for (auto& k : kept) destroy_surface_image(&k);
      kept.clear();
    } else {
      for (size_t i = 0; i < kept.size(); ++i) {
        const Surface& k = kept[i];
        if (k.width == width && k.height == height && k.extent.width == extent.width && k.extent.height == extent.height &&
            k.fmt.pixel == like->fmt.pixel && k.image) {
          Surface take = kept[i];
          kept.erase(kept.begin() + long(i));
          if (dst.image) kept.insert(kept.begin(), dst);
          dst = take;
          while (kept.size() > 8) { destroy_surface_image(&kept.back()); kept.pop_back(); }
          return &dst;
        }
      }
      if (dst.image) {
        kept.insert(kept.begin(), dst);
        dst.image = VK_NULL_HANDLE; dst.memory = VK_NULL_HANDLE; dst.view = VK_NULL_HANDLE;
        dst.layerViews.clear(); dst.sampledViews.clear(); dst.mipChain.reset(); dst.allocationBytes = 0;
        while (kept.size() > 8) { destroy_surface_image(&kept.back()); kept.pop_back(); }
      }
    }
    destroy_surface_image(&dst);
    dst = *like;
    dst.own_image_only();  // (stage M2b: like may be eye 1's half of a two-eye image)
    dst.mipChain.reset(); dst.forget_replacement();
    dst.image = VK_NULL_HANDLE; dst.memory = VK_NULL_HANDLE;
    dst.view = VK_NULL_HANDLE;
    dst.layerViews.clear(); dst.sampledViews.clear(); dst.guestLayout.reset();
    dst.addr = dst.mipAddr = 0;
    dst.width = width; dst.height = height; dst.slices = dst.mips = 1;
    dst.dim = uint32_t(Latte::E_DIM::DIM_2D);
    dst.gpuWritten = true; // Private render image: never upload guest address 0.
    dst.dirty = false;
    create_surface_image(&dst, false, extent);
  }
  prepare_ao_neighbours(dst, like);
  return &dst;
}

// (docs/VR_EYE_PROJECTION.md, "The game's UI as its own layer") with each eye drawn with its own projection, the game's
// UI (layouts: HUD, menus, text) is drawn once into an image of its own, the symmetric field's shape, and shown as a
// quad layer ahead of the head (vr_vulkan.cpp, openxr_session.cpp kUi), as UEVR shows a game's UI. In the eyes' own
// pictures it would need each eye's correction, and the game draws its UI through a dozen kinds of matrices (headset
// counts, 2026-11-03: a third of it was left uncorrected, and showed twice). Premultiplied: the colour as the game blends
// it over a cleared image, the alpha written as coverage (pipeline_key / build_pipeline)
namespace ui_layer {
Surface g_surface;
uint64_t g_drawnFrame = ~uint64_t{0};
// a layout drawn while the eyes have their own frustums (XR)
bool candidate() {
  return stereo::frame.mode == stereo::XR && stereo::layout_draw_depth() &&
         !(stereo::frame.frustum[0].identity() && stereo::frame.frustum[1].identity());
}
bool is(const Surface* s) { return s && s == &g_surface && g_surface.image; }
// (diagnostics) WWHD_VR_UI_TRACE=<frame>: each UI draw of that frame logged (where it went, its viewport and scissor)
bool trace() {
  static const uint64_t at = getenv("WWHD_VR_UI_TRACE") ? strtoull(getenv("WWHD_VR_UI_TRACE"), nullptr, 10) : ~0ull;
  return R.frame == at && candidate();
}
// the image (made, or remade at a new size, like `like`: an eye's picture, at the symmetric field's shape), cleared at
// the frame's first UI draw
// (docs/VR_CUTSCENE_TEXT.md, "Dialogue in the UI layer") its own format: RGBA8 sRGB. It took the game's picture's
// (A2B10G10R10: two bits of alpha), so the coverage the quad is blended by had four levels: a translucent text box or a
// glyph's soft edge rounded to a third, two thirds or nothing (the dialogue box faint, the text dim or gone through
// Virtual Desktop). The game's colours are linear in its picture; an sRGB image keeps them so (encoded on write,
// decoded on read and blending), at 8 bits a channel, and the blit into the sRGB swapchain copies them unchanged
constexpr uint32_t kUiGx2Format = 0x41A;  // (GX2 RGBA8 sRGB)
Surface* surface(const Surface* like) {
  static const FormatInfo uiFormat = format_info(kUiGx2Format, false);
  const float shared = aspect::game();
  const uint32_t h = like->extent.height;
  const uint32_t w = uint32_t(std::lround(double(h) * double(shared > 0 ? shared : 16.0f / 9.0f)));
  const auto limit = R.properties.limits.maxImageDimension2D;
  if (!w || !h || w > limit || h > limit) exception_report::raise("UI layer image dimensions exceed device limits");
  if (!g_surface.image || g_surface.extent.width != w || g_surface.extent.height != h || g_surface.fmt.pixel != uiFormat.pixel ||
      g_surface.width != like->width || g_surface.height != like->height) {
    end_encoder();
    destroy_surface_image(&g_surface);
    g_surface = *like;
    g_surface.own_image_only();
    g_surface.mipChain.reset(); g_surface.forget_replacement();
    g_surface.image = VK_NULL_HANDLE; g_surface.memory = VK_NULL_HANDLE; g_surface.view = VK_NULL_HANDLE;
    g_surface.layerViews.clear(); g_surface.sampledViews.clear(); g_surface.guestLayout.reset();
    g_surface.addr = g_surface.mipAddr = 0;
    g_surface.slices = g_surface.mips = 1;
    g_surface.dim = uint32_t(Latte::E_DIM::DIM_2D);
    g_surface.format = kUiGx2Format;
    g_surface.fmt = uiFormat;
    g_surface.gpuWritten = true;  // (a private image: never uploaded from guest memory)
    g_surface.dirty = false;
    create_surface_image(&g_surface, false, {w, h, 1});
    g_drawnFrame = ~uint64_t{0};
  }
  if (g_drawnFrame != R.frame) {
    g_drawnFrame = R.frame;
    end_encoder();
    transition_image(&g_surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue clear{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdClearColorImage(command_buffer(), g_surface.image, g_surface.layout, &clear, 1, &range);
  }
  return &g_surface;
}
}  // namespace ui_layer
Surface* ui_layer_drawn() {
  return ui_layer::g_surface.image && ui_layer::g_drawnFrame == R.frame ? &ui_layer::g_surface : nullptr;
}
// (the frame dumps) the image as the last frame with UI left it
Surface* ui_layer_last() { return ui_layer::g_surface.image ? &ui_layer::g_surface : nullptr; }

float f32(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
UploadSlice snapshot(const void *data, size_t size, VkDeviceSize alignment) {
  thread_check::note(thread_check::Area::Uploads);  // (diagnostics: thread_check.h)
  auto slice = allocate_upload(std::max<size_t>(size, 16), alignment);
  // Every allocation owns its bytes until the submission fence completes.
  // Zero padding also defines shader reads for empty/small uniform payloads.
  if (data && size) {
    std::memcpy(slice.mapped, data, size);
    if (slice.size > size)
      std::memset(static_cast<uint8_t *>(slice.mapped) + size, 0,
                  slice.size - size);
  } else {
    std::memset(slice.mapped, 0, slice.size);
  }
  return slice;
}

UploadSlice vertex_snapshot(uint32_t binding, uint32_t address, uint32_t size,
                            bool bounded) {
  static const bool enabled = [] {
    const char *value = std::getenv("WWHD_VK_REUSE_VERTEX_SNAPSHOTS");
    return value && std::strcmp(value, "1") == 0;
  }();
  static const bool probeEnabled = [] {
    const char* e = std::getenv("WWHD_VK_VERTEX_HISTORY_PROBE");
    return e && !std::strcmp(e, "1") && preparation_stats_enabled();
  }();
  if (probeEnabled) {
    static VertexHistoryProbe probe;
    const uint32_t distance = probe.observe(reinterpret_cast<uintptr_t>(R.device),
        R.submissionGeneration, binding, address, size, bounded);
    if (bounded && binding < probe.bindings.size()) {
      ++draw_counters().vertexHistoryRequests;
      if (distance) {
        ++draw_counters().vertexHistoryMatches;
        draw_counters().vertexHistoryBytes += size;
        ++draw_counters().vertexHistoryDistances[distance - 1];
      }
    }
  }
  if (!enabled)
    return snapshot(mem::ptr(address), size, 4);
  static const bool historyEnabled = [] {
    const char* e = std::getenv("WWHD_VK_VERTEX_HISTORY_REUSE");
    return e && !std::strcmp(e, "1");
  }();
  // (per thread, stage 2.3: each thread reuses only slices of its own arena)
  PER_THREAD_STATIC(cache, std::array<VertexSnapshotHistory<UploadSlice>, 16>);
  static thread_local uint64_t generation = ~0ull;
  static thread_local VkDevice device = VK_NULL_HANDLE;
  if (device != R.device || generation != R.submissionGeneration) {
    for (auto& h : cache) h.reset();  // keeps the CPU copies' capacity
    device = R.device;
    generation = R.submissionGeneration;
  }
  if (binding >= cache.size())
    return snapshot(mem::ptr(address), size, 4);
  auto &history = cache[binding];
  auto &last = history.last;
  if (!bounded) {
    history.reset();
    return snapshot(mem::ptr(address), size, 4);
  }
  // One exact matching payload per draw; secondary is consulted only when
  // the original consecutive key differs, never after a changed-byte miss.
  auto* candidate = VertexSnapshotHistory<UploadSlice>::matches(last, address, size)
      ? &last : historyEnabled ? history.secondary(address, size) : nullptr;
  const bool secondary = candidate && candidate != &last;
  if (candidate) {
    ++draw_counters().vertexReuseChecks;
    if (secondary) ++draw_counters().vertexHistoryReuseChecks;
    static const bool timed = std::getenv("WWHD_VK_STATS") != nullptr;
    std::chrono::steady_clock::time_point start;
    if (timed) start = std::chrono::steady_clock::now();
    // the entry's CPU copy, not candidate->slice.mapped, unless the entry was made with direct reads
    // (host-cached upload memory: see vertex_snapshot_history.h)
    const bool equal = VertexSnapshotHistory<UploadSlice>::equal(*candidate, mem::ptr(address));
    if (timed)
      draw_counters().vertexReuseCompareNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count();
    if (equal) {
      ++draw_counters().vertexReuseHits;
      draw_counters().vertexReuseBytes += size;
      if (secondary) {
        ++draw_counters().vertexHistoryReuseHits;
        draw_counters().vertexHistoryReuseBytes += size;
        history.promote();
      }
      return history.last.slice;
    }
  }
  return history.remember(address, size, mem::ptr(address), historyEnabled, R.uploadReadsDirect,
                          [](const void* bytes, size_t n) { return snapshot(bytes, n, 4); });
}

uint32_t vertex_format_bytes(VkFormat format);
// Diagnostic "Convert vertices on the CPU" (issue #207): the group's attributes, decoded exactly like
// the shader decoder would (vertex_cpu_decode.h), 16 bytes each, interleaved per element; elements:
// 0..limit-1 when the draw's vertex extent is known (limit), else every element of the declared buffer.
UploadSlice cpu_vertex_stream(const LatteParsedFetchShaderBufferGroup& g, uint32_t addr, uint32_t size,
                              uint32_t stride, uint64_t limit) {
  uint64_t elements = stride ? (uint64_t(size) + stride - 1) / stride : 1;
  if (limit && limit < elements) elements = limit;
  const uint32_t attributes = vk::cpu_vertex_count(g, R.vertexDiagnostics.cpuClasses);
  const uint64_t bytes = elements * attributes * 16;
  if (bytes > 0x10000000u) exception_report::raise("CPU vertex conversion exceeds 256 MiB");
  auto slice = allocate_upload(std::max<size_t>(size_t(bytes), 16), 16);
  auto* out = static_cast<uint32_t*>(slice.mapped);
  if (bytes < slice.size) std::memset(static_cast<uint8_t*>(slice.mapped) + bytes, 0, slice.size - bytes);
  const auto* guest = static_cast<const uint8_t*>(mem::ptr(addr));
  for (int j = 0; j < g.attribCount; ++j) {
    const auto& a = g.attrib[j];
    if (!vk::cpu_vertex_selected(a, R.vertexDiagnostics.cpuClasses)) continue;
    uint32_t probe[4];
    static thread_local std::set<std::tuple<unsigned, unsigned, unsigned, unsigned>> unsupported;
    uint8_t zero[16]{};
    if (!vk::cpu_decode_attribute(a, zero, probe) &&
        unsupported.insert({unsigned(a.format), unsigned(a.nfa), unsigned(a.isSigned), unsigned(a.endianSwap)}).second)
      LOG("[vulkan cpu vertices] no decoder for format 0x%02x nfa %u signed %u endian %u (reads as zero, like the shader)",
          unsigned(a.format), unsigned(a.nfa), unsigned(a.isSigned), unsigned(a.endianSwap));
  }
  for (uint64_t e = 0; e < elements; ++e) {
    uint32_t slot = 0;
    for (int j = 0; j < g.attribCount; ++j) {
      const auto& a = g.attrib[j];
      if (!vk::cpu_vertex_selected(a, R.vertexDiagnostics.cpuClasses)) continue;
      const uint64_t at = e * stride + a.offset;
      uint32_t value[4] = {0, 0, 0, 0};
      if (at + vertex_format_bytes(vk::vertex_format(a.format)) <= size)
        vk::cpu_decode_attribute(a, guest + at, value);
      std::memcpy(out + (e * attributes + slot++) * 4, value, 16);
    }
  }
  rprof::add_upload(rprof::kUpVertex, bytes);
  return slice;
}

// Separate immutable window cache: a partially initialized reservation must
// never be accepted by the full-prefix snapshot cache.
UploadSlice vertex_window_snapshot(uint32_t binding,uint32_t address,uint32_t reservation,
                                   uint32_t begin,uint32_t length,
                                   const void* source=nullptr,bool poisonUnused=false) {
  using Entry=VertexWindowEntry<UploadSlice>;
  PER_THREAD_STATIC(entries, std::array<Entry,16>);  // (per thread, stage 2.3)
  static thread_local VkDevice device=VK_NULL_HANDLE;
  static thread_local uint64_t generation=~0ull;
  if(device!=R.device || generation!=R.submissionGeneration) {
    for(auto& e:entries) e.clear();
    device=R.device;generation=R.submissionGeneration;
  }
  static const bool reuse=[] {const char* e=std::getenv("WWHD_VK_REUSE_VERTEX_SNAPSHOTS");
    return e && !std::strcmp(e,"1");}();
  Entry* entry=reuse && binding<entries.size()?&entries[binding]:nullptr;
  const auto* fresh=static_cast<const uint8_t*>(source?source:mem::ptr(address))+begin;
  // compares the entry's CPU copy of the window, not the mapped slice, unless the entry was made with
  // direct reads (host-cached upload memory; vertex_snapshot_history.h)
  if(entry && entry->matches(address,reservation,begin,length)) {
    ++draw_counters().vertexReuseChecks;
    if(entry->equal(fresh)) {
      ++draw_counters().vertexReuseHits;draw_counters().vertexReuseBytes+=length;return entry->slice;
    }
  }
  // the slice gets the bytes of the entry's copy (one read of guest memory), or fresh if direct
  const uint8_t* bytes=entry?entry->remember(address,reservation,begin,length,fresh,R.uploadReadsDirect)
                            :fresh;
  auto slice=allocate_upload(std::max<uint32_t>(reservation,16),4);
  if(poisonUnused) std::memset(slice.mapped,0xCD,slice.size);
  std::memcpy(static_cast<uint8_t*>(slice.mapped)+begin,bytes,length);
  // Bytes before the proven minimum are never fetched. Preserve the previous
  // small-allocation padding contract without reading that unused prefix.
  if(slice.size>reservation)
    std::memset(static_cast<uint8_t*>(slice.mapped)+reservation,0,slice.size-reservation);
  if(entry) entry->slice=slice;
  return slice;
}

// Exact widths of the raw UINT formats used by the fetch pipeline.
uint32_t vertex_format_bytes(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8_UINT: return 1;
  case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16_UINT: return 2;
  case VK_FORMAT_R8G8B8_UINT: return 3;
  case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16_UINT:
  case VK_FORMAT_R32_UINT: return 4;
  case VK_FORMAT_R16G16B16_UINT: return 6;
  case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32_UINT: return 8;
  case VK_FORMAT_R32G32B32_UINT: return 12;
  case VK_FORMAT_R32G32B32A32_UINT: return 16;
  default: return 0;
  }
}
struct VertexExtent {
  bool valid = false;
  uint32_t maximum = 0;
  uint32_t minimum = 0;
};
bool vertex_copy_window_enabled() {
  static const bool enabled=[] {const char* e=std::getenv("WWHD_VK_VERTEX_COPY_WINDOW");
    return e && !std::strcmp(e,"1");}();
  return enabled;
}
template<class Index, bool Restart, bool ZeroBase, bool Window=false>
VertexExtent index_extent_reduction(const void* data, size_t count, int32_t baseVertex) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  constexpr uint32_t marker = sizeof(Index) == 2 ? UINT16_MAX : UINT32_MAX;
  uint32_t minimum = UINT32_MAX, maximum = 0;
  bool any = Restart ? false : count != 0;
  for (size_t i = 0; i < count; ++i) {
    Index index;
    // Fixed-size memcpy supports unaligned guest snapshots and compiles to a
    // native load. Branches on width, restart and zero base stay outside here.
    std::memcpy(&index, bytes + i * sizeof(Index), sizeof(Index));
    const uint32_t value = index;
    if constexpr (Restart) {
      const bool included = value != marker;
      any |= included;
      maximum = std::max(maximum, included ? value : 0);
      if constexpr (!ZeroBase || Window)
        minimum = std::min(minimum, included ? value : UINT32_MAX);
    } else {
      maximum = std::max(maximum, value);
      if constexpr (!ZeroBase || Window) minimum = std::min(minimum, value);
    }
  }
  if (!any) return {}; // Empty/all-restart draws retain the full binding.
  if constexpr (ZeroBase) return {true, maximum, Window ? minimum : 0};
  const int64_t low = int64_t(minimum) + baseVertex;
  const int64_t high = int64_t(maximum) + baseVertex;
  if (low < 0 || high > UINT32_MAX) return {};
  return {true, uint32_t(high), Window ? uint32_t(low) : 0};
}
template<class Index, bool Window=false>
VertexExtent index_extent_typed(const void* data, size_t count, bool restart, int32_t baseVertex) {
  if (!baseVertex) {
    if constexpr (Window) {
      if(count) {
        Index first;
        std::memcpy(&first,data,sizeof(first));
        // A host index zero proves the unsigned global minimum. Zero is
        // never a restart marker; use the original max-only SIMD reduction.
        if(first==0)
          return restart ? index_extent_reduction<Index,true,true,false>(data,count,baseVertex)
                         : index_extent_reduction<Index,false,true,false>(data,count,baseVertex);
      }
    }
    return restart ? index_extent_reduction<Index, true, true, Window>(data, count, baseVertex)
                   : index_extent_reduction<Index, false, true, Window>(data, count, baseVertex);
  }
  return restart ? index_extent_reduction<Index, true, false, Window>(data, count, baseVertex)
                 : index_extent_reduction<Index, false, false, Window>(data, count, baseVertex);
}
VertexExtent indexed_vertex_extent(const void* data, size_t count,
                                   uint32_t width, bool restart, int32_t baseVertex) {
  if(vertex_copy_window_enabled())
    return width == 2 ? index_extent_typed<uint16_t,true>(data,count,restart,baseVertex)
                      : index_extent_typed<uint32_t,true>(data,count,restart,baseVertex);
  return width == 2 ? index_extent_typed<uint16_t>(data, count, restart, baseVertex)
                    : index_extent_typed<uint32_t>(data, count, restart, baseVertex);
}
// Index extents of buffer cache entries (buffer_cache.h): the extent of the cached bytes at base
// vertex 0 is memoized in the entry, the draw's base vertex is applied per draw. The result equals
// indexed_vertex_extent over the same bytes.
VertexExtent cached_index_extent(bufcache::Entry& e, const void* data, size_t count, uint32_t width,
                                 bool restart, int32_t baseVertex) {
  draw_threads::CacheLock bufferLocked(draw_threads::buffer_mutex());  // (stage 2.8: an entry's memo is shared)
  const uint64_t key = uint64_t(count) | uint64_t(width) << 40 | uint64_t(restart) << 48;
  if (e.memoKey != key) {
    if (!data) return {};  // no bytes to scan: the full binding is copied (always safe)
    const VertexExtent raw = width == 2 ? index_extent_typed<uint16_t, true>(data, count, restart, 0)
                                        : index_extent_typed<uint32_t, true>(data, count, restart, 0);
    e.memo[0] = raw.valid; e.memo[1] = raw.minimum; e.memo[2] = raw.maximum;
    e.memoKey = key;
  }
  if (!e.memo[0]) return {};
  const bool window = vertex_copy_window_enabled();
  if (!baseVertex) return {true, e.memo[2], window ? e.memo[1] : 0};
  const int64_t low = int64_t(e.memo[1]) + baseVertex, high = int64_t(e.memo[2]) + baseVertex;
  if (low < 0 || high > UINT32_MAX) return {};
  return {true, uint32_t(high), window ? uint32_t(low) : 0};
}
VkPrimitiveTopology primitive_topology(uint32_t prim) {
  switch (prim) {
  case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case 3: case 0x12: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case 4: case 5: case 0x13: case 0x14: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  default: exception_report::raise("unsupported Vulkan primitive");
  }
}
uint32_t vertex_prefix_size(uint32_t declared, uint32_t stride,
                            uint64_t attributeEnd, VertexExtent extent) {
  if (!extent.valid || !attributeEnd)
    return declared;
  return uint32_t(std::min<uint64_t>(declared,
      uint64_t(extent.maximum) * stride + attributeEnd));
}

// Metadata-only opportunity measurement. It never changes the upload extent.
struct VertexWindowExtent { bool valid=false; uint32_t minimum=0, maximum=0; };
template<class Index>
VertexWindowExtent vertex_window_extent(const void* data, size_t count,
                                       bool restart, int32_t base) {
  uint32_t lo=UINT32_MAX,hi=0; bool any=false;
  const auto* bytes=static_cast<const uint8_t*>(data);
  for(size_t i=0;i<count;++i) {
    Index value; std::memcpy(&value,bytes+i*sizeof(Index),sizeof(value));
    if(restart && value==std::numeric_limits<Index>::max()) continue;
    any=true;lo=std::min(lo,uint32_t(value));hi=std::max(hi,uint32_t(value));
  }
  const int64_t low=int64_t(lo)+base,high=int64_t(hi)+base;
  if(!any || low<0 || high>UINT32_MAX) return {};
  return {true,uint32_t(low),uint32_t(high)};
}
bool vertex_window_stats_enabled() {
  static const bool enabled=[] {const char* e=std::getenv("WWHD_VK_VERTEX_WINDOW_STATS");
    return e && !std::strcmp(e,"1");}();
  return enabled;
}
struct VertexWindowStats {
  uint64_t prefix=0,window=0,bindings=0,eligible=0,fallback=0,zeroStride=0;
};
struct vertexWindowStats_per_thread_tag; /*per-thread:vertexWindowStats*/ inline VertexWindowStats& vertexWindowStats() { return per_thread::get<vertexWindowStats_per_thread_tag, VertexWindowStats>(); }
uint32_t vertex_window_unused(uint32_t copied,uint32_t stride,uint64_t attributeEnd,
                             VertexWindowExtent extent,bool supported) {
  if(!supported || !extent.valid || !attributeEnd || !stride) return 0;
  const uint64_t begin=uint64_t(extent.minimum)*stride;
  const uint64_t end=uint64_t(extent.maximum)*stride+attributeEnd;
  // Out-of-declaration fetches cannot justify changing a snapshot contract.
  if(end>copied || begin>end) return 0;
  return uint32_t(begin);
}
void report_vertex_window_stats() {
  static thread_local uint64_t previousFrame=0;
  if(R.frame<previousFrame) {previousFrame=R.frame;vertexWindowStats()={};}
  if(R.frame-previousFrame<120) return;
  const double frames=double(R.frame-previousFrame);
  const auto& t=vertexWindowStats();
  LOG("[vulkan vertex window opportunity] %.3f MiB prefix/frame %.3f MiB reachable window/frame %.3f MiB unused prefix/frame; bindings/eligible/fallback/zero-stride %llu/%llu/%llu/%llu; metadata only, uploads unchanged",
    t.prefix/frames/1048576.,t.window/frames/1048576.,(t.prefix-t.window)/frames/1048576.,
    (unsigned long long)t.bindings,(unsigned long long)t.eligible,
    (unsigned long long)t.fallback,(unsigned long long)t.zeroStride);
  vertexWindowStats()={};previousFrame=R.frame;
}

VkBlendFactor blend(uint32_t v) {
  static const VkBlendFactor t[] = {VK_BLEND_FACTOR_ZERO,
                                    VK_BLEND_FACTOR_ONE,
                                    VK_BLEND_FACTOR_SRC_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                                    VK_BLEND_FACTOR_SRC_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                    VK_BLEND_FACTOR_DST_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                    VK_BLEND_FACTOR_DST_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
                                    VK_BLEND_FACTOR_SRC_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                    VK_BLEND_FACTOR_CONSTANT_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
                                    VK_BLEND_FACTOR_SRC1_COLOR,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
                                    VK_BLEND_FACTOR_SRC1_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
                                    VK_BLEND_FACTOR_CONSTANT_ALPHA,
                                    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
  if (v >= std::size(t))
    exception_report::raise("unsupported blend factor");
  return t[v];
}
VkBlendOp blendop(uint32_t v) {
  static const VkBlendOp t[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT,
                                VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                VK_BLEND_OP_REVERSE_SUBTRACT};
  if (v >= 5)
    exception_report::raise("unsupported blend operation");
  return t[v];
}
VkStencilOp stencil(uint32_t v) {
  static const VkStencilOp t[] = {VK_STENCIL_OP_KEEP,
                                  VK_STENCIL_OP_ZERO,
                                  VK_STENCIL_OP_REPLACE,
                                  VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                  VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                  VK_STENCIL_OP_INVERT,
                                  VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                  VK_STENCIL_OP_DECREMENT_AND_WRAP};
  if (v >= 8)
    exception_report::raise("unsupported stencil operation");
  return t[v];
}
struct BindingTrimMetadata {
  uint64_t attributeEnd = 0;
  uint32_t stride = 0;
  bool supported = true;
  std::optional<LatteConst::VertexFetchType2> rate;
};
template<class Group>
BindingTrimMetadata binding_trim_metadata(const uint32_t* r, vk::Shader* vs, const Group& g) {
  BindingTrimMetadata trim;
  for (int j = 0; j < g.attribCount; ++j) {
    const auto &a = g.attrib[j];
    if (vs->mapping.attributeMapping[a.semanticId] < 0)
      continue;
    const uint32_t bytes = vertex_format_bytes(vk::vertex_format(a.format));
    if (!bytes || (trim.rate && *trim.rate != a.fetchType) ||
      (a.fetchType != LatteConst::VertexFetchType2::VERTEX_DATA &&
       a.fetchType != LatteConst::VertexFetchType2::INSTANCE_DATA) ||
      (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA &&
       a.aluDivisor != 1)) {
      trim.supported = false;
      break;
    }
    trim.rate = a.fetchType;
    trim.attributeEnd = std::max(trim.attributeEnd, uint64_t(a.offset) + bytes);
  }
  trim.stride =
    (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >>
     11) & 0xFFFF;
  return trim;
}
// Far-plane clipping off (PA_CL_CLIP_CNTL ZCLIP_FAR_DISABLE) becomes depth clamp where the device has
// it. panvk (Mali) has no geometric depth clipper: depth clipping culls each fragment whose depth
// leaves [0,1]. Sky geometry at the far plane (the title screen's cloud band: z = w - 0.0002) that
// passes beside or behind the camera then loses parts along a straight line where interpolated depth
// rounds above 1.0, flickering with the camera. There every pipeline clamps depth instead
// (R.depthClampAll, chosen at device creation); draws in front of the near plane aren't clipped then.
static bool depth_clamp_wanted(const uint32_t* r) {
  return R.enabledFeatures.depthClamp && ((r[REGADDR::PA_CL_CLIP_CNTL] & (1u << 27)) || R.depthClampAll);
}
// (docs/VR_PERFORMANCE.md, "Draws left out while their pipeline is made") a draw whose pipeline is still being made is
// left out of the frame instead of waiting (skip_while_made): on by default in a VR session; WWHD_VK_SKIP_PENDING_DRAWS=0
// turns it off, =1 turns it on outside VR too (the desktop stereo mode, for tests)
static bool skip_pending_draws() {
  static const int env = [] { const char* e = std::getenv("WWHD_VK_SKIP_PENDING_DRAWS"); return e ? std::atoi(e) : -1; }();
  return env == 1 || (env != 0 && stereo::frame.mode == stereo::XR);
}
struct PendingPipeline {
  vk::Shader* shaders[2]{};
  VkDevice device{};
  VkShaderModule modules[2]{};
  std::optional<PipelineDemandTicket> ticket;
  std::shared_ptr<PipelineRecipe> recipe;
  bool keepRecipe = true;  // false: a HUD coverage variant (hud_coverage.h), never recorded as a recipe
  ~PendingPipeline() {
    // Even abandoned jobs can still read these immutable prerequisites.
    if (ticket) ticket->discard_and_wait();
    for (auto module : modules)
      if (module) vkDestroyShaderModule(device, module, nullptr);
  }
};
struct Pipeline {
  VkPipeline pipeline{};
  std::shared_ptr<PendingPipeline> pendingCompile;
  // (skip_while_made) the same compile, for the draw threads' readiness check only: pendingCompile and its result belong
  // to whoever draws with it (the record worker resolves it when it replays the deferred bind, and resets it); this one
  // is set with it here and reset only by the draw threads, so they never read what the worker changes
  std::shared_ptr<PendingPipeline> watch;
  uint64_t skippedFrame = ~uint64_t{0};  // VR: the frame its draws were left out while it was made (skip_while_made)
  // (kMvSkipBuilding) the frames its draws were left out while a two-eye twin was built: the first of this run, the last
  uint64_t mvSkipFirst = 0, mvSkipLast = 0;
  std::exception_ptr compileError;
  VkPipelineLayout layout{};
  VkDescriptorSetLayout sets[2]{};
  bool dynamicUniforms = false;
#ifdef WWHD_MOBILE_GPU_PATHS
  // (uniform_plan.h) over the device's dynamic cap: the blocks that are dynamic anyway, per stage (dynamicUniforms then
  // false); with dynamicUniforms, every slot
  bool partialUniforms = false;
  uint32_t dynamicMask[2] = {0, 0};
#endif
  const LatteFetchShader* trimFetch = nullptr;
  const vk::Shader* trimVertex = nullptr;
  std::vector<BindingTrimMetadata> bindingTrims;
  // (docs/VR_MULTIVIEW.md, stage M4; multiview draws on) what its multiview twins are made from, and the twins by
  // which bindings are per view (multiview_key)
  std::shared_ptr<const vk::multiview::Base> mvBase;
  std::map<std::array<uint32_t, 6>, std::shared_ptr<vk::multiview::Pipeline>> mvTwins;
  // (docs/VR_DLSS.md, stage D2b 2c; a DLSS feature chosen) its motion twin, while one is wanted
  std::shared_ptr<vk::motion::Pipeline> motionTwin;
};
// Pipeline key: the two shader identities plus exactly the fixed-function state pipeline() bakes
// in, normalized so that state the pipeline ignores (blend factors of attachments that do not
// blend, stencil words with the stencil test off, depth bias words with the bias off, write masks
// of absent attachments) does not make a new pipeline. Fixed size, compared with memcmp.
constexpr uint32_t kMaxPipelineStrides = 16;
struct PipelineKey {
  uint64_t vs = 0, ps = 0, fetch = 0;
  uint32_t topology = 0, colorControl = 0, targetMask = 0, depthControl = 0;
  uint32_t stencilMask = 0, stencilMaskBack = 0, raster = 0, clip = 0;
  std::array<uint32_t, 3> depthBias{};
  std::array<uint32_t, 8> blend{};
  std::array<uint32_t, 8> formats{};
  uint32_t depthFormat = 0, strideCount = 0;
  std::array<uint32_t, kMaxPipelineStrides> strides{};
  uint32_t reserved = 0;  // no padding bytes: the key is hashed and compared as bytes
  bool operator==(const PipelineKey& other) const {
    return !std::memcmp(this, &other, sizeof *this);
  }
};
static_assert(std::has_unique_object_representations_v<PipelineKey>,
              "pipeline keys are compared and hashed as bytes");
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& key) const {
    const auto* p = reinterpret_cast<const uint8_t*>(&key);
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < sizeof key; i += 8) {
      uint64_t word;
      std::memcpy(&word, p + i, 8);
      hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
      hash ^= hash >> 32;
    }
    return size_t(hash ^ (hash >> 29));
  }
};
static_assert(sizeof(PipelineKey) % 8 == 0);
static_assert(sizeof(PipelineKey) == 208, "update the persisted pipeline recipe schema");
std::unordered_map<PipelineKey, Pipeline, PipelineKeyHash> pipelines;
// covSlot (60 fps (generated), hud_coverage.h): the colour attachment the pixel shader's coverage variant
// writes (0: none). It is kept in the key's reserved word, so the key (and the persisted recipes, which
// never hold such a pipeline) keeps its size; that attachment's blend and mask are the coverage's own.
PipelineKey pipeline_key(const uint32_t* r, const vk::Shader* vs, const vk::Shader* ps,
                         const LatteFetchShader* fs, VkPrimitiveTopology topology,
                         const std::array<Surface*, 8>& colors, const Surface* depth, uint32_t covSlot = 0) {
  PipelineKey key;
  key.vs = vs->key; // process-local canonical variant; portable identity is separate
  key.ps = ps->key;
  key.fetch = fs->vkPipelineHashFragment;
  key.topology = uint32_t(topology);
  uint32_t ncolor = 0;
  for (uint32_t i = 0; i < 8; ++i)
    if (colors[i]) {
      key.formats[i] = uint32_t(colors[i]->fmt.pixel);
      ncolor = i + 1;
    }
  const uint32_t colorControl = r[REGADDR::CB_COLOR_CONTROL];
  uint32_t blending = 0;
  for (uint32_t i = 0; i < ncolor; ++i)
    if (colors[i] && colors[i]->fmt.kind == FormatInfo::FLOAT && ((colorControl >> (8 + i)) & 1)) {
      blending |= 1u << i;
      // without SEPARATE_ALPHA_BLEND the alpha factors are the color ones
      const uint32_t raw = r[REGADDR::CB_BLEND0_CONTROL + i];
      key.blend[i] = raw & (raw & (1u << 29) ? 0x3FFF1FFFu : 0x00001FFFu);
    }
  key.colorControl = (colorControl & 0x00FF0000u) | (blending << 8);  // ROP, blend enables
  if (ui_layer::is(colors[0])) key.colorControl |= 1u << 31;  // (the UI layer's image: alpha as coverage, below)
  key.targetMask = ncolor >= 8 ? r[REGADDR::CB_TARGET_MASK]
                               : r[REGADDR::CB_TARGET_MASK] & ((1u << (4 * ncolor)) - 1);
  if (depth) {
    key.depthFormat = uint32_t(depth->fmt.pixel);
    const uint32_t dc = r[REGADDR::DB_DEPTH_CONTROL];
    uint32_t canonical = dc & 4;              // depth write
    if (dc & 2) canonical |= dc & 0x72;       // depth test and its function
    if (depth->fmt.stencil && (dc & 1)) {
      const bool back = dc & 0x80;
      canonical |= dc & (back ? 0xFFFFFF81u : 0x000FFF01u);
      key.stencilMask = r[REGADDR::DB_STENCILREFMASK] & 0x00FFFF00;
      if (back) key.stencilMaskBack = r[REGADDR::DB_STENCILREFMASK_BF] & 0x00FFFF00;
    }
    key.depthControl = canonical;
  }
  const uint32_t mode = r[REGADDR::PA_SU_SC_MODE_CNTL];
  key.raster = mode & 0x807;  // cull front/back, front face, depth bias enable
  if (mode & 0x800)
    key.depthBias = {r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE], r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET],
                     r[REGADDR::PA_SU_POLY_OFFSET_CLAMP]};
  key.clip = depth_clamp_wanted(r) ? 1u << 27 : 0;  // depth clamp
  if (fs->bufferGroups.size() > kMaxPipelineStrides)
    exception_report::raise("fetch shader has more vertex buffers than a pipeline key holds");
  for (auto& g : fs->bufferGroups)
    key.strides[key.strideCount++] =
        (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
  if (covSlot) {
    key.reserved = covSlot;
    key.blend[covSlot] = 0;
    key.colorControl &= ~(1u << (8 + covSlot));
    key.targetMask &= ~(15u << (4 * covSlot));
  }
  return key;
}
struct LastPipelineLookup {
  PipelineKey key{};
  VkDevice device = VK_NULL_HANDLE;
  Pipeline* value = nullptr;
};
// (per thread, stage 2.4: each thread's own memo of its last lookup; the pipeline map itself is shared and locked)
struct lastPipelineLookup_per_thread_tag; /*per-thread:lastPipelineLookup*/ inline LastPipelineLookup& lastPipelineLookup() { return per_thread::get<lastPipelineLookup_per_thread_tag, LastPipelineLookup>(); }
// advanced by reset_pipeline_lookup_cache: a thread's memos made before then are dropped at its next lookup (the
// reset clears only the calling thread's)
std::atomic<uint64_t> pipelineMemoGeneration{1};
PER_THREAD(uint64_t, threadPipelineMemoGeneration, 0)
struct PipelineLookaside {
  std::array<LastPipelineLookup, 8> entries{};
  uint32_t next = 0;
  Pipeline* find(const PipelineKey& key, VkDevice device) const {
    for (const auto& entry : entries)
      if (entry.value && entry.device == device && entry.key == key)
        return entry.value;
    return nullptr;
  }
  void remember(const LastPipelineLookup& entry) {
    if (!entry.value) return;
    entries[next] = entry;
    next = (next + 1) % entries.size();
  }
};
struct pipelineLookaside_per_thread_tag; /*per-thread:pipelineLookaside*/ inline PipelineLookaside& pipelineLookaside() { return per_thread::get<pipelineLookaside_per_thread_tag, PipelineLookaside>(); }  // (per thread, stage 2.4)
bool pipeline_lookaside_enabled() {
  static const bool enabled = [] {
    const char* e = std::getenv("WWHD_VK_PIPELINE_LOOKASIDE");
    return e && !std::strcmp(e, "1");
  }();
  return enabled;
}
void capture_pipeline_failure(VkResult result, vk::Shader* vs, vk::Shader* ps,
                              const std::vector<uint32_t>& vertexWords,
                              const std::vector<uint32_t>& pixelWords) noexcept {
  try {
    LOG("[vulkan] graphics pipeline failed (Vulkan result %d): vs %016llX ps %016llX; its draws are skipped",
        int(result), (unsigned long long)vs->key, (unsigned long long)ps->key);
    std::error_code error; std::filesystem::create_directories("captures", error);
    vk::Shader* shaders[]{vs,ps};
    const std::vector<uint32_t>* words[]{&vertexWords,&pixelWords};
    for(size_t i=0;i<2;++i) {
      char name[96];
      snprintf(name,sizeof name,"captures/pipeline-failed-%s-%016llX",
          shaders[i]->vertex ? "vs" : "ps",(unsigned long long)shaders[i]->key);
      // GLSL text is kept only with WWHD_VK_SHADER_SOURCE_VERIFY=1 (or key verify).
      const auto& glsl=shaders[i]->source_text();
      if(glsl.empty())
        LOG("[vulkan] %s GLSL not kept; set WWHD_VK_SHADER_SOURCE_VERIFY=1 to capture it",name);
      else if(FILE* f=fopen((std::string(name)+".glsl").c_str(),"wb")) {
        fwrite(glsl.data(),1,glsl.size(),f); fclose(f);
      }
      if(FILE* f=fopen((std::string(name)+".spv").c_str(),"wb")) {
        fwrite(words[i]->data(),4,words[i]->size(),f); fclose(f);
      }
    }
  } catch(...) { /* Diagnostic capture must never replace driver recovery with a crash. */ }
}
// wait=false: poll, or wait at most budgetNs (Shader compile Fast), and return with the
// compile still pending when it is not done by then
Pipeline& resolve_pending_pipeline(Pipeline& pipeline, bool wait = true, bool present = false, uint64_t budgetNs = 0) {
  thread_check::note(thread_check::Area::Pipelines);  // (diagnostics: thread_check.h)
  draw_threads::CacheLock locked(draw_threads::pipeline_mutex());  // (stage 2.4: the pipeline is shared)
  if (pipeline.compileError) std::rethrow_exception(pipeline.compileError);
  if (!pipeline.pendingCompile) return pipeline;
  try {
    auto& pending = *pipeline.pendingCompile;
    std::optional<PipelineBuildResult> built = pending.ticket->wait_for(wait ? 0 : budgetNs);
    if (!built && !wait) return pipeline;
    if (!built) {
      // something must wait for this pipeline now: its compile and its shaders' SPIR-V jump the queues
      // (Shader compile Fast queues the work of draws that were left out)
      if (bgshaders::enabled()) {
        for (auto* shader : pending.shaders)
          if (shader && shader->binary) {
            std::lock_guard lock(shader->binary->mutex);
            if (shader->binary->ticket) shader->binary->ticket->prioritize();
          }
        pending.ticket->prioritize();
      }
      if (present) begin_pipeline_pending_frame();
      built = pending.ticket->wait(present ? std::function<void()>(present_completed_frame_while_pipeline_pending)
                                           : std::function<void()>{});
    }
    draw_counters().pipelineCreateNs += built->compileNs;
    ++draw_counters().pipelineCreates;
    if(recoverable_pipeline_failure(built->result)) {
      capture_pipeline_failure(built->result,pending.shaders[0],pending.shaders[1],pending.recipe->words(0),pending.recipe->words(1));
      pipeline.pendingCompile.reset(); // terminal failure, never a pending omission
      return pipeline;
    }
    vk_check(built->result, "graphics pipeline");
    pipeline.pipeline = built->pipeline;
    auto recipe = std::move(pending.recipe);
    const bool keepRecipe = pending.keepRecipe;
    // Publish the consumed result before fallible recipe bookkeeping. Never
    // retain a consumed ticket, even when recording or driver validation throws.
    pipeline.pendingCompile.reset();
    R.pipelineCacheDirty = true;
    R.pipelineCacheChangedFrame = R.frame;
    if (keepRecipe) remember_pipeline_recipe(std::move(recipe));
    return pipeline;
  } catch (...) {
    if (!pipeline.pipeline) pipeline.compileError = std::current_exception();
    pipeline.pendingCompile.reset();
    throw;
  }
}
// VR (docs/VR_PERFORMANCE.md, "Draws left out while their pipeline is made"): a draw whose pipeline (and its shaders'
// SPIR-V) is still being made the first time it is needed is left out of this frame instead of the render thread
// waiting for it: in the headset that wait was a freeze (20 to 120 ms for a new shader, e.g. the sword's glow the first
// time it is drawn), now the thing appears a frame or two later. Checked without waiting (resolve_pending_pipeline,
// wait = false); once made it is drawn as always. Within a frame the decision is kept for the other eye (eye 1 draws
// after eye 0: it must not show what eye 0 left out). The desktop keeps every draw (it waits, as before). Counted and
// logged every 10 s while it happens
std::atomic<uint64_t> g_skippedDraws{0};
bool skip_while_made(Pipeline& p) {
  // (2026-10-31) Turned off on 2026-10-28 after it crashed the record worker in the driver: this check resolved the
  // pipeline on the render thread (taking its result) while the worker resolved the same one replaying the deferred
  // bind. It now only looks: `watch` (set with the compile, never touched by the worker) and the ticket's ready()
  // (no result taken). Once made, the draw goes the usual way and whoever binds it takes the result, as without this
  if (!skip_pending_draws()) return false;
  draw_threads::CacheLock locked(draw_threads::pipeline_mutex());  // (the eye's and the atlas's threads draw too)
  if (p.skippedFrame == R.frame) return true;
  if (!p.watch) return false;
  if (!p.watch->ticket || p.watch->ticket->ready()) {  // (made, or failed: the usual paths)
    p.watch.reset();
    return false;
  }
  p.skippedFrame = R.frame;
  g_skippedDraws.fetch_add(1, std::memory_order_relaxed);
  static uint64_t loggedFrame = 0;
  if (R.frame >= loggedFrame + 900) {
    loggedFrame = R.frame;
    LOG("[vulkan] %llu draws left out so far while their pipeline was made for its first use (no wait)",
        (unsigned long long)g_skippedDraws.load(std::memory_order_relaxed));
  }
  return true;
}
// Shader compile (background_shaders.h): the targets a draw writes
static bool writes_color(const uint32_t* r, int i) { return (r[REGADDR::CB_TARGET_MASK] >> (4 * i)) & 15; }
static bool writes_depth(const uint32_t* r, const Surface* depth) {
  const uint32_t dc = r[REGADDR::DB_DEPTH_CONTROL];  // bit 0 stencil, bit 2 depth write
  return depth && ((dc & 4) || ((dc & 1) && depth->fmt.stencil));
}
static void track_drawn(const uint32_t* r, const std::array<Surface*, 8>& colors, Surface* depth) {
  auto& t = bgshaders::tracker();
  for (int i = 0; i < 8; i++)
    if (colors[i] && writes_color(r, i)) t.drawn(colors[i]->addr, R.frame);
  if (writes_depth(r, depth)) t.drawn(depth->addr, R.frame);
}
// The pipeline is still compiling: wait the frame's budget if this draw may be left out (true: draw
// it now, ready or with the exact wait that follows), false: left out of this frame.
static bool background_wait(const uint32_t* r, Pipeline& p, const std::array<Surface*, 8>& colors, Surface* depth) {
  auto& t = bgshaders::tracker();
  bgshaders::TargetFacts written[8], depthFacts;
  int count = 0;
  for (int i = 0; i < 8; i++)
    if (colors[i] && writes_color(r, i)) written[count++] = t.facts(colors[i]->addr, R.frame);
  const bool depthWritten = writes_depth(r, depth);
  if (depthWritten) depthFacts = t.facts(depth->addr, R.frame);
  // the depth buffer the game's sun check peeks (peek_z) waits even before the first peek
  if (depthWritten && depth->addr == R.mainDepthAddr) depthFacts.peeked = true;
  const auto verdict = bgshaders::classify(bgshaders::level(), written, count, depthWritten ? &depthFacts : nullptr);
  if (verdict != bgshaders::kSkippable) { bgshaders::exact(verdict); return true; }
  const uint64_t left = bgshaders::budget_left_ns(R.frame);
  const auto started = std::chrono::steady_clock::now();
  resolve_pending_pipeline(p, false, false, left);
  const bool ready = !p.pendingCompile;
  bgshaders::spent(R.frame, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - started).count()), !ready);
  if (ready) return true;
  bgshaders::skipped(R.frame);
  for (int i = 0; i < 8; i++)
    if (colors[i] && writes_color(r, i)) t.left_out(colors[i]->addr, R.frame);
  if (depthWritten) t.left_out(depth->addr, R.frame);
  return false;
}
// ---------------------------------------------------------------- HUD coverage (60 fps generated)
// (WindWakerHD-Android-Plus fc5ddee, via GreenNaugahyde v0.9.9 vk_draw.cpp, MPL-2.0) From the first layout
// drawn on the TV picture (reproj::layout, where the scene is copied aside) each draw into that picture
// also writes how much it covers what lies behind: an R8 image of the picture's size cleared to 0
// (reproj.cpp), written by a variant of the pixel shader (hud_coverage.h) with a blend that follows the
// draw's own. The in-between frame then moves the scene behind translucent HUD parts too; a frame with a
// HUD draw that couldn't write it (a shader without the variant, another target alongside, its pipeline
// still compiling) is generated with the HUD found by comparing pictures. WWHD_HUD_COVERAGE=0: off.
// Nothing of it runs unless 60 fps (generated) does (reproj::hud_coverage_wanted).
struct CoverageWords { std::vector<uint32_t> words; uint32_t location = 0; };
std::unordered_map<uint64_t, CoverageWords> coverageVariants;  // by pixel shader key; empty words: none
// the pixel shader's coverage variant and the location it writes, or null (none, or its SPIR-V not ready yet)
const std::vector<uint32_t>* coverage_words(const vk::Shader* ps, uint32_t& location) {
  if (!ps) return nullptr;
  if (auto it = coverageVariants.find(ps->key); it != coverageVariants.end()) {
    location = it->second.location;
    return it->second.words.empty() ? nullptr : &it->second.words;
  }
  std::vector<uint32_t> words = ps->spirv();
  if (words.empty() && ps->binary) {
    if (!vk::shader_binary_ready(ps->binary)) return nullptr;  // (asked again at its next draw)
    words = vk::shader_binary_words(ps->binary);
  }
  if (words.empty()) return nullptr;
  CoverageWords v;
  if (hud_coverage::add_output(words, v.location)) v.words = std::move(words);
  static int failures = 0;
  if (v.words.empty() && failures++ < 8) LOG("[framegen] HUD shader %016llx: no coverage variant", (unsigned long long)ps->key);
  auto& kept = coverageVariants[ps->key] = std::move(v);
  location = kept.location;
  return kept.words.empty() ? nullptr : &kept.words;
}
// the coverage attachment's blend from the picture's: what the draw leaves of the picture behind
VkPipelineColorBlendAttachmentState coverage_blend(const VkPipelineColorBlendAttachmentState& c0) {
  VkPipelineColorBlendAttachmentState c{};
  c.colorWriteMask = (c0.colorWriteMask & 7) ? VK_COLOR_COMPONENT_R_BIT : 0;
  if (!c0.blendEnable) return c;  // replaces what was there: covered
  c.blendEnable = VK_TRUE;
  c.colorBlendOp = c.alphaBlendOp = VK_BLEND_OP_ADD;
  VkBlendFactor src = VK_BLEND_FACTOR_SRC_ALPHA, dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;  // "over"
  if (c0.colorBlendOp == VK_BLEND_OP_ADD || c0.colorBlendOp == VK_BLEND_OP_SUBTRACT) switch (c0.dstColorBlendFactor) {
      case VK_BLEND_FACTOR_ZERO: src = VK_BLEND_FACTOR_ONE; dst = VK_BLEND_FACTOR_ZERO; break;          // replaces
      case VK_BLEND_FACTOR_ONE: src = VK_BLEND_FACTOR_ZERO; dst = VK_BLEND_FACTOR_ONE; break;           // adds: covers nothing
      case VK_BLEND_FACTOR_SRC_ALPHA: src = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; dst = VK_BLEND_FACTOR_SRC_ALPHA; break;
      default: break;
    }
  c.srcColorBlendFactor = c.srcAlphaBlendFactor = src;
  c.dstColorBlendFactor = c.dstAlphaBlendFactor = dst;
  return c;
}
// Setup's Vulkan preparation (derive_guest_pipeline_recipe): pipeline() hands the recipe it would compile to this
// sink instead of compiling it or taking a warmed pipeline. Render thread only.
std::vector<std::shared_ptr<PipelineRecipe>>* g_recipeSink = nullptr;
Pipeline &pipeline(const uint32_t *r, vk::Shader *vs, vk::Shader *ps,
                   LatteFetchShader *fs, VkPrimitiveTopology topology,
                   const std::array<Surface *, 8> &colors, Surface *depth, uint32_t covSlot = 0) {
  thread_check::note(thread_check::Area::Pipelines);  // (diagnostics: thread_check.h)
  if(preparation_stats_enabled())++draw_counters().cpuPreparation.pipelineLookups;
  if (g_recipeSink) covSlot = 0;  // (the setup's preparation: never a coverage variant)
  const PipelineKey key = pipeline_key(r, vs, ps, fs, topology, colors, depth, covSlot);
  // the setup's model sweep: every pipeline a swept archive's models use is filed under that archive (once each)
  if (!covSlot && pipeline_groups::sweeping() && !pipeline_groups::sweeping_new_only()) {
    static std::unordered_set<uint64_t> noted;
    for (const uint32_t group : pipeline_groups::sweeping_all()) {
      const uint64_t id = PipelineKeyHash{}(key) * 1099511628211ull ^ group;
      if (noted.insert(id).second) {
        auto portable = key;
        portable.vs = vs->pipelineId; portable.ps = ps->pipelineId;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&portable);
        note_pipeline_group(std::vector<uint8_t>(bytes, bytes + sizeof portable), group);
      }
    }
  }
  std::array<VkFormat, 8> formats{};
  uint32_t ncolor = 0;
  for (int i = 0; i < 8; i++) {
    formats[i] = colors[i] ? colors[i]->fmt.pixel : VK_FORMAT_UNDEFINED;
    if (colors[i])
      ncolor = i + 1;
  }
  VkFormat df = depth ? depth->fmt.pixel : VK_FORMAT_UNDEFINED;
  if (const uint64_t g = pipelineMemoGeneration.load(std::memory_order_acquire); threadPipelineMemoGeneration() != g) {
    threadPipelineMemoGeneration() = g;  // reset since this thread's memos were made
    lastPipelineLookup() = {};
    pipelineLookaside() = {};
  }
  if (lastPipelineLookup().device != R.device || pipelines.empty()) {
    lastPipelineLookup() = {};
    pipelineLookaside() = {};
  }
  if (lastPipelineLookup().value && lastPipelineLookup().key == key) {
    if(preparation_stats_enabled())++draw_counters().cpuPreparation.pipelineLastHits;
    return *lastPipelineLookup().value;
  }
  const bool useLookaside = pipeline_lookaside_enabled();
  if (useLookaside)
    if (auto* value = pipelineLookaside().find(key, R.device)) {
      if(preparation_stats_enabled())++draw_counters().cpuPreparation.pipelineLookasideHits;
      lastPipelineLookup() = {key, R.device, value};
      return *value;
    }
  auto remember = [&](Pipeline& value) -> Pipeline& {
    render_objects::set_pipelines(pipelines.size());  // crash logs, performance report
    lastPipelineLookup() = {key, R.device, &value};
    if (useLookaside) pipelineLookaside().remember(lastPipelineLookup());
    return value;
  };
  if(preparation_stats_enabled())++draw_counters().cpuPreparation.pipelineMapLookups;
  // the map is shared: found and inserted under its lock (stage 2.4). Its elements stay where they are when it
  // grows, so a reference found here stays valid
  draw_threads::CacheLock locked(draw_threads::pipeline_mutex());
  if (auto it = pipelines.find(key); it != pipelines.end())
    return remember(it->second);
  Pipeline p;
  vk::Shader *shaders[] = {vs, ps};
  uint32_t uniformBindings = 0;
  for (auto *shader : shaders) {
    uniformBindings += shader->mapping.uniformVarsBufferBindingPoint >= 0;
    for (int binding : shader->mapping.uniformBuffersBindingPoint)
      uniformBindings += binding >= 0;
  }
#ifdef WWHD_MOBILE_GPU_PATHS
  // (uniform_plan.h) over the cap, some blocks dynamic rather than none: the vertex stage's first
  const uniform_plan::Plan uplan = uniform_plan::plan(vs->mapping, ps->mapping,
      uniform_plan::dynamic_limit(R.properties.limits.maxDescriptorSetUniformBuffersDynamic));
  (void)uniformBindings;
  p.dynamicUniforms = uplan.all;
  p.partialUniforms = uplan.partial();
  p.dynamicMask[0] = uplan.mask[0];
  p.dynamicMask[1] = uplan.mask[1];
  auto uniformTypeOf = [&](int st, uint32_t slot) {
    return uplan.dynamic(st, slot) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  };
#else
  // Dynamic UBO limits apply across both sets in the pipeline layout. Keep
  // both stages on the existing regular path if the complete layout exceeds it.
  p.dynamicUniforms = uniformBindings <=
      R.properties.limits.maxDescriptorSetUniformBuffersDynamic;
  const auto uniformType = p.dynamicUniforms
      ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
      : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
#endif
  std::array<std::vector<VkDescriptorSetLayoutBinding>, 2> descriptorBindings;
  for (int st = 0; st < 2; st++) {
    auto &m = shaders[st]->mapping;
    auto& b = descriptorBindings[st];
    auto add = [&](int i, VkDescriptorType t) {
      if (i >= 0)
        b.push_back(
            {uint32_t(i), t, 1,
             VkShaderStageFlags(st ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT),
             nullptr});
    };
#ifdef WWHD_MOBILE_GPU_PATHS
    add(m.uniformVarsBufferBindingPoint, uniformTypeOf(st, uniform_plan::kSupportSlot));
    for (uint32_t slot = 0; slot < LATTE_NUM_MAX_UNIFORM_BUFFERS; ++slot)
      add(m.uniformBuffersBindingPoint[slot], uniformTypeOf(st, slot));
#else
    add(m.uniformVarsBufferBindingPoint, uniformType);
    for (auto i : m.uniformBuffersBindingPoint)
      add(i, uniformType);
#endif
    for (int i = 0; i < m.textureUnitCount; i++)
      add(m.textureUnitBaseBindingPoint + i,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    if (m.tfStorageBindingPoint >= 0)
      exception_report::raise("Vulkan transform feedback unsupported");
  }
  auto portableKey = key;
  portableKey.vs = vs->pipelineId; portableKey.ps = ps->pipelineId;
  const auto* keyBytes = reinterpret_cast<const uint8_t*>(&portableKey);
  std::vector<uint8_t> recipeKey(keyBytes, keyBytes + sizeof key);
  // debug: WWHD_VK_KEY_LOG=file appends one line per pipeline this run creates: steady-clock ns, renderer frame,
  // portable key digest (FNV-1a of the recipe key), in-memory key hash (as in the stutter trace), D-sweep tag
  static FILE* keyLog = [] { const char* p = getenv("WWHD_VK_KEY_LOG"); return p && *p ? std::fopen(p, "a") : nullptr; }();
  if (keyLog) {
    uint64_t digest = 1469598103934665603ull;
    for (uint8_t byte : recipeKey) digest = (digest ^ byte) * 1099511628211ull;
    std::fprintf(keyLog, "%lld %llu %016llx %016llx %d\n",
        (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
        (unsigned long long)R.frame, (unsigned long long)digest, (unsigned long long)PipelineKeyHash{}(key), dsweep::tag());
    std::fflush(keyLog);
  }
  // debug: WWHD_VK_PIPELINE_WHO=file: per pipeline created, the renderer frame, the key hash (as in the stutter trace)
  // and what draws it (gx2::current_draw_context, described by dsweep.cpp: guest call chain, archive resources, actors)
  static FILE* whoLog = [] { const char* p = getenv("WWHD_VK_PIPELINE_WHO"); return p && *p ? std::fopen(p, "a") : nullptr; }();
  if (whoLog) {
    std::fprintf(whoLog, "%llu %016llx %s\n", (unsigned long long)R.frame, (unsigned long long)PipelineKeyHash{}(key),
                 gx2::current_draw_context().c_str());
    std::fflush(whoLog);
  }
  // Own prerequisite identities independently of the mutable shader cache.
  const std::array<std::shared_ptr<vk::ShaderBinary>, 2> shaderBinaries{vs->binary, ps->binary};
  std::array<std::vector<uint32_t>, 2> recipeSpirv{vs->spirv(), ps->spirv()};
  for(size_t i=0;i<shaderBinaries.size();++i)
    if(recipeSpirv[i].empty() && shaderBinaries[i]) {
      if(!async_pipeline_enabled()) recipeSpirv[i]=vk::shader_binary_words_exact(shaderBinaries[i],R.frame);
      else if(vk::shader_binary_ready(shaderBinaries[i])) recipeSpirv[i]=vk::shader_binary_words(shaderBinaries[i]);
    }
  // 60 fps (generated): the pixel shader's HUD coverage variant (the draw checked it can be made)
  if (covSlot) {
    uint32_t location = 0;
    const auto* words = coverage_words(ps, location);
    if (!words || location != covSlot) exception_report::raise("HUD coverage variant missing");
    recipeSpirv[1] = *words;
  }
  for (int st = 0; st < 2; st++) {
    auto& b = descriptorBindings[st];
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = b.size();
    ci.pBindings = b.data();
    vk_check(vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &p.sets[st]),
             "descriptor layout");
  }
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.setLayoutCount = 2;
  lc.pSetLayouts = p.sets;
  vk_check(vkCreatePipelineLayout(R.device, &lc, nullptr, &p.layout),
           "pipeline layout");
  VkShaderModule modules[2]{};
  VkPipelineShaderStageCreateInfo stages[2]{};
  for (int i = 0; i < 2; i++) {
    if(!async_pipeline_enabled()) {
      VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      mc.codeSize = recipeSpirv[i].size() * sizeof(uint32_t);
      mc.pCode = recipeSpirv[i].data();
      vk_check(vkCreateShaderModule(R.device, &mc, nullptr, &modules[i]), "shader module");
    }
    stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[i].stage =
        i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    stages[i].module = modules[i];
    stages[i].pName = "main";
  }
  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  p.trimFetch = fs;
  p.trimVertex = vs;
  p.bindingTrims.reserve(fs->bufferGroups.size());
  uint32_t tailLocation = vk::vertex_tail_location(vs->mapping.attributeMapping);
  for (auto &g : fs->bufferGroups) {
    const uint32_t cpuCount = vk::cpu_vertex_count(g, R.vertexDiagnostics.cpuClasses);
    const uint32_t cpuBinding = cpuCount ? vk::cpu_vertex_binding(*fs, g, R.vertexDiagnostics.cpuClasses,
        R.properties.limits.maxVertexInputBindings) : g.attributeBufferIndex;
    if (cpuCount && cpuBinding >= R.properties.limits.maxVertexInputBindings)
      exception_report::raise("CPU vertex classes exceed device vertex binding limit");
    bool instance = false;
    BindingTrimMetadata trim;
    std::optional<LatteConst::VertexFetchType2> fetchRate;
    for (int j = 0; j < g.attribCount; j++) {
      auto &a = g.attrib[j];
      const bool cpu = vk::cpu_vertex_selected(a, R.vertexDiagnostics.cpuClasses);
      const uint32_t cpuOffset = vk::cpu_vertex_offset(g, j, R.vertexDiagnostics.cpuClasses);
      int loc = vs->mapping.attributeMapping[a.semanticId];
      if (loc < 0)
        continue;
      auto fmt = R.vertexFormats.fetch(vk::vertex_format(a.format));
      if (fmt == VK_FORMAT_UNDEFINED)
        exception_report::raise("unsupported vertex format");
      if (cpu)
        // CPU-converted stream: 16 bytes per attribute of the group (cpu_vertex_stream)
        attributes.push_back({uint32_t(loc), cpuBinding, vk::cpu_vertex_format(a), cpuOffset});
      else
      attributes.push_back(
          {uint32_t(loc), g.attributeBufferIndex, fmt, a.offset});
      const auto nativeFormat = vk::vertex_format(a.format);
      // Metadata only: never record vertex contents, guest addresses, or save data.
      static const bool observeVertexInputs = [] {
#ifdef __ANDROID__
        return true;
#else
        const char* setting = std::getenv("WWHD_VK_VERTEX_METADATA");
        return setting && !std::strcmp(setting, "1");
#endif
      }();
      if (observeVertexInputs) {
        // Keep the offset and its own guest binding stride together. An offset-limit check
        // alone says nothing about access beyond stride; zero stride is a constant input.
        const uint32_t observedStride =
            (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
        const uint64_t attributeEnd = uint64_t(a.offset) + vertex_format_bytes(nativeFormat);
        static thread_local std::set<std::tuple<unsigned, unsigned, unsigned, unsigned, unsigned, unsigned>> seenVertexInputs;
        const auto diagnostic = std::make_tuple(unsigned(a.format), unsigned(a.endianSwap),
            unsigned(a.nfa), unsigned(a.isSigned), unsigned(a.offset), unsigned(observedStride));
        if (seenVertexInputs.size() < 64 && seenVertexInputs.insert(diagnostic).second)
          LOG("[vulkan vertex observed] guestFormat=0x%02x native=%s fetch=%s endian=%u numeric=%u signed=%u offset=%u offsetMod4=%u offsetWithinLimit=%u fallback=%u stride=%u attributeEnd=%llu offsetBeyondStride=%u attributeEndBeyondStride=%u cpu=%u",
              unsigned(a.format), vk::vertex_format_name(nativeFormat),
              cpu ? (vk::cpu_vertex_float(a) ? "R32G32B32A32_SFLOAT" : "R32G32B32A32_UINT") : vk::vertex_format_name(fmt), unsigned(a.endianSwap),
              unsigned(a.nfa), unsigned(a.isSigned), a.offset, a.offset % 4, unsigned(a.offset <= R.properties.limits.maxVertexInputAttributeOffset),
              unsigned(!cpu && R.vertexFormats.fallback(nativeFormat)), observedStride, static_cast<unsigned long long>(attributeEnd),
              unsigned(observedStride && a.offset >= observedStride), unsigned(observedStride && attributeEnd > observedStride), unsigned(cpu));
      }
      if (!cpu && R.vertexFormats.fallback(nativeFormat)) {
        if (tailLocation >= R.properties.limits.maxVertexInputAttributes ||
            attributes.size() >= R.properties.limits.maxVertexInputAttributes)
          exception_report::raise("RGB fallback exceeds device vertex attribute limit");
        const uint64_t tailOffset = uint64_t(a.offset) + vk::vertex_tail_offset(nativeFormat);
        if (tailOffset > R.properties.limits.maxVertexInputAttributeOffset)
          exception_report::raise("RGB fallback exceeds device vertex attribute offset limit");
        attributes.push_back({tailLocation++, g.attributeBufferIndex,
            vk::vertex_tail_format(nativeFormat), uint32_t(tailOffset)});
      }
      // Split fetches cover exactly the native attribute bytes.
      if (trim.supported) {
        const uint32_t width = vertex_format_bytes(vk::vertex_format(a.format));
        if (!width || (trim.rate && *trim.rate != a.fetchType) ||
            (a.fetchType != LatteConst::VertexFetchType2::VERTEX_DATA &&
             a.fetchType != LatteConst::VertexFetchType2::INSTANCE_DATA) ||
            (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA && a.aluDivisor != 1))
          trim.supported = false;
        else {
          trim.rate = a.fetchType;
          trim.attributeEnd = std::max(trim.attributeEnd, uint64_t(a.offset) + width);
        }
      }
      if (fetchRate && *fetchRate != a.fetchType)
        exception_report::raise("mixed vertex/instance rate in one buffer");
      fetchRate = a.fetchType;
      if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) {
        if (a.aluDivisor != 1)
          exception_report::raise("instance divisor unsupported");
        instance = true;
      }
    }
    uint32_t stride =
        (r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >>
         11) &
        0xFFFF;
#ifdef __ANDROID__
    static thread_local std::set<uint32_t> seenStrides;
    if (seenStrides.size() < 32 && seenStrides.insert(stride).second)
      LOG("[vulkan stride observed] stride=%u mod4=%u withinLimit=%u", stride, stride % 4,
          stride <= R.properties.limits.maxVertexInputBindingStride);
#endif
    const auto rate = instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    if (cpuCount < uint32_t(g.attribCount)) bindings.push_back({g.attributeBufferIndex, stride, rate});
    if (cpuCount) bindings.push_back({cpuBinding, stride ? 16 * cpuCount : 0, rate});
    trim.stride = stride;
    p.bindingTrims.push_back(trim);
  }
  if (R.vertexFormats.fallbackMask && attributes.size() > R.properties.limits.maxVertexInputAttributes)
    exception_report::raise("RGB fallback exceeds device vertex attribute limit");
  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = bindings.size();
  vi.pVertexBindingDescriptions = bindings.data();
  vi.vertexAttributeDescriptionCount = attributes.size();
  vi.pVertexAttributeDescriptions = attributes.data();
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topology;
  // Strip restart is native on Metal and required by MoltenVK portability.
  ia.primitiveRestartEnable = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                              topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  LATTE_PA_SU_SC_MODE_CNTL pm;
  std::memcpy(&pm, r + REGADDR::PA_SU_SC_MODE_CNTL, 4);
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  // Far-plane clipping off: depth clamp (depth_clamp_wanted), or without the device feature the vertex
  // shader's clamp at the near plane (vk::depth_clamp_variant_wanted, part of the vertex shader's key)
  if (depth_clamp_wanted(r))
    rs.depthClampEnable = VK_TRUE;
  rs.lineWidth = 1;
  rs.cullMode = (pm.get_CULL_FRONT() ? VK_CULL_MODE_FRONT_BIT : 0) |
                (pm.get_CULL_BACK() ? VK_CULL_MODE_BACK_BIT : 0);
  rs.frontFace =
      pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW
          ? VK_FRONT_FACE_COUNTER_CLOCKWISE
          : VK_FRONT_FACE_CLOCKWISE;
  rs.depthBiasEnable = pm.get_OFFSET_FRONT_ENABLED();
  rs.depthBiasConstantFactor = f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]);
  rs.depthBiasSlopeFactor = f32(r[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16;
  if (rs.depthBiasEnable) {
    rs.depthBiasClamp = f32(r[REGADDR::PA_SU_POLY_OFFSET_CLAMP]);
    if (rs.depthBiasClamp != 0 && !R.enabledFeatures.depthBiasClamp)
      exception_report::raise("device lacks depth bias clamp requested by guest");
  }
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  LATTE_DB_DEPTH_CONTROL dc;
  std::memcpy(&dc, r + REGADDR::DB_DEPTH_CONTROL, 4);
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = depth && dc.get_Z_ENABLE();
  ds.depthWriteEnable = depth && dc.get_Z_WRITE_ENABLE();
  ds.depthCompareOp = VkCompareOp(dc.get_Z_FUNC());
  ds.stencilTestEnable = depth && depth->fmt.stencil && dc.get_STENCIL_ENABLE();
  ds.front = {stencil(static_cast<uint32_t>(dc.get_STENCIL_FAIL_F())),
              stencil(static_cast<uint32_t>(dc.get_STENCIL_ZPASS_F())),
              stencil(static_cast<uint32_t>(dc.get_STENCIL_ZFAIL_F())),
              VkCompareOp(dc.get_STENCIL_FUNC_F()),
              (r[REGADDR::DB_STENCILREFMASK] >> 8) & 255,
              (r[REGADDR::DB_STENCILREFMASK] >> 16) & 255,
              r[REGADDR::DB_STENCILREFMASK] & 255};
  ds.back = ds.front;
  if (dc.get_BACK_STENCIL_ENABLE())
    ds.back = {stencil(static_cast<uint32_t>(dc.get_STENCIL_FAIL_B())),
               stencil(static_cast<uint32_t>(dc.get_STENCIL_ZPASS_B())),
               stencil(static_cast<uint32_t>(dc.get_STENCIL_ZFAIL_B())),
               VkCompareOp(dc.get_STENCIL_FUNC_B()),
               (r[REGADDR::DB_STENCILREFMASK_BF] >> 8) & 255,
               (r[REGADDR::DB_STENCILREFMASK_BF] >> 16) & 255,
               r[REGADDR::DB_STENCILREFMASK_BF] & 255};
  std::array<VkPipelineColorBlendAttachmentState, 8> cb{};
  for (uint32_t i = 0; i < ncolor; i++) {
    auto &b = cb[i];
    b.colorWriteMask = (r[REGADDR::CB_TARGET_MASK] >> (4 * i)) & 15;
    b.blendEnable = colors[i] && colors[i]->fmt.kind == FormatInfo::FLOAT &&
                    ((r[REGADDR::CB_COLOR_CONTROL] >> (8 + i)) & 1);
    LATTE_CB_BLENDN_CONTROL raw;
    std::memcpy(&raw, r + REGADDR::CB_BLEND0_CONTROL + i, 4);
    b.srcColorBlendFactor =
        blend(static_cast<uint32_t>(raw.get_COLOR_SRCBLEND()));
    b.dstColorBlendFactor =
        blend(static_cast<uint32_t>(raw.get_COLOR_DSTBLEND()));
    b.colorBlendOp = blendop(static_cast<uint32_t>(raw.get_COLOR_COMB_FCN()));
    b.srcAlphaBlendFactor =
        raw.get_SEPARATE_ALPHA_BLEND()
            ? blend(static_cast<uint32_t>(raw.get_ALPHA_SRCBLEND()))
            : b.srcColorBlendFactor;
    b.dstAlphaBlendFactor =
        raw.get_SEPARATE_ALPHA_BLEND()
            ? blend(static_cast<uint32_t>(raw.get_ALPHA_DSTBLEND()))
            : b.dstColorBlendFactor;
    b.alphaBlendOp =
        raw.get_SEPARATE_ALPHA_BLEND()
            ? blendop(static_cast<uint32_t>(raw.get_ALPHA_COMB_FCN()))
            : b.colorBlendOp;
    // (the UI layer's image, ui_layer) its alpha is the coverage the quad layer blends by: always written, and blended
    // "over" (a draw onto the game's opaque picture need not write it, or writes it as the colour factors say); the
    // colour keeps the game's blend, which over the cleared image leaves it premultiplied
    if (i == 0 && ui_layer::is(colors[0])) {
      // (each pipeline once: the blends the game's UI uses, for the dumps' frame_<n>_ui.png)
      static int logged = 0;
      if (logged++ < 64)
        LOG("[vr ui] pipeline: blend %s colour %d*src %s %d*dst, alpha %d*src %s %d*dst, writes %X%s",
            b.blendEnable ? "on" : "off", int(b.srcColorBlendFactor), b.colorBlendOp == VK_BLEND_OP_ADD ? "+" : "op",
            int(b.dstColorBlendFactor), int(b.srcAlphaBlendFactor), b.alphaBlendOp == VK_BLEND_OP_ADD ? "+" : "op",
            int(b.dstAlphaBlendFactor), unsigned(b.colorWriteMask),
            dc.get_Z_ENABLE() || dc.get_STENCIL_ENABLE() ? " (the game tests depth or stencil: none here)" : "");
    }
    if (i == 0 && ui_layer::is(colors[0]) && (b.colorWriteMask & 7)) {
      b.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
      if (b.blendEnable) {
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
      }
    }
  }
  if (covSlot && covSlot < ncolor) cb[covSlot] = coverage_blend(cb[0]);  // (60 fps (generated): hud_coverage.h)
  VkPipelineColorBlendStateCreateInfo bs{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  bs.attachmentCount = ncolor;
  bs.pAttachments = cb.data();
  uint32_t rop = (r[REGADDR::CB_COLOR_CONTROL] >> 16) & 255;
  bs.logicOp = VK_LOGIC_OP_COPY;
  if (rop != 0xCC) {
    if (!R.enabledFeatures.logicOp)
      exception_report::raise("color logic operation unsupported by device");
    bs.logicOpEnable = VK_TRUE;
    switch (rop) {
    case 0x00:
      bs.logicOp = VK_LOGIC_OP_CLEAR;
      break;
    case 0x88:
      bs.logicOp = VK_LOGIC_OP_AND;
      break;
    case 0x44:
      bs.logicOp = VK_LOGIC_OP_AND_REVERSE;
      break;
    case 0x22:
      bs.logicOp = VK_LOGIC_OP_AND_INVERTED;
      break;
    case 0xAA:
      bs.logicOp = VK_LOGIC_OP_NO_OP;
      break;
    case 0x66:
      bs.logicOp = VK_LOGIC_OP_XOR;
      break;
    case 0xEE:
      bs.logicOp = VK_LOGIC_OP_OR;
      break;
    case 0x11:
      bs.logicOp = VK_LOGIC_OP_NOR;
      break;
    case 0x99:
      bs.logicOp = VK_LOGIC_OP_EQUIVALENT;
      break;
    case 0x55:
      bs.logicOp = VK_LOGIC_OP_INVERT;
      break;
    case 0xDD:
      bs.logicOp = VK_LOGIC_OP_OR_REVERSE;
      break;
    case 0x33:
      bs.logicOp = VK_LOGIC_OP_COPY_INVERTED;
      break;
    case 0xBB:
      bs.logicOp = VK_LOGIC_OP_OR_INVERTED;
      break;
    case 0x77:
      bs.logicOp = VK_LOGIC_OP_NAND;
      break;
    case 0xFF:
      bs.logicOp = VK_LOGIC_OP_SET;
      break;
    default:
      exception_report::raise("unsupported color logic operation");
    }
  }
  for (uint32_t i = 0; i < ncolor; ++i)
    if (cb[i].blendEnable) {
      auto &b = cb[i];
      if (!R.enabledFeatures.dualSrcBlend &&
          (b.srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
           b.dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
           b.srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
           b.dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR))
        exception_report::raise("dual-source blend unsupported by device");
      if (!R.constantAlphaColorBlendFactors &&
          (b.srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
           b.srcColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA ||
           b.dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
           b.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA))
        exception_report::raise(
            "constant alpha color blend unsupported by device");
    }
  if (ds.stencilTestEnable && !R.separateStencilMaskRef &&
      (ds.front.compareMask != ds.back.compareMask ||
       ds.front.writeMask != ds.back.writeMask ||
       ds.front.reference != ds.back.reference))
    exception_report::raise("separate stencil state unsupported by device");
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                          VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                          VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dy{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = std::size(dyn);
  dy.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo rc{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rc.colorAttachmentCount = ncolor;
  rc.pColorAttachmentFormats = formats.data();
  rc.depthAttachmentFormat = df;
  rc.stencilAttachmentFormat =
      depth && depth->fmt.stencil ? df : VK_FORMAT_UNDEFINED;
  VkGraphicsPipelineCreateInfo ci{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_targets(ci, rc);
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
  ci.layout = p.layout;
  GraphicsPipelineState pipelineState(ci, &rc);
  if (multiview_draws()) {  // (docs/VR_MULTIVIEW.md, stage M4) what its multiview twins are made from
    auto base = std::make_shared<vk::multiview::Base>(pipelineState);
    base->descriptors = descriptorBindings;
    p.mvBase = std::move(base);
  }
  // (docs/VR_DLSS.md, stage D2b 2c; WWHD_VK_MOTION_PIPELINES=1 with a DLSS feature chosen) its motion twin built now,
  // on the motion builder's thread, and counted there; not drawn with yet
  // (WWHD_VK_MOTION_PIPELINES=1, or whenever the motion pass runs: DLSS chosen and offered)
  static const bool motionPipelines = (switches::env_on("WWHD_VK_MOTION_PIPELINES") || motion_pass::on()) && motion_on();
  if (motionPipelines && vs->retainedSource && ps->retainedSource) [[unlikely]] {
    auto base = std::make_shared<vk::multiview::Base>(pipelineState);
    base->descriptors = descriptorBindings;
    base->sets[0] = p.sets[0];
    base->sets[1] = p.sets[1];
    vk::motion::Stage stages[2];
    stages[0].variant.vertex = true;
    for (uint32_t i = 0; i < 16; ++i)
      if (const int b = vs->mapping.uniformBuffersBindingPoint[i]; b >= 0) {
        stages[0].variant.blocks |= 1u << i;
        stages[0].perFrameBindings.push_back(uint32_t(b));
      }
    if (const int b = vs->mapping.uniformVarsBufferBindingPoint; b >= 0) {
      stages[0].variant.supportBlock = true;
      stages[0].perFrameBindings.push_back(uint32_t(b));
    }
    stages[0].glsl = vs->retainedSource;
    stages[1].glsl = ps->retainedSource;
    p.motionTwin = vk::motion::build(std::move(base), std::move(stages[0]), std::move(stages[1]));
  }
  WarmPipeline warmed;
  // debug: WWHD_VK_RECIPE_KEY_DUMP=file appends the portable key (hex) of every pipeline derived for the setup's
  // preparation ("D") and of every pipeline the game asks for that the recipe database does not have ("M")
  static FILE* keyDump = [] { const char* p = getenv("WWHD_VK_RECIPE_KEY_DUMP"); return p && *p ? std::fopen(p, "a") : nullptr; }();
  auto dumpKey = [&](char kind) {
    if (!keyDump) return;
    std::fputc(kind, keyDump); std::fputc(' ', keyDump);
    for (uint8_t byte : recipeKey) std::fprintf(keyDump, "%02x", byte);
    std::fputc('\n', keyDump); std::fflush(keyDump);
  };
  if (!g_recipeSink && !covSlot) {
    ++R.pipelineKeysNew;
    const bool known = pipeline_recipe_known(recipeKey);
    R.pipelineKeysKnown += known;
    if (!known) dumpKey('M');
  } else dumpKey('D');
  if (g_recipeSink) {
    for (size_t i = 0; i < shaderBinaries.size(); ++i)
      if (recipeSpirv[i].empty() && shaderBinaries[i]) recipeSpirv[i] = vk::shader_binary_words_exact(shaderBinaries[i], R.frame);
    for (auto module : modules) if (module) vkDestroyShaderModule(R.device, module, nullptr);
    vkDestroyPipelineLayout(R.device, p.layout, nullptr);
    for (auto set : p.sets) vkDestroyDescriptorSetLayout(R.device, set, nullptr);
    if (!recipeSpirv[0].empty() && !recipeSpirv[1].empty()) {
      auto recipe = std::make_shared<PipelineRecipe>();
      recipe->key = std::move(recipeKey); recipe->spirv = std::move(recipeSpirv);
      recipe->descriptors = std::move(descriptorBindings);
      recipe->dynamicUniforms = p.dynamicUniforms;
      recipe->state = std::make_unique<GraphicsPipelineState>(std::move(pipelineState));
      g_recipeSink->push_back(std::move(recipe));
    }
    // (not kept in the map: the same key from another recipe is derived again and dropped as a repeat by the caller)
    static Pipeline none;
    none = Pipeline{};
    return none;
  }
  if (!covSlot && !recipeSpirv[0].empty() && !recipeSpirv[1].empty() &&
      take_warm_pipeline(recipeKey, recipeSpirv, descriptorBindings, p.dynamicUniforms, pipelineState, warmed)) {
    for (auto module : modules) if (module) vkDestroyShaderModule(R.device, module, nullptr);
    vkDestroyPipelineLayout(R.device, p.layout, nullptr);
    for (auto set : p.sets) vkDestroyDescriptorSetLayout(R.device, set, nullptr);
    p.pipeline = warmed.pipeline; p.layout = warmed.layout;
    std::copy(std::begin(warmed.sets), std::end(warmed.sets), p.sets);
    if (stutter::trace().enabled()) {
      const auto now = stutter::now();
      stutter::trace().add("startup_pipeline_hit", R.frame, PipelineKeyHash{}(key), now, now, 0);
    }
    return remember(pipelines.emplace(key, std::move(p)).first->second);
  }
  pipeline_groups::note_created();
  // the setup's model sweep, record only (WWHD_DSWEEP_RECORD_ONLY=1): the recipe is recorded as when the pipeline is
  // made, but the driver never compiles it; its draws are left out (nobody sees the hidden sweep)
  static const bool recordOnly = [] { const char* e = getenv("WWHD_DSWEEP_RECORD_ONLY"); return e && *e == '1'; }();
  if (recordOnly && !covSlot) {
    for (size_t i = 0; i < shaderBinaries.size(); ++i)
      if (recipeSpirv[i].empty() && shaderBinaries[i]) recipeSpirv[i] = vk::shader_binary_words_exact(shaderBinaries[i], R.frame);
    for (auto module : modules) if (module) vkDestroyShaderModule(R.device, module, nullptr);
    if (!recipeSpirv[0].empty() && !recipeSpirv[1].empty()) {
      auto recipe = std::make_shared<PipelineRecipe>();
      recipe->key = std::move(recipeKey); recipe->spirv = std::move(recipeSpirv);
      recipe->descriptors = std::move(descriptorBindings);
      recipe->dynamicUniforms = p.dynamicUniforms;
      recipe->state = std::make_unique<GraphicsPipelineState>(std::move(pipelineState));
      for (const uint32_t group : pipeline_groups::sweeping_all()) recipe->add_group(group);
      remember_pipeline_recipe(std::move(recipe));
    }
    p.pipeline = VK_NULL_HANDLE;
    return remember(pipelines.emplace(key, std::move(p)).first->second);
  }
  const auto pipelineStarted = std::chrono::steady_clock::now();
  VkResult result;
  uint64_t compileNs = 0;
  if (async_pipeline_enabled()) {
    auto pending = std::make_shared<PendingPipeline>();
    pending->device = R.device;
    std::copy(std::begin(modules), std::end(modules), pending->modules);
    auto recipe = std::make_shared<PipelineRecipe>();
    recipe->key = std::move(recipeKey); recipe->spirv = std::move(recipeSpirv);
    recipe->descriptors = std::move(descriptorBindings);
    recipe->dynamicUniforms = p.dynamicUniforms;
    recipe->state = std::make_unique<GraphicsPipelineState>(std::move(pipelineState));
    for (const uint32_t group : pipeline_groups::sweeping_all()) recipe->add_group(group);
    pending->recipe = std::move(recipe);
    pending->keepRecipe = !covSlot;
    // The owner waits for its ticket before destroying modules or itself.
    // Capturing a shared owner here would form an owner/ticket/job cycle.
    pending->shaders[0]=vs; pending->shaders[1]=ps;
    auto* owner=pending.get();
    const VkDevice device=pending->device;
    const std::function<void(GraphicsPipelineState&)> prepare=[owner,device,shaderBinaries](GraphicsPipelineState& ownedState) {
      for(uint32_t i=0;i<2;++i) {
        auto& words=owner->recipe->spirv[i];
        if(words.empty()) words=vk::shader_binary_words(shaderBinaries[i]);
        if(words.empty()) throw std::runtime_error("empty pipeline shader binary");
        VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mc.codeSize=words.size()*sizeof(uint32_t);mc.pCode=words.data();
        vk_check(vkCreateShaderModule(device,&mc,nullptr,&owner->modules[i]),"shader module");
        ownedState.set_stage_module(i,owner->modules[i]);
        owner->recipe->state->set_stage_module(i,owner->modules[i]);
      }
    };
    pending->ticket = begin_pipeline_async(*pending->recipe->state, R.frame,
        PipelineKeyHash{}(key), "guest_pipeline", prepare);
    if (!pending->ticket) {
      // Capacity pressure always completes this draw exactly. No new snapshot
      // is retained and no silently omitted draw is introduced by admission.
      // (the render thread's recorder: a draw thread's commands are in its own held stream, and it must never touch the
      // render thread's)
      if (!draw_threads::on_draw_thread()) deferred_commands().finish_exact();
      begin_pipeline_pending_frame();
      auto built = build_pipeline_async(*pending->recipe->state, R.frame,
          PipelineKeyHash{}(key), "guest_pipeline", present_completed_frame_while_pipeline_pending, prepare);
      draw_counters().pipelineCreateNs += built.compileNs; ++draw_counters().pipelineCreates;
      if(recoverable_pipeline_failure(built.result))
        capture_pipeline_failure(built.result,vs,ps,pending->recipe->words(0),pending->recipe->words(1));
      else vk_check(built.result, "graphics pipeline");
      p.pipeline = built.pipeline;
      if(p.pipeline && !covSlot) remember_pipeline_recipe(std::move(pending->recipe));
      pending.reset();
      R.pipelineCacheDirty = true; R.pipelineCacheChangedFrame = R.frame;
    } else {
      if (skip_pending_draws()) p.watch = pending;  // (before the move; only while draws may be left out)
      p.pendingCompile = std::move(pending);
    }
    return remember(pipelines.emplace(key, std::move(p)).first->second);
  } else {
    result = vkCreateGraphicsPipelines(R.device, R.pipelineCache, 1, pipelineState.get(), nullptr, &p.pipeline);
  }
  const bool recordRecipe = result == VK_SUCCESS && !covSlot;
  if (!async_pipeline_enabled() && stutter::trace().enabled())
    stutter::trace().add("guest_pipeline", R.frame, PipelineKeyHash{}(key),
        std::chrono::duration_cast<std::chrono::nanoseconds>(pipelineStarted.time_since_epoch()).count(),
        stutter::now(), int(result));
  draw_counters().pipelineCreateNs += async_pipeline_enabled() ? compileNs :
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-pipelineStarted).count();
  ++draw_counters().pipelineCreates;
  for (auto m : modules)
    vkDestroyShaderModule(R.device, m, nullptr);
  if (recoverable_pipeline_failure(result)) {
    p.pipeline = VK_NULL_HANDLE;
    capture_pipeline_failure(result,vs,ps,recipeSpirv[0],recipeSpirv[1]);
    return remember(pipelines.emplace(key,p).first->second);
  }
  vk_check(result, "graphics pipeline");
  if (recordRecipe) {
    auto recipe = std::make_shared<PipelineRecipe>();
    recipe->key = std::move(recipeKey); recipe->spirv = std::move(recipeSpirv);
    recipe->descriptors = std::move(descriptorBindings);
    recipe->dynamicUniforms = p.dynamicUniforms;
    recipe->state = std::make_unique<GraphicsPipelineState>(std::move(pipelineState));
    for (const uint32_t group : pipeline_groups::sweeping_all()) recipe->add_group(group);
    remember_pipeline_recipe(std::move(recipe));
  }
  R.pipelineCacheDirty = true;
  R.pipelineCacheChangedFrame = R.frame;
  return remember(pipelines.emplace(key, p).first->second);
}
struct SamplerMemo {
  VkDevice device = VK_NULL_HANDLE;
  std::array<uint32_t, 3> words{};
  bool compare = false, integer = false, forceAniso = false, nearestOnly = false;
  VkSampler value = VK_NULL_HANDLE;
  bool matches(VkDevice nextDevice, const uint32_t* nextWords,
               bool nextCompare, bool nextInteger, bool nextForceAniso, bool nextNearestOnly) const {
    return value && device == nextDevice && compare == nextCompare &&
        integer == nextInteger && forceAniso == nextForceAniso && nearestOnly == nextNearestOnly &&
        words[0] == nextWords[0] && words[1] == nextWords[1] && words[2] == nextWords[2];
  }
  void remember(VkDevice nextDevice, const uint32_t* nextWords,
                bool nextCompare, bool nextInteger, bool nextForceAniso, bool nextNearestOnly, VkSampler nextValue) {
    device = nextDevice;
    std::memcpy(words.data(), nextWords, sizeof(words));
    compare = nextCompare; integer = nextInteger; forceAniso = nextForceAniso; nearestOnly = nextNearestOnly;
    value = nextValue;
  }
};
struct samplerMemo_per_thread_tag; /*per-thread:samplerMemo*/ inline SamplerMemo& samplerMemo() { return per_thread::get<samplerMemo_per_thread_tag, SamplerMemo>(); }  // (per thread, stage 2.4; dropped as the pipeline memos are)
PER_THREAD(uint64_t, threadSamplerMemoGeneration, 0)
// what samplers of hand-drawn mip textures add to their LOD bias in the draw being prepared: log2 of its targets'
// internal resolution scale (gfx/artistic_mips.h; set by draw() before its stages are bound). Per thread: the draw
// threads prepare draws beside the render thread (docs/VR_SCENE_THREADS.md); worked out again only when the scale
// changes (most draws keep the last one)
struct ArtisticBias { float sx = -1, sy = -1, bias = 0; };
thread_local ArtisticBias g_artisticBias;
// (renderer smoke test) the create info of the sampler the cache made last (under the sampler lock)
VkSamplerCreateInfo g_lastSamplerCreateInfo{};
// lodOffset: a replacement texture pack's image (texture_pack_vk.cpp), with that many levels in front of the guest's:
// its LOD range shifted by them (texture_pack_core.h replacement_lods), the same picture at every distance
VkSampler sampler(const uint32_t *words, bool compare, bool integer, bool allowAniso, bool linearSupported,
                  float extraBias = 0, uint32_t lodOffset = 0) {
  const bool nearestOnly = sampler_nearest_only(integer, compare, linearSupported);
  static const bool memo = [] {
    const char* e = std::getenv("WWHD_VK_SAMPLER_MEMO");
    return e && !std::strcmp(e, "1");
  }();
  const bool forceAniso = allowAniso && aniso_enabled();
  const bool stats = preparation_stats_enabled();
  if (stats) ++draw_counters().cpuPreparation.samplerRequests;
  if (const uint64_t g = pipelineMemoGeneration.load(std::memory_order_acquire); threadSamplerMemoGeneration() != g) {
    threadSamplerMemoGeneration() = g;  // reset since this thread's memo was made
    samplerMemo() = {};
  }
  // (the memo holds the plain samplers: a bias or an offset takes the cache, whose key has them)
  const bool plain = extraBias == 0 && lodOffset == 0;
  if (memo && plain && samplerMemo().matches(R.device, words, compare, integer, forceAniso, nearestOnly)) {
    if (stats) ++draw_counters().cpuPreparation.samplerMemoHits;
    return samplerMemo().value;
  }
  if (stats) ++draw_counters().cpuPreparation.samplerMapLookups;
  auto remember = [&](VkSampler value) {
    if (memo && plain) samplerMemo().remember(R.device, words, compare, integer, forceAniso, nearestOnly, value);
    return value;
  };
  std::string key(reinterpret_cast<const char *>(words), 12);
  key.push_back(compare);
  key.push_back(integer);
  key.push_back(forceAniso);
  key.push_back(nearestOnly);
  if (extraBias != 0) key.append(reinterpret_cast<const char*>(&extraBias), sizeof extraBias);
  // (after the bias's 4 bytes or none: the key's length tells the two apart)
  if (lodOffset != 0) key.append({'L', char(lodOffset)});
  static std::unordered_map<std::string, VkSampler> cache;
  draw_threads::CacheLock locked(draw_threads::sampler_mutex());  // (stage 2.4: the cache is shared)
  if (auto it = cache.find(key); it != cache.end())
    return remember(it->second);
  LATTE_SQ_TEX_SAMPLER_WORD0_0 w;
  LATTE_SQ_TEX_SAMPLER_WORD1_0 w1;
  std::memcpy(&w, words, 4);
  std::memcpy(&w1, words + 1, 4);
  auto filter = [](uint32_t v) {
    return v == 0 || v == 4 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  };
  auto addr = [](uint32_t v) {
    switch (v) {
    case 0:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:
      if (!R.samplerMirrorClampToEdge)
        exception_report::raise("mirror-once sampler unsupported by device");
      return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5:
    case 7:
      exception_report::raise(
          "mirror-once border sampling is not implemented");
    case 2:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
  };
  VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  ci.magFilter = filter(static_cast<uint32_t>(w.get_XY_MAG_FILTER()));
  ci.minFilter = filter(static_cast<uint32_t>(w.get_XY_MIN_FILTER()));
  if (nearestOnly)
    ci.minFilter = ci.magFilter = VK_FILTER_NEAREST;
  ci.mipmapMode = static_cast<uint32_t>(w.get_MIP_FILTER()) == 2
                      ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                      : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU = addr(static_cast<uint32_t>(w.get_CLAMP_X()));
  ci.addressModeV = addr(static_cast<uint32_t>(w.get_CLAMP_Y()));
  ci.addressModeW = addr(static_cast<uint32_t>(w.get_CLAMP_Z()));
  ci.mipLodBias = std::clamp(::gfx::sampler_lod_bias::levels(words[1]) + extraBias,
                             -R.properties.limits.maxSamplerLodBias,
                             R.properties.limits.maxSamplerLodBias);
  if (!R.samplerMipLodBias && ci.mipLodBias != 0) {
    // MoltenVK cannot apply sampler LOD bias: the translated shaders add it
    // (gfx/sampler_lod_bias.h, uf_texNLodBias); native Vulkan devices keep it here.
    static thread_local bool reported = false;
    if (!reported) {
      LOG("[vulkan] device lacks sampler LOD bias; the shaders add it");
      reported = true;
    }
    ci.mipLodBias = 0;
  }
  ci.minLod = w1.get_MIN_LOD() / 64.f;
  ci.maxLod =
      static_cast<uint32_t>(w.get_MIP_FILTER()) ? w1.get_MAX_LOD() / 64.f : 0;
  if (lodOffset) {
    // guest level 0 is the replacement's level lodOffset; its finer levels stay reachable (no mip filter: level 0 only,
    // as on the original)
    const auto lods = ::gfx::texture_pack::replacement_lods(ci.minLod, ci.maxLod,
                                                            static_cast<uint32_t>(w.get_MIP_FILTER()) != 0, lodOffset);
    ci.minLod = lods.minLod;
    ci.maxLod = lods.maxLod;
  }
  ci.compareEnable = compare;
  ci.compareOp = VkCompareOp(w.get_DEPTH_COMPARE_FUNCTION());
  uint32_t anisotropy = w.get_MAX_ANISO_RATIO()
      ? std::min(1u << std::min(uint32_t(w.get_MAX_ANISO_RATIO()), 4u), 16u) : 1;
  if (forceAniso && uint32_t(w.get_MIP_FILTER()) != 0 && ci.minFilter == VK_FILTER_LINEAR &&
      uint32_t(w.get_DEPTH_COMPARE_FUNCTION()) == 0)
    anisotropy = std::max(anisotropy, aniso_max_ratio());  // (16, or 4 on a standalone headset: settings.cpp)
  if (R.enabledFeatures.samplerAnisotropy && !nearestOnly && anisotropy > 1) {
    ci.anisotropyEnable = VK_TRUE;
    ci.maxAnisotropy = std::min(float(anisotropy), R.properties.limits.maxSamplerAnisotropy);
  }

  ci.borderColor = static_cast<uint32_t>(w.get_BORDER_COLOR_TYPE()) == 1
                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                   : static_cast<uint32_t>(w.get_BORDER_COLOR_TYPE()) == 2
                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                       : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if (integer) {
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.borderColor = VkBorderColor(uint32_t(ci.borderColor) + 1);
  }
  restrict_sampler_filtering(ci, nearestOnly);
  g_lastSamplerCreateInfo = ci;
  VkSampler s;
  vk_check(vkCreateSampler(R.device, &ci, nullptr, &s), "sampler");
  cache.emplace(std::move(key), s);
  return remember(s);
}
struct StageResources {
  VkDescriptorSet set{};
  std::array<uint32_t, 17> dynamicOffsets{};
  uint32_t dynamicOffsetCount = 0;
};
// Stereo: a stage of eye 0's draw that samples only textures both eyes share (stereo.h). Eye 1
// binds the same descriptor set; only the uniform buffers eye 0's eye shift changed get eye 1's
// copy, at another dynamic offset.
struct EyeUniform {
  uint32_t dynamicIndex = 0;  // in StageResources::dynamicOffsets
  uint32_t binding = 0;  // its descriptor binding
  VkBuffer buffer = VK_NULL_HANDLE;  // the descriptor set's buffer
  std::vector<uint8_t> bytes;  // eye 0's upload
  stereo::EyeRows rows;
  // (docs/VR_EYE_THREAD.md, stage E5) with eye 1 on its own thread: eye 1's copy reserved by eye 0 in its own arena,
  // so in the set's buffer (eye 1's thread allocates from an arena of its own: another buffer, and no replay)
  UploadSlice reserved{};
};
struct StageReplay {
  bool valid = false;
  StageResources resources;
  std::array<EyeUniform, 4> uniforms;
  uint32_t uniformCount = 0;
  std::array<Surface*, LATTE_NUM_MAX_TEX_UNITS> textures{};  // shared with eye 1
  uint32_t textureCount = 0;
  // the other textures: eye 1 resolves its own images, then writes its own set from these writes
  // feedback: the draw renders into the texture it samples (eye 0 bound a copy of it, feedback_view): eye 1 makes its
  // own copy of its own layer (2026-11-09: the main colour target's feedback draws were the "a stage eye 1 binds itself"
  // that held it per eye on a Quest, with its passes switching between both eyes and one: the black squares)
  struct EyeTexture { uint32_t unit = 0; int binding = -1; float sx = 1, sy = 1; uint32_t write = 0; bool feedback = false; };
  std::array<EyeTexture, LATTE_NUM_MAX_TEX_UNITS> eyeTextures{};
  uint32_t eyeTextureCount = 0;
  VkDescriptorSetLayout layout{};
  uint32_t writeCount = 0;
  std::array<VkWriteDescriptorSet, 17 + LATTE_NUM_MAX_TEX_UNITS> writes{};  // without info pointers
  std::array<VkDescriptorBufferInfo, 17 + LATTE_NUM_MAX_TEX_UNITS> bufferInfos{};
  std::array<VkDescriptorImageInfo, 17 + LATTE_NUM_MAX_TEX_UNITS> imageInfos{};
};
VkDescriptorSet stage_descriptor_set(bool vertex, VkDescriptorSetLayout layout,
                                     VkWriteDescriptorSet* writes, uint32_t writeCount);
// Exact consecutive descriptor state, optionally used to omit redundant binds.
// Lifetime is the tracked draw pass; explicit generation/command/layout keys
// also exclude repeated handles after command/descriptor pool resets.
struct DescriptorBindProbe {
  struct Matches { bool whole=false;std::array<bool,2> stage{}; };
  struct BindRange { uint32_t first=0,count=2,offsetBegin=0,offsetCount=0; };
  static BindRange bind_range(Matches matches,bool skip,uint32_t vsCount,uint32_t psCount) {
    if(skip && matches.whole) return {0,0,0,0};
    if(skip && matches.stage[0]) return {1,1,vsCount,psCount};
    if(skip && matches.stage[1]) return {0,1,0,vsCount};
    return {0,2,0,vsCount+psCount};
  }
  VkCommandBuffer command=VK_NULL_HANDLE;
  uint64_t generation=0;
  VkPipelineLayout layout=VK_NULL_HANDLE;
  std::array<VkDescriptorSet,2> sets{};
  std::array<uint32_t,2> counts{};
  std::array<uint32_t,34> offsets{};
  bool valid=false;
  Matches observe(VkCommandBuffer nextCommand,uint64_t nextGeneration,
                  VkPipelineLayout nextLayout,const VkDescriptorSet* nextSets,
                  const uint32_t* nextOffsets,uint32_t vsCount,uint32_t psCount) {
    Matches matches;
    if(vsCount>17 || psCount>17) { *this={};return matches; }
    const std::array<uint32_t,2> nextCounts{vsCount,psCount};
    if(valid && command==nextCommand && generation==nextGeneration && layout==nextLayout) {
      for(size_t stage=0;stage<2;++stage) {
        const size_t oldBegin=stage?counts[0]:0,nextBegin=stage?vsCount:0;
        matches.stage[stage]=sets[stage]==nextSets[stage] && counts[stage]==nextCounts[stage] &&
            (!nextCounts[stage] || !std::memcmp(offsets.data()+oldBegin,
                nextOffsets+nextBegin,nextCounts[stage]*sizeof(uint32_t)));
      }
      matches.whole=matches.stage[0]&&matches.stage[1];
    }
    command=nextCommand;generation=nextGeneration;layout=nextLayout;
    std::copy_n(nextSets,2,sets.begin());counts=nextCounts;
    if(vsCount+psCount)std::copy_n(nextOffsets,vsCount+psCount,offsets.begin());
    valid=true;
    return matches;
  }
};
struct DescriptorIdentity {
  uint32_t binding = 0;
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
  VkDescriptorBufferInfo buffer{};
  VkDescriptorImageInfo image{};
  bool isBuffer = false;
};
struct LastDescriptorSet {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  uint32_t count = 0;
  std::array<DescriptorIdentity, 17 + LATTE_NUM_MAX_TEX_UNITS> identities{};
};
bool identities_match(const DescriptorIdentity *identities, const VkWriteDescriptorSet *writes,
                      uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const auto &identity = identities[i];
    const auto &write = writes[i];
    if (identity.binding != write.dstBinding || identity.type != write.descriptorType ||
        identity.isBuffer != bool(write.pBufferInfo)) return false;
    if (write.pBufferInfo) {
      const auto &info = *write.pBufferInfo;
      if (identity.buffer.buffer != info.buffer || identity.buffer.range != info.range ||
          (write.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
           identity.buffer.offset != info.offset)) return false;
    } else {
      const auto &info = *write.pImageInfo;
      if (identity.image.sampler != info.sampler || identity.image.imageView != info.imageView ||
          identity.image.imageLayout != info.imageLayout) return false;
    }
  }
  return true;
}
void store_identities(DescriptorIdentity *identities, const VkWriteDescriptorSet *writes,
                      uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    auto &identity = identities[i];
    const auto &write = writes[i];
    identity.binding = write.dstBinding;
    identity.type = write.descriptorType;
    identity.isBuffer = write.pBufferInfo != nullptr;
    if (identity.isBuffer) identity.buffer = *write.pBufferInfo;
    else identity.image = *write.pImageInfo;
  }
}
bool descriptor_matches(const LastDescriptorSet &last,
                        VkDescriptorSetLayout layout,
                        const VkWriteDescriptorSet *writes, uint32_t count) {
  return last.set && last.layout == layout && last.count == count &&
         identities_match(last.identities.data(), writes, count);
}
void remember_descriptors(LastDescriptorSet &last, VkDescriptorSetLayout layout,
                          VkDescriptorSet set, const VkWriteDescriptorSet *writes,
                          uint32_t count) {
  last.layout = layout;
  last.set = set;
  last.count = count;
  store_identities(last.identities.data(), writes, count);
}
// Descriptor sets written in this submission, found by a 64-bit hash of the layout and the
// descriptors and confirmed by comparing the stored descriptors. Valid until the submission's
// pool is reset; the vectors and buckets keep their capacity across submissions.
uint64_t descriptor_hash(VkDescriptorSetLayout layout, const VkWriteDescriptorSet *writes,
                         uint32_t count) {
  auto mix = [](uint64_t hash, uint64_t value) {
    hash = (hash ^ value) * 0xFF51AFD7ED558CCDull;
    return hash ^ (hash >> 32);
  };
  uint64_t hash = mix(0x9E3779B97F4A7C15ull, uint64_t(uintptr_t(layout)) ^ (uint64_t(count) << 56));
  for (uint32_t i = 0; i < count; ++i) {
    const auto &write = writes[i];
    hash = mix(hash, uint64_t(write.dstBinding) | (uint64_t(write.descriptorType) << 32));
    if (write.pBufferInfo) {
      const auto &info = *write.pBufferInfo;
      hash = mix(hash, uint64_t(uintptr_t(info.buffer)));
      hash = mix(hash, info.range);
      if (write.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        hash = mix(hash, info.offset);
    } else {
      const auto &info = *write.pImageInfo;
      hash = mix(hash, uint64_t(uintptr_t(info.sampler)));
      hash = mix(hash, uint64_t(uintptr_t(info.imageView)) ^ (uint64_t(info.imageLayout) << 48));
    }
  }
  return hash;
}
struct SubmissionDescriptorCache {
  struct Entry {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t count = 0, first = 0, next = UINT32_MAX;
  };
  std::unordered_map<uint64_t, uint32_t> heads;
  std::vector<Entry> entries;
  std::vector<DescriptorIdentity> identities;
  void clear() { heads.clear(); entries.clear(); identities.clear(); }
  VkDescriptorSet find(uint64_t hash, VkDescriptorSetLayout layout,
                       const VkWriteDescriptorSet *writes, uint32_t count) const {
    auto head = heads.find(hash);
    if (head == heads.end()) return VK_NULL_HANDLE;
    for (uint32_t i = head->second; i != UINT32_MAX; i = entries[i].next) {
      const auto &entry = entries[i];
      if (entry.layout == layout && entry.count == count &&
          identities_match(identities.data() + entry.first, writes, count))
        return entry.set;
    }
    return VK_NULL_HANDLE;
  }
  void insert(uint64_t hash, VkDescriptorSetLayout layout, VkDescriptorSet set,
              const VkWriteDescriptorSet *writes, uint32_t count) {
    Entry entry{layout, set, count, uint32_t(identities.size()), UINT32_MAX};
    identities.resize(identities.size() + count);
    store_identities(identities.data() + entry.first, writes, count);
    auto [head, inserted] = heads.emplace(hash, uint32_t(entries.size()));
    if (!inserted) {
      entry.next = head->second;
      head->second = uint32_t(entries.size());
    }
    entries.push_back(entry);
  }
  // a set here names one of these buffers (sorted)
  bool names_any(const std::vector<VkBuffer>& sorted) const {
    for (const auto& id : identities)
      if (id.isBuffer && std::binary_search(sorted.begin(), sorted.end(), id.buffer.buffer)) return true;
    return false;
  }
};
// A descriptor cannot sample the same subresource being written as an
// attachment without a feedback-loop extension. Snapshot before rendering.
// Separate stage/unit slots preserve every descriptor selected for one draw.
constexpr uint32_t kFeedbackUnitsPerStage = 16;
// (2026-11-09) stereo: a stage that samples its own target is replayed for eye 1 with eye 1's own copy of its layer
// (StageReplay::EyeTexture::feedback), so such a draw can be drawn for both eyes (multiview) rather than holding its
// target per eye. WWHD_VK_FEEDBACK_EYE_REPLAY=0: eye 1 binds the stage itself, as before
bool feedback_eye_replay() {
  static const bool on = [] { const char* e = std::getenv("WWHD_VK_FEEDBACK_EYE_REPLAY"); return !(e && e[0] == '0'); }();
  return on;
}
// Retained feedback copies, bounded. 128 MiB held the copies of 1280x720 targets, but a VR eye at 3x is 3840x2771
// (41 MiB a copy) and the two eyes' colour and depth copies took about 250 MiB: over the budget, three of them were
// destroyed and allocated again every frame (4.4 ms of vkFreeMemory per frame in VR, docs/VR_PERFORMANCE.md).
// Desktop: 1/16 of the largest device-local heap, between 128 and 768 MiB; Android keeps 128 MiB.
VkDeviceSize feedback_retained_budget() {
#ifdef __ANDROID__
  return 128ull << 20;
#else
  static const VkDeviceSize budget = [] {
    VkPhysicalDeviceMemoryProperties p{};
    vkGetPhysicalDeviceMemoryProperties(R.physicalDevice, &p);
    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < p.memoryHeapCount; ++i)
      if (p.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max(heap, p.memoryHeaps[i].size);
    VkDeviceSize b = std::clamp<VkDeviceSize>(heap / 16, 128ull << 20, 768ull << 20);
    if (const char* e = getenv("WWHD_VK_FEEDBACK_BUDGET_MIB")) b = VkDeviceSize(std::max(1, atoi(e))) << 20;  // A/B tests
    LOG("[vulkan feedback] retained copies up to %llu MiB (device-local heap %llu MiB)",
        (unsigned long long)(b >> 20), (unsigned long long)(heap >> 20));
    return b;
  }();
  return budget;
#endif
}
struct FeedbackScratch {
  Surface surface;
  VkDevice device = VK_NULL_HANDLE;
  VkDeviceSize allocationBytes = 0;
};
// per stereo eye: each eye's command stream reuses only its own copies (stereo.h)
std::array<std::array<FeedbackScratch, kFeedbackUnitsPerStage * 2>, 2> feedbackScratchEyes;
VkDeviceSize feedbackRetainedBytes = 0;
// the draw (draw_counters().drawCount while it is prepared) that last used each retained slot
std::array<std::array<uint64_t, kFeedbackUnitsPerStage * 2>, 2> feedbackUseDrawEyes = [] {
  std::array<std::array<uint64_t, kFeedbackUnitsPerStage * 2>, 2> a;
  for (auto& eye : a) eye.fill(UINT64_MAX);
  return a; }();
bool feedback_compatible(const Surface& copy, const Surface& source) {
  return copy.image && copy.fmt.pixel == source.fmt.pixel &&
         copy.aspect == source.aspect && copy.imageType == source.imageType &&
         copy.viewType == source.viewType && copy.dim == source.dim &&
         copy.extent.width == source.extent.width &&
         copy.extent.height == source.extent.height &&
         copy.extent.depth == source.extent.depth &&
         copy.mips == source.mips && copy.arrayLayers == source.arrayLayers;
}
void make_feedback_image(Surface& copy, const Surface& source) {
  copy = source;
  copy.own_image_only();  // (stage M2b: never the source's layer of a two-eye image, nor its links)
  copy.image = VK_NULL_HANDLE;
  copy.memory = VK_NULL_HANDLE;
  copy.view = VK_NULL_HANDLE;
  copy.layerViews.clear();
  copy.sampledViews.clear();
  copy.guestLayout.reset();
  copy.addr = copy.mipAddr = 0;
  copy.gpuWritten = true;
  copy.dirty = false;
  // The snapshot is already at physical resolution. Shader texture-scale
  // uniforms continue to use source metadata in bind_stage, never this copy.
  copy.width = source.extent.width;
  copy.height = source.extent.height;
  if (source.imageType == VK_IMAGE_TYPE_3D) copy.slices = source.extent.depth;
  create_surface_image(&copy, false);
}
void reset_feedback_scratch() {
  for (auto& feedbackScratch : feedbackScratchEyes)
  for (auto& slot : feedbackScratch) {
    // Reset must run before replacing the device. Foreign handles cannot be
    // placed into this device's deferred retirement lists.
    if (slot.surface.image && slot.device != R.device) continue;
    destroy_surface_image(&slot.surface);
    feedbackRetainedBytes -= slot.allocationBytes;
    slot = {};
  }
}
struct FeedbackStatsProbe {
  struct Key {
    VkDevice device; VkImage image; uint64_t version;
    VkFormat format; VkImageAspectFlags aspect;
    VkImageType imageType; VkImageViewType viewType;
    VkExtent3D extent; uint32_t mips,layers;
  };
  std::array<Key,2*LATTE_NUM_MAX_TEX_UNITS> keys;
  size_t count=0;
  static bool equal(const Key& a,const Key& b) {
    return a.device==b.device && a.image==b.image && a.version==b.version &&
        a.format==b.format && a.aspect==b.aspect && a.imageType==b.imageType &&
        a.viewType==b.viewType && a.extent.width==b.extent.width &&
        a.extent.height==b.extent.height && a.extent.depth==b.extent.depth &&
        a.mips==b.mips && a.layers==b.layers;
  }
  bool observe(const Surface& source) {
    const Key key{R.device,source.image,source.writeSeq,source.fmt.pixel,
        source.aspect,source.imageType,source.viewType,source.extent,
        source.mips,source.arrayLayers};
    for(size_t i=0;i<count;++i) if(equal(keys[i],key)) return true;
    if(count<keys.size()) keys[count++]=key;
    return false;
  }
};
struct FeedbackStats {
  uint64_t aliases=0,duplicates=0,texels=0,duplicateTexels=0;
  uint64_t color=0,depth=0,stencil=0,compressed=0,saturated=0;
};
struct feedbackStats_per_thread_tag; /*per-thread:feedbackStats*/ inline FeedbackStats& feedbackStats() { return per_thread::get<feedbackStats_per_thread_tag, FeedbackStats>(); }
bool feedback_stats_enabled() {
  static const bool enabled=[] {
    const char* e=std::getenv("WWHD_VK_FEEDBACK_STATS");
    return e && !std::strcmp(e,"1");
  }();
  return enabled;
}
uint64_t feedback_texels(const Surface& source) {
  auto multiply=[](uint64_t a,uint64_t b) {
    return b && a>UINT64_MAX/b ? UINT64_MAX : a*b;
  };
  uint64_t total=0;
  for(uint32_t mip=0;mip<source.mips;++mip) {
    const auto dimension=[&](uint32_t d) { return mip<32?std::max(1u,d>>mip):1u; };
    uint64_t n=multiply(dimension(source.extent.width),dimension(source.extent.height));
    n=multiply(n,dimension(source.extent.depth));n=multiply(n,source.arrayLayers);
    total=n>UINT64_MAX-total?UINT64_MAX:total+n;
  }
  return total;
}
void report_feedback_stats() {
  static thread_local uint64_t previousFrame=0;
  if(R.frame-previousFrame<120) return;
  const double frames=double(R.frame-previousFrame);
  const auto& t=feedbackStats();
  LOG("[vulkan feedback copies] %.2f aliases/frame %.2f duplicate source versions/frame; %.3f M physical texels/frame %.3f M duplicate texels/frame; color/depth/stencil/compressed %llu/%llu/%llu/%llu; %llu saturated; copies unchanged",
      t.aliases/frames,t.duplicates/frames,t.texels/frames/1e6,t.duplicateTexels/frames/1e6,
      (unsigned long long)t.color,(unsigned long long)t.depth,(unsigned long long)t.stencil,
      (unsigned long long)t.compressed,(unsigned long long)t.saturated);
  feedbackStats()={};previousFrame=R.frame;
}
VkImageView feedback_view(Surface *source, const uint32_t *textureWords,
                          bool vertex, uint32_t unit, FeedbackStatsProbe* probe) {
  if(probe) {
    const bool duplicate=probe->observe(*source);
    const uint64_t texels=feedback_texels(*source);
    if(texels==UINT64_MAX) ++feedbackStats().saturated;
    auto add=[&](uint64_t& sum) { if(texels>UINT64_MAX-sum) {sum=UINT64_MAX;++feedbackStats().saturated;} else sum+=texels; };
    ++feedbackStats().aliases;feedbackStats().duplicates+=duplicate;
    add(feedbackStats().texels);if(duplicate)add(feedbackStats().duplicateTexels);
    feedbackStats().color+=bool(source->aspect&VK_IMAGE_ASPECT_COLOR_BIT);
    feedbackStats().depth+=bool(source->aspect&VK_IMAGE_ASPECT_DEPTH_BIT);
    feedbackStats().stencil+=bool(source->aspect&VK_IMAGE_ASPECT_STENCIL_BIT);
    feedbackStats().compressed+=source->fmt.compressed;
  }
  static const bool reuse = [] {
    const char* e = std::getenv("WWHD_VK_REUSE_FEEDBACK_IMAGES");
    return e && !std::strcmp(e, "1");
  }();
  // Stereo retains allocations, never snapshot contents: every use still copies
  // the source. Compatibility includes extent, format, aspects, mips and layers.
  // Slots are per-eye; feedbackUseDraw prevents two units in one draw from
  // aliasing a slot. transition_image preserves the earlier shader-read -> next
  // transfer-write dependency (including held eye commands); replacements are
  // fence-retired, and retained allocations are bounded by 128 MiB. Mono keeps
  // its prior opt-in policy. This removes repeated allocation work in stereo;
  // it does not establish a Quest performance result.
  auto& feedbackScratch = feedbackScratchEyes[stereoEye()];
  auto& feedbackUseDraw = feedbackUseDrawEyes[stereoEye()];
  Surface temporary;
  Surface* copy = &temporary;
  FeedbackScratch* slot = (reuse || stereo::frame.mode != stereo::Off) && unit < kFeedbackUnitsPerStage
      ? &feedbackScratch[(vertex ? kFeedbackUnitsPerStage : 0) + unit]
      : nullptr;
  // Device recreation is not a supported lifecycle today; refuse to reuse or
  // retire foreign handles if a caller nevertheless changes the device.
  if (slot && slot->surface.image && slot->device != R.device) slot = nullptr;
  // The game samples render targets of several sizes through the same unit, so one retained
  // image per slot was destroyed and recreated on almost every draw (an expensive kernel memory
  // allocation per draw on Android drivers). Look for a compatible retained image in the other
  // slots that this draw does not use (a draw's units always get distinct copies) and swap it in.
  if (slot && !feedback_compatible(slot->surface, *source)) {
    const size_t mine = slot - feedbackScratch.data();
    for (size_t i = 0; i < feedbackScratch.size(); ++i) {
      auto& other = feedbackScratch[i];
      if (i == mine || other.device != R.device || !feedback_compatible(other.surface, *source)) continue;
      if (feedbackUseDraw[i] == draw_counters().drawCount) continue;  // another unit of this draw
      std::swap(*slot, other);
      break;
    }
    // None retained: keep this slot's image for later draws by parking it in an empty slot
    // (draws alternate a few kinds, e.g. the 1280x720 colour and depth copies, through unit 0).
    if (!feedback_compatible(slot->surface, *source) && slot->surface.image) {
      for (size_t i = 0; i < feedbackScratch.size(); ++i) {
        auto& other = feedbackScratch[i];
        if (i == mine || other.surface.image || feedbackUseDraw[i] == draw_counters().drawCount) continue;
        std::swap(*slot, other);
        std::swap(feedbackUseDraw[i], feedbackUseDraw[mine]);
        break;
      }
    }
  }
  if (slot) feedbackUseDraw[slot - feedbackScratch.data()] = draw_counters().drawCount;
  if (slot && feedback_compatible(slot->surface, *source)) {
    copy = &slot->surface;
  } else {
    if (slot && slot->surface.image) {
      end_encoder();
      destroy_surface_image(&slot->surface);
      feedbackRetainedBytes -= slot->allocationBytes;
      *slot = {};
    }
    make_feedback_image(temporary, *source);
    // debug: WWHD_VK_FEEDBACK_ALLOC_LOG=1 reports feedback image creations every 120 frames
    static const bool allocLog = getenv("WWHD_VK_FEEDBACK_ALLOC_LOG") != nullptr;
    if (allocLog) {
      static thread_local uint64_t creates = 0, unslotted = 0, overBudget = 0, lastFrame = 0;
      ++creates;
      if (!slot) ++unslotted;
      if (R.frame - lastFrame >= 120) {
        LOG("[vulkan feedback allocs] %.1f/frame (%.1f without a slot: unit %u, %.1f over budget), retained %.1f MiB, last %ux%u fmt %d mips %u",
            creates / double(R.frame - lastFrame), unslotted / double(R.frame - lastFrame), unit,
            overBudget / double(R.frame - lastFrame), feedbackRetainedBytes / 1048576.0, source->extent.width,
            source->extent.height, int(source->fmt.pixel), source->mips);
        creates = unslotted = overBudget = 0;
        lastFrame = R.frame;
      }
      if (slot) {
        VkMemoryRequirements rq{};
        vkGetImageMemoryRequirements(R.device, temporary.image, &rq);
        if (rq.size > feedback_retained_budget() - feedbackRetainedBytes) ++overBudget;
      }
    }
    if (slot) {
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(R.device, temporary.image, &requirements);
      if (requirements.size <= feedback_retained_budget() - feedbackRetainedBytes) {
        slot->surface = std::move(temporary);
        temporary = {};
        slot->device = R.device;
        slot->allocationBytes = requirements.size;
        feedbackRetainedBytes += requirements.size;
        copy = &slot->surface;
      }
    }
  }
  transition_image(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  // Retained layout preserves the read→write dependency on preceding draws,
  // including draws in an earlier submission on this same graphics queue.
  transition_image(copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT);
  std::vector<VkImageCopy> regions;
  for (auto aspect : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_ASPECT_STENCIL_BIT}) {
    if (!(source->aspect & aspect))
      continue;
    for (uint32_t mip = 0; mip < source->mips; ++mip) {
      VkImageCopy region{};
      region.srcSubresource = {VkImageAspectFlags(aspect), mip, source->baseLayer,
                               source->arrayLayers};
      region.dstSubresource = region.srcSubresource;
      region.dstSubresource.baseArrayLayer = copy->baseLayer;
      region.extent = {std::max(1u, source->extent.width >> mip),
                       std::max(1u, source->extent.height >> mip),
                       std::max(1u, source->extent.depth >> mip)};
      regions.push_back(region);
    }
  }
  GpuScopeToken feedbackTiming;
  if(R.gpuPassTimestampsEnabled) feedbackTiming=gpu_begin_feedback_scope(*source);
  record::vkCmdCopyImage(command_buffer(), source->image,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions.size(),
                 regions.data());
  if(R.gpuPassTimestampsEnabled) gpu_end_feedback_scope(feedbackTiming);
  transition_image(copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
  VkImageView view = sampled_texture_view(copy, textureWords);
  destroy_surface_image(&temporary); // Temporary snapshots remain fence-retired.
  return view;
}
// ---------------------------------------------------------------- continued-draw fast path
// On by default (WWHD_VK_FASTPATH=0 turns it off): while only "continued" register groups changed since the previous resolved
// draw (gx2/state_groups.h: ALU constants, uniform-block and vertex-buffer pointers, per-draw
// dynamic words), in the same frame and surface epoch (backend.h), with the same primitive type and
// graphics settings, a draw reuses that draw's fetch/vertex/pixel shaders, render targets, pipeline
// and texture/sampler bindings (Cemu's continued draw pass: LatteCommandProcessor.cpp,
// VulkanRendererCore.cpp). Uniforms, support uniforms (incl. texture scales), descriptor sets,
// indices, vertex buffers and dynamic state are still prepared per draw, and every reused texture
// still goes through upload_surface (write tracking/invalidations): an upload ends the epoch and the
// draw resolves fully. Draws that sample one of their own targets' addresses (feedback loops and
// lookups whose winner can change with the target's write sequence), AO replays and AO-substituted
// textures are never continued.
// Recent states (issue #1): draws often alternate between a few states (A, B, A, B: in heavy scenes up to 71% of the
// draws resolved in full had the state of the draw two back, and always resolved the same). An eye keeps its last
// kRecentStates resolved states besides the previous draw's; a draw that would resolve in full because registers
// changed continues one of them instead when every register changed since it (gx2::ChangeLog: the changes that end
// continuations, with the values they replaced) holds the value it had then, in the same frame, part and surface epoch,
// with the same primitive type and settings. The fetch shader's registers (SQ_PGM_START_FS and its size) are not
// compared but its lookup: the game writes its fetch shaders anew for each draw, so their address changes while the
// shader (get_fetch_shader: the same key, the same LatteFetchShader) does not; the full resolution's shaders and
// pipeline follow from the fetch shader and its key, never from its address. The previous draw's state is continued
// the same way. WWHD_VK_FASTPATH_RECENT=0 turns this off.
// WWHD_VK_FASTPATH_VERIFY=N (unless WWHD_VK_FASTPATH=0): on every Nth continued draw
// also run the full resolution, compare shaders, targets, pipeline and texture/sampler bindings,
// count and log mismatches (the draw then uses the full result). Statistics every 120 frames
// only with WWHD_VK_STATS or verification: synchronous console writes can stall rendering.
struct FastPathConfig {
  bool on = false;
  bool recent = false;  // (recent states)
  bool stats = false;
  uint32_t verifyEvery = 0;
};
const FastPathConfig& fastpath_config() {
  static const FastPathConfig config = [] {
    FastPathConfig c;
    if (const char* v = std::getenv("WWHD_VK_FASTPATH_VERIFY")) c.verifyEvery = uint32_t(std::max(0, std::atoi(v)));
    const char* e = std::getenv("WWHD_VK_FASTPATH");
    c.on = e ? std::atoi(e) != 0 : true;
    const char* recent = std::getenv("WWHD_VK_FASTPATH_RECENT");
    c.recent = c.on && !(recent && !std::strcmp(recent, "0"));
    if (!c.on) c.verifyEvery = 0;
    c.stats = preparation_stats_enabled() || c.verifyEvery;
    LOG("[vulkan] continued-draw fast path %s%s%s", c.on ? "on" : "off (WWHD_VK_FASTPATH=0)",
        c.on ? (c.recent ? ", recent states on" : ", recent states off (WWHD_VK_FASTPATH_RECENT=0)") : "",
        c.verifyEvery ? " (verify mode)" : "");
    return c;
  }();
  return config;
}
struct ContinuedTexture {
  uint32_t unit = 0;
  int binding = -1;
  Surface* surface = nullptr;  // bound: the guest texture, or a texture pack's replacement of it (resolve_texture)
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  Surface* guest = nullptr;  // looked up and uploaded: `surface`, or the guest texture the replacement stands for
};
struct ContinuedStage {
  uint32_t count = 0;
  bool continuable = true;
  std::array<ContinuedTexture, LATTE_NUM_MAX_TEX_UNITS> textures{};
  void add(const ContinuedTexture& t) {
    if (count < textures.size()) textures[count++] = t;
    else continuable = false;
  }
};
struct DrawTargets {
  std::array<Surface*, 8> colors{};
  std::array<uint32_t, 8> slices{};
  Surface* depth = nullptr;
  Surface* target = nullptr;  // first colour target, else depth (before AO replay drops a mismatch)
  uint32_t depthSlice = 0, width = 0, height = 0;
  float sx = 1, sy = 1;
  uint8_t activeColors = 0;  // GetActiveColorBufferMask: it also reads the (dynamic) scissor, see continued_reason
  bool same(const DrawTargets& o) const {
    return colors == o.colors && slices == o.slices && depth == o.depth && target == o.target && depthSlice == o.depthSlice &&
           width == o.width && height == o.height && !std::memcmp(&sx, &o.sx, 4) && !std::memcmp(&sy, &o.sy, 4);
  }
};
enum ContinuedReason : uint8_t {
  kContinued, kNoState, kNewFrame, kRegisters, kSurfaces, kUpload, kPrimitive, kSettings,
  kVerifyMismatch, kNotContinuable, kAoReplay, kScissorTargets, kReasons
};
const char* const continuedReasonNames[kReasons] = {
  "continued", "no state", "new frame", "registers", "surfaces", "texture upload", "primitive",
  "settings", "verify mismatch", "not continuable", "AO replay", "scissor targets"};
struct ContinuedDraw {
  bool valid = false;
  ContinuedReason invalidReason = kNoState;  // why the state is not valid
  uint64_t stateVersion = 0, surfaceEpoch = 0, frame = ~uint64_t{0};
  uint64_t logPos = 0;  // the thread's gx2::ChangeLog position when the state was resolved (recent states)
  uint64_t partEpoch = 0;  // (stage S1: a scene part's draws continue none from before it)
  uint32_t prim = 0;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  int aoMode = 0;
  bool aoHires = false, aniso = false;
  LatteFetchShader* fs = nullptr;
  uint64_t fsKey = 0;
  vk::Shader *vs = nullptr, *ps = nullptr;
  DrawTargets targets;
  uint32_t scissorBR = 0;  // PA_SC_GENERIC_SCISSOR_BR that targets.activeColors was resolved with
  Pipeline* pipeline = nullptr;
  ContinuedStage stages[2];
};
// Stereo: each eye's command stream continues its own previous draw (stereo.h).
struct continuedEyes_per_thread_tag; /*per-thread:continuedEyes*/ inline std::array<ContinuedDraw, 2>& continuedEyes() { return per_thread::get<continuedEyes_per_thread_tag, std::array<ContinuedDraw, 2>>(); }  // (per thread, stage 2.9)
// each eye's recent states (see fastpath_config), a ring: slot next is the oldest; cleared wherever continuedEyes is
constexpr uint32_t kRecentStates = 3;
constexpr uint32_t kRecentChangeLimit = gx2::ChangeLog::kSize;  // at most this many logged changes since a recent state
struct RecentStates {
  std::array<ContinuedDraw, kRecentStates> states{};
  uint32_t next = 0;
};
struct recentEyes_per_thread_tag; inline std::array<RecentStates, 2>& recentEyes() { return per_thread::get<recentEyes_per_thread_tag, std::array<RecentStates, 2>>(); }
inline void forget_continued() { continuedEyes() = {}; recentEyes() = {}; }
// Stereo: eye 1 draws the same guest operation right after eye 0 (draw_stereo), with the same
// registers, so it takes eye 0's fetch, vertex and pixel shaders instead of looking them up again.
// When eye 0's draw completed in the same submission, eye 1 also replays its index and vertex
// buffers, its pipeline and each stage that samples no texture (StageReplay): of its own, eye 1
// resolves only its render targets and the textures its stages sample.
struct StereoShaders {
  LatteFetchShader* fs = nullptr;
  uint64_t fsKey = 0;
  vk::Shader *vs = nullptr, *ps = nullptr;
  bool replay = false;
  uint64_t submission = 0;
  Pipeline* pipeline = nullptr;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  std::array<VkFormat, 9> formats{};  // the pipeline's colour and depth formats
  float sx = 0, sy = 0;
  uint32_t width = 0, height = 0;
  VkViewport viewport{};
  VkRect2D scissor{};
  bool rasterReady = false;
  StageReplay stages[2];
  DrawTargets targets; // exact attachments/dimensions used by this resolved draw
  struct VertexBind { uint32_t binding; VkBuffer buffer; VkDeviceSize offset; };
  std::array<VertexBind, 32> vertexBinds{};
  uint32_t vertexBindCount = 0;
  bool indexed = false;
  VkBuffer indexBuffer = VK_NULL_HANDLE;
  VkDeviceSize indexOffset = 0;
  VkIndexType indexType = VK_INDEX_TYPE_UINT32;
  uint32_t drawCount = 0;
  bool multiview = false;  // (stage M4) eye 0's draw drew both eyes: eye 1 draws nothing
  void reset() {
    fs = nullptr; fsKey = 0; vs = ps = nullptr;
    replay = false; vertexBindCount = 0; rasterReady = false; multiview = false;
    stages[0].valid = stages[1].valid = false;
  }
  void add_vertex_bind(uint32_t binding, VkBuffer buffer, VkDeviceSize offset) {
    if (vertexBindCount < vertexBinds.size()) vertexBinds[vertexBindCount] = {binding, buffer, offset};
    ++vertexBindCount;  // past capacity: no replay
  }
};
// (per thread, stage 3.1: the atlas's thread's draws and the render thread's eyes hand their own over)
struct stereoShaders_per_thread_tag; /*per-thread:stereoShaders*/ inline StereoShaders& stereoShaders() { return per_thread::get<stereoShaders_per_thread_tag, StereoShaders>(); }
std::array<VkFormat, 9> target_formats(const std::array<Surface*, 8>& colors, const Surface* depth) {
  std::array<VkFormat, 9> f{};
  for (int i = 0; i < 8; ++i) f[i] = colors[i] ? colors[i]->fmt.pixel : VK_FORMAT_UNDEFINED;
  f[8] = depth ? depth->fmt.pixel : VK_FORMAT_UNDEFINED;
  return f;
}
PER_THREAD(StereoShaders*, stereoShadersOut, nullptr)       // eye 0's draw records
PER_THREAD(const StereoShaders*, stereoShadersIn, nullptr)  // eye 1's draw reuses
// (docs/VR_EYE_THREAD.md, stage E4) eye 0's hand-overs while eye 1 draws on its own thread: a ring, each slot reused
// once the thread is past it (their vectors keep their capacity)
namespace {
constexpr uint64_t kHandoffRing = 512;
StereoShaders handoffRing[kHandoffRing];
}
// The record of a GamePad backdrop pane draw (outside stereo): one reused object, no per-draw allocation.
// Only the outermost draw writes it; its private replay reads it (backdropShadersIn) and writes none.
// (VR fork) the render thread's only (backdrop_draws: never on a draw thread, never in a VR start)
StereoShaders backdropSnapshot;
const StereoShaders* backdropShadersIn = nullptr; // exact original draw, no guest resource rereads
// (VR fork) the GamePad backdrop (gfx/gamepad_backdrop.h) takes part in this draw: off by default, never possible in
// a start that tries VR (possible(): a once-read flag first, so VR's draws make no call), the render thread only
static bool backdrop_draws() {
  return ::gfx::gamepad_backdrop::possible() && ::gfx::gamepad_backdrop::enabled.load(std::memory_order_relaxed) &&
         !draw_threads::on_draw_thread();
}
struct ContinuedStats {
  uint64_t lastFrame = 0, draws = 0, verified = 0, mismatches = 0, totalMismatches = 0, totalVerified = 0;
  std::array<uint64_t, kReasons> reasons{};
  uint64_t recent = 0;  // continued draws that continued a recent state
  std::array<uint64_t, gx2::kSgCount> groups{};  // register groups that ended a continuation
  std::array<uint64_t, 8> mismatchKinds{};
};
struct continuedStats_per_thread_tag; /*per-thread:continuedStats*/ inline ContinuedStats& continuedStats() { return per_thread::get<continuedStats_per_thread_tag, ContinuedStats>(); }
const char* const mismatchKindNames[8] = {"fetch shader", "vertex shader", "pixel shader", "targets",
                                          "pipeline", "texture count", "texture binding", "topology"};
void report_continued_stats() {
  auto& st = continuedStats();
  if (R.frame < st.lastFrame + 120) return;
  const double frames = double(R.frame - st.lastFrame);
  if (st.draws) {
    char buf[1024];
    int k = std::snprintf(buf, sizeof buf, "[vulkan fastpath] %s frame %llu: %.1f%% of %.0f draws/frame continued (%.1f%% through the change log); full:",
                          scene_thread::on_scene_b() ? "(B)" : scene_thread::on_scene_thread() ? "(A)" :
                          draw_threads::on_draw_thread() ? "(atlas)" : "(render)", (unsigned long long)R.frame, 100.0 * st.reasons[kContinued] / st.draws, st.draws / frames,
                          100.0 * st.recent / st.draws);
    for (int i = 1; i < kReasons && k < int(sizeof buf); ++i)
      if (st.reasons[i]) k += std::snprintf(buf + k, sizeof buf - k, " %s %.1f%%", continuedReasonNames[i], 100.0 * st.reasons[i] / st.draws);
    if (st.reasons[kRegisters] && k < int(sizeof buf)) {
      k += std::snprintf(buf + k, sizeof buf - k, "; groups/frame:");
      for (int g = 0; g < gx2::kSgCount && k < int(sizeof buf); ++g)
        if (st.groups[g]) k += std::snprintf(buf + k, sizeof buf - k, " %s %.0f", gx2::state_group_name(g), st.groups[g] / frames);
    }
    if (fastpath_config().verifyEvery && k < int(sizeof buf))
      k += std::snprintf(buf + k, sizeof buf - k, "; verify: %llu checked, %llu mismatches (total %llu of %llu)",
                         (unsigned long long)st.verified, (unsigned long long)st.mismatches,
                         (unsigned long long)st.totalMismatches, (unsigned long long)st.totalVerified);
    LOG("%s", buf);
  }
  const uint64_t totalMismatches = st.totalMismatches, totalVerified = st.totalVerified;
  const auto kinds = st.mismatchKinds;
  st = {};
  st.lastFrame = R.frame;
  st.totalMismatches = totalMismatches;
  st.totalVerified = totalVerified;
  st.mismatchKinds = kinds;
}
// Render targets of a draw (both paths); false: the draw has no target.
bool resolve_targets(const uint32_t* r, vk::Shader* ps, DrawTargets& t) {
  t = {};
  const uint32_t eyeSize = stereo_eye_size_generation();
  const auto &lcr = *reinterpret_cast<const LatteContextRegister *>(r);
  auto mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
  const auto claim = gfx::size_request(ps->kind);
  for (int i = 0; i < 8; i++)
    if (mask & (1 << i))
      t.colors[i] = color_target(r, i, &t.slices[i], claim);
  t.activeColors = mask;
  t.depth = LatteMRT::GetActiveDepthBufferMask(lcr) ? depth_target(r, &t.depthSlice) : nullptr;
  // a lookup switched stereo eyes to full size (an unscalable format, surfaces.cpp): the targets
  // looked up before it still have the eye size; looked up again, they are rescaled
  if (eyeSize != stereo_eye_size_generation()) return resolve_targets(r, ps, t);
  if (t.depth && t.depth->width == 1280 && t.depth->height == 720) selected_main_depth() = t.depth->addr;
  auto& colors = t.colors;
  auto& depth = t.depth;
  const uint32_t guestWidth = colors[0] ? colors[0]->width : 0;
  const uint32_t guestHeight = colors[0] ? colors[0]->height : 0;
  if (aoPrivateReplay() && colors[0]) {
    auto& ao = ao_private();
    ao.source = colors[0]->addr;
    colors[0] = private_ao_surface(ao.color, colors[0]);
    t.slices[0] = 0;
    if (depth) {
      depth = private_ao_surface(ao.depth, depth);
      t.depthSlice = 0;
    }
  }
  // (docs/VR_EYE_PROJECTION.md) the game's UI into an eye's picture, with the eyes' own frustums: into the UI layer's
  // image instead (eye 0's; eye 1's copy is left out in draw), without depth
  if (ui_layer::candidate() && stereoEye() == 0 && colors[0] && stereo_screen_target(colors[0]->width, colors[0]->height)) {
    // (each target once: the game's own targets the UI was taken from; more than the eye's picture means one was a
    // picture the game draws its UI into and then shows, which is left empty here)
    static uint32_t seen[16] = {};
    static int nseen = 0;
    if (const uint32_t addr = colors[0]->addr; nseen < 16 && std::find(seen, seen + nseen, addr) == seen + nseen) {
      seen[nseen++] = addr;
      LOG("[vr ui] layouts taken from target %08X (%ux%u, format %u, frame %llu)", addr, colors[0]->width,
          colors[0]->height, unsigned(colors[0]->fmt.pixel), (unsigned long long)R.frame);
    }
    colors[0] = ui_layer::surface(colors[0]);
    t.slices[0] = 0;
    for (int i = 1; i < 8; ++i) colors[i] = nullptr;
    depth = nullptr;
  }
  Surface *target = depth;
  for (auto *c : colors)
    if (c) {
      target = c;
      break;
    }
  if (!target)
    return false;
  t.target = target;
  t.width = target->extent.width;
  t.height = target->extent.height;
  t.sx = target->sx;
  t.sy = target->sy;
  if (aoPrivateReplay() && guestWidth) {
    // the viewport registers describe the game's smaller buffer (x and y differ at other aspect ratios)
    t.sx = float(colors[0]->extent.width) / guestWidth;
    t.sy = guestHeight ? float(colors[0]->extent.height) / guestHeight : t.sx;
  }
  for (auto *&c : colors)
    if (c && (c->extent.width != t.width || c->extent.height != t.height)) {
      if (aoPrivateReplay()) c = nullptr;
      else exception_report::raise("mismatched Vulkan attachments");
    }
  if (depth && (depth->extent.width < t.width || depth->extent.height < t.height))
    depth = nullptr;
  return true;
}
// Eye 1's geometry replay still needs its own attachments, but consecutive draws do
// not need to resolve the same attachment registers again. This cache only owns
// lookup results; uploads and transitions below remain in command order.
// (2026-11-07; docs/VR_PERFORMANCE.md "The helper threads' eye 1 target lookup kept by its registers") On the scene's
// drawing threads it is kept by the registers resolve_targets reads, not the full-state version: the target, output
// and other groups' keys (gx2::ChangeLog::groupHash, with its reset count), 8 entries. A draw continued from a recent
// state, or a full draw that changed only its shaders' programs, fetch, textures or samplers, moves the version but
// keeps those groups, so its lookup is kept too. The groups hold every register resolve_targets reads (color_target:
// CB_COLORn BASE, SIZE, VIEW, INFO, TILE, FRAG; depth_target: DB_DEPTH_SIZE, VIEW, BASE, INFO, DB_HTILE_DATA_BASE, the
// slice count: targets; GetActiveColorBufferMask: CB_COLOR_CONTROL, CB_TARGET_MASK: output, the scissor, compared as
// before; GetActiveDepthBufferMask: DB_DEPTH_CONTROL: output); the pixel shader, the surface epoch, the eye size and the
// AO and UI modes are compared besides. The render thread keeps the one-entry memo by the version exactly as before
// (its change log keeps no key: nothing added there). WWHD_VK_REPLAY_TARGETS_KEYED=0: the version on every thread.
bool resolve_replay_targets(const uint32_t* r, vk::Shader* ps, DrawTargets& targets) {
  struct Cached {
    uint64_t frame = ~0ull, version = 0, epoch = 0;
    uint32_t eyeSize = 0, scissorBR = 0;
    vk::Shader* shader = nullptr;
    DrawTargets targets;
  };
  static const bool keyedOn = [] {
    const char* e = std::getenv("WWHD_VK_REPLAY_TARGETS_KEYED");
    return !(e && !std::strcmp(e, "0"));
  }();
  if (!keyedOn || !draw_threads::on_draw_thread()) {
    PER_THREAD_STATIC(cache, Cached);
    if (cache.frame == R.frame && cache.version == gx2::full_state_version() &&
        cache.epoch == g_surface_epoch && cache.eyeSize == stereo_eye_size_generation() && cache.shader == ps) {
      // (upstream #399) the scissor does not bump the version but selects the active colour buffers (continued_reason)
      const uint32_t scissorBR = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
      if (scissorBR == cache.scissorBR ||
          LatteMRT::GetActiveColorBufferMask(ps->dec, *reinterpret_cast<const LatteContextRegister*>(r)) ==
              cache.targets.activeColors) {
        cache.scissorBR = scissorBR;
        targets = cache.targets;
        return true;
      }
    }
    if (!resolve_targets(r, ps, targets)) return false;
    cache = {R.frame, gx2::full_state_version(), g_surface_epoch, stereo_eye_size_generation(),
             r[REGADDR::PA_SC_GENERIC_SCISSOR_BR], ps, targets};
    return true;
  }
  // (the drawing threads) by the groups' keys
  struct Keyed {
    uint64_t frame = ~0ull, epoch = 0, resets = ~0ull, keys[3] = {};
    uint32_t eyeSize = 0, scissorBR = 0;
    bool ao = false, ui = false;
    vk::Shader* shader = nullptr;
    DrawTargets targets;
  };
  struct KeyedCache {
    std::array<Keyed, 8> entries{};
    uint32_t next = 0;
  };
  PER_THREAD_STATIC(keyedCache, KeyedCache);
  const auto& log = gx2::thread_change_log();
  const uint64_t keys[3] = {log.groupHash[gx2::kSgTargets], log.groupHash[gx2::kSgOutput], log.groupHash[gx2::kSgOther]};
  const bool ao = aoPrivateReplay(), ui = ui_layer::candidate();
  const uint32_t eyeSize = stereo_eye_size_generation();
  const uint32_t scissorBR = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
  for (auto& c : keyedCache.entries) {
    if (c.frame != R.frame || c.epoch != g_surface_epoch || c.eyeSize != eyeSize || c.shader != ps || c.ao != ao ||
        c.ui != ui || c.resets != log.resets || std::memcmp(c.keys, keys, sizeof keys))
      continue;
    if (scissorBR == c.scissorBR ||
        LatteMRT::GetActiveColorBufferMask(ps->dec, *reinterpret_cast<const LatteContextRegister*>(r)) ==
            c.targets.activeColors) {
      c.scissorBR = scissorBR;
      targets = c.targets;
      return true;
    }
  }
  if (!resolve_targets(r, ps, targets)) return false;
  keyedCache.entries[keyedCache.next] = {R.frame, g_surface_epoch, log.resets, {keys[0], keys[1], keys[2]}, eyeSize,
                                         scissorBR, ao, ui, ps, targets};
  keyedCache.next = (keyedCache.next + 1) % keyedCache.entries.size();
  return true;
}
bool aliases_target(const Surface* s, const std::array<Surface*, 8>& colors, const Surface* depth);
// (resolve_texture, texture packs on, a guest texture that has had a replacement) the replacement image to bind in
// place of `s` (texture_pack_vk.cpp), or `s`. Every rule holds (texture_pack_core.h may_bind): the replacement was
// made for the texture's contents now and the texture is still one the CPU wrote (replacement_for checks both), the
// draw does not render into it (aliases_target, the callers' own test), the shader reads it as plain sampled colour
// on this unit (no texel fetch, size or LOD query, explicit LOD: textureNeedsGuestLevels), and its levels are not
// hand-drawn. The caller's view, uf_texNScale (sx, sy: the scale) and sampler (lodOffset) then all come from the
// surface returned, so a draw is consistent whichever it gets.
//
// Under the surface lock, as everything the pack's images hold: the drawing threads (scene's, second, atlas's)
// bind replacements already made (texture_pack_step makes them at the frame's end, on the render thread). Its view
// is made here, under the lock, so two threads that first bind a new replacement do not both add it.
// No allocation: the only state written is replacement_for's (the frame it was bound, a hit counted).
__attribute__((noinline)) Surface* pack_replacement(Surface* s, const uint32_t* words, vk::Shader* sh, uint32_t unit,
                                                    const std::array<Surface*, 8>& colors, const Surface* depth) {
  // (diagnostics) the lookups' cost, lock wait included; WWHD_TEXTURE_PACK_NO_BIND=1 does all of the lookup and binds
  // the original anyway, which separates this CPU cost from the GPU's for the larger images
  static const bool noBind = std::getenv("WWHD_TEXTURE_PACK_NO_BIND") != nullptr;
  // (only with the stats lines: backend.cpp's perf_enabled / cpu_only_stats_enabled, file-local there)
  static const bool timing = [] {
    const char* cpu = std::getenv("WWHD_VK_CPU_ONLY_STATS");
    return std::getenv("WWHD_VK_STATS") != nullptr || (cpu && !std::strcmp(cpu, "1"));
  }();
  struct Timed {
    bool on;
    std::chrono::steady_clock::time_point t;
    ~Timed() {
      if (!on) return;
      g_stat_texpack_bind_calls.fetch_add(1, std::memory_order_relaxed);
      g_stat_texpack_bind_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - t).count()), std::memory_order_relaxed);
    }
  } timed{timing, timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
  draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());
  const bool target = aliases_target(s, colors, depth);
  const bool needsGuestLevels = sh->dec->textureNeedsGuestLevels[unit];
  // the draw's own rules first: replacement_for counts a hit and keeps the image from the pack's budget, which a
  // unit that cannot take it must not
  Surface* replacement = !target && !needsGuestLevels && !s->artisticMips ? replacement_for(s) : nullptr;
  const ::gfx::texture_pack::Key key{s->format, s->width, s->height, s->level0Hash};
  const ::gfx::texture_pack::BindFacts facts{replacement != nullptr, replacement && s->replacementKey == key,
                                             s->gpuWritten, target, needsGuestLevels, s->artisticMips};
  if (!::gfx::texture_pack::may_bind(facts)) {
    if (needsGuestLevels && s->replacement) ++g_stat_texpack_guest_levels;  // (the stats line's guest-levels)
    return s;
  }
  sampled_texture_view(replacement, words);
  return noBind ? s : replacement;
}
// A sampled texture of a stage (both paths): lookup, AO substitution, the write-tracking check and a replacement
// texture pack's image (pack_replacement: colors and depth are the draw's targets, for its rule that the draw does
// not render into the texture).
// substituted: the private AO image replaced the guest texture.
// guest: set to the guest texture when a replacement is returned instead (else left as it is)
Surface* resolve_texture(const uint32_t* r, vk::Shader* sh, uint32_t unit, const std::array<Surface*, 8>& colors,
                         const Surface* depth, bool* substituted = nullptr, Surface** guest = nullptr) {
  const uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
  // the mip levels its sampler reaches (sampler(): no mip filter -> the base level, else up to MAX_LOD)
  uint32_t reach = 16;
  if (uint32_t si = sh->dec->textureUnitSamplerAssignment[unit]; si < 18) {
    const uint32_t* smp = r + REGADDR::SQ_TEX_SAMPLER_WORD0_0 + ((sh->vertex ? 18 : 0) + si) * 3;
    Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0 s0;
    Latte::LATTE_SQ_TEX_SAMPLER_WORD1_0 s1;
    memcpy(&s0, &smp[0], 4);
    memcpy(&s1, &smp[1], 4);
    if (uint32_t(s0.get_MIP_FILTER()) == 0) reach = 1;
    else reach = std::max(uint32_t(s1.get_MIN_LOD()), uint32_t(s1.get_MAX_LOD())) / 64 + 2;
  }
  auto *s = sampled_texture(r + texbase + unit * 7, sh->dec->textureUsesDepthCompare[unit], reach);
  // debug D-sweep (WWHD_DSWEEP): a swept model can sample a texture its actor sets by code. An earlier texture stands
  // in (pipelines do not depend on the texture); its texture words replace the missing ones in this draw's
  // register copy, so the view matches.
  static const bool sweep = getenv("WWHD_DSWEEP") && *getenv("WWHD_DSWEEP");
  struct StandIn { Surface* surface = nullptr; uint32_t words[7] = {}; };
  static StandIn standIn[2];
  const bool depthCompare = sh->dec->textureUsesDepthCompare[unit];
  uint32_t* words = const_cast<uint32_t*>(r + texbase + unit * 7);
  if (!s && sweep && standIn[depthCompare].surface) {
    s = standIn[depthCompare].surface;
    memcpy(words, standIn[depthCompare].words, sizeof standIn[depthCompare].words);
  }
  if (!s)
    exception_report::raise("missing sampled texture");
  if (sweep) { standIn[depthCompare].surface = s; memcpy(standIn[depthCompare].words, words, sizeof standIn[depthCompare].words); }
  auto& ao = ao_private();
  if (!sh->vertex && ao_hires_enabled() && ao.source &&
      s->addr == ao.source && ao.frame == R.frame &&
      sh->kind == gfx::ProgramKind::OcclusionPixel) {
    s = &ao.color;
    if (substituted) *substituted = true;
  }
  upload_surface(s);
  if (scene_stats::kOn) [[unlikely]] scene_stats::sampled(s);
  // (texture packs) after the upload, which keeps or drops the replacement for the contents now. Off: this one test.
  // On: the lock only for a texture with a replacement or one the budget released (its key is set only then:
  // texture_pack_vk.cpp; a relaxed read of a value written under the surface lock, and a stale zero only draws the
  // original once more)
  if (g_texture_packs_on) [[unlikely]]
    if (__atomic_load_n(&s->replacementKey.hash, __ATOMIC_RELAXED) != 0)
      if (Surface* replacement = pack_replacement(s, words, sh, unit, colors, depth); replacement != s) {
        if (guest) *guest = s;
        return replacement;
      }
  return s;
}
bool aliases_target(const Surface* s, const std::array<Surface*, 8>& colors, const Surface* depth) {
  bool aliases = depth && s->image == depth->image;
  for (auto *color : colors)
    if (color && s->image == color->image)
      aliases = true;
  return aliases;
}
// a lookup at one of the draw's own target addresses can pick another surface once the target's
// write sequence advances (find_or_create_surface): such textures are not continued
bool shares_target_address(const Surface* s, const std::array<Surface*, 8>& colors, const Surface* depth) {
  if (depth && depth->addr == s->addr) return true;
  for (auto *color : colors)
    if (color && color->addr == s->addr) return true;
  return false;
}
// guest: the guest texture when `s` is its texture pack replacement (resolve_texture's `guest`), else null
VkSampler resolve_sampler(const uint32_t* r, vk::Shader* sh, uint32_t unit, const Surface* s, const Surface* guest = nullptr) {
  uint32_t samplerId = sh->dec->textureUnitSamplerAssignment[unit];
  if (samplerId >= 18)
    exception_report::raise("missing texture sampler");
  uint32_t samplerBase = sh->vertex ? 18 : 0;
  const uint32_t* samplerWords = r + REGADDR::SQ_TEX_SAMPLER_WORD0_0 +
                                  (samplerBase + samplerId) * 3;
  uint32_t patchedSampler[3];
  if (!sh->vertex && ao_mode() >= 1 && unit == 0 &&
      sh->kind == gfx::ProgramKind::OcclusionPixel) {
    memcpy(patchedSampler, samplerWords, sizeof patchedSampler);
    patchedSampler[0] = (patchedSampler[0] & ~0x7E00u) | (1u << 9) | (1u << 12);
    samplerWords = patchedSampler;
  }
  // hand-drawn mip levels (gfx/artistic_mips.h): no forced anisotropy; the resolution-aware bias in the sampler where
  // the device applies sampler biases (else in the shader's uniform, bind_stage). A replacement texture pack's image
  // (resolve_texture): its levels in front of the guest's (0 on every guest surface), and the forced anisotropy the
  // guest's own levels decide (a single-level original gets none with its replacement either)
  const Surface* levels = guest ? guest : s;
  return sampler(samplerWords, sh->dec->textureUsesDepthCompare[unit],
                 s->fmt.kind != FormatInfo::FLOAT,
                 !levels->gpuWritten && levels->mips > 1 && !levels->artisticMips, s->linearFiltering,
                 s->artisticMips && R.samplerMipLodBias ? g_artisticBias.bias : 0.0f, s->lodOffset);
}
// ---------------------------------------------------------------- batched same-state runs
// (gfx/draw_batch.h; issue #1, docs/VR_PERFORMANCE.md "Batched same-state runs") a run of draws that differ only in
// some vertex-shader uniform blocks' addresses, drawn as one instanced draw of the vertex shader's batched variant
// (batch_glsl.h): instance k reads piece k's blocks. Set by draw_stereo for the draws of one run's chunk (this thread)
struct BatchDraw {
  const draw_batch::Batch* run = nullptr;
  vk::Shader* base = nullptr;     // the run's vertex shader
  vk::Shader* variant = nullptr;  // its batched variant
  const vk::batch::Variant* layout = nullptr;
  uint32_t first = 0, count = 0;  // the chunk's pieces
};
BatchDraw*& batch_draw() {
  static thread_local BatchDraw* batch = nullptr;
  return batch;
}
// (docs/VR_SCENE_THREADS.md, "S6: batched runs on the drawing threads", render thread) a held run draw_stereo offers
// the scene's drawing threads: scene_thread::draw hands it over whole (t_sceneRunTaken), or nothing is drawn of it
constinit thread_local const draw_batch::Batch* t_sceneRun = nullptr;
constinit thread_local bool t_sceneRunTaken = false;
bool batch_accept(const uint32_t* r, const draw_batch::Batch& run, BatchDraw& out, bool atlasThread);  // (below)
// the chunk's pieces of batched block i, one after another at the variant's stride (zero after each guest block)
void batch_gather(const BatchDraw& b, const uint32_t* r, uint32_t i, std::vector<uint8_t>& out) {
  const uint32_t stride = b.layout->vec4s[i] * 16;
  const uint32_t size = std::min<uint32_t>(r[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7 + 1] + 1, 0x10000);
  const uint32_t copy = std::min(size, stride);
  static const bool submittedOn = gx2::submitted_uniforms_enabled();
  out.assign(size_t(b.count) * stride, 0);
  for (uint32_t k = 0; k < b.count; ++k) {
    const uint32_t addr = b.run->addrs[b.first + k][i];
    const uint8_t* submitted = addr && submittedOn ? gx2::submitted_uniform_block(addr, size) : nullptr;
    const uint8_t* source = submitted ? submitted : static_cast<const uint8_t*>(addr ? mem::ptr(addr) : nullptr);
    if (source) std::memcpy(out.data() + size_t(k) * stride, source, copy);
  }
}
// The point lights' per-eye correction (docs/VR_EYE_PROJECTION.md, "Point lights"): WWHD_VK_POINT_LIGHT_EYES=ray (the
// default: each eye's ray, stereo_projection.h point_light_uniforms), full (also the light's place moved for the eye,
// as first made on 2026-11-04), 0 (none: as before then, the passes shared by the eyes, replayed or multiview), none
// (the passes not drawn at all: no torch or searchlight light; a Quest test, 2026-11-05)
enum PointLightEyes { kPointLightOff, kPointLightRay, kPointLightFull, kPointLightNone };
static PointLightEyes point_light_eyes() {
  static const PointLightEyes m = [] {
    const char* e = std::getenv("WWHD_VK_POINT_LIGHT_EYES");
    // (2026-11-06) Android: the passes not drawn by default. A Quest 3 player's torches and searchlights shone a
    // different way in each eye with the per-eye correction (full, then ray); the test build without them (2026-11-05)
    // was the one that looked right. =0, =ray or =full in env.txt draws them again; PC VR keeps ray
#ifdef __ANDROID__
    constexpr PointLightEyes kDefault = kPointLightNone;
#else
    constexpr PointLightEyes kDefault = kPointLightRay;
#endif
    const PointLightEyes v = !e || !*e ? kDefault
                             : !std::strcmp(e, "0") || !std::strcmp(e, "off") ? kPointLightOff
                             : !std::strcmp(e, "full") ? kPointLightFull
                             : !std::strcmp(e, "none") ? kPointLightNone : kPointLightRay;
    LOG("[vulkan stereo] point lights (torches, searchlights): %s (WWHD_VK_POINT_LIGHT_EYES=ray, full, 0 or none)",
        v == kPointLightNone ? "not drawn (no torch or searchlight light)"
        : v == kPointLightOff ? "shared by the eyes, not corrected per eye"
        : v == kPointLightFull ? "each eye's ray and the light's place moved for the eye" : "each eye's ray");
    return v;
  }();
  return m;
}
StageResources bind_stage(const uint32_t *r, vk::Shader *sh,
                          VkDescriptorSetLayout layout, bool dynamicUniforms,
#ifdef WWHD_MOBILE_GPU_PATHS
                          uint32_t dynamicMask,  // (uniform_plan.h) the stage's dynamic slots; dynamicUniforms: any
#endif
                          float sx, float sy,
                          const std::array<Surface *, 8> &colors,
                          Surface *depth, FeedbackStatsProbe* feedbackProbe, float eyeOffset = 0,
                          // a pass that rebuilds positions from depth, adjusted for the eye (the sun shadow, the point
                          // lights: stereo_projection.h); Other: none
                          gfx::ProgramKind eyeRebuild = gfx::ProgramKind::Other,
                          const ContinuedStage* reuse = nullptr, ContinuedStage* record = nullptr,
                          StageReplay* replayOut = nullptr) {
  const uint64_t stageEntered = rprof::sub_begin();
  StageResources out;
  auto &m = sh->mapping;
  // stereo eye 0: what eye 1 needs to bind this stage again (replay_stage)
  const bool sunShadow = eyeRebuild == gfx::ProgramKind::SunShadowMask;
  const bool pointLight = eyeRebuild == gfx::ProgramKind::PointLight;
  bool replayable = replayOut && dynamicUniforms && !((sunShadow || pointLight) && eyeOffset != 0);
  if (replayOut) replayOut->valid = false, replayOut->uniformCount = 0, replayOut->textureCount = 0,
                 replayOut->eyeTextureCount = 0;
  // own: the AO substitute, which eye 1 cannot resolve on its own; a feedback copy eye 1 makes of its own layer
  // (StageReplay::EyeTexture::feedback; WWHD_VK_FEEDBACK_EYE_REPLAY=0: eye 1 binds the stage itself as before)
  auto replayTexture = [&](Surface* s, uint32_t unit, int binding, bool own, bool feedback) {
    if (!replayable) return;
    if (feedback && !own && feedback_eye_replay())
      replayOut->eyeTextures[replayOut->eyeTextureCount++] = {unit, binding, s->sx, s->sy, 0, true};
    else if (own || feedback) replayable = false;
    else if (stereo_shared_texture(s)) replayOut->textures[replayOut->textureCount++] = s;
    else replayOut->eyeTextures[replayOut->eyeTextureCount++] = {unit, binding, s->sx, s->sy, 0};
  };
  std::array<int, 4> eyeUniformBindings{};
  UploadSlice lastUpload;
  auto eyeUniform = [&](int binding, const uint8_t* bytes, size_t size, const stereo::EyeRows& rows) {
    if (!replayable || binding < 0 || !rows.count) return;
    if (rows.overflow || replayOut->uniformCount >= replayOut->uniforms.size()) { replayable = false; return; }
    eyeUniformBindings[replayOut->uniformCount] = binding;
    auto& u = replayOut->uniforms[replayOut->uniformCount++];
    u.binding = uint32_t(binding);
    u.buffer = lastUpload.buffer;
    u.bytes.assign(bytes, bytes + size);
    u.rows = rows;
    u.reserved = eye_thread::enabled() && !draw_threads::on_draw_thread()
                     ? allocate_upload(std::max<size_t>(size, 16), R.properties.limits.minUniformBufferOffsetAlignment)
                     : UploadSlice{};
  };
  // Descriptor info pointers must remain stable until this stage's one update.
  std::array<VkDescriptorBufferInfo, 17> bufferInfos;
  std::array<VkDescriptorImageInfo, LATTE_NUM_MAX_TEX_UNITS> imageInfos;
  std::array<VkWriteDescriptorSet, 17 + LATTE_NUM_MAX_TEX_UNITS> writes;
  uint32_t bufferCount = 0, imageCount = 0, writeCount = 0;
  std::array<std::pair<uint32_t, uint32_t>, 17> dynamicBindings{};
  uint32_t dynamicCount = 0;
  static const bool ranksEnabled = [] {
    const char* value = std::getenv("WWHD_VK_DESCRIPTOR_RANKS");
    return value && std::strcmp(value, "1") == 0;
  }();
  const auto& rankPlan = sh->descriptorRanks;
  const bool useRanks = ranksEnabled && rankPlan.valid;
  std::array<bool, 17 + LATTE_NUM_MAX_TEX_UNITS> occupied;
  std::array<uint32_t, 17 + LATTE_NUM_MAX_TEX_UNITS> rankedOffsets;
  if (useRanks) occupied.fill(false);
  auto appendWrite = [&](uint8_t rank) -> VkWriteDescriptorSet & {
    if (writeCount >= writes.size())
      exception_report::raise("stage descriptor write capacity exceeded");
    const size_t slot = useRanks ? size_t(rank) : size_t(writeCount);
    if (useRanks && (slot >= rankPlan.count || occupied[slot]))
      exception_report::raise("stage descriptor rank metadata mismatch");
    ++writeCount;
    if (useRanks) occupied[slot] = true;
    auto &write = writes[slot];
    write = {}; // Initialize every active field; unused capacity is never read.
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = out.set;
    write.descriptorCount = 1;
    return write;
  };
  // guestAddr: a guest uniform block (bytes == mem::ptr(guestAddr)), served by the buffer cache if on
  // known: an upload slice already holding these bytes (a submitted block uploaded by an earlier draw)
#ifdef WWHD_MOBILE_GPU_PATHS
  const bool stageDynamic = dynamicUniforms;
#endif
  auto uniform = [&](int binding, const void *bytes, size_t size, size_t logicalSlot,
                     uint32_t guestAddr = 0, const UploadSlice* known = nullptr) {
    if (binding < 0)
      return;
#ifdef WWHD_MOBILE_GPU_PATHS
    // (uniform_plan.h) this block's own kind: dynamic, or a plain buffer of a pipeline over the cap (its offset in the
    // write; an eye-shifted block there leaves the draw per eye: replayOut->valid below needs its dynamic offset)
    const bool dynamicUniforms = stageDynamic && ((dynamicMask >> logicalSlot) & 1u);
#endif
    // (stage D2b 2b) the vertex stage's blocks: the motion variant computes the previous position there only
    if (sh->vertex && bytes && motion_records()) [[unlikely]] {
      if (motion::DrawData* d = motion_store().slot(current_draw_index(), stereoEye())) {
        const bool support = logicalSlot == 16;
        d->add(support ? motion::Part::VertexSupport : motion::Part::VertexBlock, uint8_t(support ? 0 : logicalSlot),
               bytes, uint32_t(size));
      }
    }
    if (size > R.properties.limits.maxUniformBufferRange)
      exception_report::raise("uniform buffer exceeds device range");
    if(preparation_stats_enabled()) {
      ++draw_counters().cpuPreparation.uniformSnapshotCalls;
      draw_counters().cpuPreparation.uniformSnapshotBytes+=size;
    }
    // On by default on Windows again (docs/VR_PERFORMANCE.md, "PERF-05": off from the fourth round, when the view
    // shook in the headset, back on with multiview, the atlas thread and PERF-07a for a headset A/B), and on desktop
    // Linux on x86-64 as on Windows (2026-11-04), and on Steam Frame - Native, Linux on ARM (asked for, the same day;
    // Android unchanged); WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=1 or 0 sets it anywhere
    static const bool reuseUniforms = [] {
      const char* value = std::getenv("WWHD_VK_REUSE_UNIFORM_SNAPSHOTS");
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
      return !value || std::strcmp(value, "0") != 0;
#else
      return value && std::strcmp(value, "1") == 0;
#endif
    }();
    // compares CPU copies of the slots' bytes, not mapped upload memory, unless the upload memory is
    // host-cached (R.uploadReadsDirect; uniform_snapshot.h)
    PER_THREAD_STATIC(uniformCache, UniformSnapshotCache<UploadSlice, VkDevice>);  // (per thread, stage 2.3)
    rprof::UploadKind uploads(logicalSlot == 16 ? rprof::kUpUniformVars : rprof::kUpUbo);
    auto fresh = [&](const void* source, size_t length) {
      return snapshot(source, length, R.properties.limits.minUniformBufferOffsetAlignment);
    };
    UploadSlice cachedSlice;
    const bool cached = known || (guestAddr && buffer_cache_enabled() &&
        cached_guest_range(guestAddr, uint32_t(size), rprof::kUpUbo, cachedSlice));
    if (known) cachedSlice = *known;
    const auto b = cached ? cachedSlice
        : reuseUniforms
        ? uniformCache.get(R.device, R.submissionGeneration,
                           (sh->vertex ? 0 : 17) + logicalSlot, bytes, size,
                           R.uploadReadsDirect, fresh)
        : fresh(bytes, size);
    if (reuseUniforms && preparation_stats_enabled()) {
      const auto& counts = uniformCache.counters;
      draw_counters().cpuPreparation.uniformReuseChecks = counts.checks;
      draw_counters().cpuPreparation.uniformReuseComparisons = counts.comparisons;
      draw_counters().cpuPreparation.uniformReuseHits = counts.hits;
      draw_counters().cpuPreparation.uniformReuseBytes = counts.reusedBytes;
    }
    lastUpload = b;
    if (bufferCount >= bufferInfos.size())
      exception_report::raise("stage uniform descriptor capacity exceeded");
    auto &info = bufferInfos[bufferCount++];
    info = {b.buffer, dynamicUniforms ? 0 : b.offset, b.size};
    if (dynamicUniforms) {
      if (b.offset > UINT32_MAX)
        exception_report::raise("dynamic uniform offset exceeds uint32 range");
      if (useRanks) {
        const auto rank = logicalSlot == 16 ? rankPlan.support : rankPlan.blocks[logicalSlot];
        if (rank >= rankPlan.count)
          exception_report::raise("stage uniform rank metadata mismatch");
        rankedOffsets[rank] = uint32_t(b.offset);
      } else dynamicBindings[dynamicCount++] = {uint32_t(binding), uint32_t(b.offset)};
    }
    auto &write = appendWrite(logicalSlot == 16 ? rankPlan.support : rankPlan.blocks[logicalSlot]);
    write.dstBinding = binding;
    write.descriptorType = dynamicUniforms
        ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
        : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
  };
  // CPU-only scratch supplies an immutable upload slice below.
  // Separate stages retain capacity without retaining guest payload contents.
  PER_THREAD_STATIC(supportScratch, std::array<std::vector<uint8_t>, 2>);
  auto& supportUniforms = supportScratch[sh->vertex ? 0 : 1];
  stereo::EyeRows supportRows;
  rprof::sub_end(rprof::kSubUniformSetup, stageEntered);
  const uint64_t packStarted = rprof::sub_begin();
  if (m.uniformVarsBufferBindingPoint >= 0) {
    const size_t capacity=preparation_stats_enabled()?supportUniforms.capacity():0;
    vk::pack_uniforms_into(r, *sh, supportUniforms, sx, sy, eyeOffset, stereo::frame.convergence,
                           replayOut ? &supportRows : nullptr);
    // WWHD_VK_UNIFORM_VERIFY=1 (debug): packs again into a separate buffer and compares, byte for
    // byte, with what this draw uploads; any change to how support uniforms are packed or reused
    // must keep "0 mismatches"
    static const bool verifyPack = [] {
      const char* e = std::getenv("WWHD_VK_UNIFORM_VERIFY");
      return e && std::strcmp(e, "1") == 0;
    }();
    if (verifyPack) {
      static thread_local std::vector<uint8_t> fresh;
      static uint64_t checks = 0, mismatches = 0;
      vk::pack_uniforms_into(r, *sh, fresh, sx, sy, eyeOffset, stereo::frame.convergence);
      ++checks;
      if (fresh != supportUniforms && mismatches++ < 16) {
        // where: the first differing byte, and the guest uniform-block entry it was copied from (if any)
        size_t at = 0;
        while (at < fresh.size() && at < supportUniforms.size() && fresh[at] == supportUniforms[at]) ++at;
        uint32_t source = 0;
        const int base = sh->uniforms.offset_remapped;
        const uint32_t blockBase = sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
        if (sh->dec && base >= 0)
          for (const auto& group : sh->dec->list_remappedUniformEntries_bufferGroups)
            for (const auto& entry : group.entries)
              if (at >= size_t(base + entry.mappedIndexOffset) && at < size_t(base + entry.mappedIndexOffset + 16))
                source = r[blockBase + group.kcacheBankIdOffset / 4] + entry.indexOffset;
        LOG("[vulkan uniform verify] MISMATCH: %s shader %016llx, %zu vs %zu bytes, first difference at +%zu%s%08X",
            sh->vertex ? "vertex" : "pixel", (unsigned long long)sh->key, supportUniforms.size(), fresh.size(), at,
            source ? ", read from guest memory at " : ", from registers ", source);
      }
      if ((checks & 0xFFFFF) == 0)
        LOG("[vulkan uniform verify] %llu checks, %llu mismatches", (unsigned long long)checks,
            (unsigned long long)mismatches);
    }
    if(preparation_stats_enabled()) {
      ++draw_counters().cpuPreparation.packCalls;
      draw_counters().cpuPreparation.packBytes+=supportUniforms.size();
      if(supportUniforms.capacity()>capacity) {
        ++draw_counters().cpuPreparation.packCapacityGrowths;
        draw_counters().cpuPreparation.packCapacityGrowthBytes+=supportUniforms.capacity()-capacity;
      }
    }
  }
  else
    supportUniforms.clear();
  // a batched run's support block (BatchDraw: its pieces at the upload, below)
  const BatchDraw* batch = sh->vertex ? batch_draw() : nullptr;
  const bool batchSupport = batch && sh == batch->variant && batch->layout->support &&
                            m.uniformVarsBufferBindingPoint >= 0;
  // Bloom extract cThresholdParam.z: scale once, before blur/downsampling.
  if (!sh->vertex && (r[mmSQ_PGM_START_PS] << 8) == 0x44F91200 &&
      sh->uniforms.offset_remapped >= 0 && size_t(sh->uniforms.offset_remapped) < supportUniforms.size())
    render::scale_bloom_uniforms(supportUniforms.data() + sh->uniforms.offset_remapped,
                                supportUniforms.size() - sh->uniforms.offset_remapped);
  // Metal AO mode 2 tiles noise per 960x540 output pixel rather than 640x360.
  const int remapped = sh->uniforms.offset_remapped;
  // (VR fork) Disable ambient occlusion: the AO pass's strength (remapped[0].y, its last multiply) 0, so it writes no
  // occlusion (1, 0, 0, 1: its own result at strength 0), and its translated shader returns that at once (shaders.cpp
  // ao_early_out), without its depth taps. Only the AO pass: the sun's shadow mask is its own pass
  if (!sh->vertex && sh->kind == gfx::ProgramKind::OcclusionPixel && render::ambient_occlusion_off() && remapped >= 0 &&
      size_t(remapped) + 8 <= supportUniforms.size()) {
    const float zero = 0.0f;
    memcpy(supportUniforms.data() + remapped + 4, &zero, sizeof zero);
  }
  if (sunShadow && eyeOffset != 0 && remapped >= 0 &&
      size_t(remapped) <= supportUniforms.size()) {
    // Unknown layouts keep their original uniforms. The helper validates
    // the complete block before writing any stereo adjustments.
    stereo::shadow_uniforms(supportUniforms.data()+remapped,
        supportUniforms.size()-remapped,sh->vertex,eyeOffset,stereo::frame.convergence,stereo::eye_frustum(eyeOffset));
  }
  if (pointLight && eyeOffset != 0 && remapped >= 0 && size_t(remapped) <= supportUniforms.size()) {
    // (diagnostics, 2026-11-07: PC VR's torchlight "shining in inverted directions") the first passes' uniforms per eye,
    // before and after, with the eye's frustum: the signs of the ray's terms checked against real values
    static std::atomic<int> logged{0};
    const bool log = logged.load(std::memory_order_relaxed) < 12 && supportUniforms.size() - remapped >= 96;
    float before[24] = {};
    if (log) std::memcpy(before, supportUniforms.data() + remapped, 96);
    stereo::point_light_uniforms(supportUniforms.data()+remapped, supportUniforms.size()-remapped, sh->vertex,
        eyeOffset, stereo::frame.convergence, stereo::eye_frustum(eyeOffset), point_light_eyes() == kPointLightFull);
    if (log && logged.fetch_add(1, std::memory_order_relaxed) < 12) {
      const auto f = stereo::eye_frustum(eyeOffset);
      float after[24];
      std::memcpy(after, supportUniforms.data() + remapped, 96);
      std::string b, a;
      char t[32];
      for (int i = 0; i < 24; ++i) {
        std::snprintf(t, sizeof t, "%s%.4g", i % 4 ? "," : " | ", double(before[i])); b += t;
        std::snprintf(t, sizeof t, "%s%.4g", i % 4 ? "," : " | ", double(after[i])); a += t;
      }
      LOG("[vulkan stereo] point light %s %08X eye %.3f conv %.1f frustum sx %.4f ox %.4f sy %.4f oy %.4f\n  before%s\n  after %s",
          sh->vertex ? "VS" : "PS", r[sh->vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] << 8, double(eyeOffset),
          double(stereo::frame.convergence), double(f.sx), double(f.ox), double(f.sy), double(f.oy), b.c_str(), a.c_str());
    }
  }
  if (sh->vertex && ao_mode() == 2 &&
      sh->kind == gfx::ProgramKind::OcclusionVertex && remapped >= 0 &&
      size_t(remapped) + 16 <= supportUniforms.size()) {
    float noiseScale;
    memcpy(&noiseScale, supportUniforms.data() + remapped + 12, sizeof noiseScale);
    noiseScale *= 1.5f;
    memcpy(supportUniforms.data() + remapped + 12, &noiseScale, sizeof noiseScale);
  }

  rprof::sub_end(rprof::kSubUniformPack, packStarted);
  const uint64_t blocksStarted = rprof::sub_begin();
  uint32_t block =
      sh->vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
  for (int i = 0; i < 16; i++)
    if (m.uniformBuffersBindingPoint[i] >= 0) {
      if (batch && sh == batch->variant && (batch->layout->blocks >> i & 1)) {
        // a batched run's block: its pieces' bytes, one per instance (never a camera projection: batch_accept)
        PER_THREAD_STATIC(pieces, std::vector<uint8_t>);
        batch_gather(*batch, r, uint32_t(i), pieces);
        uniform(m.uniformBuffersBindingPoint[i], pieces.data(), pieces.size(), size_t(i));
        continue;
      }
      uint32_t addr = r[block + i * 7];
      uint32_t size = std::min<uint32_t>(r[block + i * 7 + 1] + 1, 0x10000);
      // a block of a display list: its bytes as submitted (gx2/submitted_uniforms.h), uploaded once
      // per submission and upload generation, not through the buffer cache (which holds guest memory)
      static const bool submittedOn = gx2::submitted_uniforms_enabled();  // (VR fork: off with draw threads)
      const uint8_t* submitted = addr && submittedOn ? gx2::submitted_uniform_block(addr, size) : nullptr;
      const unsigned char* source = submitted ? submitted : static_cast<const unsigned char*>(addr ? mem::ptr(addr) : nullptr);
      // Only a block holding a camera projection gets an eye copy; the others keep their
      // buffer-cache upload by address.
      if (sh->vertex && eyeOffset != 0 && addr &&
          stereo::has_camera_projection(source, size)) {
        // (docs/VR_FRAME_SYNTHESIS.md) the camera's planes and the eye's depth target, while the runtime takes them
        if (vr_framesynth::active()) [[unlikely]] vr_framesynth::note_camera(stereoEye(), r, source, size);
        PER_THREAD_STATIC(eyeBlock, std::vector<uint8_t>);
        eyeBlock.resize(size);memcpy(eyeBlock.data(),source,size);
        stereo::EyeRows blockRows;
        stereo::project_uniform_block(eyeBlock.data(),size,eyeOffset,stereo::frame.convergence,nullptr,
                                      replayOut ? &blockRows : nullptr,0,stereo::draw_frustum(eyeOffset,stereoEye()));
        if (fov_diag::kOn) [[unlikely]] fov_diag::note(eyeOffset, eyeBlock.data(), size);
        uniform(m.uniformBuffersBindingPoint[i],eyeBlock.data(),size,size_t(i));
        eyeUniform(m.uniformBuffersBindingPoint[i],eyeBlock.data(),size,blockRows);
      } else if (submitted) {
        struct Uploaded { uint32_t size; UploadSlice slice; };
        // (per thread: each draw thread uploads into its own arena)
        static thread_local std::unordered_map<uint32_t, Uploaded> uploaded;
        static thread_local uint64_t uploadedFor = 0, uploadedGeneration = 0;
        static thread_local VkDevice uploadedDevice = VK_NULL_HANDLE;
        if (uploadedFor != gx2::submitted_uniforms_id() || uploadedGeneration != R.submissionGeneration ||
            uploadedDevice != R.device) {
          uploaded.clear();
          uploadedFor = gx2::submitted_uniforms_id();
          uploadedGeneration = R.submissionGeneration;
          uploadedDevice = R.device;
        }
        auto it = uploaded.find(addr);
        if (it != uploaded.end() && it->second.size == size)
          uniform(m.uniformBuffersBindingPoint[i], submitted, size, size_t(i), 0, &it->second.slice);
        else {
          uniform(m.uniformBuffersBindingPoint[i], submitted, size, size_t(i));
          if (lastUpload.buffer) uploaded[addr] = {size, lastUpload};
        }
      } else uniform(m.uniformBuffersBindingPoint[i], addr ? mem::ptr(addr) : nullptr,
              size, size_t(i), addr);
      if (addr) rprof::guest_read(rprof::kUpUbo, addr, size);
    }
  rprof::sub_end(rprof::kSubUniformBlocks, blocksStarted);
  rprof::mark(rprof::kUniforms);
  uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS
                                : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
  auto bindTexture = [&](uint32_t unit, int binding, Surface* s, VkImageView view, VkSampler samplerValue) {
    overlap::used(s);  // (atlas_overlap.h: a texture the atlas's thread samples)
    // (diagnostics, WWHD_VK_ATLAS_TIMELINE=1) the textures the sun atlas's draws sample, and whether the scene samples
    // them too (docs/VR_ATLAS_THREAD.md: what a thread of the atlas's own would share with the render thread)
    if (stereo_timeline_on())  // (the depth target is the sun atlas: sun_atlas_target, below)
      stereo_timeline_texture(s, r[mmDB_HTILE_DATA_BASE] == ((1024u << 16) | 1024u) && r[gx2::kDepthSlicesReg] == 3);
    int scaleOffset = sh->uniforms.offset_texScale[unit];
    if (scaleOffset >= 0 && size_t(scaleOffset) + 8 <= supportUniforms.size()) {
      const float scale[] = {s->sx, s->sy};
      memcpy(supportUniforms.data() + scaleOffset, scale, sizeof scale);
    }
    // hand-drawn mip levels where the shader applies the bias (MoltenVK): the resolution-aware part on top
    const int biasOffset = sh->uniforms.offset_texLodBias[unit];
    if (s->artisticMips && !R.samplerMipLodBias && biasOffset >= 0 && size_t(biasOffset) + 4 <= supportUniforms.size()) {
      float bias;
      memcpy(&bias, supportUniforms.data() + biasOffset, 4);
      bias += g_artisticBias.bias;
      memcpy(supportUniforms.data() + biasOffset, &bias, 4);
    }
    // the half-size light buffer's grid on an upscaled buffer (gfx/area_sample.h)
    if ((sh->gatherUnits >> unit & 1) && sh->uniforms.offset_remapped >= 0 &&
        size_t(sh->uniforms.offset_remapped) < supportUniforms.size())
      ::gfx::area_sample::scale_gather_grid(reinterpret_cast<float*>(supportUniforms.data() + sh->uniforms.offset_remapped),
                                            (supportUniforms.size() - sh->uniforms.offset_remapped) / 16, s->width, s->height,
                                            s->sx, s->sy);
    if (imageCount >= imageInfos.size())
      exception_report::raise("stage image descriptor capacity exceeded");
    auto &info = imageInfos[imageCount++];
    info = {samplerValue, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto &write = appendWrite(rankPlan.textures[unit]);
    write.dstBinding = binding;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
  };
  if (reuse) {
    // continued draw: the previous draw's textures (upload_surface already ran for this draw)
    for (uint32_t i = 0; i < reuse->count; ++i) {
      const auto& t = reuse->textures[i];
      transition_image(t.surface, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       narrow_barriers()
                           ? (sh->vertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                         : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                           : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT),
                       VK_ACCESS_SHADER_READ_BIT);
      bindTexture(t.unit, t.binding, t.surface, t.view, t.sampler);
      replayTexture(t.surface, t.unit, t.binding, false, false);  // continued: neither feedback nor AO
    }
  } else
  for (int i = 0; i < sh->dec->textureUnitListCount; i++) {
    uint32_t unit = sh->dec->textureUnitList[i];
    if (unit >= LATTE_NUM_MAX_TEX_UNITS)
      exception_report::raise("sampled texture unit exceeds stage capacity");
    int binding = m.textureUnitToBindingPoint[unit];
    if (binding < 0)
      continue;
    bool substituted = false;
    Surface* guest = nullptr;  // (a texture pack's replacement bound: the guest texture, for continued draws' uploads)
    Surface* s = resolve_texture(r, sh, unit, colors, depth, &substituted, &guest);
    // the point-light passes read the buffer's depth copy on unit 0: they claim the buffer's size for it,
    // which it gets from the next time it is drawn (target_scale)
    if (sh->kind == gfx::ProgramKind::PointLight && unit == 0 && s && s->gpuWritten && !substituted)
      s->sizeClaim.request(gfx::size_request(sh->kind), R.frame);
    bool aliases = aliases_target(s, colors, depth);
    int scaleOffset = sh->uniforms.offset_texScale[unit];
    if (scaleOffset >= 0 && size_t(scaleOffset) + 8 <= supportUniforms.size()) {
      const float scale[] = {s->sx, s->sy};
      memcpy(supportUniforms.data() + scaleOffset, scale, sizeof scale);
    }
    VkImageView sampledView =
        aliases ? feedback_view(s, r + texbase + unit * 7, sh->vertex, unit, feedbackProbe)
                : sampled_texture_view(s, r + texbase + unit * 7);
    if (!aliases)
      transition_image(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       narrow_barriers()
                           ? (sh->vertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                         : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                           : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT),
                       VK_ACCESS_SHADER_READ_BIT);
    VkSampler samplerValue = resolve_sampler(r, sh, unit, s, guest);
    bindTexture(unit, binding, s, sampledView, samplerValue);
    replayTexture(s, unit, binding, substituted, aliases);
    if (record) {
      if (aliases || substituted || shares_target_address(s, colors, depth)) record->continuable = false;
      record->add({unit, binding, s, sampledView, samplerValue, guest ? guest : s});
    }
  }
  rprof::mark(rprof::kTextures);
  if (batchSupport) {
    // each piece: piece 0's final bytes, with its own values of the batched blocks' remapped entries, copied as
    // pack_uniforms_into copies them (eye-shifted for this eye). The rest is the same in every piece: registers and
    // textures (the vertex stage's changes in place that could differ are refused, batch_accept)
    const uint32_t stride = batch->layout->supportBytes;
    const int remapped = sh->uniforms.offset_remapped;
    const float eye = stereo::layout_draw_depth() ? 0.0f : eyeOffset;  // (as pack_uniforms_into)
    PER_THREAD_STATIC(pieces, std::vector<uint8_t>);
    pieces.resize(size_t(batch->count) * stride);
    // the eye-shifted rows (the camera's projection): the same in every piece, or eye 1 packs its own pieces
    bool sameRows = !supportRows.overflow;
    stereo::EyeRows rows0;
    auto sameAs0 = [&](const stereo::EyeRows& rows) {
      if (rows.overflow || rows.count != rows0.count) return false;
      for (int j = 0; j < rows.count; ++j) {
        const auto &a = rows.rows[j], &b0 = rows0.rows[j];
        if (a.offset != b0.offset || std::memcmp(&a.p0, &b0.p0, 4) || std::memcmp(&a.p2, &b0.p2, 4) ||
            std::memcmp(&a.p3, &b0.p3, 4) || std::memcmp(&a.p5, &b0.p5, 4) || a.yOffset != b0.yOffset)
          return false;
      }
      return true;
    };
    for (uint32_t k = 0; k < batch->count; ++k) {
      uint8_t* out = pieces.data() + size_t(k) * stride;
      const size_t base = std::min<size_t>(stride, supportUniforms.size());
      std::memcpy(out, supportUniforms.data(), base);
      std::memset(out + base, 0, stride - base);
      stereo::EyeRows rows;
      const uint32_t* addrs = batch->run->addrs[batch->first + k];
      for (const auto& group : sh->dec->list_remappedUniformEntries_bufferGroups) {
        const uint32_t i = group.kcacheBankIdOffset / 4 / 7;
        if (i >= 16 || !(batch->run->blocks >> i & 1)) continue;
        const uint32_t address = addrs[i];
        if (!address) continue;  // (refused: batch_accept)
        const size_t blockSize =
            std::min<uint32_t>(r[mmSQ_VTX_UNIFORM_BLOCK_START + group.kcacheBankIdOffset / 4 + 1] + 1, 0x10000);
        const uint8_t* submitted = gx2::submitted_uniform_block(address, uint32_t(blockSize));
        auto source = [&](const auto& entry) {
          return submitted && entry.indexOffset + 16 <= blockSize ? submitted + entry.indexOffset
                                                                  : ppc_ptr(address + entry.indexOffset);
        };
        for (const auto& entry : group.entries) {
          const int at = remapped + entry.mappedIndexOffset;
          if (at < 0 || size_t(at) + 16 > stride) continue;
          std::memcpy(out + at, source(entry), 16);
        }
        // (after the copies: a camera projection's x row, and its y row for the eye's own field of view)
        const stereo::EyeFrustum frustum = stereo::draw_frustum(eye, stereoEye());
        for (const auto& entry : group.entries) {
          const int at = remapped + entry.mappedIndexOffset;
          if (at < 0 || size_t(at) + 16 > stride) continue;
          if ((eye != 0 || !frustum.identity()) && entry.indexOffset < blockSize && blockSize - entry.indexOffset >= 64) {
            const uint8_t* src = source(entry);
            float m4[16];
            std::memcpy(m4, src, 64);
            const float unshifted[4] = {m4[0], m4[1], m4[2], m4[3]};
            const float unshiftedY = m4[5];
            if (stereo::project_eye(m4, eye, stereo::frame.convergence, frustum)) {
              std::memcpy(out + at, m4, 16);
              int yAt = -1;
              for (const auto& e : group.entries)
                if (e.indexOffset == entry.indexOffset + 16) yAt = remapped + int(e.mappedIndexOffset);
              if (yAt >= 0 && size_t(yAt) + 16 > stride) yAt = -1;
              if (!frustum.identity() && yAt >= 0) std::memcpy(out + yAt, m4 + 4, 16);
              rows.add(size_t(at), unshifted, unshiftedY, yAt >= 0 ? unsigned(yAt) : stereo::EyeRows::kNoRow);
              if (eye == 0) {
                rows.layout = true;  // (a layout in an eye's picture)
                if (fov_diag::kOn) [[unlikely]] fov_diag::layout_patched(2);
              }
            }
          }
        }
      }
      if (k == 0) rows0 = rows;
      else if (sameRows) sameRows = sameAs0(rows);
    }
    uniform(m.uniformVarsBufferBindingPoint, pieces.data(), pieces.size(), 16);
    stereo::EyeRows rows = supportRows;
    rows.repeat = batch->count;
    rows.stride = stride;
    if (!sameRows) rows.count = 1, rows.overflow = true;
    eyeUniform(m.uniformVarsBufferBindingPoint, pieces.data(), pieces.size(), rows);
  } else if (m.uniformVarsBufferBindingPoint >= 0) {
    const uint64_t uploadStarted = rprof::sub_begin();
    uniform(m.uniformVarsBufferBindingPoint, supportUniforms.data(),
            supportUniforms.size(), 16);
    rprof::sub_end(rprof::kSubUniformSupportUpload, uploadStarted);
    eyeUniform(m.uniformVarsBufferBindingPoint, supportUniforms.data(), supportUniforms.size(), supportRows);
  }
  rprof::mark(rprof::kUniforms);
  struct DescriptorMark { ~DescriptorMark() { rprof::mark(rprof::kDescriptors); } } descriptorMark;
  if (useRanks) {
    uint32_t dense = 0;
    for (uint32_t rank = 0; rank < rankPlan.count; ++rank) {
      if (!occupied[rank]) continue;
      const auto type = writes[rank].descriptorType;
      writes[dense++] = writes[rank];
      if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
        dynamicBindings[dynamicCount++] = {writes[dense-1].dstBinding, rankedOffsets[rank]};
    }
  } else {
    std::sort(writes.begin(), writes.begin() + writeCount,
        [](const auto &a, const auto &b) { return a.dstBinding < b.dstBinding; });
    std::sort(dynamicBindings.begin(), dynamicBindings.begin() + dynamicCount);
  }
  out.dynamicOffsetCount = dynamicCount;
  for (uint32_t i = 0; i < dynamicCount; ++i)
    out.dynamicOffsets[i] = dynamicBindings[i].second;
  if (replayOut && replayable) {
    replayOut->valid = true;
    for (uint32_t u = 0; u < replayOut->uniformCount; ++u) {
      uint32_t i = 0;
      while (i < dynamicCount && dynamicBindings[i].first != uint32_t(eyeUniformBindings[u])) ++i;
      if (i == dynamicCount) replayOut->valid = false;
      else replayOut->uniforms[u].dynamicIndex = i;
    }
    // eye 1 resolves its own image of the other textures and writes its own set with them; and a multiview draw writes
    // a set of both eyes' from them (stage M4)
    // (and the motion pass: its packets write the vertex stage's set again, docs/VR_DLSS.md stage D2b 2d)
    if (replayOut->eyeTextureCount || (multiview_draws() && replayOut->uniformCount) || motion_pass::on()) {
      replayOut->layout = layout;
      replayOut->writeCount = writeCount;
      for (uint32_t i = 0; i < writeCount; ++i) {
        auto& w = replayOut->writes[i];
        w = writes[i];
        if (w.pBufferInfo) replayOut->bufferInfos[i] = *w.pBufferInfo;
        if (w.pImageInfo) replayOut->imageInfos[i] = *w.pImageInfo;
        w.pBufferInfo = nullptr;
        w.pImageInfo = nullptr;
      }
      for (uint32_t e = 0; e < replayOut->eyeTextureCount; ++e) {
        auto& t = replayOut->eyeTextures[e];
        uint32_t i = 0;
        while (i < writeCount && (writes[i].dstBinding != uint32_t(t.binding) || !writes[i].pImageInfo)) ++i;
        if (i == writeCount) replayOut->valid = false;
        else t.write = i;
      }
    }
  }
  out.set = stage_descriptor_set(sh->vertex, layout, writes.data(), writeCount);
  return out;
}
// (issue #1) The render thread's sets kept across submissions: the same layout and writes (buffers, image views,
// samplers; the uniforms' offsets are dynamic) bind the same set again, from pools of its own, instead of a set written
// anew in every submission (about four a frame: the driver's descriptor updates were 4.7 % of the render thread in the
// headset). They are dropped when a buffer or image view a set may name could have been destroyed
// (descriptor_targets_generation) or the device changed, and at 65536 sets; their pools are destroyed once the
// recording submission's GPU work is done. WWHD_VK_KEEP_DESCRIPTORS=0: written in every submission, as before
// (docs/VR_SCENE_THREADS.md, "the drawing threads' descriptor sets") the scene's drawing threads keep theirs the same way,
// each in pools of its own; a pool they drop goes to the render thread, which destroys it as it does its own
namespace {
std::mutex g_droppedPoolsMutex;
std::vector<std::pair<VkDevice, VkDescriptorPool>> g_droppedPools;  // (the drawing threads', for the render thread)
std::atomic<bool> g_droppedAny{false};
struct KeptDescriptors {
  VkDevice device = VK_NULL_HANDLE;
  uint64_t generation = ~uint64_t{0};
  uint64_t bufferGeneration = 0;  // (destroyed_buffers_generation as last checked)
  std::vector<VkDescriptorPool> pools;
  SubmissionDescriptorCache cache;
  std::vector<VkBuffer> destroyed;  // (scratch)
  uint32_t sets = 0;
  uint64_t hits = 0, made = 0, drops = 0, frame0 = 0;
  void drop() {
    if (!pools.empty() && device == R.device && draw_threads::on_draw_thread()) {
      std::lock_guard<std::mutex> lock(g_droppedPoolsMutex);
      for (auto pool : pools) g_droppedPools.emplace_back(device, pool);
      g_droppedAny.store(true, std::memory_order_release);
    } else if (!pools.empty() && device == R.device) {
      // (any submission still to complete may bind them: the recording one completes after all of those)
      R.completions.push_back([d = device, old = pools] {
        for (auto pool : old) vkDestroyDescriptorPool(d, pool, nullptr);
      });
    }
    pools.clear();
    cache.clear();
    sets = 0;
    ++drops;
  }
  VkDescriptorSet allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!pools.empty()) {
      ai.descriptorPool = pools.back();
      if (vkAllocateDescriptorSets(R.device, &ai, &set) == VK_SUCCESS) return set;
    }
    // a new pool (the last one is full)
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 4096;
    dp.poolSizeCount = uint32_t(std::size(sizes));
    dp.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(R.device, &dp, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    pools.push_back(pool);
    ai.descriptorPool = pool;
    if (vkAllocateDescriptorSets(R.device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    return set;
  }
};
KeptDescriptors g_keptDescriptors;  // (render thread)
struct keptDescriptors_per_thread_tag;  // (the scene's drawing threads: per_thread::get)
bool keep_descriptors_on() {
  static const bool on = [] {
    const char* e = std::getenv("WWHD_VK_KEEP_DESCRIPTORS");
    const bool v = !(e && !std::strcmp(e, "0"));
    LOG("[vulkan] descriptor sets %s", v ? "kept across submissions (WWHD_VK_KEEP_DESCRIPTORS=0: written in each)"
                                         : "written in each submission");
    return v;
  }();
  return on;
}
}  // namespace
// The stage's descriptor set for these writes: kept across submissions on the render thread (above), reused within
// the submission elsewhere, else written.
VkDescriptorSet stage_descriptor_set(bool vertex, VkDescriptorSetLayout layout,
                                     VkWriteDescriptorSet* writes, uint32_t writeCount) {
  thread_check::note(thread_check::Area::Descriptors);  // (diagnostics: thread_check.h)
  VkDescriptorSet set{};
  // Resource preparation above must always run, even when the descriptor set
  // itself is reusable. Pool reset/slot activation invalidates every old set.
  // (per thread, stage 2.5: each thread's sets come from its own pool; both pools reset with the submission)
  static thread_local uint64_t cacheGeneration = ~uint64_t{0};
  PER_THREAD_STATIC(descriptorCache, SubmissionDescriptorCache);
  PER_THREAD_STATIC(lastDescriptors, std::array<LastDescriptorSet, 2>);
  if (cacheGeneration != R.submissionGeneration) {
    descriptorCache.clear();
    lastDescriptors[0] = {}; lastDescriptors[1] = {};
    cacheGeneration = R.submissionGeneration;
  }
  ++draw_counters().descriptorLookups;
  auto &last = lastDescriptors[vertex ? 0 : 1];
  if (descriptor_matches(last, layout, writes, writeCount)) {
    ++draw_counters().descriptorCacheHits;
    ++draw_counters().descriptorFastHits;
    return last.set;
  }
  const uint64_t hash = descriptor_hash(layout, writes, writeCount);
  const bool drawThread = draw_threads::on_draw_thread();
  if (keep_descriptors_on() && (drawThread ? scene_thread::on_scene_thread() : descriptor_pool() == R.descriptorPool)) {
    auto& k = drawThread ? per_thread::get<keptDescriptors_per_thread_tag, KeptDescriptors>() : g_keptDescriptors;
    if (!drawThread && g_droppedAny.load(std::memory_order_acquire)) [[unlikely]] {  // (the drawing threads' dropped pools: above)
      std::lock_guard<std::mutex> lock(g_droppedPoolsMutex);
      R.completions.push_back([old = std::move(g_droppedPools)] {
        for (auto [d, pool] : old) vkDestroyDescriptorPool(d, pool, nullptr);
      });
      g_droppedPools.clear();
      g_droppedAny.store(false, std::memory_order_relaxed);
    }
    const uint64_t generation = descriptor_targets_generation();
    const uint64_t bufferGeneration = destroyed_buffers_generation();
    if (k.device != R.device || k.generation != generation || k.sets >= 65536) {
      k.drop();
      k.device = R.device;
      k.generation = generation;
      k.bufferGeneration = bufferGeneration;
    } else if (k.bufferGeneration != bufferGeneration) {
      // buffers destroyed (no images): dropped only if a kept set names one (a handle may be made again for a new buffer)
      k.destroyed.clear();
      const bool known = destroyed_buffers_since(k.bufferGeneration, k.destroyed);
      std::sort(k.destroyed.begin(), k.destroyed.end());
      if (!known || k.cache.names_any(k.destroyed)) k.drop();
      k.bufferGeneration = bufferGeneration;
    }
    if (R.frame >= k.frame0 + 600) {
      const double f = k.frame0 ? double(R.frame - k.frame0) : 600.0;
      LOG("[vulkan] kept descriptor sets per frame%s: %.0f reused, %.1f written, %.2f drops; %u kept",
          drawThread ? (scene_thread::on_scene_b() ? " (scene thread B)" : " (scene thread A)") : "", k.hits / f,
          k.made / f, k.drops / f, k.sets);
      k.hits = k.made = k.drops = 0;
      k.frame0 = R.frame;
    }
    if (auto found = k.cache.find(hash, layout, writes, writeCount)) {
      ++k.hits;
      ++draw_counters().descriptorCacheHits;
      remember_descriptors(last, layout, found, writes, writeCount);
      return found;
    }
    if (VkDescriptorSet kept = k.allocate(layout)) {
      ++k.made;
      ++k.sets;
      ++draw_counters().descriptorAllocations;
      for (uint32_t i = 0; i < writeCount; ++i) writes[i].dstSet = kept;
      if (writeCount) vkUpdateDescriptorSets(R.device, writeCount, writes, 0, nullptr);
      k.cache.insert(hash, layout, kept, writes, writeCount);
      remember_descriptors(last, layout, kept, writes, writeCount);
      return kept;
    }
    // (no pool memory: this submission's pool, as before)
  }
  if (auto found = descriptorCache.find(hash, layout, writes, writeCount)) {
    ++draw_counters().descriptorCacheHits;
    remember_descriptors(last, layout, found, writes, writeCount);
    return found;
  }
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = descriptor_pool();  // this thread's (stage 2.5)
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  vk_check(vkAllocateDescriptorSets(R.device, &ai, &set), "descriptor set");
  ++draw_counters().descriptorAllocations;
  for (uint32_t i = 0; i < writeCount; ++i) writes[i].dstSet = set;
  if (writeCount)
    vkUpdateDescriptorSets(R.device, writeCount, writes, 0, nullptr);
  descriptorCache.insert(hash, layout, set, writes, writeCount);
  remember_descriptors(last, layout, set, writes, writeCount);
  return set;
}
// Eye 1's stage from eye 0's (StageReplay); false: eye 1 binds the stage itself.
bool replay_stage(const StageReplay& in, const uint32_t* r, vk::Shader* sh, float eyeOffset,
                  const std::array<Surface*, 8>& colors, const Surface* depth, StageResources& out) {
  out = in.resources;
  for (uint32_t i = 0; i < in.uniformCount; ++i) {
    const auto& u = in.uniforms[i];
    PER_THREAD_STATIC(scratch, std::vector<uint8_t>);
    scratch.assign(u.bytes.begin(), u.bytes.end());
    // (eye 1 replays eye 0's stage: a layout's rows take eye 1's frustum, on whichever thread this runs)
    stereo::apply_eye_rows(scratch.data(), u.rows, eyeOffset, stereo::frame.convergence, stereo::rows_frustum(u.rows, eyeOffset, 1));
    if (fov_diag::kOn && u.rows.layout && eyeOffset == 0) [[unlikely]] fov_diag::layout_replay(1);
    if (fov_diag::kOn) [[unlikely]] fov_diag::note(eyeOffset, scratch.data(), scratch.size());
    rprof::UploadKind uploads(rprof::kUpUbo);
    UploadSlice b;
    if (u.reserved.buffer && u.reserved.size >= scratch.size()) {
      // (stage E5) the slot eye 0 reserved: filled as snapshot fills its slices
      b = u.reserved;
      std::memcpy(b.mapped, scratch.data(), scratch.size());
      if (b.size > scratch.size()) std::memset(static_cast<uint8_t*>(b.mapped) + scratch.size(), 0, b.size - scratch.size());
    } else
      b = snapshot(scratch.data(), scratch.size(), R.properties.limits.minUniformBufferOffsetAlignment);
    if (b.buffer != u.buffer || b.offset > UINT32_MAX) return false;  // the set names another buffer
    out.dynamicOffsets[u.dynamicIndex] = uint32_t(b.offset);
  }
  if (in.eyeTextureCount) {
    std::array<VkWriteDescriptorSet, 17 + LATTE_NUM_MAX_TEX_UNITS> writes;
    std::array<VkDescriptorImageInfo, 17 + LATTE_NUM_MAX_TEX_UNITS> images;
    for (uint32_t i = 0; i < in.writeCount; ++i) {
      writes[i] = in.writes[i];
      if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        images[i] = in.imageInfos[i];
        writes[i].pImageInfo = &images[i];
      } else writes[i].pBufferInfo = &in.bufferInfos[i];
    }
    const uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    for (uint32_t e = 0; e < in.eyeTextureCount; ++e) {
      const auto& t = in.eyeTextures[e];
      bool substituted = false;
      Surface* guest = nullptr;
      Surface* s = resolve_texture(r, sh, t.unit, colors, depth, &substituted, &guest);
      // the texture scale in eye 0's uniforms
      if (substituted || aliases_target(s, colors, depth) != t.feedback || s->sx != t.sx || s->sy != t.sy) return false;
      if (t.feedback) {  // (as bind_stage: eye 1's copy of its own layer, in its own slots)
        images[t.write] = {resolve_sampler(r, sh, t.unit, s, guest),
                           feedback_view(s, r + texbase + t.unit * 7, sh->vertex, t.unit, nullptr),
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        continue;
      }
      transition_image(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       narrow_barriers()
                           ? (sh->vertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                           : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT),
                       VK_ACCESS_SHADER_READ_BIT);
      images[t.write] = {resolve_sampler(r, sh, t.unit, s, guest), sampled_texture_view(s, r + texbase + t.unit * 7),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    out.set = stage_descriptor_set(sh->vertex, in.layout, writes.data(), in.writeCount);
  }
  for (uint32_t i = 0; i < in.textureCount; ++i) stereo_hold_texture(in.textures[i]);
  return true;
}
// WWHD_VK_FASTPATH_VERIFY: the full resolution of a continued draw, compared with the reused state
bool verify_continued(const uint32_t* r, uint32_t prim) {
  const auto& c = continuedEyes()[stereoEye()];
  int kind = -1;
  char detail[200] = "";
  uint64_t fsKey = 0;
  auto* fs = vk::get_fetch_shader(r, &fsKey, R.frame);
  vk::Shader* vs = fs ? vk::translate(r, true, fs, fsKey, R.frame, gx2::shader_state_gen()) : nullptr;
  vk::Shader* ps = fs ? vk::translate(r, false, fs, fsKey, R.frame, gx2::shader_state_gen(),
                                     vs && vs->ready() ? vs : nullptr) : nullptr;
  DrawTargets t;
  if (fs != c.fs || fsKey != c.fsKey) kind = 0;
  else if (vs != c.vs || !vs || !vs->ready()) kind = 1;
  else if (ps != c.ps || !ps || !ps->ready()) kind = 2;
  else if (primitive_topology(prim) != c.topology) kind = 7;
  else if (!resolve_targets(r, ps, t) || !t.same(c.targets)) kind = 3;
  else if (&pipeline(r, vs, ps, fs, c.topology, t.colors, t.depth) != c.pipeline) kind = 4;
  else
    for (int stage = 0; stage < 2 && kind < 0; ++stage) {
      vk::Shader* sh = stage ? ps : vs;
      const auto& cs = c.stages[stage];
      const uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
      uint32_t n = 0;
      for (int i = 0; i < sh->dec->textureUnitListCount && kind < 0; i++) {
        const uint32_t unit = sh->dec->textureUnitList[i];
        if (unit >= LATTE_NUM_MAX_TEX_UNITS) { kind = 6; break; }
        const int binding = sh->mapping.textureUnitToBindingPoint[unit];
        if (binding < 0) continue;
        if (n >= cs.count) { kind = 5; break; }
        const auto& e = cs.textures[n++];
        Surface* guest = nullptr;
        Surface* s = resolve_texture(r, sh, unit, t.colors, t.depth, nullptr, &guest);
        const bool aliases = aliases_target(s, t.colors, t.depth);
        VkImageView view = aliases ? VK_NULL_HANDLE : sampled_texture_view(s, r + texbase + unit * 7);
        VkSampler smp = resolve_sampler(r, sh, unit, s, guest);
        if (e.unit != unit || e.binding != binding || e.surface != s || aliases || view != e.view || smp != e.sampler) {
          kind = 6;
          std::snprintf(detail, sizeof detail, " (%s unit %u: surface %08X%s/%08X, view %s, sampler %s%s)",
                        stage ? "ps" : "vs", unit, s->addr, e.surface == s ? "" : " (cached differs)",
                        e.surface ? e.surface->addr : 0, view == e.view ? "same" : "differs",
                        smp == e.sampler ? "same" : "differs", aliases ? ", aliases a target" : "");
        }
      }
      if (kind < 0 && n != cs.count) kind = 5;
    }
  if (kind < 0) return true;
  auto& st = continuedStats();
  ++st.mismatches;
  ++st.totalMismatches;
  ++st.mismatchKinds[kind];
  static thread_local int logged = 0;
  if (logged < 50) {
    ++logged;
    LOG("[vulkan fastpath] verify mismatch #%llu: %s%s; frame %llu draw %llu prim %u vs %08X ps %08X fs %08X",
        (unsigned long long)st.totalMismatches, mismatchKindNames[kind], detail, (unsigned long long)R.frame,
        (unsigned long long)draw_counters().drawCount, prim, r[mmSQ_PGM_START_VS] << 8, r[mmSQ_PGM_START_PS] << 8,
        r[mmSQ_PGM_START_FS] << 8);
  }
  return false;
}
ContinuedReason continued_reason(const uint32_t* r, uint32_t prim) {
  auto& c = continuedEyes()[stereoEye()];
  if (!c.valid) return c.invalidReason;
  if (c.frame != R.frame) return kNewFrame;
  if (c.partEpoch != part_epoch()) return kNoState;  // (docs/VR_SCENE_THREADS.md, stages S1, S6)
  if (c.stateVersion != gx2::full_state_version()) return kRegisters;
  if (c.surfaceEpoch != g_surface_epoch) return kSurfaces;
  if (c.prim != prim) return kPrimitive;
  if (c.aoMode != ao_mode() || c.aoHires != ao_hires_enabled() || c.aniso != aniso_enabled()) return kSettings;
  // (upstream #399) the scissor is a continued (dynamic) word, but GetActiveColorBufferMask drops the colour buffers
  // smaller than it: a new scissor continues only while it leaves the same targets active
  if (r[REGADDR::PA_SC_GENERIC_SCISSOR_BR] != c.scissorBR) {
    if (LatteMRT::GetActiveColorBufferMask(c.ps->dec, *reinterpret_cast<const LatteContextRegister*>(r)) !=
        c.targets.activeColors)
      return kScissorTargets;
    c.scissorBR = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
  }
  return kContinued;
}
} // namespace
// (renderer smoke test, smoke.cpp texture_pack_check) a draw's texture and sampler resolution as bind_stage makes it,
// and the create info of the sampler the cache made last
Surface* resolve_texture_for_test(const uint32_t* r, vk::Shader* sh, uint32_t unit, const std::array<Surface*, 8>& colors,
                                  const Surface* depth, bool* substituted, Surface** guest) {
  return resolve_texture(r, sh, unit, colors, depth, substituted, guest);
}
VkSampler resolve_sampler_for_test(const uint32_t* r, vk::Shader* sh, uint32_t unit, const Surface* s, const Surface* guest) {
  return resolve_sampler(r, sh, unit, s, guest);
}
VkSamplerCreateInfo& last_sampler_create_info_for_test() { return g_lastSamplerCreateInfo; }
// Sizing (WWHD_VK_MEMORY_LOG_FRAMES): the game's pipelines (never freed), those still compiling (each holds its two
// VkShaderModules until done; a finished pipeline keeps none) and the failed ones.
void guest_pipeline_counts(size_t& total, size_t& compiling, size_t& failed) {
  total = pipelines.size(); compiling = failed = 0;
  for (const auto& [key, p] : pipelines) {
    if (p.pendingCompile) ++compiling;
    else if (!p.pipeline) ++failed;
  }
}
VkDescriptorSet texture_stage_smoke_bind(const uint32_t* registers,
    VkDescriptorSetLayout layout, uint32_t textureCount) {
  if (textureCount > LATTE_NUM_MAX_TEX_UNITS)
    exception_report::raise("invalid smoke texture count");
  LatteDecompilerShader decoded(LatteConst::ShaderType::Pixel);
  vk::Shader shader;
  shader.dec = &decoded;
  decoded.textureUnitListCount = uint8_t(textureCount);
  for (uint32_t i = 0; i < textureCount; ++i) {
    decoded.textureUnitList[i] = uint8_t(i);
    decoded.textureUnitSamplerAssignment[i] = uint16_t(i);
    shader.mapping.textureUnitToBindingPoint[i] = int8_t(i);
  }
#ifdef WWHD_MOBILE_GPU_PATHS
  return bind_stage(registers, &shader, layout, false, 0u, 1, 1, {}, nullptr, nullptr).set;
#else
  return bind_stage(registers, &shader, layout, false, 1, 1, {}, nullptr, nullptr).set;
#endif
}
void reset_feedback_images() { reset_feedback_scratch(); }
UploadSlice vertex_window_smoke_snapshot(uint32_t binding,uint32_t address,
    uint32_t reservation,uint32_t windowOffset,uint32_t windowLength,
    const void* data,bool poisonUnused) {
  if(!data || !windowLength || windowOffset>reservation ||
     windowLength>reservation-windowOffset)
    exception_report::raise("invalid vertex window smoke bounds");
  return vertex_window_snapshot(binding,address,reservation,windowOffset,
                                windowLength,data,poisonUnused);
}

void stereo_release_ao() {
  auto& ao = aoPrivateEyes[1];
  destroy_surface_image(&ao.color);
  destroy_surface_image(&ao.depth);
  ao = {};
}
void reset_ao_private_cache() {
  for (auto& ao : aoPrivateEyes) {
    ao.frame = ~0ull;
    ao.source = 0;
  }
}
void drain_pending_guest_pipelines() {
  for (auto& [key, value] : pipelines) resolve_pending_pipeline(value);
}
void reset_pipeline_lookup_cache() {
  lastPipelineLookup() = {}; pipelineLookaside() = {}; samplerMemo() = {}; drawBatchState = {}; forget_continued();
  pipelineMemoGeneration.fetch_add(1, std::memory_order_acq_rel);  // (other threads' memos: at their next lookup)
}
uint64_t draw_batch_submissions() { return drawBatchSubmissions; }
// WWHD_VK_TRACE_PASSES=1 (issue #144): log each new render pass shape once, just before the driver
// sees it, so a crash inside vkCmdBeginRendering names the last pass in the crash report's log lines
static void trace_pass(const std::array<Surface *, 8> &colors, uint32_t ncolor, const Surface *depth,
                       uint32_t depthSlice, uint32_t width, uint32_t height) {
  static const bool on = [] { const char *e = getenv("WWHD_VK_TRACE_PASSES"); return e && *e == '1'; }();
  if (!on) return;
  static thread_local std::unordered_set<std::string> seen;
  std::string key = std::to_string(ncolor) + ":";
  for (uint32_t i = 0; i < ncolor; ++i)
    key += colors[i] ? std::to_string(colors[i]->fmt.pixel) + "/" + std::to_string(colors[i]->usage) + "," : "-,";
  if (depth)
    key += " d" + std::to_string(depth->fmt.pixel) + "/" + std::to_string(depth->usage) + "/" +
           std::to_string(depth->arrayLayers) + "/" + std::to_string(depth->mips) + "/" +
           std::to_string(depth->createFlags);
  if (!seen.insert(key).second) return;
  std::string text;
  for (uint32_t i = 0; i < ncolor; ++i)
    text += colors[i] ? " c" + std::to_string(i) + "=fmt" + std::to_string(colors[i]->fmt.pixel) + "/usage" +
                            std::to_string(colors[i]->usage)
                      : " c" + std::to_string(i) + "=none";
  if (depth)
    text += " depth=fmt" + std::to_string(depth->fmt.pixel) + "/usage" + std::to_string(depth->usage) +
            "/layers" + std::to_string(depth->arrayLayers) + "/slice" + std::to_string(depthSlice) + "/mips" +
            std::to_string(depth->mips) + "/flags" + std::to_string(depth->createFlags) +
            (depth->fmt.stencil ? "/stencil" : "");
  LOG("[vulkan pass] %ux%u, %u colour attachment(s)%s", width, height, ncolor, text.c_str());
}
// The depth target is the light-space sun atlas both eyes share (stereo_light_atlas).
static bool sun_atlas_target(const uint32_t* r) {
  return r[mmDB_HTILE_DATA_BASE]==((1024u<<16)|1024u) && r[gx2::kDepthSlicesReg]==3 &&
      LatteMRT::GetActiveDepthBufferMask(*reinterpret_cast<const LatteContextRegister*>(r));
}
// (stage 3.2) an atlas draw whose registers alone show it writes no colour (LatteMRT::GetActiveColorBufferMask without
// the pixel shader: colour writes disabled, or no colour target both unmasked and bound): view-independent, so the
// atlas's thread draws it and the render thread need not wait
static bool atlas_draw_writes_no_colour(const uint32_t* r) {
  if(((r[REGADDR::CB_COLOR_CONTROL]>>4)&7)==1) return true;  // SPECIAL_OP DISABLE
  const uint32_t mask=r[REGADDR::CB_TARGET_MASK];
  for(uint32_t i=0;i<8;++i)
    if(((mask>>(i*4))&0xF) && r[mmCB_COLOR0_BASE+i]) return false;
  return true;
}
// (diagnostics) the sun atlas's thread, logged every 600 frames while it is on (atlas_thread.h)
static void atlas_thread_log() {
  static uint64_t frame0=0;
  if(R.frame<frame0+600) return;
  frame0=R.frame;
  const auto s=atlas_thread::take_stats();
  LOG("[vulkan stereo] sun atlas thread, per frame: %.2f overlaps, %.0f draws (%.1f waited for), %.2f clears, %.2f "
      "guest flushes deferred, %.0f scene draws overlapped, %.1f textures made ready; the render thread waited %.2f ms "
      "at the joins",
      s.overlaps/600.0,s.draws/600.0,s.waited/600.0,s.clears/600.0,s.flushes/600.0,s.sceneDraws/600.0,
      s.prepared/600.0,s.joinMs/600.0);
  static const char* const names[]={"shaders","pipelines","samplers","surfaces","passes","buffers","other"};
  std::string waits;
  for(int i=0;i<7;++i) {
    auto& c=draw_threads::contention(i);
    const uint64_t n=c.waits.exchange(0),ns=c.ns.exchange(0),locks=c.locks.exchange(0);
    if(n) waits+=" "+std::string(names[i])+" "+std::to_string(n/600)+" ("+std::to_string(ns/600000)+" us)";
    if(locks) waits+=" ["+std::string(names[i])+" taken "+std::to_string(locks/600)+"]";
  }
  for(int i=0;i<24;++i) {
    const uint64_t n=draw_threads::g_lockSites[i].exchange(0);
    if(n) waits+=" {site "+std::to_string(i)+": "+std::to_string(n/600)+"}";
  }
  LOG("[vulkan stereo] sun atlas thread: lock waits per frame (both threads):%s",waits.empty()?" none":waits.c_str());
  // (WWHD_VK_LOCK_COUNT=1) the waits by site: where the thread waited, and where the holder had taken the lock
  if(draw_threads::kLockCount) {
    std::vector<std::pair<draw_threads::SiteWaits,std::pair<std::string,std::string>>> sites;
    {
      std::lock_guard<std::mutex> l(draw_threads::site_waits_mutex());
      for(auto& [k,w]:draw_threads::site_waits()) sites.push_back({w,k});
      draw_threads::site_waits().clear();
    }
    std::sort(sites.begin(),sites.end(),[](const auto& a,const auto& b){return a.first.ns>b.first.ns;});
    std::string top;
    for(size_t i=0;i<sites.size()&&i<12;++i)
      top+=" ["+sites[i].second.first+" waited for "+sites[i].second.second+": "+
           std::to_string(sites[i].first.waits/600)+" ("+std::to_string(sites[i].first.ns/600000)+" us)]";
    LOG("[vulkan stereo] lock waits per frame by site (waiter for holder, the 12 longest):%s",top.empty()?" none":top.c_str());
  }
}
// (diagnostics) the time of each eye's draws per frame, logged every 600 frames: eye 0's own (the shared sun atlas
// apart), the atlas, eye 1's, and the held eye 1 commands' flushes (stereo.cpp)
static void stereo_eye_time(int eye,std::chrono::steady_clock::duration d,bool sharedAtlas) {
  static double ms[3]={};
  static uint64_t frame0=0;
  ms[sharedAtlas?2:eye]+=std::chrono::duration<double,std::milli>(d).count();
  if(R.frame>=frame0+600) {
    LOG("[vulkan stereo] ms per frame: eye 0 draws %.2f, shared sun atlas %.2f, eye 1 draws %.2f, eye 1 flushes %.2f",
        ms[0]/600.0,ms[2]/600.0,ms[1]/600.0,stereo_flush_ms(true)/600.0);
    if(stereo_atlas_stream_enabled()) {
      uint64_t holds=0,held=0,why[int(AtlasEmit::Count)]={};
      stereo_atlas_stats(holds,held,why);
      LOG("[vulkan stereo] sun atlas held stream: %.2f holds per frame, %.0f atlas draws held per frame; emitted by: "
          "the next draw %.2f, a submission or the swap %.2f, a clear or copy %.2f, an eye switch %.2f, other %.2f, "
          "a scene draw that may sample it %.2f, an invalidate %.2f, the render thread out of commands %.2f",
          holds/600.0,held/600.0,why[0]/600.0,why[1]/600.0,why[2]/600.0,why[3]/600.0,why[4]/600.0,why[5]/600.0,
          why[6]/600.0,why[7]/600.0);
    }
    ms[0]=ms[1]=ms[2]=0;
    frame0=R.frame;
  }
}
// ---------------------------------------------------------------- multiview draws (docs/VR_MULTIVIEW.md, stage M4)
// Eye 0's draw drawn for both eyes at once, with view mask 0b11, into both eyes' layers of its targets (eye 1's are the
// second layers of eye 0's images, stage M2b), when eye 1 would only replay it: the same pipeline, vertices and
// raster state, the same descriptors except its eye-shifted uniform blocks (two descriptors each: view 0's copy, view
// 1's), and its targets the twins of eye 0's. Eye 1 then draws nothing. Anything else draws each eye as before.
struct MultiviewDraw {
  bool on = false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet sets[2]{};
  std::array<uint32_t, 2 * 21> offsets{};  // both stages' dynamic offsets (a per-view block: view 0's, then view 1's)
  uint32_t offsetCount = 0;
  std::array<Surface*, 8> twins{};
  Surface* depthTwin = nullptr;
};
// why draws were not drawn for both eyes (diagnostics, logged every 600 frames)
// (kMvState is split by its causes, for the Quest's logs: docs/VR_ANDROID.md, "Multiview falls back")
enum MultiviewWhy : int { kMvDrawn, kMvState, kMvStage, kMvEyeTextures, kMvTargets, kMvEyeTargets, kMvBuilding,
                          kMvFailed, kMvUpload, kMvNotDynamic, kMvNoBase, kMvCompiling, kMvPath, kMvAo, kMvAtlasDepth,
                          kMvEye1Held,
#ifdef WWHD_MOBILE_GPU_PATHS
                          kMvMixed, kMvSkipBuilding, kMvStateScene, kMvAtlasJoined,
#endif
                          kMvCount };
const char* const multiviewWhyNames[kMvCount] = {"drawn for both eyes", "not now (state)", "a stage eye 1 binds itself",
    "eye-specific textures", "targets not two-eye", "eye 1's targets differ", "pipeline building", "pipeline failed",
    "uniform copy elsewhere", "not now (uniform blocks over the device's dynamic limit)", "not now (no multiview base)",
    "not now (pipeline compiling)", "not now (eye, thread or held stream)", "not now (AO at full size)",
    "not now (light atlas depth)", "not now (eye 1 held its lookups)",
#ifdef WWHD_MOBILE_GPU_PATHS
    "not now (its targets were drawn one eye at a time recently: no mixed passes on a tiled GPU)",
    "left out while its two-eye pipeline is built (both eyes)",
    "not now (state, on the scene's thread)", "eye 1's held commands emitted by ending the atlas overlap",
#endif
};
#ifdef WWHD_MOBILE_GPU_PATHS
// (QUEST3_IMPLEMENTATION_ROADMAP.md, mixed passes) a two-eye image written one eye at a time recently (kMixHoldFrames) takes
// no multiview draws: a tiled GPU's passes then never write one image both ways in a frame (both layers in one pass, one
// layer in another: the black squares on Quest 3 came with that mix, docs/VR_ANDROID.md). WWHD_VK_MULTIVIEW_NO_MIX=0 off
const bool kMultiviewNoMix = [] {
  const char* e = std::getenv("WWHD_VK_MULTIVIEW_NO_MIX");
  return !(e && e[0] == '0');
}();
// (2026-11-04) how long a mark holds: it held this frame and the last, so an image written one eye at a time every frame
// (the sky's on a Quest 3: black squares along the horizon again) took multiview draws again two frames later and was
// written both ways in that frame, at its start, before its per-eye draw marked it again. Held for kMixHoldFrames
// (90: a second or more) since its last per-eye write, such an image stays per eye from the start of every frame: an
// image written both ways in one frame is then only the first frame it goes per eye, at most once per hold.
// WWHD_VK_MULTIVIEW_NO_MIX_FRAMES=n sets the hold (1: as before)
const uint64_t kMixHoldFrames = [] {
  const char* e = std::getenv("WWHD_VK_MULTIVIEW_NO_MIX_FRAMES");
  const long n = e && *e ? std::strtol(e, nullptr, 10) : 90;
  return uint64_t(n >= 1 && n <= 100000 ? n : 90);
}();
// (2026-11-07) the long hold only for an image written one eye at a time in most recent frames: in kMixRepeatMarks or
// more of the last 8 (the sky's, every frame); one marked only now and then (a Quest 3 log: the scene's depth, held for a
// draw on the scene's thread refused about once every few frames, "not now (state)", so 499-562 of its draws a frame were
// refused all session) is held per eye in its frame and the next only: the frames between have no per-eye draw into it
// to mix with. WWHD_VK_MULTIVIEW_NO_MIX_REPEAT=n (1..8) sets the count, =0 the long hold for every mark as before
const int kMixRepeatMarks = [] {
  const char* e = std::getenv("WWHD_VK_MULTIVIEW_NO_MIX_REPEAT");
  const int n = e && *e ? std::atoi(e) : 4;
  return n >= 0 && n <= 8 ? n : 4;
}();
bool written_per_eye(const Surface* s) {
  if (!s || !s->perEyeFrame) return false;
  const uint64_t since = R.frame - s->perEyeFrame;
  if (since <= 1) return true;  // (marked in this frame or the last: always)
  if (since > kMixHoldFrames) return false;
  return kMixRepeatMarks == 0 || std::popcount(unsigned(s->perEyeHistory)) >= kMixRepeatMarks;
}
// this thread's last multiview_draw refused for that reason alone: its draw does not mark the images again (else one
// fallback kept them per eye for good)
thread_local bool t_mvRefusedMixed = false;
#endif
uint64_t multiviewWhy[kMvCount] = {}, multiviewFlushes = 0, multiviewFeedbackCopies = 0;
// (diagnostics) the targets that kept draws from both eyes, by what they are: draws per target, counted on the target
// itself (one relaxed atomic add a missed draw: no formatting, no map, no lock on the drawing threads; it formatted a
// string and updated a map shared by the drawing threads for each, about 1-2 us a draw on a Quest, until 2026-11-07),
// listed every 10 s (multiview_log, take_multiview_target_stats)
thread_local uint8_t t_mvLastWhy = 255;  // (this thread's last multiview_draw refusal: what a per-eye draw held, below)
const char* const kMvMissWhat[7] = {"?", "colour, no eye 1 twin", "colour, one eye's image", "colour, layered",
                                    "depth, no eye 1 twin", "depth, one eye's image", "depth, layered"};
const char* const kProgramKindNames[8] = {"other", "depth downsample", "occlusion (pixel)", "occlusion (vertex)",
                                          "sun shadow", "bloom chain", "bloom helper", "point light"};
void multiview_target_miss(Surface* s, uint8_t what) {
  // (2026-11-05) on by default on a standalone headset: a Quest 3 log had multiview draws fall from 150-780 a frame to
  // 4-5 with 700-2,400 "targets not two-eye", and nothing said which targets (WWHD_VK_MULTIVIEW_STATS=0 turns it off)
  static const bool on = [] {
    if (const char* e = std::getenv("WWHD_VK_MULTIVIEW_STATS")) return *e != '0';
#ifdef __ANDROID__
    return quest_theater::standalone_headset();
#elif defined(WWHD_STEAM_FRAME_NATIVE) || (defined(__linux__) && defined(__aarch64__))
    return true;  // (Steam Frame - Native, 2026-11-07)
#else
    return false;
#endif
  }();
  if (!on) return;
  std::atomic_ref<uint32_t>(s->mvMissDraws).fetch_add(1, std::memory_order_relaxed);
  s->mvMissWhat = what;
}
#ifdef WWHD_MOBILE_GPU_PATHS
std::atomic<uint64_t> g_mixBarriers{0};  // (mix_barrier: the global barriers put before passes, listed every 10 s)
// (2026-11-08, the black squares in the sky again on a Quest 3 with the shorter per-eye hold) a pass into a two-eye image
// that switches between both eyes (multiview: both layers) and one layer at a time, this frame or the last, starts after
// a global memory barrier: each pass's own barriers (transition_image) name only its surface's layer, and Quest's GPU
// driver then let the passes on the two layers of one image overlap, losing blocks of it (wait_before_layer_clear,
// surfaces.cpp, is the same for a layer's clear). Nothing changes for an image written only one way: no barrier, no
// cost; only the frames that mix. WWHD_VK_MULTIVIEW_MIX_BARRIER=0 turns it off
void mix_barrier(const std::array<Surface*, 8>& colors, Surface* depth, bool multiview) {
  static const bool on = [] { const char* e = std::getenv("WWHD_VK_MULTIVIEW_MIX_BARRIER"); return !(e && e[0] == '0'); }();
  if (!on || !R.multiview) return;
  bool need = false;
  auto look = [&](Surface* s) {
    if (!s) return;
    Surface* o = s->layerTwin && s->layerTwin->image == s->image ? s
               : s->layerOwner && s->layerOwner->image == s->image ? s->layerOwner : nullptr;
    if (!o) return;
    std::atomic_ref<uint64_t> both(o->mvPassFrame), layer(o->layerPassFrame);  // (the scene's threads begin passes too)
    const uint64_t other = (multiview ? layer : both).load(std::memory_order_relaxed);
    if (other && R.frame - other <= 1) need = true;
    (multiview ? both : layer).store(R.frame, std::memory_order_relaxed);
  };
  for (Surface* c : colors) look(c);
  look(depth);
  if (!need) return;
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_SHADER_READ_BIT;
  record::vkCmdPipelineBarrier(command_buffer(),
      VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  g_mixBarriers.fetch_add(1, std::memory_order_relaxed);
}
#endif
void multiview_log() {
  static uint64_t frame0 = 0;
  if (R.frame < frame0 + 600) return;
  const double f = frame0 ? double(R.frame - frame0) : 600.0;
  frame0 = R.frame;
  uint64_t built = 0, failed = 0;
  vk::multiview::counts(built, failed);
  std::string line;
  for (int i = 0; i < kMvCount; ++i)
    if (multiviewWhy[i]) {
      char b[96];
      std::snprintf(b, sizeof b, "%s%.0f %s", line.empty() ? "" : ", ", multiviewWhy[i] / f, multiviewWhyNames[i]);
      line += b;
    }
  LOG("[vulkan multiview] eye 0's draws per frame: %s; %.1f flushes of eye 1's held commands; pipelines %llu built, %llu "
      "failed", line.c_str(), multiviewFlushes / f, (unsigned long long)built, (unsigned long long)failed);
#ifdef WWHD_MOBILE_GPU_PATHS
  LOG("[vulkan multiview] passes switching a two-eye image between both eyes and one layer: %.2f per frame, each after a "
      "global barrier", double(g_mixBarriers.exchange(0, std::memory_order_relaxed)) / f);
#endif
  if (multiviewFeedbackCopies)
    LOG("[vulkan multiview] draws sampling their own target drawn for both eyes: %.1f per frame (eye 1 copies its own "
        "layer)", multiviewFeedbackCopies / f);
  LOG("[vulkan multiview] targets paired again after a rescale: %s", multiview_repair_note().c_str());
  // the targets: what kept draws from both eyes, and (mobile GPU paths) what the per-eye hold refused and who held it
  auto stats = take_multiview_target_stats();
  std::sort(stats.begin(), stats.end(), [](const MvTargetStat& a, const MvTargetStat& b) { return a.missDraws > b.missDraws; });
  for (size_t i = 0; i < stats.size() && i < 8 && stats[i].missDraws; ++i) {
    const auto& t = stats[i];
    LOG("[vulkan multiview]   target %08X %ux%u f%u %s (eye %u): %.0f draws per frame", t.addr, t.width, t.height,
        t.format, kMvMissWhat[t.missWhat < 7 ? t.missWhat : 0], unsigned(t.eye), t.missDraws / f);
  }
  std::sort(stats.begin(), stats.end(), [](const MvTargetStat& a, const MvTargetStat& b) { return a.heldDraws > b.heldDraws; });
  for (size_t i = 0; i < stats.size() && i < 8 && stats[i].heldDraws; ++i) {
    const auto& t = stats[i];
    LOG("[vulkan multiview]   held per eye: %s target %08X %ux%u f%u: %.0f draws per frame refused; held by a %s draw "
        "(%s)", t.depth ? "depth" : "colour", t.addr, t.width, t.height, t.format, t.heldDraws / f,
        kProgramKindNames[t.heldKind < 8 ? t.heldKind : 0],
        t.heldWhy < kMvCount ? multiviewWhyNames[t.heldWhy] : "not now (its targets drawn one eye at a time)");
  }
  for (auto& n : multiviewWhy) n = 0;
  multiviewFlushes = multiviewFeedbackCopies = 0;
}
// the per-view bindings of a stage: its eye-shifted uniforms' and its eye-specific textures', as the variant names them
struct PerView {
  vk::multiview::Variant variant;
  std::array<uint32_t, 4 + LATTE_NUM_MAX_TEX_UNITS> bindings{};
  uint32_t count = 0;
  bool any() const { return count != 0; }
  bool has(uint32_t b) const { return std::find(bindings.begin(), bindings.begin() + count, b) != bindings.begin() + count; }
  void push_back(uint32_t b) { bindings[count++] = b; }
};
bool per_view_blocks(const vk::Shader* sh, const StageReplay& stage, PerView& out) {
  out.variant = {};
  out.variant.vertex = sh->vertex;
  out.count = 0;
  for (uint32_t k = 0; k < stage.uniformCount; ++k) {
    const int b = int(stage.uniforms[k].binding);
    if (b == sh->mapping.uniformVarsBufferBindingPoint) out.variant.supportBlock = true;
    else {
      int i = 0;
      while (i < 16 && sh->mapping.uniformBuffersBindingPoint[i] != b) ++i;
      if (i == 16) return false;
      out.variant.blocks |= 1u << i;
    }
    out.push_back(uint32_t(b));
  }
  for (uint32_t e = 0; e < stage.eyeTextureCount; ++e) {
    const auto& tex = stage.eyeTextures[e];
    if (tex.unit >= 32 || tex.binding < 0) return false;
    out.variant.textures |= 1u << tex.unit;
    out.push_back(uint32_t(tex.binding));
  }
  return true;
}
// eye 1's surface maps for its lookups, while eye 0's stream and pass stay attached: whatever a lookup records (an
// upload, a layout change) goes before the draw that reads it. Only after eye 1's held commands were emitted, so no
// lookup can release a texture they hold (stereo_release_texture: a flush with eye 1 selected but its pass not swapped)
struct Eye1Lookups {
  Eye1Lookups() { stereoEye() = 1; }
  ~Eye1Lookups() { stereoEye() = 0; }
  Eye1Lookups(const Eye1Lookups&) = delete;
  Eye1Lookups& operator=(const Eye1Lookups&) = delete;
};
bool multiview_draw(const uint32_t* r, vk::Shader* vs, vk::Shader* ps, Pipeline& p, const DrawTargets& t,
                    const StageResources& vres, const StageResources& pres, bool sunShadow, StereoShaders& out,
                    MultiviewDraw& mv) {
  multiview_log();
  auto no = [](MultiviewWhy why) { ++multiviewWhy[why]; t_mvLastWhy = uint8_t(why); return false; };
  // (a draw thread's draws are one eye's; the scene's thread's are eye 0's of both: docs/VR_SCENE_THREADS.md, S3)
  if (stereoEye() != 0 || !stereo_streaming() || (draw_threads::on_draw_thread() && !scene_thread::on_scene_thread()) ||
      eye_thread::enabled() ||
      stereo_eye_segments() || stereo_atlas_holding() || aoPrivateReplay())
    return no(kMvPath);
  if (!p.pipeline || p.pendingCompile) return no(kMvCompiling);
#ifdef WWHD_MOBILE_GPU_PATHS
  // (uniform_plan.h) a pipeline over the cap with some blocks dynamic: multiview when its eye-shifted blocks are among
  // them (the stages' replays are valid only then: kMvStage below)
  if (!p.dynamicUniforms && !p.partialUniforms) return no(kMvNotDynamic);
#else
  if (!p.dynamicUniforms) return no(kMvNotDynamic);
#endif
  if (!p.mvBase) return no(kMvNoBase);
  if (ao_hires_enabled() && (ps->kind == gfx::ProgramKind::DepthDownsample || ps->kind == gfx::ProgramKind::OcclusionPixel))
    return no(kMvAo);
  if (!out.stages[0].valid || !out.stages[1].valid) return no(kMvStage);
  // the targets: eye 0's two-eye targets, at layer 0
  Surface* any = nullptr;
  for (int i = 0; i < 8; ++i) {
    Surface* c = t.colors[i];
    if (!c) continue;
    if (!c->layerTwin || c->arrayLayers != 1 || t.slices[i] || c->layerTwin->image != c->image) {
      multiview_target_miss(c, !c->layerTwin ? (c->imageLayers == 2 * c->arrayLayers ? 1 : 2) : 3);
      // (one eye's image: made two-eye at its next use, surfaces.cpp rescale)
      if (!c->layerTwin && c->imageLayers == c->arrayLayers && !t.slices[i]) note_one_eye_target(c);
      return no(kMvTargets);
    }
    mv.twins[i] = c->layerTwin;
    any = c;
  }
  if (Surface* d = t.depth) {
    if (stereo_light_atlas(d->width, d->height, d->slices, d->isDepth)) return no(kMvAtlasDepth);
    if (!d->layerTwin || d->arrayLayers != 1 || t.depthSlice || d->layerTwin->image != d->image) {
      multiview_target_miss(d, !d->layerTwin ? (d->imageLayers == 2 * d->arrayLayers ? 4 : 5) : 6);
      if (!d->layerTwin && d->imageLayers == d->arrayLayers && !t.depthSlice) note_one_eye_target(d);
      return no(kMvTargets);
    }
    mv.depthTwin = d->layerTwin;
    any = d;
  }
  if (!any) return no(kMvTargets);
  // the pipeline's twin for these per-view bindings
  PerView stages[2];
  if (!per_view_blocks(vs, out.stages[0], stages[0]) || !per_view_blocks(ps, out.stages[1], stages[1]))
    return no(kMvStage);
  const auto& vv = stages[0].variant;
  const auto& pv = stages[1].variant;
  const std::array<uint32_t, 6> key{vv.blocks, uint32_t(vv.supportBlock), vv.textures,
                                    pv.blocks, uint32_t(pv.supportBlock), pv.textures};
  auto found = p.mvTwins.find(key);
  if (found == p.mvTwins.end()) {
    vk::Shader* shaders[2] = {vs, ps};
    vk::multiview::Stage made[2];
    bool text = true;
    for (int st = 0; st < 2; ++st) {
      made[st].variant = stages[st].variant;
      made[st].perViewBindings.assign(stages[st].bindings.begin(), stages[st].bindings.begin() + stages[st].count);
      if (stages[st].any()) {
        text = text && shaders[st]->retainedSource;
        made[st].glsl = shaders[st]->retainedSource;
      } else {
        made[st].words = shaders[st]->spirv();
        if (made[st].words.empty() && shaders[st]->binary && vk::shader_binary_ready(shaders[st]->binary))
          made[st].words = vk::shader_binary_words(shaders[st]->binary);
        if (made[st].words.empty()) return no(kMvBuilding);  // (its words come with its compile)
      }
    }
    std::shared_ptr<vk::multiview::Pipeline> twin;
    if (!text) {  // (translated before multiview draws kept the text)
      twin = std::make_shared<vk::multiview::Pipeline>();
      twin->state = vk::multiview::Pipeline::Failed;
    } else {
      auto base = std::make_shared<vk::multiview::Base>(*p.mvBase);
      base->sets[0] = p.sets[0];
      base->sets[1] = p.sets[1];
      twin = vk::multiview::build(std::move(base), std::move(made[0]), std::move(made[1]));
    }
    found = p.mvTwins.emplace(key, std::move(twin)).first;
  }
  const auto& twin = found->second;
  const int state = twin->state.load(std::memory_order_acquire);
  if (state == vk::multiview::Pipeline::Building) return no(kMvBuilding);
  if (state != vk::multiview::Pipeline::Ready) return no(kMvFailed);
#ifdef WWHD_MOBILE_GPU_PATHS
  // (mixed passes) checked after the draw's own reasons, so a refusal here is for that reason alone: a draw that could
  // never be multiview (no twin, its blocks) is per eye on its own account and marks its images (below, in draw), which
  // keeps an image written one eye at a time every frame per eye for good instead of lapsing every kMixHoldFrames
  if (kMultiviewNoMix) {
    Surface* held = written_per_eye(t.depth) ? t.depth : nullptr;
    for (int i = 0; i < 8 && !held; ++i) if (written_per_eye(t.colors[i])) held = t.colors[i];
    if (held) {
      t_mvRefusedMixed = true;
      std::atomic_ref<uint32_t>(held->mvHeldDraws).fetch_add(1, std::memory_order_relaxed);  // (diagnostics: which)
      return no(kMvMixed);
    }
  }
#endif
  // eye 1's held commands (its draws that were not drawn for both eyes) come first: they wrote its layers before
  // (docs/VR_SCENE_THREADS.md, stage S5) on the scene's thread the eye 1 halves it holds come first, in its part, as
  // eye 1's held commands do below on the render thread
  if (scene_thread::on_scene_thread() && scene_thread::eye1_held()) {
    scene_thread::flush_eye1();
    ++multiviewFlushes;
  }
  if (stereo_eye1_holding()) {
    // (not while the scene's commands are held for the atlas's thread: the flush would join it, ending its overlap)
    // (nor on the scene's thread, which never emits them: its parts run only while eye 1 holds nothing)
#ifdef WWHD_MOBILE_GPU_PATHS
    // (2026-11-07) on the render thread while the scene's commands are held for the atlas's thread: the flush ends that
    // overlap early, kept for a draw into two-eye images no one holds per eye (here: the targets and the mixed checks
    // passed above), which would otherwise go one eye at a time and mark them (a Quest 3 log: the scene's depth held
    // per eye most of the session, "not now (state)"). Rare (under one a frame), so the overlap lost is small.
    // WWHD_VK_MULTIVIEW_JOIN_ATLAS=0: refused as before. The scene's thread cannot emit them: counted on its own
    static const bool kJoinAtlas = [] {
      const char* e = std::getenv("WWHD_VK_MULTIVIEW_JOIN_ATLAS");
      return !(e && e[0] == '0');
    }();
    if (scene_thread::on_scene_thread()) return no(kMvStateScene);
    if (stereo_scene_holding()) {
      if (!kJoinAtlas || !kMultiviewNoMix) return no(kMvState);
      ++multiviewWhy[kMvAtlasJoined];
      stereo_flush();  // (counted in multiviewFlushes below)
    } else
#else
    if (stereo_scene_holding() || scene_thread::on_scene_thread()) return no(kMvState);
#endif
    // (stage S7) beside the second drawing thread: into the part, after eye 0's commands, without a join
    if (switches::kSceneFront && stereo_part_holding()) stereo_release_flush();
    else stereo_flush();
    ++multiviewFlushes;
  }
  // eye 1's own lookups: its targets must be the twins (it would draw into them), and its own images of the
  // eye-specific textures (replay_stage's checks) are view 1's descriptors
  std::array<std::array<VkDescriptorImageInfo, 17 + LATTE_NUM_MAX_TEX_UNITS>, 2> eye1Images;
  vk::Shader* stageShaders[2] = {vs, ps};
  std::array<Surface*, 2 * LATTE_NUM_MAX_TEX_UNITS> eye1Textures{};
  uint32_t eye1TextureCount = 0;
  rprof::mark(rprof::kMvSetup);
  {
    Eye1Lookups eye1Maps;
    DrawTargets t1;
    if (!resolve_replay_targets(r, ps, t1) || t1.colors != mv.twins || t1.depth != mv.depthTwin ||
        t1.slices != t.slices || t1.depthSlice != t.depthSlice || t1.width != t.width || t1.height != t.height ||
        t1.sx != t.sx || t1.sy != t.sy)
      return no(kMvEyeTargets);
    for (int st = 0; st < 2; ++st) {
      const StageReplay& in = out.stages[st];
      vk::Shader* sh = stageShaders[st];
      const uint32_t texbase = sh->vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
      for (uint32_t e = 0; e < in.eyeTextureCount; ++e) {
        const auto& tex = in.eyeTextures[e];
        bool substituted = false;
        Surface* guest1 = nullptr;
        Surface* s1 = resolve_texture(r, sh, tex.unit, mv.twins, mv.depthTwin, &substituted, &guest1);
        if (substituted || aliases_target(s1, mv.twins, mv.depthTwin) != tex.feedback || s1->sx != tex.sx ||
            s1->sy != tex.sy)
          return no(kMvEyeTextures);
        if (tex.feedback) {  // view 1 samples eye 1's copy of its own layer (eye 0's copy is view 0's)
          eye1Images[st][tex.write] = {resolve_sampler(r, sh, tex.unit, s1, guest1),
                                       feedback_view(s1, r + texbase + tex.unit * 7, sh->vertex, tex.unit, nullptr),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          ++multiviewFeedbackCopies;
          continue;
        }
        eye1Images[st][tex.write] = {resolve_sampler(r, sh, tex.unit, s1, guest1), sampled_texture_view(s1, r + texbase + tex.unit * 7),
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        eye1Textures[eye1TextureCount++] = s1;
      }
    }
  }
  // their layouts, in eye 0's stream (the draw is), as each stage samples them
  for (uint32_t i = 0; i < eye1TextureCount; ++i)
    transition_image(eye1Textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
  rprof::mark(rprof::kMvLookups);
  // eye 1's copies of the eye-shifted blocks: replay_stage's
  const float eye1 = stereo::frame.mode != stereo::Off && !stereo::layout_draw_depth() &&
      stereo_screen_target(t.target->width, t.target->height) ? stereo::eye_offset(1) : 0.0f;
  std::array<std::array<uint32_t, 4>, 2> eye1Offsets{};
  for (int st = 0; st < 2; ++st) {
    const StageReplay& in = out.stages[st];
    for (uint32_t k = 0; k < in.uniformCount; ++k) {
      const auto& u = in.uniforms[k];
      PER_THREAD_STATIC(scratch, std::vector<uint8_t>);
      scratch.assign(u.bytes.begin(), u.bytes.end());
      const float rowsEye = st == 0 || sunShadow ? eye1 : 0.0f;
      stereo::apply_eye_rows(scratch.data(), u.rows, rowsEye, stereo::frame.convergence,
                             st == 0 ? stereo::rows_frustum(u.rows, rowsEye, 1) : stereo::eye_frustum(rowsEye));
      if (fov_diag::kOn && st == 0 && u.rows.layout && rowsEye == 0) [[unlikely]] fov_diag::layout_replay(1);
      if (fov_diag::kOn && st == 0) [[unlikely]] fov_diag::note(rowsEye, scratch.data(), scratch.size());
      rprof::UploadKind uploads(rprof::kUpUbo);
      const UploadSlice b = snapshot(scratch.data(), scratch.size(), R.properties.limits.minUniformBufferOffsetAlignment);
      if (b.buffer != u.buffer || b.offset > UINT32_MAX) return no(kMvUpload);  // the set names another buffer
      eye1Offsets[st][k] = uint32_t(b.offset);
    }
  }
  rprof::mark(rprof::kMvUniforms);
  // the sets: a per-view stage's has its per-view bindings twice (a block: the same buffer at each view's offset; a
  // texture: each eye's image), else eye 0's own
  const StageResources* eye0[2] = {&vres, &pres};
  for (int st = 0; st < 2; ++st) {
    const StageReplay& in = out.stages[st];
    const StageResources& res = *eye0[st];
    // the dynamic offsets, by binding and array element
    for (uint32_t j = 0; j < res.dynamicOffsetCount; ++j) {
      mv.offsets[mv.offsetCount++] = res.dynamicOffsets[j];
      for (uint32_t k = 0; k < in.uniformCount; ++k)
        if (in.uniforms[k].dynamicIndex == j) mv.offsets[mv.offsetCount++] = eye1Offsets[st][k];
    }
    if (!stages[st].any()) {
      mv.sets[st] = res.set;
      continue;
    }
    std::array<VkWriteDescriptorSet, 17 + LATTE_NUM_MAX_TEX_UNITS + 4> writes;
    uint32_t n = 0;
    for (uint32_t i = 0; i < in.writeCount; ++i) {
      auto w = in.writes[i];
      if (w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) w.pImageInfo = &in.imageInfos[i];
      else w.pBufferInfo = &in.bufferInfos[i];
      writes[n++] = w;
      if (stages[st].has(w.dstBinding)) {
        w.dstArrayElement = 1;
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) w.pImageInfo = &eye1Images[st][i];
        writes[n++] = w;
      }
    }
    mv.sets[st] = stage_descriptor_set(st == 0, twin->sets[st], writes.data(), n);
  }
  // (a lookup recorded nothing for eye 1: everything went to eye 0's stream)
  if (stereo_eye1_holding()) return no(kMvEye1Held);
  mv.pipeline = twin->pipeline;
  mv.layout = twin->layout;
  mv.on = true;
  ++multiviewWhy[kMvDrawn];
  return true;
}
namespace scene_thread { extern std::atomic<uint64_t> g_threadDrawNs, g_threadDraws; }  // (below: diagnostics)
// (docs/VR_SCENE_THREADS.md, "S6: the tail on the render thread") the tail: the part ends where the second drawing
// thread's span does, and the render thread draws the rest of the frame itself (no new part until the next).
// Off by default. WWHD_VK_SCENE_TAIL=1 always: in the 4,000-draw views it gained 78.8 to 87.1 fps, but it cost the
// 3,000-draw ones 86.9 to 76.6, where the threads kept up. =auto ("S6: the automatic tail", 2026-11-03): in a frame
// after one whose render thread waited at the joins more than 2.5 ms, kept while its own wait at the span's end stays
// over 1 ms; in the headset it stayed on in 91 to 99 % of the busy frames (the threads are always behind at the span's
// end) and lost to the tail off (85.0 against 86.8 fps), so it is not the default. Now (=auto, "S6: the tail by measured
// cost") the tail is the run's last piece only (to the next flush; parts begin again after it), chosen by the two ways'
// costs from the span's end to the render thread being past the part, measured for the way taken and estimated for the
// other (scene_tail_measured)
const int kSceneTailMode = [] {
  const char* e = std::getenv("WWHD_VK_SCENE_TAIL");
  return !e ? 0 : !std::strcmp(e, "auto") ? -1 : std::strcmp(e, "0") ? 1 : 0;
}();
bool g_sceneTailNext = kSceneTailMode == 1;  // (render thread) this frame's part ends at the span's end
uint64_t g_sceneTailFrames = 0, g_sceneTailCounted = 0;  // (diagnostics) frames with the tail, frames with a part
bool g_sceneTailNow = false;  // (render thread) this frame's part ended at the span's end
std::atomic<uint64_t> g_sceneBothDoneNs{0};  // (the scene's thread, at A1's end) A1 and B both done, steady clock ns
namespace {
uint64_t tail_now_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
// (render thread) the tail's rule: from the span's end (the run's end, B done handing) to the render thread past the
// part. Without the tail: A1 and B finish, A draws the rest (A2), the render thread reads on and waits at the join.
// With it: the render thread waits for A1 and B there and draws the rest itself
struct TailRule {
  int phase = 0;              // 1: the span ended, its join to come; 2: the tail drawn here, the flush to come
  bool tail = false;          // this frame's way
  uint64_t spanEndNs = 0, joinEndNs = 0;
  uint32_t spanDraw = 0;
  // (per draw, ns: running averages) the render thread reading and handing a draw over, A drawing one of A2, the
  // render thread drawing one of the tail itself
  double handNs = 1500, threadNs = 9500, renderNs = 5000;
  int favour = 0;             // frames in a row the other way was estimated cheaper
  // (diagnostics, since the log line) frames each way, their measured and the other way's estimated costs
  uint64_t frames[2] = {}, costNs[2] = {}, otherNs[2] = {};
} g_tail;
void scene_tail_measured(uint64_t costNs, uint64_t otherNs) {
  auto& t = g_tail;
  ++t.frames[t.tail];
  t.costNs[t.tail] += costNs;
  t.otherNs[t.tail] += otherNs;
  if (kSceneTailMode >= 0) return;
  // the other way, when it is the cheaper by 0.3 ms three frames in a row
  t.favour = otherNs + 300000 < costNs ? t.favour + 1 : 0;
  if (t.favour >= 3) {
    g_sceneTailNext = !t.tail;
    t.favour = 0;
  }
}
}  // namespace
// (render thread) the span ended at the run's end: the tail's way for this frame (g_sceneTailNext)
void scene_tail_span_end(uint32_t draw) {
  auto& t = g_tail;
  t.phase = 1;
  t.tail = g_sceneTailNext;
  t.spanEndNs = tail_now_ns();
  t.spanDraw = draw;
}
uint32_t scene_split_draw_index();
void scene_tail_note_join(uint64_t t0, uint64_t t1) {
  auto& t = g_tail;
  if (t.phase != 1) return;
  const double n = double(std::max<uint32_t>(1, scene_split_draw_index() - t.spanDraw));
  if (t.tail) {  // (the join at the span's end: the render thread draws the rest next, to the flush)
    t.joinEndNs = t1;
    t.phase = 2;
    return;
  }
  t.phase = 0;
  const uint64_t both = g_sceneBothDoneNs.load(std::memory_order_acquire);
  if (both < t.spanEndNs || both > t1 || t0 < t.spanEndNs) return;  // (another span's, or none)
  t.handNs += 0.1 * ((double(t0 - t.spanEndNs) / n) - t.handNs);
  t.threadNs += 0.1 * ((double(t1 - both) / n) - t.threadNs);
  // with the tail: waited for A1 and B, then the rest drawn here
  const double tailNs = double(both - t.spanEndNs) + n * t.renderNs;
  scene_tail_measured(t1 - t.spanEndNs, uint64_t(tailNs));
}
void scene_tail_flush() {
  auto& t = g_tail;
  if (t.phase != 2) return;
  t.phase = 0;
  g_sceneTailNow = false;  // (the tail is the run's last piece: parts begin again after this flush)
  const uint64_t now = tail_now_ns();
  const double n = double(std::max<uint32_t>(1, scene_split_draw_index() - t.spanDraw));
  t.renderNs += 0.1 * ((double(now - t.joinEndNs) / n) - t.renderNs);
  // without it: A1 and B done when the render thread stopped waiting (or before), A2 after them; the render thread
  // reading the rest meanwhile
  const double waited = double(t.joinEndNs - t.spanEndNs);
  const double offNs = std::max(n * t.handNs, waited + n * t.threadNs);
  scene_tail_measured(now - t.spanEndNs, uint64_t(offNs));
}
uint64_t g_renderPartNs = 0, g_renderPartDraws = 0;  // (diagnostics) a part's draws recorded on the render thread
uint64_t g_samePassNs = 0, g_samePassDraws = 0;  // (diagnostics) the render thread's draws before the split, in its pass
bool scene_split_in_half_pass();
// (docs/VR_SCENE_THREADS.md, stages S1 and S2) where the scene's parts begin, from the frame before's passes: before the
// draw at which half of the frame's draws are done, if that draw is in a two-eye pass, and at the start of every two-eye
// pass after it that had at least 16 draws. A part ends with its pass. Render thread only (the atlas's thread draws
// none of the scene).
namespace {
struct SceneSplit {
  std::vector<std::pair<uint32_t, bool>> passes, last;  // this frame's / the last frame's passes: draws, two-eye
  uint32_t halfPass = UINT32_MAX, halfDraw = 0;
  uint64_t frames = 0, draws = 0, heldDraws = 0;
  bool threadOn = false;           // (the scene's thread) this frame uses it: the last frame was a busy one
  uint64_t threadFrames = 0;       // (diagnostics) frames that used it
  // (stage S6) the frame's scene draws in the guest's order (render thread, as they are drawn or handed over), and its
  // passes by the number of their first draw: the render thread's and the scene's thread's (one at a time), and the
  // second drawing thread's (beside the scene's thread)
  struct Mark { uint32_t first; bool multiview; };
  uint32_t drawIndex = 0;
  std::vector<Mark> marks, marksB;
  uint64_t runFrames = 0, runDraws = 0;  // (diagnostics) the main geometry run found
} sceneSplit;
constinit thread_local uint32_t t_drawIndex = 0;  // the frame's scene draw being drawn on this thread
uint32_t current_draw_index() { return t_drawIndex; }
constexpr uint32_t kSplitPassDraws = 16;  // (a two-eye pass after the half point: held from its start with this many)
}  // namespace
uint32_t scene_split_draw_index() { return sceneSplit.drawIndex; }
void scene_split_pass_begin(bool multiview) {
  auto& g = sceneSplit;
  // (the scene's thread's passes are the scene's: it runs one draw at a time, the render thread waiting)
  if (draw_threads::on_draw_thread() && !scene_thread::on_scene_thread()) return;
  // (stage S6) the second drawing thread's beside it: kept apart, put in order at the frame's end
  if (scene_thread::on_scene_b()) {
    if (switches::kSceneTwo) g.marksB.push_back({t_drawIndex, multiview});
    return;
  }
  if (switches::kSceneTwo) g.marks.push_back({t_drawIndex, multiview});
  const size_t index = g.passes.size();
  g.passes.push_back({0, multiview});
  if (switches::kSceneFront) return;  // (S7 holds no S2 parts)
  if (switches::kSceneThread && !g.threadOn) return;  // (a lighter frame: as without the thread)
  // (the pass has begun in the renderer's stream; its draws go into a part)
  if (multiview && g.halfPass != UINT32_MAX && index > g.halfPass && index < g.last.size() && g.last[index].second &&
      g.last[index].first >= kSplitPassDraws && pass_state().rendering && pass_state().multiview)
    stereo_part_hold();
}
void scene_split_draw() {
  if (draw_threads::on_draw_thread() && !scene_thread::on_scene_thread()) return;
  if (scene_thread::on_scene_b()) return;  // (stage S6: counted by number, sceneSplit.drawIndex)
  if (!sceneSplit.passes.empty()) ++sceneSplit.passes.back().first;
  ++sceneSplit.draws;
  if (stereo_part_holding()) ++sceneSplit.heldDraws;
}
uint64_t scene_split_frame_draws() {  // (diagnostics) the frame's draws so far, both threads' (one at a time here)
  uint64_t n = 0;
  for (const auto& p : sceneSplit.passes) n += p.first;
  return n;
}
bool scene_split_in_half_pass() {
  const auto& g = sceneSplit;
  return !g.passes.empty() && g.passes.size() - 1 == g.halfPass && g.passes.back().second;
}
void scene_split_before_draw() {
  auto& g = sceneSplit;
  if (!draw_threads::on_draw_thread()) {  // (stage S6: the draw's number, handed over too; a held run's, its pieces')
    t_drawIndex = g.drawIndex;
    g.drawIndex += t_sceneRun ? t_sceneRun->count : 1;
  }
  if (switches::kSceneFront) return;  // (stage S7: its span starts in front_draw)
  if (draw_threads::on_draw_thread() || stereo_part_holding() || g.passes.empty()) return;
  // (stage S5) with the scene's thread the part begins at the scene's first draw it can, and again after each join;
  // only in the busy frames (switches::scene_thread_min_draws)
  if (switches::kSceneThread) {
    if (g.threadOn && !g_sceneTailNow && !ui_layer::candidate()) stereo_part_hold();  // (the game's UI: here, below)
    return;
  }
  if (g.passes.size() - 1 != g.halfPass || g.passes.back().first != g.halfDraw || !g.passes.back().second) return;
  if (!pass_state().rendering || !pass_state().multiview) return;
  stereo_part_hold();
}
void scene_split_frame_end() {
  auto& g = sceneSplit;
  {
    const uint64_t waited = stereo_part_take_frame_wait_ns();
    if (g.threadOn) ++g_sceneTailCounted, g_sceneTailFrames += g_sceneTailNow;
    (void)waited;
    g_tail.phase = 0;
  }
  g_sceneTailNow = false;
  uint64_t total = 0;
  for (const auto& p : g.passes) total += p.first;
  g.halfPass = UINT32_MAX;
  g.halfDraw = 0;
  uint64_t before = 0;
  for (size_t i = 0; i < g.passes.size() && total >= 2 * kSplitPassDraws; ++i) {
    if (2 * (before + g.passes[i].first) >= total) {
      g.halfPass = uint32_t(i);
      g.halfDraw = uint32_t(total / 2 - before);
      break;
    }
    before += g.passes[i].first;
  }
  if (switches::kSceneThread) {
    const uint64_t min = switches::scene_thread_min_draws();
    // (stage S6: the second drawing thread's draws are counted by number only)
    const uint64_t frameDraws = switches::kSceneTwo ? g.drawIndex : total;
    g.threadOn = frameDraws >= (g.threadOn ? min * 9 / 10 : min);
    if (g.threadOn) ++g.threadFrames;
  }
  if (switches::kSceneFront) scene_thread::front_frame_end(g.drawIndex);  // (stage S7: next frame's split)
  if (switches::kSceneTwo) {
    // (stage S6) the main geometry run: the most draws in consecutive two-eye passes, both threads' passes in order
    auto& m = g.marks;
    m.insert(m.end(), g.marksB.begin(), g.marksB.end());
    std::stable_sort(m.begin(), m.end(), [](const SceneSplit::Mark& a, const SceneSplit::Mark& b) { return a.first < b.first; });
    uint32_t bestFirst = 0, bestEnd = 0, runFirst = 0;
    bool inRun = false;
    for (size_t i = 0; i < m.size(); ++i) {
      const uint32_t end = i + 1 < m.size() ? m[i + 1].first : g.drawIndex;
      if (!m[i].multiview || end <= m[i].first) {
        if (!m[i].multiview) inRun = false;
        continue;
      }
      if (!inRun) runFirst = m[i].first, inRun = true;
      if (end - runFirst > bestEnd - bestFirst) bestFirst = runFirst, bestEnd = end;
    }
    scene_thread::set_main_run(bestFirst, bestEnd);
    if (bestEnd > bestFirst) ++g.runFrames, g.runDraws += bestEnd - bestFirst;
    m.clear();
    g.marksB.clear();
  }
  if (motion_on()) {
    // (stage D2b 2b) the frame's draws' data becomes the previous frame's; room for the next frame's
    auto& st = motion_store();
    static motion::Stats window;
    static uint64_t windowFrames = 0;
    window.add(st.end_frame());
    st.begin_frame(std::max<uint32_t>(4096, st.draws() * 2));
    if (++windowFrames >= 600) {
      const double d = window.draws ? double(window.draws) : 1.0, f = double(windowFrames);
      LOG("[motion] store: %.0f draws a frame, %.1f%% found their previous frame's draw; %.2f MiB kept, %u slots over "
          "the budget, %llu draws past a frame's room so far",
          window.draws / f, 100.0 * double(window.unique + window.ordered) / d, double(st.bytes()) / 1048576.0,
          st.over_budget(), (unsigned long long)st.beyond());
      window = {};
      windowFrames = 0;
    }
  }
  g.drawIndex = 0;
  g.last.swap(g.passes);
  g.passes.clear();
  if (++g.frames < 600) return;
  if (switches::kSceneFront) scene_thread::front_log(double(g.frames));
  uint64_t holds, passEnds, flushes, others;
  stereo_part_stats(holds, passEnds, flushes, others);
  const double f = double(g.frames);
  if (!switches::kSceneThread && g_renderPartDraws) {
    LOG("[vulkan scene split] per frame: %.0f part draws on the render thread, %.2f us each", g_renderPartDraws / f,
        double(g_renderPartNs) / 1e3 / double(g_renderPartDraws));
    g_renderPartNs = g_renderPartDraws = 0;
  }
  if (switches::kSceneThread) {
    LOG("[vulkan scene thread] used in %.0f%% of frames (after a frame of %u scene draws or more)",
        100.0 * double(g.threadFrames) / f, switches::scene_thread_min_draws());
    g.threadFrames = 0;
    if (switches::kSceneTwo) {
      scene_thread::log_b(f, g.runFrames ? double(g.runDraws) / double(g.runFrames) : 0.0);
      g.runFrames = g.runDraws = 0;
    }
    uint64_t published, exact;
    double waitMs, emitMs;
    stereo_part_join_stats(published, exact, waitMs, emitMs);
    const uint64_t threadDraws = scene_thread::g_threadDraws.exchange(0), threadNs = scene_thread::g_threadDrawNs.exchange(0);
    uint64_t sites[int(PartJoinSite::Count)];
    stereo_part_site_stats(sites);
    LOG("[vulkan scene thread] per frame: its draws took %.2f ms, %.2f ms of it while the render thread waited at a join",
        double(threadNs) / 1e6 / f, stereo_part_busy_during_join_ms() / f);
    {
      // (diagnostics) both drawing threads' sampled draws, by phase: us a draw
      static const char* names[rprof::kDrawPhases] = {"shader", "indices", "targets", "pipeline", "uniforms", "textures",
                                                      "descriptors", "mv-setup", "mv-lookups", "mv-uniforms", "mv-sets",
                                                      "pass", "record", "vertex", "submit"};
      uint64_t ns[rprof::kDrawPhases];
      const uint64_t n = rprof::thread_phases(ns);
      std::string ph;
      double sum = 0;
      for (int p = 0; p < rprof::kDrawPhases; ++p) {
        char part[48];
        std::snprintf(part, sizeof part, " %s %.2f", names[p], n ? double(ns[p]) / 1e3 / double(n) : 0.0);
        ph += part;
        sum += n ? double(ns[p]) / 1e3 / double(n) : 0.0;
      }
      LOG("[vulkan scene thread] drawing threads' draw phases, us a draw (%llu sampled):%s; sum %.2f", (unsigned long long)n,
          ph.c_str(), sum);
    }
    LOG("[vulkan scene thread] draw threads' register jobs per frame: %.2f whole-file loads, %.0f writes of %.0f words; "
        "loads by block checked %llu, registers that differed %llu", DrawThread::g_loads.exchange(0) / f,
        DrawThread::g_writes.exchange(0) / f, DrawThread::g_writeWords.exchange(0) / f,
        (unsigned long long)DrawThread::g_verified.exchange(0), (unsigned long long)DrawThread::g_verifyMismatches.exchange(0));
    LOG("[vulkan scene thread] joins per frame: a flush %.2f, in order (clears, copies) %.2f, eye 0's pass ending %.2f, "
        "eye 1 %.2f, recording %.2f, the clouds %.2f, idle %.2f", sites[int(PartJoinSite::Flush)] / f,
        sites[int(PartJoinSite::InOrder)] / f, sites[int(PartJoinSite::EndEncoder)] / f, sites[int(PartJoinSite::Eye1)] / f,
        sites[int(PartJoinSite::Recording)] / f, sites[int(PartJoinSite::DrawState)] / f, sites[int(PartJoinSite::Idle)] / f);
    LOG("[vulkan scene thread] per frame: %.0f draws there, %.2f us each; joins waited %.2f ms and emitted %.2f ms; "
        "parts queued to the record worker %.2f, replayed here %.2f; the render thread's draws before the split in its "
        "pass: %.0f, %.2f us each", threadDraws / f,
        threadDraws ? double(threadNs) / 1e3 / double(threadDraws) : 0.0, waitMs / f, emitMs / f, published / f, exact / f,
        g_samePassDraws / f, g_samePassDraws ? double(g_samePassNs) / 1e3 / double(g_samePassDraws) : 0.0);
    g_samePassNs = g_samePassDraws = 0;
  }
  if (g_sceneTailCounted) {
    auto& t = g_tail;
    const auto ms = [&](uint64_t ns, int way) { return t.frames[way] ? double(ns) / 1e6 / double(t.frames[way]) : 0.0; };
    LOG("[vulkan scene thread] the tail on the render thread (%s): in %.0f%% of the frames with the scene's thread; "
        "from the span's end to past the part: without it %.0f frames, %.2f ms (with it estimated %.2f), with it %.0f "
        "frames, %.2f ms (without it estimated %.2f); us a draw: handed %.2f, A2 %.2f, the render thread's tail %.2f",
        kSceneTailMode < 0 ? "by measured cost" : kSceneTailMode ? "always" : "never",
        100.0 * double(g_sceneTailFrames) / double(g_sceneTailCounted), double(t.frames[0]), ms(t.costNs[0], 0),
        ms(t.otherNs[0], 0), double(t.frames[1]), ms(t.costNs[1], 1), ms(t.otherNs[1], 1), t.handNs / 1e3,
        t.threadNs / 1e3, t.renderNs / 1e3);
    g_sceneTailFrames = g_sceneTailCounted = 0;
    t.frames[0] = t.frames[1] = t.costNs[0] = t.costNs[1] = t.otherNs[0] = t.otherNs[1] = 0;
  }
  LOG("[vulkan scene split] per frame: %.2f parts, %.0f of %.0f draws held (%.0f%%); emitted at their pass's end %.2f, "
      "at a flush %.2f, otherwise %.2f", holds / f, g.heldDraws / f, g.draws / f,
      g.draws ? 100.0 * double(g.heldDraws) / double(g.draws) : 0.0, passEnds / f, flushes / f, others / f);
  g.frames = g.draws = g.heldDraws = 0;
}
// (docs/VR_SCENE_THREADS.md, stages S3 and S4) the scene's parts on a draw thread of their own, beside the render thread
namespace scene_thread {
std::atomic<uint64_t> g_threadDrawNs{0}, g_threadDraws{0};  // (diagnostics) the thread's draws and their time
namespace {
// (experiment, WWHD_VK_SCENE_THREAD_SAME_L3=1) both threads kept on the logical processors that share the L3 cache of
// the one the render thread runs on when the first part begins: the renderer's shared caches are then in one L3
#ifdef _WIN32
uint64_t l3_mask_of_current_processor() {
  PROCESSOR_NUMBER pn{};
  GetCurrentProcessorNumberEx(&pn);
  DWORD bytes = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
  std::vector<uint8_t> buf(bytes);
  auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
  if (!bytes || !GetLogicalProcessorInformationEx(RelationCache, info, &bytes)) return 0;
  for (DWORD off = 0; off < bytes;) {
    auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == pn.Group &&
        (e->Cache.GroupMask.Mask >> pn.Number & 1))
      return uint64_t(e->Cache.GroupMask.Mask);
    off += e->Size;
  }
  return 0;
}
#endif
std::atomic<uint64_t> g_l3Mask{0};
const bool kSameL3 = switches::env_on("WWHD_VK_SCENE_THREAD_SAME_L3");
constinit thread_local bool onSceneThreadTls = false;
constinit thread_local bool onSceneBTls = false;  // (stage S6) the second drawing thread
enum : uint32_t { kPass = DrawThread::kUser, kDraw, kEndEye1, kState, kClearColor, kClearDepth, kCopy, kScan,
                  kEndA1, kEndB, kDoneB, kDrawRun };
// (stage S6) the scene's thread's span before the second thread's (A1) and the second thread's (B): A1 recorded, B
// done, the waits for them, and B's counters (diagnostics)
struct Split {
  std::mutex mutex;
  std::condition_variable changed;
  bool a1Done = true, bDone = true;
  std::atomic<uint64_t> bDraws{0}, waitsInPlace{0}, undone{0}, conflicts{0}, bWaitNs{0}, aWaitNs{0}, spans{0};
  // (the split's balance) the last span's wait of A for B at A1's end, -1 once read (render thread, set_main_run)
  std::atomic<int64_t> lastAWaitNs{-1}, lastBWaitNs{0};
  std::atomic<uint64_t> spanBWaitNs{0};  // (this span's waits of B for A1, its draws' and in place)
  // (diagnostics) what it waited for A1 before: a layout, an upload, an image destroyed, anything else
  std::atomic<uint64_t> why[4] = {};
  std::atomic<uint64_t> prepared{0};  // textures made ready at the parts' starts
  std::atomic<bool> off{false};  // (a change that could be neither waited for nor undone: off for the session)
  // ("S6: B's reads deferred") a1Done, read without the lock; the span's number (Surface::bPreRead); the surfaces the
  // second drawing thread samples in it before A1's end (made readable at A1's end); conflicts with them
  std::atomic<bool> a1Over{true};
  uint64_t span = 0;
  // (each: the layout, stages and access its span starts with; before them its copy from a newer alias, its mip chain)
  struct PreRead {
    Surface* s; uint32_t addr; int eye; bool adopt; bool chain; SurfaceDesc d; uint32_t maxLevels;
    int layout; uint32_t stage, access;
  };
  std::vector<PreRead> preReads;
  std::atomic<uint64_t> deferred{0}, preConflicts{0};
} g_split;
struct NeedA1 {};  // (the second drawing thread) its draw is undone, and drawn again after A1
void split_set(bool Split::*flag) {
  {
    std::lock_guard<std::mutex> lock(g_split.mutex);
    g_split.*flag = true;
  }
  g_split.changed.notify_all();
}
uint64_t split_wait(bool Split::*flag, std::atomic<uint64_t>& ns) {
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(g_split.mutex);
  g_split.changed.wait(lock, [&] { return g_split.*flag; });
  const uint64_t waited =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  ns.fetch_add(waited, std::memory_order_relaxed);
  return waited;
}
bool split_is(bool Split::*flag) {
  std::lock_guard<std::mutex> lock(g_split.mutex);
  return g_split.*flag;
}
const bool kTwoSequential = switches::env_on("WWHD_VK_SCENE_TWO_SEQUENTIAL");  // (S6.2's check mode)
uint64_t g_bEyeGeneration = 1;  // (the second drawing thread's own: Surface::eyeHeldB)
// (the second drawing thread) the marks of the draw being drawn: undone back to them
struct BDraw {
  DeferredCommands::Mark eye0, eye1;
  DeferredCommands *eye0Stream = nullptr, *eye1Stream = nullptr;
  bool can_undo() const { return eye0Stream->can_rollback(eye0) && eye1Stream->can_rollback(eye1); }
};
constinit thread_local BDraw* t_bDraw = nullptr;
// the guest structs' sizes (gx2_core.cpp unpack_struct): GX2ColorBuffer, GX2DepthBuffer, GX2Surface, in words
constexpr uint32_t kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
class Worker : public DrawThread {
public:
  // its upload arena and descriptor pool (the renderer's, per submission), and its name in thread profiles
  Worker(std::vector<Renderer::UploadBlock>* uploads, VkDescriptorPool* pool, const char* name, bool b = false)
      : uploads_(uploads), pool_(pool), name_(name), b_(b) {
    start();
  }
  PassState pass;  // eye 0's pass as the render thread had it at the part's start (read by the thread at kPass)
  DeferredCommands* eye1Stream = nullptr;  // (set before the part's first hand-over)
  std::atomic<bool> eye1Drawn{false};  // a draw of this part was drawn as two: the threads join before the next
private:
  void setup() override {
    DrawThread::setup();
    onSceneThreadTls = true;
    onSceneBTls = b_;
    t_worker = this;
    rprof::g_phase_thread() = true;  // (its sampled draws' phases: the scene thread's log line)
    if (b_) {  // (its own layout state, filled at each span's start: kPass)
      stereo::t_layoutOverride = &layout_;
      aspect::set_thread_content_clip(&contentClip_);
    }
    // scheduled like the render thread (raised priority, the fast cores), which it stands in for
    if (!switches::env_on("WWHD_VK_SCENE_THREAD_NO_BOOST")) host::boost_thread_priority();
    core_groups::place_current_thread(core_groups::Role::Scene, name_);  // (WWHD_VK_PLACE_THREADS=0: not placed)
    thread_sampler::add_current_thread(name_);  // (WWHD_SAMPLE_THREADS profiles)
    set_thread_upload_blocks(uploads_);
    set_thread_descriptor_pool(pool_);
  }
  // eye 1's half of a draw, into the part's eye 1 stream, with eye 1's pass (as stereo_select_eye(1) would)
  template <class F> void as_eye1(F&& f) {
    PassState* const eye0Pass = &pass_state();
    DeferredCommands* const eye0Stream = eye_deferred_commands();
    set_thread_pass_state(&eye1Pass_);
    stereoEye() = 1;
    eye_deferred_commands() = eye1Stream;
    struct Back {
      PassState* pass; DeferredCommands* stream;
      ~Back() { stereoEye() = 0; eye_deferred_commands() = stream; set_thread_pass_state(pass); }
    } back{eye0Pass, eye0Stream};
    f();
  }
  void execute(const Job& j, const uint32_t* words) override {
    switch (j.kind) {
    case kPass:
      pass_state() = pass;  // eye 0's pass, open (the second drawing thread's: none)
      eye1Pass_ = {};
      if (b_) {
        // the layout state as the render thread had handed it at the switch (scene_thread::g_handed)
        layout_.drawDepth = j.a[0];
        layout_.photograph = j.a[1] != 0;
        contentClip_ = j.a[2] != 0;
        ++t_partEpochSalt;  // (its draws bind everything themselves: its last span's state is not in this stream)
        if (kTwoSequential) g_split.spanBWaitNs.fetch_add(split_wait(&Split::a1Done, g_split.bWaitNs), std::memory_order_relaxed);  // (S6.2: after A1, one at a time)
      }
#ifdef _WIN32
      if (const uint64_t mask = g_l3Mask.load(std::memory_order_relaxed); mask && !pinned_) {
        pinned_ = true;
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(mask));
      }
#endif
      break;
    case kDraw: {
      t_drawIndex = j.a[6];
      // (diagnostics) one draw in 16 timed by phase, as the render thread's are
      struct Sampled {
        bool on;
        ~Sampled() { if (on) rprof::end_thread_draw(); }
      } sampled{rprof::enabled() && (++phaseCounter_ & 15) == 0};
      if (sampled.on) rprof::begin_thread_draw();
      draw_one(j);
      break;
    }
    case kDrawRun: {  // (docs/VR_SCENE_THREADS.md, "S6: batched runs on the drawing threads")
      t_drawIndex = j.a[6];
      draw_run(j, words);
      break;
    }
    case kEndEye1: as_eye1([] { end_encoder(); }); break;  // eye 1's pass ends at the end of its commands
    case kEndA1: {
      // (stage S6, the scene's thread) A1 ends here: its passes end and its eye 1 halves follow, so the second drawing
      // thread's commands come after all of it. Then it waits for that thread, and its commands follow
      end_both();
      g_split.a1Over.store(true, std::memory_order_release);
      split_set(&Split::a1Done);
      {
        const uint64_t aWaited = split_wait(&Split::bDone, g_split.aWaitNs);  // (B done: its waits for A1 are in)
        g_sceneBothDoneNs.store(tail_now_ns(), std::memory_order_release);  // (the tail's rule: A2 starts here)
        g_split.lastBWaitNs.store(int64_t(g_split.spanBWaitNs.exchange(0, std::memory_order_relaxed)),
                                  std::memory_order_relaxed);
        g_split.lastAWaitNs.store(int64_t(aWaited), std::memory_order_release);
      }
      b_commit_reads();  // (its reads' layouts, after A1's commands and before its own)
      stereo_part_emit_b();
      ++t_partEpochSalt;  // (A2 binds everything itself: the second thread's commands went between)
      forget_continued();
      break;
    }
    case kEndB: end_both(); break;  // (stage S6, the second drawing thread) its passes end, its eye 1 halves follow
    case kDoneB: split_set(&Split::bDone); break;
    case kClearColor: {
      const uint32_t cb = scratch(0, words, kColorBufferWords);
      float rgba[4];
      std::memcpy(rgba, words + kColorBufferWords, sizeof rgba);
      in_order([&] { gfxvk::clear_color(regs(), cb, rgba); });
      break;
    }
    case kClearDepth: {
      const uint32_t db = scratch(0, words, kDepthBufferWords);
      float depth;
      std::memcpy(&depth, &j.f, sizeof depth);
      in_order([&] { gfxvk::clear_depth_stencil(regs(), db, depth, j.a[0], j.a[1]); });
      break;
    }
    case kCopy: {
      const uint32_t src = scratch(0, words, kSurfaceWords), dst = scratch(1, words + kSurfaceWords, kSurfaceWords);
      in_order([&] { gfxvk::copy_surface(src, j.a[0], j.a[1], dst, j.a[2], j.a[3]); });
      break;
    }
    case kScan: {
      const uint32_t cb = scratch(0, words, kColorBufferWords);
      in_order([&] { gfxvk::copy_to_scan(cb, j.a[0]); });
      break;
    }
    case kState:  // (stage S5) state draws read outside the registers, as gx2_core.cpp would have set it here
      switch (j.a[0]) {
      case render::kStateProjectionTag: stereo::tag_layout(words[0], words + 1); break;
      case render::kStateLayoutContent:
        aspect::set_content_clip(j.n && words[0]);
        if (j.n >= 2) stereo::layout_draw(words[1] != 0);
        stereo::layoutPhotograph = j.n >= 3 && words[2];
        break;
      case render::kStateLayoutRoot: aspect::layout_root_target(words[0], words[1] != 0); break;
      }
      break;
    }
  }
public:
  // (stage S5) as stereo_flush on the render thread: eye 1's pass ends at the end of its held halves, eye 0's pass ends,
  // and the halves follow eye 0's commands in the part (the second drawing thread's: in its own stream)
  void flush_eye1() {
    if (!eye1_held()) return;
    as_eye1([] { end_encoder(); });
    end_encoder();
    if (b_) stereo_part_b_append_eye1();
    else stereo_part_append_eye1();
  }
  bool eye1_held() const { return b_ ? stereo_part_b_eye1_held() : stereo_part_eye1_held(); }
  static inline thread_local Worker* t_worker = nullptr;  // the calling drawing thread's
private:
  // (stage S6) both eyes' passes end, then the eye 1 halves follow (as flush_eye1, even with none held)
  void end_both() {
    as_eye1([] { end_encoder(); });
    end_encoder();
    if (eye1_held()) {
      if (b_) stereo_part_b_append_eye1();
      else stereo_part_append_eye1();
    }
  }
  // (stage S6.3, the second drawing thread) a draw beside A1: undone and drawn again after A1 if it would change what
  // A1 may still use while holding a cache lock (b_before_change)
  void draw_b(const Job& j) {
    for (;;) {
      BDraw d;
      d.eye0Stream = eye_deferred_commands();
      d.eye1Stream = eye1Stream;
      d.eye0 = d.eye0Stream->mark();
      d.eye1 = d.eye1Stream->mark();
      const PassState eye0Pass = pass_state(), eye1Pass = eye1Pass_;
      t_bDraw = &d;
      try {
        draw_both(j);
        t_bDraw = nullptr;
        g_split.bDraws.fetch_add(1, std::memory_order_relaxed);
        return;
      } catch (const NeedA1&) {
        t_bDraw = nullptr;  // (eye 0's selection, stream and pass are back: as_eye1's scope ended on the way out)
        stereoShadersOut() = nullptr;
        stereoShadersIn() = nullptr;
        d.eye0Stream->rollback(d.eye0);
        d.eye1Stream->rollback(d.eye1);
        pass_state() = eye0Pass;
        eye1Pass_ = eye1Pass;
        g_split.undone.fetch_add(1, std::memory_order_relaxed);
        g_split.spanBWaitNs.fetch_add(split_wait(&Split::a1Done, g_split.bWaitNs), std::memory_order_relaxed);
        ++t_partEpochSalt;  // (its undone draw's state is in no stream)
        forget_continued();
        machine().note_full_state_change();
      } catch (...) { t_bDraw = nullptr; throw; }
    }
  }
  // a draw (j.a[5]: its instances), both halves: the second drawing thread's undoable, the scene thread's timed
  void draw_one(const Job& j) {
    if (b_) {
      draw_b(j);
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    struct Timed {
      std::chrono::steady_clock::time_point t0;
      ~Timed() {
        g_threadDrawNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        g_threadDraws.fetch_add(1, std::memory_order_relaxed);
      }
    } timed{t0};
    draw_both(j);
  }
  // (docs/VR_SCENE_THREADS.md, "S6: batched runs on the drawing threads") a held run the render thread handed over
  // whole (words: each piece's 16 vertex-shader uniform block addresses; j.u: the pieces, and the blocks that differ
  // above bit 32): one instanced draw per chunk when batch_accept takes it, as draw_stereo draws one on the render
  // thread, else piece by piece with each piece's addresses (as batch_draw_run on the atlas's thread)
  void draw_run(const Job& j, const uint32_t* words) {
    if (!run_) run_ = std::make_unique<draw_batch::Batch>();
    draw_batch::Batch& run = *run_;
    run.count = uint32_t(j.u);
    run.blocks = uint32_t(j.u >> 32);
    std::memcpy(run.addrs, words, size_t(run.count) * 16 * 4);
    uint32_t* const r = regs();
    uint32_t saved[16];
    for (uint32_t i = 0; i < 16; ++i) saved[i] = r[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7];
    auto set = [&](const uint32_t* addrs) {
      for (uint32_t i = 0; i < 16; ++i) r[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7] = addrs[i];
    };
    struct Restore {
      uint32_t* saved;
      std::function<void(const uint32_t*)> set;
      ~Restore() { batch_draw() = nullptr; set(saved); }
    } restore{saved, set};
    set(run.addrs[0]);
    Job piece = j;
    BatchDraw b;
    if (batch_accept(r, run, b, true)) {
      for (uint32_t first = 0; first < run.count; first += b.layout->pieces) {
        b.first = first;
        b.count = std::min(b.layout->pieces, run.count - first);
        piece.a[5] = b.count;
        batch_draw() = &b;
        draw_one(piece);
        batch_draw() = nullptr;
      }
      return;
    }
    piece.a[5] = 1;
    for (uint32_t k = 0; k < run.count; ++k) {
      set(run.addrs[k]);
      draw_one(piece);
    }
  }
  void draw_both(const Job& j) {
    out_.reset();
    bool shared = false;
    stereoShadersOut() = &out_;
    try {
      gfxvk::draw(regs(), j.a[0], j.a[1], j.a[2], j.a[3], j.a[4], j.a[5], &shared);
    } catch (...) { stereoShadersOut() = nullptr; throw; }
    stereoShadersOut() = nullptr;
    if (shared || out_.multiview) return;
    as_eye1([&] {
      stereoShadersIn() = out_.vs ? &out_ : nullptr;
      try {
        gfxvk::draw(regs(), j.a[0], j.a[1], j.a[2], j.a[3], j.a[4], j.a[5]);
      } catch (...) { stereoShadersIn() = nullptr; throw; }
      stereoShadersIn() = nullptr;
    });
  }
  // a guest struct copied with the job, in guest memory of this thread's own for the operation to read
  uint32_t scratch(int slot, const uint32_t* words, uint32_t count) {
    if (!scratch_) scratch_ = mem::host_alloc(2 * 0x100, 0x40);
    const uint32_t addr = scratch_ + uint32_t(slot) * 0x100;
    std::memcpy(mem::ptr(addr), words, count * 4);
    return addr;
  }
  // as stereo_pair and StereoInOrder on the render thread: eye 1's held halves first, then each eye in turn in the
  // part's stream, ending its pass
  template <class F> void in_order(F&& f) {
    flush_eye1();
    stereo_thread_in_order(true);
    struct Scope { ~Scope() { stereo_thread_in_order(false); } } scope;
    end_encoder();
    f();
    end_encoder();
    {
      PassState* const eye0Pass = &pass_state();
      set_thread_pass_state(&eye1Pass_);
      stereoEye() = 1;
      struct Back {
        PassState* pass;
        ~Back() { stereoEye() = 0; set_thread_pass_state(pass); }
      } back{eye0Pass};
      end_encoder();  // (eye 1's own pass, if one is open, ends before it: flush_eye1 left none)
      f();
      end_encoder();
    }
  }
  StereoShaders out_;
  std::unique_ptr<draw_batch::Batch> run_;  // (draw_run: the held run being drawn, 64 KiB)
  PassState eye1Pass_;
  uint32_t phaseCounter_ = 0;
  bool pinned_ = false;
  uint32_t scratch_ = 0;
  std::vector<Renderer::UploadBlock>* uploads_;
  VkDescriptorPool* pool_;
  const char* name_;
  bool b_;
  stereo::LayoutState layout_;  // (the second drawing thread's: stereo::t_layoutOverride)
  bool contentClip_ = false;
};
Worker& worker() {
  // (eye 1's thread's arena and pool: it never runs with this one)
  static Worker w(&R.eyeUploadBlocks, &R.eyeDescriptorPool, "scene");
  return w;
}
// (stage S6) the second drawing thread: the main geometry run's later draws, beside the scene's thread's
Worker& worker_b() {
  static Worker w(&R.scene2UploadBlocks, &R.scene2DescriptorPool, "scene B", true);
  return w;
}
bool g_running = false;
// (S6 tuning) the textures the second drawing thread sampled: its span's (on it), and its last span's (render thread)
struct NotedTexture {
  uint32_t words[7];
  bool depth;
  uint32_t maxLevels;
  Surface* surface;
};
std::vector<NotedTexture> g_bTextures, g_bLastTextures;
std::unordered_set<const Surface*> g_bNoted;  // (on it: one note a surface)
bool still_cached(const Surface* s, uint32_t addr) {
  auto range = surfaces_of(0).equal_range(addr);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second.get() == s) return true;
  return false;
}
// (render thread, a part's start) as the atlas's thread's begin: the second drawing thread's textures of last time,
// their uploads (if they changed) and their layouts for sampling, before the part's commands
void prepare_b_textures() {
  if (g_bLastTextures.empty()) return;
  end_encoder();
  for (auto& t : g_bLastTextures) {
    const uint32_t addr = t.words[2] << 8;
    if (!still_cached(t.surface, addr) && !still_cached(t.surface, addr & ~0x700u)) continue;
    if (Surface* s = sampled_texture(t.words, t.depth, t.maxLevels)) {
      transition_image(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
      g_split.prepared.fetch_add(1, std::memory_order_relaxed);
    }
  }
  end_encoder();
}
// (stage S6, render thread) the layout state as handed to the scene's thread so far (it applies it later, in order):
// the second drawing thread's copy at its switch
stereo::LayoutState g_handed;
bool g_handedClip = false;
// (stage S6, render thread) the second drawing thread's span: the main geometry run's later half, last frame's
bool g_bActive = false, g_bUsed = false;
uint32_t g_runFirst = 0, g_runEnd = 0;
// (docs/VR_SCENE_THREADS.md, "S6: the split balanced") where in the run the second drawing thread's span starts, as a
// share of it: half to start, then balanced by A's wait for it at A1's end (later after a wait over 0.1 ms, earlier
// otherwise), within 0.3 to 0.8. A1 ran out of work while B still drew (A waited 0.5 to 1.6 ms a frame in the heavy
// views, and A2 started only after it). WWHD_VK_SCENE_TWO_START fixes it
float g_bStart = 0.5f;
const bool kBStartFixed = [] {
  const char* e = std::getenv("WWHD_VK_SCENE_TWO_START");
  if (e) g_bStart = std::clamp(float(std::atof(e)), 0.1f, 0.9f);
  return e != nullptr;
}();
constexpr uint32_t kMinBDraws = 64;  // (a shorter span is not worth the switch)
void switch_to_b() {
  auto& a = worker();
  auto& b = worker_b();
  a.hand();  // (A1: every job so far)
  draw_threads::stop_forwarding(&a);
  {
    std::lock_guard<std::mutex> lock(g_split.mutex);
    g_split.a1Done = false;
    g_split.bDone = false;
  }
  g_split.a1Over.store(false, std::memory_order_release);
  ++g_split.span;  // (before the hand-over: the second drawing thread reads it after)
  b.pass = {};
  b.eye1Stream = stereo_part_b_eye1_stream();
  b.begin(stereo_part_b_stream());  // (its machine loaded from the render thread's registers, forwarded to from now)
  DrawThread::Job pass{kPass};
  pass.a[0] = g_handed.drawDepth;
  pass.a[1] = g_handed.photograph;
  pass.a[2] = g_handedClip;
  b.add(pass);
  g_bActive = g_bUsed = true;
  g_split.spans.fetch_add(1, std::memory_order_relaxed);
}
void switch_back() {
  if (!g_bActive) return;
  g_bActive = false;
  auto& a = worker();
  auto& b = worker_b();
  b.add(DrawThread::Job{kEndB});
  b.finish_async(DrawThread::Job{kDoneB});
  a.add(DrawThread::Job{kEndA1});  // (A1's end; then A waits for B, and B's commands follow)
  a.load_regs(gx2::render_regs());  // (A2 from the registers as they are now)
  a.note_full_state_change();
  draw_threads::forward_to(&a);
  a.hand();
}
}  // namespace
bool enabled() {
  static const bool on = [] {
    if (!switches::kSceneThread) return false;
    if (eye_thread::enabled()) {
      LOG("[vulkan scene thread] off: eye 1's thread is on (it shares its upload arena and descriptor pool)");
      return false;
    }
    LOG("[vulkan scene thread] on (docs/VR_SCENE_THREADS.md, stage S4: the scene's parts beside the render thread)");
    if (switches::kSceneTwo) {
      worker_b();  // (stage S6.1: made, idle)
      LOG("[vulkan scene thread] a second drawing thread made (stage S6.1: idle)");
    }
    return true;
  }();
  return on;
}
bool running() { return g_running; }
bool on_scene_thread() { return per_thread::possible() && onSceneThreadTls; }
bool on_scene_b() { return (switches::kSceneTwo || switches::kSceneFront) && onSceneBTls; }
bool eye1_held() { return Worker::t_worker ? Worker::t_worker->eye1_held() : stereo_part_eye1_held(); }
uint64_t& b_eye_generation() { return g_bEyeGeneration; }
// ("S6: B's reads deferred") the second drawing thread changed a surface it had sampled in this span otherwise: its
// earlier draws read it in the layout the scene's thread makes it at the span's start, which then no longer holds. The
// split is off from the next span on (this frame's reads may be wrong: logged)
thread_local int t_wantLayout = -1;
thread_local uint32_t t_wantAccess = 0;
void b_pre_conflict(Surface* s, const char* what) {
  g_split.preConflicts.fetch_add(1, std::memory_order_relaxed);
  if (!g_split.off.exchange(true))
    LOG("[vulkan scene thread] the second drawing thread off for the session: %s of a surface it sampled before A1's end "
        "(%ux%u at %08X)", what, s->width, s->height, s->addr);
}
void b_before_change(Surface* s, const char* what) {
  // (rendering into a target: the main pass's own, which both threads only draw into)
  if (!std::strncmp(what, "its contents", 12)) return;
  if (s->bPreRead == g_split.span && std::strcmp(what, "a layout")) b_commit_read(s);  // (a layout: b_defer_layout)
  if (split_is(&Split::a1Done)) return;
  {
    // (diagnostics) the first few that make it wait: which surface
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 12)
      LOG("[vulkan scene thread] the second drawing thread waits for A1 before %s: %ux%u at %08X format %X depth %d "
          "layout %d eye %u layers %u/%u mips %u target %d dirty %d, draw %u, %u locks; wanted %d access %X", what, s->width, s->height,
          s->addr, s->format, int(s->isDepth), int(s->layout), unsigned(s->eye), s->baseLayer, s->imageLayers, s->mips,
          int(s->gpuWritten), int(s->dirty), t_drawIndex, unsigned(draw_threads::held_locks()), t_wantLayout, t_wantAccess);
  }
  g_split.why[!std::strcmp(what, "a layout") ? 0 : !std::strcmp(what, "an upload") ? 1
              : !std::strncmp(what, "its image", 9) ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
  // no cache lock held: A1 never waits for this thread, so wait here
  if (draw_threads::held_locks() == 0) {
    g_split.waitsInPlace.fetch_add(1, std::memory_order_relaxed);
    g_split.spanBWaitNs.fetch_add(split_wait(&Split::a1Done, g_split.bWaitNs), std::memory_order_relaxed);
    return;
  }
  // inside a lock A1 may want: the draw is undone, and drawn again after A1
  if (t_bDraw && t_bDraw->can_undo()) throw NeedA1{};
  // (neither: the change goes after A1's commands although A1 may use it; the split is off from the next span on)
  g_split.conflicts.fetch_add(1, std::memory_order_relaxed);
  if (!g_split.off.exchange(true))
    LOG("[vulkan scene thread] the second drawing thread off for the session: %s inside a lock, with nothing to undo",
        what);
}
Split::PreRead* b_pre_entry(const Surface* s) {
  for (auto& p : g_split.preReads)
    if (p.s == s) return &p;
  return nullptr;
}
int b_layout(const Surface* s) {
  if (s->bPreRead == g_split.span)
    if (const Split::PreRead* p = b_pre_entry(s)) return p->layout;
  return int(s->layout);
}
bool b_defer_layout(Surface* s, int layout, uint32_t stage, uint32_t access) {
  t_wantLayout = layout, t_wantAccess = access;  // (diagnostics: the wait's log line)
  if (s->bPreRead == g_split.span) {
    // already made so at the span's start: nothing to change; another change follows that, in its own commands
    const Split::PreRead* p = b_pre_entry(s);
    if (p && p->layout == layout && p->access == access) return true;
    b_commit_read(s);
    return false;
  }
  // a read, or drawing into it (colour, depth): made so at the span's start. After A1's end it changes the layout
  // itself; a surface in neither eye's map (a scan, a temporary) is not found again at A1's end: it waits, as before
  const bool read = layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && access == VK_ACCESS_SHADER_READ_BIT;
  if (!read && layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    return false;
  if (g_split.a1Over.load(std::memory_order_acquire) || s->eye > 1) return false;
  s->bPreRead = g_split.span;
  if (read) stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  g_split.preReads.push_back({s, s->addr, s->eye, false, false, {}, 0, layout, stage, access});
  g_split.deferred.fetch_add(1, std::memory_order_relaxed);
  return true;
}
bool b_defer_adopt(Surface* s) {
  const bool pending = s->bPreRead == g_split.span;
  if (!pending && (g_split.a1Over.load(std::memory_order_acquire) || s->eye > 1)) return false;
  if (pending)
    if (const Split::PreRead* p = b_pre_entry(s); p && p->layout != int(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
      return false;  // (drawn into: as before)
  if (!pending) {
    s->bPreRead = g_split.span;
    g_split.preReads.push_back({s, s->addr, s->eye, true, false, {}, 0, int(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), uint32_t(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), uint32_t(VK_ACCESS_SHADER_READ_BIT)});
    g_split.deferred.fetch_add(1, std::memory_order_relaxed);
  } else
    for (auto& p : g_split.preReads)
      if (p.s == s) p.adopt = true;
  return true;
}
bool b_defer_chain(Surface* s, const SurfaceDesc& d, uint32_t maxLevels) {
  const bool pending = s->bPreRead == g_split.span;
  if (!pending && (g_split.a1Over.load(std::memory_order_acquire) || s->eye > 1)) return false;
  if (!pending) {
    s->bPreRead = g_split.span;
    g_split.preReads.push_back({s, s->addr, s->eye, false, true, d, maxLevels, int(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL), uint32_t(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), uint32_t(VK_ACCESS_SHADER_READ_BIT)});
    g_split.deferred.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  for (auto& p : g_split.preReads)
    if (p.s == s) {
      if (p.chain && p.d.mips != d.mips) return false;  // (another chain of it: as before)
      if (p.layout != int(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return false;  // (drawn into: as before)
      p.maxLevels = p.chain ? std::max(p.maxLevels, maxLevels) : maxLevels;
      p.d = d;
      p.chain = true;
    }
  return true;
}
void b_commit_read(Surface* s) {
  // (the second drawing thread) a surface its earlier draws sampled, about to change otherwise: after A1's end (waited
  // for, or the draw undone and drawn again after it, as for any change), the surface made readable now in the
  // layouts its span starts with, and its change follows in its own commands
  if (!g_split.a1Over.load(std::memory_order_acquire)) {
    g_split.why[3].fetch_add(1, std::memory_order_relaxed);
    if (draw_threads::held_locks() == 0) {
      g_split.waitsInPlace.fetch_add(1, std::memory_order_relaxed);
      g_split.spanBWaitNs.fetch_add(split_wait(&Split::a1Done, g_split.bWaitNs), std::memory_order_relaxed);
    } else if (t_bDraw && t_bDraw->can_undo()) {
      throw NeedA1{};
    } else {
      b_pre_conflict(s, "a layout");
      return;
    }
  }
  s->bPreRead = 0;  // (no longer deferred: its layout is changed below and then followed)
  const Split::PreRead* entry = b_pre_entry(s);
  stereo_release_texture(s);  // (its eye 1 halves that sample it, and its pass, end in its own commands first)
  end_encoder();
  DeferredCommands* const previous = eye_deferred_commands();
  eye_deferred_commands() = stereo_part_b_pre_stream();
  struct Back { DeferredCommands* p; ~Back() { eye_deferred_commands() = p; } } back{previous};
  if (entry && entry->adopt) adopt_newer_alias_now(s);
  if (entry && entry->chain) with_mip_chain_now(s, entry->d, entry->maxLevels);
  if (entry) transition_image(s, VkImageLayout(entry->layout), entry->stage, entry->access);
}
void b_commit_reads() {
  // (the scene's thread at A1's end, the second drawing thread done: its list is complete) each surface readable,
  // recorded after A1's commands, as the second thread's first draw that samples it would have
  for (const auto& p : g_split.preReads) {
    bool cached = false;  // (still in its eye's map: not destroyed since)
    {
      draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());
      auto range = surfaces_of(p.eye).equal_range(p.addr);
      for (auto it = range.first; it != range.second && !cached; ++it) cached = it->second.get() == p.s;
    }
    if (!cached || p.s->bPreRead != g_split.span) continue;  // (or made readable by the second thread itself since)
    if (p.adopt) adopt_newer_alias_now(p.s);
    if (p.chain) with_mip_chain_now(p.s, p.d, p.maxLevels);
    transition_image(p.s, VkImageLayout(p.layout), p.stage, p.access);
  }
  g_split.preReads.clear();
}
void note_texture(const uint32_t* words, bool isDepthSampler, uint32_t maxLevels, Surface* s) {
  if (!g_bNoted.insert(s).second) return;
  NotedTexture t{};
  std::memcpy(t.words, words, sizeof t.words);
  t.depth = isDepthSampler;
  t.maxLevels = maxLevels;
  t.surface = s;
  g_bTextures.push_back(t);
}
void set_main_run(uint32_t first, uint32_t end) {
  g_runFirst = first;
  g_runEnd = end;
  // (the split's balance: from the last span's wait, once)
  // B waiting for A1 (a surface A1 still changes, nearer A1's end the later B starts) moves it earlier; A waiting for B
  // later; neither: a little earlier, B taking more (the headset run of 2026-11-03: by A's wait alone it went to 80 %
  // and B then waited for A1 0.8 to 1.7 ms a frame)
  const int64_t aWaited = g_split.lastAWaitNs.exchange(-1, std::memory_order_acquire);
  const int64_t bWaited = g_split.lastBWaitNs.load(std::memory_order_relaxed);
  if (aWaited >= 0 && !kBStartFixed) {
    const float step = bWaited > aWaited + 100000 ? -0.02f : aWaited > 100000 ? 0.02f : -0.01f;
    g_bStart = std::clamp(g_bStart + step, 0.3f, 0.8f);
  }
}
void log_b(double frames, double runDraws) {
  const auto take = [](std::atomic<uint64_t>& v) { return double(v.exchange(0, std::memory_order_relaxed)); };
  const double spans = take(g_split.spans), draws = take(g_split.bDraws), inPlace = take(g_split.waitsInPlace),
               undone = take(g_split.undone), conflicts = take(g_split.conflicts), bWait = take(g_split.bWaitNs),
               aWait = take(g_split.aWaitNs), layouts = take(g_split.why[0]), uploads = take(g_split.why[1]),
               images = take(g_split.why[2]), others = take(g_split.why[3]), prepared = take(g_split.prepared),
               deferred = take(g_split.deferred), preConflicts = take(g_split.preConflicts);
  LOG("[vulkan scene thread] second drawing thread, per frame: %.1f surfaces' reads made ready at A1's end, %.2f "
      "conflicts with them", deferred / frames, preConflicts / frames);
  LOG("[vulkan scene thread] second drawing thread%s, per frame: %.2f spans, %.0f draws (main geometry run %.0f "
      "draws, its span from %.0f%% of it%s); it waited for A1 %.2f ms (%.2f times in place, %.2f draws undone), A "
      "waited for it %.2f ms; %.2f conflicts", kTwoSequential ? " (after A1, one at a time)" : "", spans / frames,
      draws / frames, runDraws, double(g_bStart) * 100.0, kBStartFixed ? ", fixed" : "", bWait / 1e6 / frames,
      inPlace / frames, undone / frames, aWait / 1e6 / frames, conflicts / frames);
  LOG("[vulkan scene thread] second drawing thread's waits for A1 per frame, before: a layout %.2f, an upload %.2f, an "
      "image destroyed %.2f, other %.2f; %.1f textures made ready at the parts' starts", layouts / frames,
      uploads / frames, images / frames, others / frames, prepared / frames);
}
void begin(DeferredCommands* stream, DeferredCommands* eye1Stream) {
  auto& w = worker();
#ifdef _WIN32
  if (kSameL3 && !g_l3Mask.load(std::memory_order_relaxed)) {
    if (const uint64_t mask = l3_mask_of_current_processor()) {
      SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(mask));
      g_l3Mask.store(mask, std::memory_order_relaxed);
      LOG("[vulkan scene thread] the render thread and the scene's thread on the L3 group %016llX", (unsigned long long)mask);
    }
  }
#endif
  // (S6 tuning) the second drawing thread's textures of last time, ready before the part (its eye 0 pass ends first)
  if (switches::kSceneTwo && !g_split.off.load(std::memory_order_relaxed)) prepare_b_textures();
  w.pass = pass_state();
  w.eye1Stream = eye1Stream;
  // (stage S6) the layout state as it is now, before any is handed over (the second drawing thread's copy follows it)
  g_handed = {stereo::layoutDrawDepth, stereo::layoutPhotograph};
  g_handedClip = aspect::content_clip();
  w.eye1Drawn.store(false, std::memory_order_relaxed);
  w.begin(stream);
  w.add(DrawThread::Job{kPass});
  g_running = true;
}
// (render thread) a draw handed over: both its halves are drawn there. False (nothing handed) when the part must end
// first: a draw of it was drawn as two
bool draw(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances) {
  // (stage S6) the main geometry run's later half to the second drawing thread: from the middle of last frame's run,
  // back to the scene's thread at its end
  if (switches::kSceneTwo) {
    if (g_bActive && t_drawIndex >= g_runEnd) {
      switch_back();
      scene_tail_span_end(t_drawIndex);
      if (g_sceneTailNext) {  // (the part ends here: the caller joins it and draws this draw itself)
        g_sceneTailNow = true;
        return false;
      }
    }
    if (!g_bActive && !g_bUsed && !g_split.off.load(std::memory_order_relaxed) && g_runEnd >= g_runFirst + 2 * kMinBDraws &&
        t_drawIndex >= g_runFirst + uint32_t(g_bStart * float(g_runEnd - g_runFirst)) && t_drawIndex + kMinBDraws <= g_runEnd)
      switch_to_b();
  }
  // (a chunk of a held run drawn here, draw_stereo's: never handed over without its run)
  if (batch_draw()) return false;
  auto& w = g_bActive ? worker_b() : worker();
  DrawThread::Job j{t_sceneRun ? kDrawRun : kDraw};
  j.a[0] = prim; j.a[1] = count; j.a[2] = indexType; j.a[3] = indexAddr; j.a[4] = baseVertex; j.a[5] = instances;
  j.a[6] = t_drawIndex;
  if (const draw_batch::Batch* run = t_sceneRun) {  // (docs/VR_SCENE_THREADS.md, "S6: batched runs on the drawing threads")
    j.u = uint64_t(run->count) | uint64_t(run->blocks) << 32;
    w.add(j, &run->addrs[0][0], run->count * 16);
    t_sceneRunTaken = true;
  } else
    w.add(j);
  g_partOverlapping = true;  // (from the first hand-over on the render thread records nothing: it joins first)
  w.hand();
  return true;
}
// (render thread, stage S5) state for the draws handed over after it, applied there in order: false if no part runs
bool hand_state(int kind, const uint32_t* words, uint32_t n) {
  if (!g_running) return false;
  switch_back();  // (stage S6: the second drawing thread's span is draws only)
  if (kind == render::kStateLayoutContent) {  // (as the scene's thread will apply it: the switch's copy, g_handed)
    g_handedClip = n && words[0];
    if (n >= 2) {
      if (words[1]) ++g_handed.drawDepth;
      else if (g_handed.drawDepth) --g_handed.drawDepth;
    }
    g_handed.photograph = n >= 3 && words[2];
  }
  DrawThread::Job j{kState};
  j.a[0] = uint32_t(kind);
  worker().add(j, words, n);
  return true;
}
void flush_eye1() { (Worker::t_worker ? *Worker::t_worker : worker()).flush_eye1(); }
uint64_t busy_ns() { return g_threadDrawNs.load(std::memory_order_relaxed); }
namespace {
bool hand_in_order(DrawThread::Job j, const uint32_t* words, uint32_t n) {
  if (!g_running) return false;
  switch_back();  // (stage S6: the second drawing thread's span is draws only)
  worker().add(j, words, n);
  g_partOverlapping = true;
  worker().hand();
  return true;
}
}  // namespace
bool clear_color(const uint32_t* colorBuffer, const float* rgba) {
  uint32_t w[kColorBufferWords + 4];
  std::memcpy(w, colorBuffer, kColorBufferWords * 4);
  std::memcpy(w + kColorBufferWords, rgba, 16);
  return hand_in_order(DrawThread::Job{kClearColor}, w, kColorBufferWords + 4);
}
bool clear_depth_stencil(const uint32_t* depthBuffer, float depth, uint32_t stencil, uint32_t flags) {
  DrawThread::Job j{kClearDepth};
  std::memcpy(&j.f, &depth, sizeof depth);
  j.a[0] = stencil;
  j.a[1] = flags;
  return hand_in_order(j, depthBuffer, kDepthBufferWords);
}
bool copy_surface(const uint32_t* src, uint32_t srcMip, uint32_t srcSlice, const uint32_t* dst, uint32_t dstMip,
                  uint32_t dstSlice) {
  uint32_t w[2 * kSurfaceWords];
  std::memcpy(w, src, kSurfaceWords * 4);
  std::memcpy(w + kSurfaceWords, dst, kSurfaceWords * 4);
  DrawThread::Job j{kCopy};
  j.a[0] = srcMip; j.a[1] = srcSlice; j.a[2] = dstMip; j.a[3] = dstSlice;
  return hand_in_order(j, w, 2 * kSurfaceWords);
}
bool copy_to_scan(const uint32_t* colorBuffer, uint32_t target) {
  DrawThread::Job j{kScan};
  j.a[0] = target;
  return hand_in_order(j, colorBuffer, kColorBufferWords);
}
void finish() {
  if (!g_running) return;
  struct End { ~End() { g_running = false; } } end;
  switch_back();  // (stage S6: A1 ends, A waits for the second drawing thread, its commands follow)
  worker().add(DrawThread::Job{kEndEye1});
  worker().finish();
  if (g_bUsed) {
    g_bUsed = false;
    worker_b().complete();  // (done before A was: A waited for it)
    g_bLastTextures.swap(g_bTextures);  // (S6 tuning: made ready at the next part's start)
    g_bTextures.clear();
    g_bNoted.clear();
  }
}
// ---------------------------------------------------------------- stage S7: the pass's front on the second drawing thread
// (docs/VR_SCENE_THREADS.md, stage S7; WWHD_VK_SCENE_FRONT=1, stereo.h) S6's second drawing thread (worker_b) draws the
// front of the largest two-eye pass while the render thread reads on and then draws the rest itself, into the part. The
// render thread draws in S6 drew nothing: it only read and handed over, and waited for the threads at the frame's end
namespace {
struct Front {
  uint32_t passFirst = 0, passEnd = 0;  // last frame's largest two-eye pass, in scene draws: [first, end)
  uint32_t spanEnd = 0;                 // this frame's: the thread's span ends before this draw
  float share = 0.5f;                   // the thread's share of the pass, balanced by the joins' waits
  bool span = false;                    // (render thread) the thread's span runs: the render thread records nothing
  bool overlap = false;                 // (render thread) the thread's span ran in this part, not joined yet
  uint32_t targets[9] = {};             // the pass's colour bases and depth base, at the span's start
  uint64_t nextId = 0;
  std::atomic<uint64_t> id{0};          // the overlap's number (the surfaces' marks), 0 while none runs
  std::atomic<bool> off{false};         // (a conflict: off for the session)
  // (diagnostics, since the last log line)
  bool tried = false;                   // (render thread) this frame's span started, or the run is past
  // (render thread) this frame's stretches of consecutive draws into the same targets: the current one, the longest
  uint64_t stretchKey = 0;
  uint32_t stretchFirst = 0, bestFirst = 0, bestEnd = 0;
  uint64_t stretchLen = 0;  // (diagnostics) the longest stretches' draws
  uint64_t spans = 0, threadDraws = 0, renderDraws = 0, waitNs = 0, skipped = 0, otherTargets = 0, sampled = 0;
  uint64_t notYet[4] = {};  // (diagnostics) draws of the run it could not start at: eye 1 held, the atlas's thread, a
                            // held stream, other
  std::atomic<uint64_t> conflicts{0};
} g_front;
const uint32_t kFrontMinDraws = [] {
  const char* e = std::getenv("WWHD_VK_SCENE_FRONT_MIN_DRAWS");
  return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 256u;
}();
constexpr uint32_t kFrontSideDraws = 64;  // (each side of the split draws at least this many)
const bool kFrontFixedShare = std::getenv("WWHD_VK_SCENE_FRONT_SHARE") != nullptr;
[[maybe_unused]] const bool g_frontShareSet = [] {
  if (const char* e = std::getenv("WWHD_VK_SCENE_FRONT_SHARE")) g_front.share = std::clamp(float(std::atof(e)), 0.05f, 0.95f);
  return true;
}();
// the draw renders into the pass's targets
bool front_same_targets(const uint32_t* r) {
  for (uint32_t i = 0; i < 8; ++i)
    if (r[mmCB_COLOR0_BASE + i] != g_front.targets[i]) return false;
  return r[mmDB_DEPTH_BASE] == g_front.targets[8];
}
// the draw's targets as one key: the frame's stretches of draws into the same targets (front_draw)
uint64_t front_targets_key(const uint32_t* r) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < 8; ++i) h = (h ^ r[mmCB_COLOR0_BASE + i]) * 0x100000001B3ull;
  return (h ^ r[mmDB_DEPTH_BASE]) * 0x100000001B3ull;
}
// the draw may sample one of the pass's targets (every stage's texture units, bound by its shaders or not)
bool front_samples_targets(const uint32_t* r) {
  for (uint32_t base : {uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                        uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                        uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
    for (uint32_t unit = 0; unit < 18; ++unit) {
      const uint32_t addr = r[base + unit * 7 + 2];
      if (!addr) continue;
      // (the texture word holds the address >> 8; the target registers may hold it either way: both compared, a
      // macro-tiled swizzle aside, as the atlas's thread does)
      for (uint32_t t : g_front.targets)
        if (t && (((addr ^ t) & ~0x7u) == 0 || (((addr << 8) ^ t) & ~0x7FFu) == 0)) return true;
    }
  return false;
}
void front_conflict(const Surface* s, const char* what, const char* who) {
  g_front.conflicts.fetch_add(1, std::memory_order_relaxed);
  if (!g_front.off.exchange(true))
    LOG("[vulkan scene front] off for the session: %s changed a surface the other thread used in this overlap (%s, "
        "%ux%u at %08X)", who, what, s->width, s->height, s->addr);
}
// (render thread) the span's start, at the pass's first draw: the render thread's own pass ends in its stream, the
// thread's textures of last time are made ready there (before the thread's commands), and the render thread's commands
// from now on are held in the part, after the thread's
bool front_begin(const uint32_t* r) {
  auto& f = g_front;
  // (not yet: tried again at the run's next draw)
  const int notYet = stereo_eye1_holding() ? 0 : atlas_thread::running() || stereo_scene_holding() ? 1
                   : stereo_atlas_holding() || stereo_part_holding() ? 2
                   : !stereo_streaming() || stereoEye() != 0 || eye_thread::running() || draw_batch::current() ||
                     eye_deferred_commands() != nullptr ? 3 : -1;
  if (notYet >= 0) {
    ++f.notYet[notYet];
    return false;
  }
  end_encoder();
  prepare_b_textures();
  stereo_part_hold();
  if (!stereo_part_holding()) {
    ++f.notYet[3];
    return false;
  }
  f.tried = true;
  // (above the atlas's overlap numbers: the surfaces' atlas marks are never this overlap's)
  f.id.store((uint64_t(1) << 62) | ++f.nextId, std::memory_order_release);
  for (uint32_t i = 0; i < 8; ++i) f.targets[i] = r[mmCB_COLOR0_BASE + i];
  f.targets[8] = r[mmDB_DEPTH_BASE];
  {
    std::lock_guard<std::mutex> lock(g_split.mutex);
    g_split.bDone = false;
  }
  auto& b = worker_b();
  b.pass = {};
  b.eye1Stream = stereo_part_b_eye1_stream();
  b.begin(stereo_part_b_stream());  // (its machine loaded from the render thread's registers, forwarded to from now)
  DrawThread::Job pass{kPass};
  pass.a[0] = stereo::layoutDrawDepth;  // (the layouts' state as it is now: the thread's copy for its span)
  pass.a[1] = stereo::layoutPhotograph;
  pass.a[2] = aspect::content_clip();
  b.add(pass);
  g_partOverlapping = true;  // (the render thread records nothing while the span runs: it ends the span first)
  f.span = f.overlap = true;
  ++f.spans;
  // (from here to the run's end: the thread's share, at least kFrontSideDraws on each side)
  const uint32_t len = f.passEnd - t_drawIndex;
  f.spanEnd = t_drawIndex + std::clamp(uint32_t(f.share * float(len)), kFrontSideDraws, len - kFrontSideDraws);
  return true;
}
}  // namespace
bool front_span() { return g_front.span; }
bool front_overlapping() { return g_front.overlap; }
bool front_draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                uint32_t baseVertex, uint32_t instances) {
  auto& f = g_front;
  if (draw_threads::on_draw_thread()) return false;
  // next frame's split: the frame's longest stretch of draws into the same targets (front_frame_end)
  if (const uint64_t key = front_targets_key(r); key != f.stretchKey) {
    if (t_drawIndex - f.stretchFirst > f.bestEnd - f.bestFirst) f.bestFirst = f.stretchFirst, f.bestEnd = t_drawIndex;
    f.stretchKey = key;
    f.stretchFirst = t_drawIndex;
  }
  if (f.span) {
    // (the span is the pass's front: a draw into other targets ends it, as the split point does)
    if (t_drawIndex < f.spanEnd && front_same_targets(r)) {
      DrawThread::Job j{kDraw};
      j.a[0] = prim; j.a[1] = count; j.a[2] = indexType; j.a[3] = indexAddr; j.a[4] = baseVertex; j.a[5] = instances;
      j.a[6] = t_drawIndex;
      auto& b = worker_b();
      b.add(j);
      b.hand();
      ++f.threadDraws;
      return true;
    }
    front_end_span();
  }
  if (f.overlap) {
    // the render thread's own draws beside the thread: into the pass's targets, sampling none of them; else the
    // threads join first (outside every cache lock: here, before the draw)
    const bool other = !front_same_targets(r);
    if (other || front_samples_targets(r)) {
      ++(other ? f.otherTargets : f.sampled);
      stereo_part_join(AtlasEmit::Sampled);
    } else ++f.renderDraws;
    return false;
  }
  // the span starts at the run's first draw it can (last frame's run), while enough of the run is left
  if (f.tried || t_drawIndex < f.passFirst || f.off.load(std::memory_order_relaxed)) return false;
  if (t_drawIndex + std::max(kFrontMinDraws / 2, 2 * kFrontSideDraws) > f.passEnd) {
    f.tried = true;
    ++f.skipped;
    return false;
  }
  if (!front_begin(r)) return false;
  return front_draw(r, prim, count, indexType, indexAddr, baseVertex, instances);
}
void front_end_span() {
  auto& f = g_front;
  if (!f.span) return;
  f.span = false;
  g_partOverlapping = false;
  auto& b = worker_b();
  b.add(DrawThread::Job{kEndB});  // (its passes end, its eye 1 halves follow its eye 0 commands)
  b.finish_async(DrawThread::Job{kDoneB});
}
void front_join() {
  auto& f = g_front;
  if (!f.overlap) return;
  front_end_span();
  f.overlap = false;
  struct End { ~End() { g_front.id.store(0, std::memory_order_release); } } end;
  // (the thread may wait for a cache lock the render thread holds: never here, every join site is outside them)
  if (draw_threads::held_locks() && !f.off.exchange(true))
    LOG("[vulkan scene front] off for the session: the render thread joined the second drawing thread inside a cache lock");
  const auto t0 = std::chrono::steady_clock::now();
  worker_b().complete();
  const uint64_t waited = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - t0).count());
  f.waitNs += waited;
  stereo_front_emit_b();
  // (S6 tuning, the same) the thread's textures, made ready at the next span's start
  g_bLastTextures.swap(g_bTextures);
  g_bTextures.clear();
  g_bNoted.clear();
  // the split balanced: the render thread waited for the thread (its share was too large), or found it done
  if (!kFrontFixedShare) f.share = std::clamp(f.share + (waited > 100000 ? -0.02f : 0.01f), 0.15f, 0.85f);
}
void front_used(Surface* s) {
  const uint64_t o = g_front.id.load(std::memory_order_acquire);
  if (!o) return;
  if (on_scene_b()) {
    if (s->frontRenderChange == o) front_conflict(s, "a surface's state", "the render thread");
    s->frontUse = o;
  } else if (!draw_threads::on_draw_thread()) s->frontRenderUse = o;
}
void front_change(Surface* s, const char* what) {
  const uint64_t o = g_front.id.load(std::memory_order_acquire);
  if (!o) return;
  // (both threads only render into the pass's targets, the thread first on the GPU, as in the guest's order)
  const bool contents = !std::strncmp(what, "its contents", 12);
  if (on_scene_b()) {
    // the thread's change goes before everything of the render thread's on the GPU: wrong for what that already used
    if (!contents && (s->frontRenderUse == o || s->frontRenderChange == o)) front_conflict(s, what, "the second drawing thread");
    s->frontUse = o;
  } else if (!draw_threads::on_draw_thread()) {
    // the render thread's goes after the thread's commands on the GPU: right, unless the thread still uses it (it finds
    // the mark: front_used)
    s->frontRenderUse = o;
    if (!contents) s->frontRenderChange = o;
  }
}
void front_frame_end(uint32_t draws) {
  auto& f = g_front;
  if (draws - f.stretchFirst > f.bestEnd - f.bestFirst) f.bestFirst = f.stretchFirst, f.bestEnd = draws;
  f.passFirst = f.bestFirst;
  // (too short to split: none next frame)
  f.passEnd = f.bestEnd >= f.bestFirst + std::max(kFrontMinDraws, 2 * kFrontSideDraws) ? f.bestEnd : f.bestFirst;
  f.stretchLen += f.bestEnd - f.bestFirst;
  f.stretchKey = 0;
  f.stretchFirst = f.bestFirst = f.bestEnd = 0;
  f.tried = false;
}
void front_log(double frames) {
  auto& f = g_front;
  LOG("[vulkan scene front] per frame: the longest stretch into the same targets %.0f draws", f.stretchLen / frames);
  f.stretchLen = 0;
  LOG("[vulkan scene front] per frame: %.2f spans, %.0f draws on the second drawing thread, %.0f on the render thread "
      "beside it; the render thread waited for it %.2f ms; share %.2f; %.2f joins at a draw into other targets, %.2f at "
      "one sampling the pass's; %.2f runs not started (too little left); run draws it could not start at: eye 1 held "
      "%.1f, the atlas's thread %.1f, a held stream %.1f, other %.1f; %.0f conflicts%s",
      f.spans / frames, f.threadDraws / frames, f.renderDraws / frames, double(f.waitNs) / 1e6 / frames, double(f.share),
      f.otherTargets / frames, f.sampled / frames, f.skipped / frames, f.notYet[0] / frames, f.notYet[1] / frames,
      f.notYet[2] / frames, f.notYet[3] / frames, double(f.conflicts.exchange(0, std::memory_order_relaxed)),
      f.off.load() ? " (off for the session)" : "");
  f.spans = f.threadDraws = f.renderDraws = f.waitNs = f.skipped = f.otherTargets = f.sampled = 0;
  for (auto& n : f.notYet) n = 0;
}
}  // namespace scene_thread
// ---------------------------------------------------------------- batched same-state runs (BatchDraw, above)
namespace {
// WWHD_VK_BATCH_RUNS=0 turns them off (gx2 holds no runs then)
bool batch_runs_on() {
  static const bool on = [] { const char* e = std::getenv("WWHD_VK_BATCH_RUNS"); return !(e && !std::strcmp(e, "0")); }();
  return on;
}
// a draw whose run may be held: drawn here on the render thread from the gx2 registers, not on another drawing thread
// (a sun atlas run too, while the atlas has a thread of its own: draw_stereo hands it to that thread, atlas_thread.h)
bool batch_can_hold(const uint32_t* r) {
  // (docs/VR_SCENE_THREADS.md, "S6: batched runs on the drawing threads": with the scene's thread too; the split
  // without it, S1 to S3's diagnostics, still holds none)
  return batch_runs_on() && R.device && (!sun_atlas_target(r) || atlas_thread::enabled()) &&
         !(switches::kSceneSplit && !switches::kSceneThread) && !eye_thread::enabled() &&
         !ui_layer::candidate();  // (the game's UI: drawn one by one into its own image, in order: ui_layer)
}
[[maybe_unused]] const bool g_batchHoldSet = (draw_batch::can_hold() = batch_can_hold, true);
// a vertex shader's batched variant for some blocks: compiled on a thread of its own the first time it is wanted
struct BatchVariant {
  std::atomic<int> state{0};  // 0 being made, 1 ready, 2 failed
  const vk::Shader* base = nullptr;
  const LatteDecompilerShader* baseDec = nullptr;
  std::unique_ptr<vk::Shader> shader;
  vk::batch::Variant layout;
};
std::map<std::pair<uint64_t, uint32_t>, std::shared_ptr<BatchVariant>> g_batchVariants;  // (g_batchVariantsMutex)
std::mutex g_batchVariantsMutex;  // (the render thread and the sun atlas's thread)
std::atomic<uint64_t> g_batchVariantsMade{0}, g_batchVariantsFailed{0};
// blocks: the batched guest blocks (bit i); bit 16: the support block too
BatchVariant* batch_variant(vk::Shader* base, uint32_t blocks) {
  std::lock_guard<std::mutex> lock(g_batchVariantsMutex);
  auto& slot = g_batchVariants[{base->key, blocks}];
  // (a shader cache cleared since: the variant was made from another translation; the old one is kept, unused)
  if (slot && (slot->base != base || slot->baseDec != base->dec)) slot.reset();
  if (slot) return slot.get();
  slot = std::make_shared<BatchVariant>();
  slot->base = base;
  slot->baseDec = base->dec;
  auto sh = std::make_unique<vk::Shader>();
  const uint64_t salt = 0xBA7C4ED000000000ull ^ (0x9E3779B97F4A7C15ull * (uint64_t(blocks) + 1));
  sh->key = base->key ^ salt;
  sh->pipelineId = base->pipelineId ^ salt;
  sh->kind = base->kind;
  sh->vertex = base->vertex;
  sh->gatherUnits = base->gatherUnits;
  sh->dec = base->dec;
  sh->mapping = base->mapping;
  sh->descriptorRanks = base->descriptorRanks;
  sh->uniforms = base->uniforms;
  slot->shader = std::move(sh);
  slot->layout.blocks = blocks & 0xFFFF;
  slot->layout.support = blocks >> 16 & 1;
  slot->layout.supportBytes = uint32_t((std::max(base->uniforms.offset_endOfBlock, 16) + 15) & ~15);  // (pack_uniforms_into)
  std::thread([v = slot, source = base->retainedSource, maxRange = R.properties.limits.maxUniformBufferRange] {
    std::string glsl, why;
    bool ok = false;
    try {
      ok = vk::batch::rewrite(*source, v->layout, maxRange, glsl, &why);
      if (ok) {
        auto compiled = vk::compile_shader(glsl, vk::ShaderStage::Vertex, ~uint64_t{0}, 0, {}, false);
        ok = !compiled.words.empty();
        if (ok) {
          v->shader->spirvWords = std::make_shared<const std::vector<uint32_t>>(std::move(compiled.words));
          v->shader->retainedSource = std::make_shared<const std::string>(std::move(glsl));
        } else why = compiled.error.empty() ? "an empty compile" : compiled.error;
      }
    } catch (const std::exception& e) {
      ok = false;
      why = e.what();
    }
    if (!ok && g_batchVariantsFailed.load() < 5) LOG("[vulkan batch] a vertex shader's batched variant failed: %s", why.c_str());
    ++(ok ? g_batchVariantsMade : g_batchVariantsFailed);
    v->state.store(ok ? 1 : 2, std::memory_order_release);
  }).detach();
  return slot.get();
}
// why runs were drawn draw by draw (diagnostics, the log line in draw_stereo)
enum BatchWhy { kBatchDrawn, kBatchState, kBatchFetch, kBatchShader, kBatchBlocks, kBatchProjection, kBatchMaking,
                kBatchFailed, kBatchWhys };
std::atomic<uint64_t> g_batchWhy[kBatchWhys]{}, g_batchPieces{0}, g_batchDraws{0};  // (both threads)
// atlasThread: a sun atlas run on the atlas's thread (batch_draw_run), from its registers
bool batch_accept(const uint32_t* r, const draw_batch::Batch& run, BatchDraw& out, bool atlasThread = false) {
  auto no = [](BatchWhy why) { ++g_batchWhy[why]; return false; };
  if (!batch_runs_on() || !R.device ||
      (!atlasThread && (!batch_can_hold(r) || sun_atlas_target(r) || draw_threads::on_draw_thread())))
    return no(kBatchState);
  uint64_t fsKey = 0;
  LatteFetchShader* fs = vk::get_fetch_shader(r, &fsKey, R.frame);
  if (!fs) return no(kBatchFetch);
  // (an attribute read per instance would read another element in each instance)
  for (const auto& g : fs->bufferGroups)
    for (int j = 0; j < g.attribCount; ++j)
      if (g.attrib[j].fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) return no(kBatchFetch);
  vk::Shader* vs = vk::translate(r, true, fs, fsKey, R.frame, gx2::shader_state_gen());
  vk::Shader* ps = vs && vs->ready() ? vk::translate(r, false, fs, fsKey, R.frame, gx2::shader_state_gen(), vs) : nullptr;
  // (the vertex uniforms these change in place are the pieces' own: AO's noise scale, the sun shadow's eye shift)
  if (!vs || !vs->ready() || !vs->retainedSource || !ps || vs->kind == gfx::ProgramKind::OcclusionVertex ||
      ps->kind == gfx::ProgramKind::SunShadowMask) {
    // (diagnostics) WWHD_VK_BATCH_DUMP=dir: the refused vertex shaders' GLSL, once each
    static const char* dump = std::getenv("WWHD_VK_BATCH_DUMP");
    static std::unordered_set<uint64_t> dumped;
    if (dump && vs && vs->retainedSource && dumped.insert(vs->key).second) {
      char name[64];
      std::snprintf(name, sizeof name, "/vs_%016llx_mode%d.glsl", (unsigned long long)vs->key, int(vs->dec->uniformMode));
      std::ofstream(std::string(dump) + name) << *vs->retainedSource;
    }
    return no(kBatchShader);
  }
  // the blocks the shader reads whose address changes (the others' changes do not matter to it): bound as blocks, or
  // their entries remapped into the support block (which is then per piece too: bit 16)
  uint32_t blocks = 0, remapped = 0;
  for (uint32_t i = 0; i < 16; ++i)
    if ((run.blocks >> i & 1) && vs->mapping.uniformBuffersBindingPoint[i] >= 0) blocks |= 1u << i;
  if (vs->uniforms.offset_remapped >= 0 && vs->mapping.uniformVarsBufferBindingPoint >= 0)
    for (const auto& group : vs->dec->list_remappedUniformEntries_bufferGroups) {
      const uint32_t i = group.kcacheBankIdOffset / 4 / 7;
      if (i < 16 && (run.blocks >> i & 1)) remapped |= 1u << i;
    }
  // a bound block with a camera projection is drawn with each eye's own copy (bind_stage): never batched (the support
  // block's pieces are packed for each eye: bind_stage)
  static const bool submittedOn = gx2::submitted_uniforms_enabled();
  for (uint32_t i = 0; i < 16; ++i) {
    if (!((blocks | remapped) >> i & 1)) continue;
    const uint32_t size = std::min<uint32_t>(r[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7 + 1] + 1, 0x10000);
    for (uint32_t k = 0; k < run.count; ++k) {
      const uint32_t addr = run.addrs[k][i];
      if (!addr) return no(kBatchBlocks);
      const uint8_t* submitted = submittedOn ? gx2::submitted_uniform_block(addr, size) : nullptr;
      const auto* source = submitted ? submitted : static_cast<const unsigned char*>(mem::ptr(addr));
      if ((blocks >> i & 1) && stereo::has_camera_projection(source, size)) return no(kBatchProjection);
    }
  }
  BatchVariant* v = batch_variant(vs, blocks | (remapped ? 1u << 16 : 0));
  const int state = v->state.load(std::memory_order_acquire);
  if (state == 0) return no(kBatchMaking);
  if (state != 1) return no(kBatchFailed);
  out = {&run, vs, v->shader.get(), &v->layout, 0, 0};
  return true;
}
void batch_log() {
  static uint64_t frame0 = 0;
  if (R.frame < frame0 + 600) return;
  const double f = frame0 ? double(R.frame - frame0) : 600.0;
  frame0 = R.frame;
  LOG("[vulkan batch] per frame: %.1f runs drawn as %.1f instanced draws (%.0f draws in them); runs drawn draw by draw: "
      "%.1f state, %.1f fetch, %.1f shader, %.1f blocks, %.1f projection, %.1f variant being made, %.1f variant failed; "
      "variants made %llu, failed %llu",
      g_batchWhy[kBatchDrawn].load() / f, g_batchDraws.load() / f, g_batchPieces.load() / f,
      g_batchWhy[kBatchState].load() / f, g_batchWhy[kBatchFetch].load() / f, g_batchWhy[kBatchShader].load() / f,
      g_batchWhy[kBatchBlocks].load() / f, g_batchWhy[kBatchProjection].load() / f, g_batchWhy[kBatchMaking].load() / f,
      g_batchWhy[kBatchFailed].load() / f, (unsigned long long)g_batchVariantsMade.load(),
      (unsigned long long)g_batchVariantsFailed.load());
  for (auto& w : g_batchWhy) w = 0;
  g_batchPieces = 0;
  g_batchDraws = 0;
}
}  // namespace
static void draw_stereo_one(const uint32_t *r,uint32_t prim,uint32_t count,uint32_t indexType,
                            uint32_t indexAddr,uint32_t baseVertex,uint32_t instances);
// (diagnostics, docs/VR_DLSS.md stage D2a) WWHD_VK_MOTION_STATS=1: how well this frame's 3D draws (a vertex uniform
// block holds the camera's projection) match the previous frame's by key (motion_match.h), every 600 frames. Render
// thread, in the game's draw order; a held run counts its pieces, once it is drawn as a run
static void motion_note(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                        uint32_t baseVertex, uint32_t instances, const draw_batch::Batch* run) {
  // the key, and (to find what changes between frames) the key without the vertex buffer, without the indices, without
  // both
  constexpr int kVariants = 4;
  static std::vector<motion::Seen> prev[kVariants], cur[kVariants];
  static uint64_t frame = 0, windowStart = 0, frames = 0, allDraws = 0;
  static motion::Stats window, relaxed[kVariants];
  if (R.frame != frame) {
    if (frame) {
      window.add(motion::match_frames(prev[0], cur[0]));
      for (int v = 1; v < kVariants; ++v) relaxed[v].add(motion::match_frames(prev[v], cur[v]));
      ++frames;
    }
    for (int v = 0; v < kVariants; ++v) {
      prev[v].swap(cur[v]);
      cur[v].clear();
    }
    frame = R.frame;
    if (!windowStart) windowStart = frame;
    if (frame >= windowStart + 600 && frames) {
      const double f = double(frames), d = window.draws ? double(window.draws) : 1.0;
      LOG("[motion] per frame: %.0f draws, %.0f of them 3D; 3D matched %.1f%% (%.1f%% unique key, %.1f%% by order, "
          "%.1f%% of those with a changed count), unmatched %.1f%%, previous frame's left over %.0f; matched with "
          "unchanged transforms %.1f%%",
          allDraws / f, window.draws / f, 100.0 * double(window.unique + window.ordered) / d, 100.0 * window.unique / d,
          100.0 * window.ordered / d, 100.0 * window.countChanged / d, 100.0 * window.unmatched / d, window.gone / f,
          100.0 * window.still / d);
      auto pct = [&](int v) { return 100.0 * double(relaxed[v].unique + relaxed[v].ordered) / d; };
      LOG("[motion] 3D matched without the vertex buffer in the key %.1f%%, without the indices %.1f%%, without both %.1f%%",
          pct(1), pct(2), pct(3));
      window = {};
      for (auto& x : relaxed) x = {};
      frames = allDraws = 0;
      windowStart = frame;
    }
  }
  const uint32_t pieces = run ? run->count : 1;
  allDraws += pieces;
  motion::DrawKey key{r[mmSQ_PGM_START_VS], r[mmSQ_PGM_START_PS], r[mmSQ_VTX_ATTRIBUTE_BLOCK_START],
                      r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + 1], indexAddr, count, indexType, baseVertex, instances, prim};
  uint64_t keys[kVariants];
  keys[0] = key.hash();
  motion::DrawKey noVertex = key;
  noVertex.vertexAddr = noVertex.vertexSize = 0;
  keys[1] = noVertex.hash();
  motion::DrawKey noIndex = key;
  noIndex.indexAddr = 0;
  keys[2] = noIndex.hash();
  noVertex.indexAddr = 0;
  keys[3] = noVertex.hash();
  for (uint32_t piece = 0; piece < pieces; ++piece) {
    uint64_t h = 1469598103934665603ull;
    bool scene = false;
    for (uint32_t i = 0; i < 16; ++i) {
      uint32_t address = r[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7];
      if (run && (run->blocks >> i & 1)) address = run->addrs[piece][i];
      if (!address) continue;
      const uint32_t size = std::min<uint32_t>(r[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7 + 1] + 1, 1024);
      const uint8_t* bytes = gx2::submitted_uniform_block(address, size);
      if (!bytes) bytes = ppc_ptr(address);
      for (uint32_t at = 0; at + 64 <= size && !scene; at += 16) {
        float m4[16];
        std::memcpy(m4, bytes + at, 64);
        scene = stereo::camera_projection(m4);
      }
      for (uint32_t b = 0; b < size; ++b) {
        h ^= bytes[b];
        h *= 1099511628211ull;
      }
    }
    if (scene)
      for (int v = 0; v < kVariants; ++v) cur[v].push_back({keys[v], h});
  }
}

void draw_stereo(const uint32_t *r,uint32_t prim,uint32_t count,uint32_t indexType,
                 uint32_t indexAddr,uint32_t baseVertex,uint32_t instances) {
  const draw_batch::Batch* run = draw_batch::current();
  static const bool motionStats = switches::env_on("WWHD_VK_MOTION_STATS") && ngx_vk::motion_wanted();
  struct MotionNote {
    bool on; const uint32_t* r; uint32_t prim, count, indexType, indexAddr, baseVertex, instances;
    const draw_batch::Batch* run;
    // a run gx2 then draws piece by piece (not consumed) comes back here once per piece: counted then
    ~MotionNote() {
      if (on && (!run || draw_batch::consumed()))
        motion_note(r, prim, count, indexType, indexAddr, baseVertex, instances, run);
    }
  } motionNote{motionStats && !draw_threads::on_draw_thread(), r, prim, count, indexType, indexAddr, baseVertex,
               instances, run};
  if (!run) [[likely]] {
    draw_stereo_one(r,prim,count,indexType,indexAddr,baseVertex,instances);
    return;
  }
  // a held run (gx2): one instanced draw per chunk of the pieces its variant's blocks hold, or (not taken: consumed()
  // stays false) nothing drawn here, and gx2 draws the pieces one by one
  batch_log();
  // (diagnostics) WWHD_VK_BATCH_REFUSE=1: every held run drawn draw by draw (gx2 times both ways: WWHD_VK_BATCH_STATS)
  static const bool refuse = switches::env_on("WWHD_VK_BATCH_REFUSE");
  if (sun_atlas_target(r)) {
    // a sun atlas run: to the atlas's thread, which draws it from its own registers (batch_draw_run), when the draws
    // write no colour (eye 1 draws none of them: draw_stereo_one's atlas branch); else gx2 draws it draw by draw
    if (refuse || !atlas_thread::running() || !atlas_draw_writes_no_colour(r)) return;
    if (!stereo_atlas_holding()) { stereo_flush(); stereo_atlas_hold(); }
    atlas_thread_log();
    atlas_thread::draw_run(prim, count, indexType, indexAddr, baseVertex, *run);
    draw_batch::consumed() = true;
    return;
  }
  // (docs/VR_SCENE_THREADS.md, stage S7) while the second drawing thread's span runs, gx2 hands it the pieces one by one
  if (switches::kSceneFront && scene_thread::front_span()) return;
  // (docs/VR_SCENE_THREADS.md, "S6: batched runs on the drawing threads") offered to the scene's drawing threads first:
  // draw_stereo_one's way to them, taken whole there, or with nothing drawn of it (t_sceneRunTaken false) when no part
  // runs or it must end first; then the render thread batches it below
  if (switches::kSceneThread && !refuse) {
    t_sceneRun = run;
    t_sceneRunTaken = false;
    {
      struct Clear { ~Clear() { t_sceneRun = nullptr; } } clear;
      draw_stereo_one(r,prim,count,indexType,indexAddr,baseVertex,instances);
    }
    if (t_sceneRunTaken) {
      draw_batch::consumed() = true;
      return;
    }
  }
  BatchDraw b;
  if (refuse || !batch_accept(r, *run, b)) return;
  draw_batch::consumed() = true;
  ++g_batchWhy[kBatchDrawn];
  g_batchPieces += run->count;
  struct Clear { ~Clear() { batch_draw() = nullptr; } } clear;
  for (uint32_t first = 0; first < run->count; first += b.layout->pieces) {
    b.first = first;
    b.count = std::min(b.layout->pieces, run->count - first);
    ++g_batchDraws;
    batch_draw() = &b;
    draw_stereo_one(r,prim,count,indexType,indexAddr,baseVertex,b.count);
    batch_draw() = nullptr;
  }
}
// (the sun atlas's thread) a held run of atlas draws from its registers (atlas_thread.h draw_run): one instanced draw
// per chunk when batch_accept takes it, else draw by draw with each piece's block addresses. shared: as draw's
void batch_draw_run(uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                    uint32_t baseVertex, const draw_batch::Batch& run, bool* shared) {
  uint32_t saved[16];
  for (uint32_t i = 0; i < 16; ++i) saved[i] = regs[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7];
  auto set = [&](const uint32_t* addrs) {
    for (uint32_t i = 0; i < 16; ++i) regs[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7] = addrs[i];
  };
  struct Restore {
    std::function<void()> f;
    ~Restore() { f(); }
  } restore{[&] { batch_draw() = nullptr; set(saved); }};
  set(run.addrs[0]);
  BatchDraw b;
  if (batch_accept(regs, run, b, true)) {
    ++g_batchWhy[kBatchDrawn];
    g_batchPieces += run.count;
    for (uint32_t first = 0; first < run.count; first += b.layout->pieces) {
      b.first = first;
      b.count = std::min(b.layout->pieces, run.count - first);
      ++g_batchDraws;
      batch_draw() = &b;
      draw(regs, prim, count, indexType, indexAddr, baseVertex, b.count, shared);
      batch_draw() = nullptr;
    }
    return;
  }
  for (uint32_t k = 0; k < run.count; ++k) {
    set(run.addrs[k]);
    draw(regs, prim, count, indexType, indexAddr, baseVertex, 1, shared);
  }
}
// (docs/VR_DLSS.md, stage D2b 2b) the draw's key in the motion store, under its number (a held run's pieces, each theirs)
static void motion_key_draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                            uint32_t baseVertex, uint32_t instances) {
  static bool begun = false;
  if (!begun) { motion_store().begin_frame(4096); begun = true; }
  const motion::DrawKey key{r[mmSQ_PGM_START_VS], r[mmSQ_PGM_START_PS], 0, 0, indexAddr, count, indexType,
                            baseVertex, instances, prim};
  const uint32_t pieces = t_sceneRun ? t_sceneRun->count : 1;
  for (uint32_t k = 0; k < pieces; ++k) motion_store().key(t_drawIndex + k, key);
}
static void draw_stereo_one(const uint32_t *r,uint32_t prim,uint32_t count,uint32_t indexType,
                            uint32_t indexAddr,uint32_t baseVertex,uint32_t instances) {
  // (diagnostics, never for play) WWHD_VK_SKIP_SUN_ATLAS=1: the sun atlas's draws are not drawn at all, so its
  // shadows are wrong. The frame time without them is the most that reusing the atlas between frames could save
  // (docs/VR_PERFORMANCE.md, roadmap 2c)
  static const bool skipAtlas=[]{const char* e=std::getenv("WWHD_VK_SKIP_SUN_ATLAS");return e&&!std::strcmp(e,"1");}();
  if(skipAtlas && sun_atlas_target(r)) return;
  if(stereo::frame.mode==stereo::Off) {
    if(t_sceneRun) return;  // (a held run offered to the scene's threads: draw_stereo batches it)
    stereo_select_eye(0);
    // (docs/VR_DLSS.md, "The plain picture") DLSS without 3D: the draw numbered and keyed for the motion store, and
    // its shaders recorded for the motion pass, as eye 0's are in 3D (only while the motion pass runs)
    if(switches::kSceneHooks && motion_on() && !draw_threads::on_draw_thread()) [[unlikely]] {
      t_drawIndex = sceneSplit.drawIndex++;
      motion_key_draw(r, prim, count, indexType, indexAddr, baseVertex, instances);
      if(motion_pass::on()) {
        StereoShaders& out = stereoShaders();
        out.reset();
        stereoShadersOut() = &out;
      }
    }
    try { draw(r,prim,count,indexType,indexAddr,baseVertex,instances); }
    catch(...) { stereoShadersOut()=nullptr; throw; }
    stereoShadersOut() = nullptr;  // (draw took it; nothing left set for the next draw)
    return;
  }
  // Eye 0 always draws first, so it makes every transition of the shared atlas in the
  // renderer's stream. Eye 1's held commands may still sample the atlas: emit them before
  // it is drawn again, and emit an eye 1 draw into it at once.
  const bool atlas=sun_atlas_target(r);
  if(stereo_timeline_on()) stereo_timeline_draw();
  if(stereo_eye_segments()) {  // (diagnostics) the eyes' held segments' cuts, every 600 frames
    static uint64_t frame0=0;
    if(R.frame>=frame0+600) {
      frame0=R.frame;
      LOG("[vulkan stereo] eyes' held segments: %.2f cuts per frame",stereo_take_cuts()/600.0);
      {
        const auto e=eye_thread::take_stats();
        LOG("[vulkan stereo] eye 1's thread, per frame: %.0f draws, %.2f cuts (%.2f for eye 0's texture changes), %.1f "
            "invalidates, %.2f joins, %.2f hand-over waits; the render thread waited %.2f ms at the joins; %.0f draws "
            "replayed, %.0f in full",
            e.draws/600.0,e.cuts/600.0,e.changeCuts/600.0,e.invalidates/600.0,e.joins/600.0,e.ringWaits/600.0,
            e.joinMs/600.0,e.replayed/600.0,e.full/600.0);
      }
    }
  }
  // (diagnostics) WWHD_VK_TARGET_STATS=1: the time of each eye's draws (eye 0 includes what it prepares for eye 1)
  static const bool timed=std::getenv("WWHD_VK_TARGET_STATS")!=nullptr;
  // (diagnostics, the same switch) how the frame's sun-atlas draws lie in its draw order: runs of consecutive atlas
  // draws per frame, and where the first and last fall among the frame's draws (docs/VR_PERFORMANCE.md, 2b)
  if(timed) {
    static uint64_t frame=~0ull, logFrom=0;
    static uint32_t drawIndex=0, runs=0, first=0, last=0, atlasDraws=0;
    static bool wasAtlas=false;
    static double sumRuns=0, sumFirst=0, sumLast=0, sumDraws=0, sumAtlas=0, frames=0;
    if(R.frame!=frame) {
      if(frame!=~0ull && drawIndex) {
        sumRuns+=runs;sumFirst+=first;sumLast+=last;sumDraws+=drawIndex;sumAtlas+=atlasDraws;frames+=1;
      }
      if(frames>=600) {
        LOG("[vulkan stereo] sun atlas order: %.1f runs per frame, %.0f atlas draws of %.0f, first at draw %.0f, last at "
            "%.0f",sumRuns/frames,sumAtlas/frames,sumDraws/frames,sumFirst/frames,sumLast/frames);
        sumRuns=sumFirst=sumLast=sumDraws=sumAtlas=frames=0;
      }
      frame=R.frame;drawIndex=runs=first=last=atlasDraws=0;wasAtlas=false;(void)logFrom;
    }
    if(atlas) {
      if(!wasAtlas) { ++runs; if(runs==1) first=drawIndex; }
      last=drawIndex;++atlasDraws;
    }
    wasAtlas=atlas;++drawIndex;
  }
  using Clock=std::chrono::steady_clock;
  const Clock::time_point t0=timed?Clock::now():Clock::time_point{};
  if(atlas) {
    // eye 1's held commands may sample the atlas: they run before it is drawn again. With the atlas's own held
    // stream (stereo_atlas_hold) that is once, at the start of the atlas's block: eye 1 holds nothing inside it
    if(!stereo_atlas_holding()) { stereo_flush(); stereo_atlas_hold(); }
    if(atlas_thread::running()) {
      atlas_thread_log();
      // recorded on the atlas's thread (atlas_thread.h), from its registers
      if(atlas_thread::draw(r,prim,count,indexType,indexAddr,baseVertex,instances,atlas_draw_writes_no_colour(r))) {
        if(timed) { stereo_eye_time(0,Clock::now()-t0,true); stereo_atlas_held_draw(); }
        return;
      }
      // (rare: it writes colour too, so eye 1 draws it as well) the threads join; eye 1 draws it here (the render
      // thread's registers are the same: every change was forwarded; its own counters and memos are its own)
      stereo_atlas_join(AtlasEmit::Draw);
      stereoShadersIn() = nullptr;  // (eye 0 drew it on the other thread: eye 1 prepares it in full)
      stereo_select_eye(1);
      try { draw(r,prim,count,indexType,indexAddr,baseVertex,instances); }
      catch(...) { stereo_select_eye(0); throw; }
      stereo_select_eye(0);
      stereo_flush();
      return;
    }
  } else {
    stereo_atlas_emit(AtlasEmit::Draw);  // the first other draw: the atlas's held commands go before it
    // (stage 3.2) the scene while the atlas's thread catches up: a draw that may sample the atlas comes after it
    if(stereo_scene_holding()) {
      if(atlas_thread::may_sample_atlas(r)) stereo_atlas_join(AtlasEmit::Sampled);
      else atlas_thread::scene_draw();
    }
  }
  if(switches::kSceneHooks) [[unlikely]] scene_split_before_draw();  // (docs/VR_SCENE_THREADS.md, stage S1)
  if(switches::kSceneHooks && motion_on() && !draw_threads::on_draw_thread()) [[unlikely]]
    motion_key_draw(r, prim, count, indexType, indexAddr, baseVertex, instances);
  // (stage S7) the front of the frame's largest two-eye pass on the second drawing thread, beside the render thread
  if(switches::kSceneFront) [[unlikely]] {
    if(scene_thread::front_draw(r,prim,count,indexType,indexAddr,baseVertex,instances)) return;
  }
  // (diagnostics) a part's draws on the render thread (the split without the scene's thread): their time, to set
  // against the thread's
  struct PartTimer {
    bool on;
    Clock::time_point t0 = on ? Clock::now() : Clock::time_point{};
    ~PartTimer() {
      if (!on) return;
      g_renderPartNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
      ++g_renderPartDraws;
    }
  } partTimer{switches::kSceneSplit && !switches::kSceneThread && stereo_part_holding()};
  // (diagnostics) with the scene's thread: the render thread's own draws in the pass the split is in, before it (the same
  // kind of draws as the part's after it)
  struct SamePassTimer {
    bool on;
    Clock::time_point t0 = on ? Clock::now() : Clock::time_point{};
    ~SamePassTimer() {
      if (!on) return;
      g_samePassNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
      ++g_samePassDraws;
    }
  } samePassTimer{switches::kSceneThread && !stereo_part_holding() && scene_split_in_half_pass()};
  // (stages S3, S4) a part on the scene's thread: this draw there, both halves, beside the render thread. The part
  // ends first if eye 1 holds commands, or a draw of it was drawn as two
  // (docs/VR_EYE_PROJECTION.md) the game's UI into its own image stays here, in order with the image's clear: drawn on the
  // scene's thread, the file-select menu's panels and text were missing from the image
  if(switches::kSceneThread && scene_thread::running()) [[unlikely]] {
    if(!stereo_eye1_holding() && !ui_layer::candidate() &&
       scene_thread::draw(prim,count,indexType,indexAddr,baseVertex,instances)) return;
    stereo_part_join_at(PartJoinSite::Eye1);
  }
  if(t_sceneRun) return;  // (a held run not handed over: nothing of it drawn here, draw_stereo batches it)
  // (diagnostics, WWHD_PROF_RENDER_PHASES=1) one draw in 16 timed by phase as the drawing threads' are (their log line's
  // numbers, beside these: the render thread's report)
  static const bool renderPhases=switches::env_on("WWHD_PROF_RENDER_PHASES");
  struct RenderSampled { bool on=false; ~RenderSampled(){ if(on) rprof::end_thread_draw(); } } renderSampled;
  if(renderPhases && rprof::enabled() && (!switches::kSceneSplit || stereo_part_holding())) [[unlikely]] {  // (the split: its part's)
    static uint32_t counter=0;
    rprof::g_phase_thread()=true;
    if((renderSampled.on=(++counter&15)==0)) rprof::begin_thread_draw();
  }
  bool shared=false;
  // (stage E4) eye 1's draw on its own thread: eye 0's hand-over in a slot of the ring (not the atlas's: its rare
  // second eye is drawn here, below)
  // (diagnostics) WWHD_VK_EYE_AFTER_ATLAS=1: not while the atlas's thread overlaps the scene
  static const bool eyeAfterAtlas=switches::env_on("WWHD_VK_EYE_AFTER_ATLAS");
  const bool eyeThread=!atlas && eye_thread::enabled() && stereo_streaming() && !(eyeAfterAtlas && atlas_thread::running());
  if(!eyeThread && eye_thread::running()) stereo_flush();  // (switched off meanwhile: it finishes before eye 1 draws here)
  try {
    stereo_select_eye(0);
    uint64_t handoffSeq=0;
    StereoShaders* out=eyeThread?static_cast<StereoShaders*>(eye_thread::handoff_slot(handoffSeq)):&stereoShaders();
    out->reset();
    stereoShadersOut()=out;
    draw(r,prim,count,indexType,indexAddr,baseVertex,instances,&shared);
    const Clock::time_point t1=timed?Clock::now():Clock::time_point{};
    if(timed) stereo_eye_time(0,t1-t0,shared);
    if(shared && timed && stereo_atlas_holding()) stereo_atlas_held_draw();
    if(shared) return; // keep the light-space render pass open
    if(out->multiview) {  // (docs/VR_MULTIVIEW.md, stage M4) eye 0's draw drew both eyes
      if(timed) stereo_eye_time(1,Clock::now()-t1,false);
      return;
    }
    if(eyeThread) {
      if(!eye_thread::running()) eye_thread::begin(stereo_eye1_segment());
      eye_thread::draw(prim,count,indexType,indexAddr,baseVertex,instances,out->vs?out:nullptr);
      if(timed) stereo_eye_time(1,Clock::now()-t1,false);
      return;
    }
    stereo_select_eye(1);
    stereoShadersIn()=stereoShaders().vs?&stereoShaders():nullptr;
    draw(r,prim,count,indexType,indexAddr,baseVertex,instances);
    if(atlas) stereo_flush();
    if(timed) stereo_eye_time(1,Clock::now()-t1,false);
  } catch(...) { stereoShadersOut()=nullptr;stereoShadersIn()=nullptr;stereo_select_eye(0);throw; }
}
namespace eye_thread {
void* handoff_ring(uint64_t i) { return &handoffRing[i % kHandoffRing]; }
uint64_t handoff_ring_size() { return kHandoffRing; }
void eye1_draw(const uint32_t* regs, const uint32_t* a, const void* handoff) {
  stereoShadersIn() = static_cast<const StereoShaders*>(handoff);
  try { gfxvk::draw(regs, a[0], a[1], a[2], a[3], a[4], a[5]); }
  catch (...) { stereoShadersIn() = nullptr; throw; }
  stereoShadersIn() = nullptr;
}
}  // namespace eye_thread
void clouds_before_draw(const uint32_t* r, const vk::Shader* ps, Surface* color0, uint32_t slice0, Surface* depth);
bool clouds_skip_card();
// (docs/VR_DLSS.md, stage D2b 2d; motion_pass.h) a depth-writing draw as a packet for an eye's motion pass: its
// motion twin, the vertex stage's set again with both frames' elements (the previous frame's bytes from the store,
// uploaded again; the current frame's where there are none: zero motion), its vertex buffers and the previous frame's
// at + 16, its index buffer and counts, viewport and scissor. Nothing when anything is missing. Eye 1 (a multiview
// draw's second view, or eye 1's replay of eye 0's draw): eye 0's record and data, the eye-shifted rows of its blocks
// (the record's EyeUniforms) made eye 1's in both frames' bytes (apply_eye_rows: the rows' unshifted values are the
// camera's projection, the same in both frames)
// eye 0's previous-frame uploads of one draw, reused by its eye 1 packet where nothing is eye-shifted (fix 3)
struct MotionShare {
  struct Uniform { uint32_t binding; VkDescriptorBufferInfo info; uint32_t offset; };
  std::array<Uniform, 20> uniforms{};
  uint32_t uniformCount = 0;
  std::array<motion_pass::Packet::Bind, 16> binds{};
  uint32_t bindCount = 0;
  bool still = false;  // eye 0 found nothing changed: no packet for either eye
  bool filled = false;
};
#ifdef WWHD_MOBILE_GPU_PATHS
// (mobile GPUs: docs/VR_DLSS.md, "Both eyes in one motion pass") an eye's packet built but not staged: its descriptor
// writes kept, so a multiview draw's two eyes can become one packet drawn once for both (motion_both)
struct MotionBuilt {
  bool ok = false;
  motion_pass::Packet p;
  std::array<VkWriteDescriptorSet, 2 * (17 + LATTE_NUM_MAX_TEX_UNITS)> writes;
  std::array<VkDescriptorBufferInfo, 2 * (17 + LATTE_NUM_MAX_TEX_UNITS)> infos;
  uint32_t n = 0;
};
// WWHD_VK_MOTION_BOTH_EYES=0, or settings.ini vkMotionBothEyes=0: each eye's packet on its own (the eyes' own
// passes), as on other GPUs; the environment variable takes precedence
static bool motion_both_eyes_on() {
  static const bool on = [] {
    const char* e = std::getenv("WWHD_VK_MOTION_BOTH_EYES");
    std::string saved;
    const bool fromSetting = !(e && e[0]) && ::hostui::get("vkMotionBothEyes", saved);
    const bool v = e && e[0] ? e[0] != '0' : !(fromSetting && saved == "0");
    LOG("[motion] both eyes in one motion pass (mobile GPUs): %s", v ? "on (WWHD_VK_MOTION_BOTH_EYES=0 or vkMotionBothEyes=0 turns it off)"
                                                                     : fromSetting ? "off (vkMotionBothEyes=0)" : "off (WWHD_VK_MOTION_BOTH_EYES=0)");
    return v;
  }();
  return on;
}
// an eye's packet staged on its own (its pipeline's own layout), as motion_packet does
static void motion_stage_one(MotionBuilt& b, const vk::motion::Pipeline& twin) {
  b.p.set = stage_descriptor_set(true, twin.sets[0], b.writes.data(), b.n);
  if (b.p.set) motion_pass::add(b.p);
}
// a multiview draw's two eyes' packets as one, drawn once for both (the twin's both-eyes pipeline): each per-frame
// binding's four elements current eye 0, current eye 1, previous eye 0, previous eye 1 (motion_glsl.h, views 2), its
// dynamic offsets in that order; the rest (textures, vertex buffers) eye 0's, which eye 1's packet shares. Where the
// two cannot be one (an eye left out, a different shape, no both-eyes pipeline), each eye's on its own
static void motion_both(MotionBuilt (&b)[2], const vk::motion::Pipeline& twin) {
  bool one = b[0].ok && b[1].ok && twin.state.load(std::memory_order_acquire) == vk::motion::Pipeline::Ready &&
             twin.pipeline2 && b[0].n == b[1].n && b[0].p.offsetCount == b[1].p.offsetCount &&
             2 * b[0].p.offsetCount <= std::size(b[0].p.offsets);
  for (uint32_t k = 0; one && k < b[0].n; ++k)
    one = b[0].writes[k].dstBinding == b[1].writes[k].dstBinding &&
          b[0].writes[k].descriptorType == b[1].writes[k].descriptorType &&
          b[0].writes[k].dstArrayElement == b[1].writes[k].dstArrayElement;
  std::array<VkWriteDescriptorSet, 4 * (17 + LATTE_NUM_MAX_TEX_UNITS)> writes;
  std::array<VkDescriptorBufferInfo, 4 * (17 + LATTE_NUM_MAX_TEX_UNITS)> infos;
  uint32_t n = 0;
  for (uint32_t k = 0; one && k < b[0].n;) {
    const VkWriteDescriptorSet& w = b[0].writes[k];
    const bool buffer = w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (!buffer) {
      writes[n++] = w;
      ++k;
      continue;
    }
    // (motion_packet writes a binding's current element, then its previous one)
    if (k + 1 >= b[0].n || w.dstArrayElement != 0 || b[0].writes[k + 1].dstArrayElement != 1 ||
        b[0].writes[k + 1].dstBinding != w.dstBinding) {
      one = false;
      break;
    }
    const VkDescriptorBufferInfo* element[4] = {&b[0].infos[k], &b[1].infos[k], &b[0].infos[k + 1], &b[1].infos[k + 1]};
    for (uint32_t e = 0; e < 4; ++e) {
      infos[n] = *element[e];
      writes[n] = w;
      writes[n].dstArrayElement = e;
      writes[n].pBufferInfo = &infos[n];
      ++n;
    }
    k += 2;
  }
  if (!one) {
    for (MotionBuilt& e : b)
      if (e.ok) motion_stage_one(e, twin);
    return;
  }
  motion_pass::Packet p = b[0].p;
  p.eye = 2;
  p.pipeline = twin.pipeline2;
  p.layout = twin.layout2;
  p.previous = b[0].p.previous && b[1].p.previous;
  p.why = b[0].p.why ? b[0].p.why : b[1].p.why;
  p.offsetCount = 0;
  for (uint32_t j = 0; j + 1 < b[0].p.offsetCount; j += 2) {
    p.offsets[p.offsetCount++] = b[0].p.offsets[j];
    p.offsets[p.offsetCount++] = b[1].p.offsets[j];
    p.offsets[p.offsetCount++] = b[0].p.offsets[j + 1];
    p.offsets[p.offsetCount++] = b[1].p.offsets[j + 1];
  }
  p.set = stage_descriptor_set(true, twin.sets2[0], writes.data(), n);
  if (p.set) motion_pass::add(p);
}
#endif
static void motion_packet(const uint32_t* r, const StereoShaders& rec, int eye, Surface* depth, uint32_t depthSlice,
                          uint32_t instances, uint32_t baseVertex, MotionShare* share = nullptr
#ifdef WWHD_MOBILE_GPU_PATHS
                          , MotionBuilt* built = nullptr  // (mobile GPUs: built, not staged: motion_both)
#endif
                          ) {
  const uint32_t dc = r[REGADDR::DB_DEPTH_CONTROL];  // bit 1 depth test, bit 2 depth write
  if (!depth || !(dc & 2) || !rec.pipeline || !rec.vs || !rec.rasterReady) return;
  // (docs/VR_DLSS.md, "Background motion") tested but not written: the sky, the sea, effects. Its own vertex shader's
  // motion, drawn only where no geometry wrote depth (its depth pinned to the value the depth was cleared to)
  const bool background = !(dc & 4);
  float backgroundDepth = 0.0f;
  if (background) {
    if (!motion_pass::background_on()) return;
    backgroundDepth = std::atomic_ref<float>(depth->clearedDepth).load(std::memory_order_relaxed);
    if (std::isnan(backgroundDepth) && depth->layerOwner)  // (multiview: eye 1's half of eye 0's image)
      backgroundDepth = std::atomic_ref<float>(depth->layerOwner->clearedDepth).load(std::memory_order_relaxed);
    if (!motion::background_depth_ok(backgroundDepth)) return;
  }
  const auto twin = rec.pipeline->motionTwin;
  if (!twin || twin->state.load(std::memory_order_acquire) != vk::motion::Pipeline::Ready) return;
  const StageReplay& vsRec = rec.stages[0];
  if (!vsRec.valid || !vsRec.writeCount || rec.vertexBindCount > 16) return;
  // eye 1 after eye 0 of the same draw: still for eye 0 is still for eye 1 (the same data, shifted alike)
  const bool reuse = share && share->filled && eye == 1;
  if (reuse && share->still) {
    motion_pass::note_still();
    return;
  }
  const motion::DrawData* prev = motion_store().previous(current_draw_index(), 0);
  const motion::DrawData* cur = motion_store().slot(current_draw_index(), 0);
  // a background draw with no 3D camera (a full-screen effect, a 2D overlay) would cover the sky's motion with zero
  if (background && (!cur || !motion::camera_draw(*cur))) return;
  motion_pass::Packet p;
  p.eye = eye;
  p.background = background;
  p.order = current_draw_index();
  const float eye1 = eye ? stereo::eye_offset(1) : 0.0f;
  p.pipeline = twin->pipeline;
  p.layout = twin->layout;
  p.previous = prev != nullptr;
  if (!prev) p.why = 1;
  bool changed = !prev;  // anything the previous frame's positions could differ by (fix 2)
  std::array<VkWriteDescriptorSet, 2 * (17 + LATTE_NUM_MAX_TEX_UNITS)> writes;
  std::array<VkDescriptorBufferInfo, 2 * (17 + LATTE_NUM_MAX_TEX_UNITS)> infos;
  uint32_t n = 0, dynamicIndex = 0;
  const auto& m = rec.vs->mapping;
  auto find_piece = [&](const motion::DrawData* d, uint32_t binding) -> const motion::DrawData::Piece* {
    if (!d) return nullptr;
    if (int(binding) == m.uniformVarsBufferBindingPoint) return d->find(motion::Part::VertexSupport, 0);
    for (uint32_t b = 0; b < 16; ++b)
      if (m.uniformBuffersBindingPoint[b] == int(binding)) return d->find(motion::Part::VertexBlock, uint8_t(b));
    return nullptr;
  };
  if (share && eye == 0) *share = {};
  for (uint32_t i = 0; i < vsRec.writeCount; ++i) {
    VkWriteDescriptorSet w = vsRec.writes[i];
    const bool buffer = w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (!buffer) {
      w.pImageInfo = &vsRec.imageInfos[i];
      writes[n++] = w;
      continue;
    }
    const bool dynamic = w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    const VkDescriptorBufferInfo current = vsRec.bufferInfos[i];
    const uint32_t currentOffset = dynamic && dynamicIndex < vsRec.resources.dynamicOffsetCount
                                       ? vsRec.resources.dynamicOffsets[dynamicIndex] : 0;
    if (dynamic) ++dynamicIndex;
    // the previous frame's bytes of this binding, and this frame's (the store's slot of this draw)
    const motion::DrawData::Piece* piece = find_piece(prev, w.dstBinding);
    const motion::DrawData::Piece* now = find_piece(cur, w.dstBinding);
    VkDescriptorBufferInfo cur1 = current;  // (eye 1: its own copy of an eye-shifted block, below)
    uint32_t cur1Offset = currentOffset;
    const EyeUniform* shifted = nullptr;
    if (eye)
      for (uint32_t k = 0; k < vsRec.uniformCount; ++k)
        if (vsRec.uniforms[k].binding == w.dstBinding) shifted = &vsRec.uniforms[k];
    if (shifted) {
      PER_THREAD_STATIC(scratch1, std::vector<uint8_t>);
      scratch1.assign(shifted->bytes.begin(), shifted->bytes.end());
      if (scratch1.size() < current.range) return;
      stereo::apply_eye_rows(scratch1.data(), shifted->rows, eye1, stereo::frame.convergence, stereo::eye_frustum(eye1));
      const UploadSlice s1 = snapshot(scratch1.data(), size_t(current.range), R.properties.limits.minUniformBufferOffsetAlignment);
      if (dynamic) {
        if (s1.offset > UINT32_MAX) return;
        cur1 = {s1.buffer, 0, current.range};
        cur1Offset = uint32_t(s1.offset);
      } else {
        cur1 = {s1.buffer, s1.offset, current.range};
      }
    }
    VkDescriptorBufferInfo previous = cur1;
    uint32_t previousOffset = cur1Offset;
    if (prev && !piece) {
      p.previous = false;
      p.why = 3;
    }
    const MotionShare::Uniform* shared = nullptr;
    if (reuse && !shifted)
      for (uint32_t k = 0; k < share->uniformCount; ++k)
        if (share->uniforms[k].binding == w.dstBinding) shared = &share->uniforms[k];
    if (shared) {
      // eye 0's previous-frame element of this binding, nothing eye-shifted in it (fix 3)
      previous = shared->info;
      previousOffset = shared->offset;
    } else if (piece && !piece->referred() && piece->size >= current.range) {
      const uint8_t* bytes = prev->bytes.data() + piece->offset;
      const bool same = now && !now->referred() && now->size >= current.range &&
                        !std::memcmp(bytes, cur->bytes.data() + now->offset, size_t(current.range));
      if (!same) {
        // (fix 1: the same bytes as this frame's: the current element serves as the previous one, nothing uploaded)
        changed = true;
        if (shifted) {
          PER_THREAD_STATIC(scratch2, std::vector<uint8_t>);
          scratch2.assign(bytes, bytes + current.range);
          stereo::apply_eye_rows(scratch2.data(), shifted->rows, eye1, stereo::frame.convergence, stereo::eye_frustum(eye1));
          bytes = scratch2.data();
        }
        const UploadSlice s = snapshot(bytes, size_t(current.range),
                                       R.properties.limits.minUniformBufferOffsetAlignment);
        if (dynamic) {
          if (s.offset > UINT32_MAX) return;
          previous = {s.buffer, 0, current.range};
          previousOffset = uint32_t(s.offset);
        } else {
          previous = {s.buffer, s.offset, current.range};
        }
      }
    }
    if (share && eye == 0 && share->uniformCount < share->uniforms.size())
      share->uniforms[share->uniformCount++] = {w.dstBinding, previous, previousOffset};
    infos[n] = cur1;
    w.descriptorCount = 1;
    w.dstArrayElement = 0;
    w.pBufferInfo = &infos[n];
    writes[n++] = w;
    infos[n] = previous;
    w.dstArrayElement = 1;
    w.pBufferInfo = &infos[n];
    writes[n++] = w;
    if (dynamic) {
      if (p.offsetCount + 2 > std::size(p.offsets)) return;
      p.offsets[p.offsetCount++] = cur1Offset;
      p.offsets[p.offsetCount++] = previousOffset;
    }
  }
  // the vertex buffers: the current frame's, and the previous frame's at + 16 (a mesh the buffer cache serves at the
  // same address: the same buffer; a copy: the same bytes as this frame's, the current buffer, else uploaded again;
  // otherwise the current frame's)
  for (uint32_t i = 0; i < rec.vertexBindCount; ++i) {
    const auto& v = rec.vertexBinds[i];
    if (v.binding >= 16) return;
    p.binds[p.bindCount++] = {v.binding, v.buffer, v.offset};
    motion_pass::Packet::Bind before{v.binding + 16, v.buffer, v.offset};
    const motion::DrawData::Piece* pv = prev ? prev->find(motion::Part::VertexBuffer, uint8_t(v.binding)) : nullptr;
    const motion::DrawData::Piece* cv = cur ? cur->find(motion::Part::VertexBuffer, uint8_t(v.binding)) : nullptr;
    const motion_pass::Packet::Bind* shared = nullptr;
    if (reuse)
      for (uint32_t k = 0; k < share->bindCount; ++k)
        if (share->binds[k].binding == v.binding + 16) shared = &share->binds[k];
    if (shared) {
      before = *shared;
    } else if (pv && !pv->referred() && pv->size) {
      const bool same = cv && !cv->referred() && cv->size == pv->size &&
                        !std::memcmp(prev->bytes.data() + pv->offset, cur->bytes.data() + cv->offset, pv->size);
      if (!same) {
        changed = true;
        const UploadSlice s = snapshot(prev->bytes.data() + pv->offset, pv->size, 4);
        before = {v.binding + 16, s.buffer, s.offset};
      }
    } else if (pv && cv && pv->referred() && pv->guestAddr != cv->guestAddr) {
      p.previous = false;  // (a cached mesh that moved address: its old data is not kept; zero motion)
      p.why = 2;
    }
    if (share && eye == 0 && share->bindCount < share->binds.size()) share->binds[share->bindCount++] = before;
    p.binds[p.bindCount++] = before;
  }
  if (share && eye == 0) share->filled = true;
  // nothing changed since the previous frame: its motion is zero, which the cleared target holds already (fix 2)
  // (eye 1 reusing eye 0's uploads: the shared blocks were not compared here, so eye 0's verdict stands, above)
  if (!changed && p.previous && !reuse) {
    if (share && eye == 0) share->still = true;
    motion_pass::note_still();
    return;
  }
#ifdef WWHD_MOBILE_GPU_PATHS
  if (!built)
#endif
  {
    p.set = stage_descriptor_set(true, twin->sets[0], writes.data(), n);
    if (!p.set) return;
  }
  p.indexed = rec.indexed;
  p.indexBuffer = rec.indexBuffer;
  p.indexOffset = rec.indexOffset;
  p.indexType = rec.indexType;
  p.count = rec.drawCount;
  p.instances = instances;
  p.baseVertex = baseVertex;
  p.viewport = rec.viewport;
  if (background) p.viewport.minDepth = p.viewport.maxDepth = backgroundDepth;
  p.scissor = rec.scissor;
  p.depth = depth;
  p.depthSlice = depthSlice;
#ifdef WWHD_MOBILE_GPU_PATHS
  if (built) {
    built->p = p;
    built->n = n;
    for (uint32_t k = 0; k < n; ++k) {
      built->writes[k] = writes[k];
      built->infos[k] = infos[k];
      if (writes[k].pBufferInfo) built->writes[k].pBufferInfo = &built->infos[k];  // (the image infos stay rec's)
    }
    built->ok = true;
    return;
  }
#endif
  motion_pass::add(p);
}
void draw(const uint32_t *r, uint32_t prim, uint32_t count, uint32_t indexType,
          uint32_t indexAddr, uint32_t baseVertex, uint32_t instances, bool* viewIndependent) {
  thread_check::note(thread_check::Area::Draws);  // (diagnostics: thread_check.h)
  const uint32_t dirtyGroups = gx2::take_thread_dirty_groups();  // register groups changed since the previous draw
  // (per thread: reset_pipeline_lookup_cache clears the calling thread's; another thread's goes at its next draw)
  static thread_local uint64_t continuedGeneration = 0;
  if (const uint64_t g = pipelineMemoGeneration.load(std::memory_order_acquire); continuedGeneration != g) {
    continuedGeneration = g;
    forget_continued();
  }
  auto& continued = continuedEyes()[stereoEye()];
  // the top-level stereo draw only, never the AO replay below
  StereoShaders* shadersOut = std::exchange(stereoShadersOut(), nullptr);
  const bool backdropOn = backdrop_draws();
  const StereoShaders* backdropReplay = backdropOn ? std::exchange(backdropShadersIn, nullptr) : nullptr;
  const StereoShaders* shadersIn = backdropReplay ? backdropReplay : std::exchange(stereoShadersIn(), nullptr);
  // Record exact upload slices/descriptor bindings while resolving the original
  // draw. The normal off path records nothing and the stereo snapshot is reused.
  if (backdropOn && !shadersOut && !stereoEye() && !aoPrivateReplay() && gamepad_backdrop_capture_active()) {
    backdropSnapshot.reset();shadersOut=&backdropSnapshot;
  }
  if(backdropReplay && (!backdropReplay->replay || backdropReplay->submission!=R.submissionGeneration))return;
  // eye 1: eye 0's draw, replayed (StereoShaders)
  const StereoShaders* replay = shadersIn && shadersIn->replay && !aoPrivateReplay() &&
      shadersIn->submission == R.submissionGeneration ? shadersIn : nullptr;
  if (stereoEye() == 1 && !aoPrivateReplay()) eye_thread::note_draw(replay != nullptr);  // (diagnostics: either thread)
  // (diagnostics, docs/VR_MULTIVIEW.md stage M1) WWHD_VK_MULTIVIEW_STATS=1: what eye 1's draws would need to be drawn with
  // eye 0's in one multiview draw: replays that differ only in their targets, also in eye-shifted uniforms, also in
  // eye-specific textures, and draws eye 1 resolves in full (render thread only: eye 1 not on a thread of its own)
  static const bool multiviewStats = switches::env_on("WWHD_VK_MULTIVIEW_STATS");
  if (multiviewStats && stereoEye() == 1 && !aoPrivateReplay() && !draw_threads::on_draw_thread()) {
    static uint64_t frame0 = 0, targetsOnly = 0, uniforms = 0, textures = 0, full = 0, uniformBlocks = 0, eyeTextures = 0;
    if (!replay) ++full;
    else {
      const uint32_t u = replay->stages[0].uniformCount + replay->stages[1].uniformCount;
      const uint32_t t = replay->stages[0].eyeTextureCount + replay->stages[1].eyeTextureCount;
      uniformBlocks += u;
      eyeTextures += t;
      ++(t ? textures : u ? uniforms : targetsOnly);
    }
    if (R.frame >= frame0 + 600) {
      const double f = R.frame > frame0 && frame0 ? double(R.frame - frame0) : 600.0;
      LOG("[vulkan multiview] eye 1's draws per frame: %.0f replays differing only in targets, %.0f also in eye-shifted "
          "uniforms (%.1f blocks a draw), %.0f also in eye-specific textures (%.1f a draw), %.0f resolved in full",
          targetsOnly / f, uniforms / f, uniforms + textures ? double(uniformBlocks) / double(uniforms + textures) : 0.0,
          textures / f, textures ? double(eyeTextures) / double(textures) : 0.0, full / f);
      frame0 = R.frame;
      targetsOnly = uniforms = textures = full = uniformBlocks = eyeTextures = 0;
    }
  }
  if (clouds_skip_card()) return;  // clouds.cpp: toon clouds replace the game's cloud cards
  if (!count || !instances || ((prim == 0x13 || prim == 0x14) && count < 4))
    return;
  if (r[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22))
    return;
  ((uint32_t *)r)[REGADDR::VGT_PRIMITIVE_TYPE] = prim;
  // continued-draw fast path (see fastpath_config); the AO replay draw stays outside of it
  const auto& fastPath = fastpath_config();
  // a replayed eye 1 draw records no continuation of its own; nor does a batched run's (batch_draw)
  BatchDraw* const batch = batch_draw();
  // (the game's UI into its own image, ui_layer: its targets and blend are not the registers' alone, so neither continued
  // from a draw before it nor continued by one after)
  const bool uiDraw = ui_layer::candidate();
  const bool fastPathDraw = fastPath.on && !aoPrivateReplay() && !replay && !batch && !uiDraw &&
                            !(backdropOn && gamepad_backdrop_replaying());
  if (replay || batch || uiDraw) {
    continued.valid = false;
    continued.invalidReason = kNoState;
    recentEyes()[stereoEye() & 1] = {};
  }
  bool fast = false;
  if (fastPathDraw) {
    if (fastPath.stats) report_continued_stats();
    ContinuedReason reason = continued_reason(r, prim);
    bool fromRecent = false;
    if (fastPath.recent && (reason == kRegisters || reason == kPrimitive)) {
      // the previous draw's state goes to the recent states; a recent state that holds again becomes the continued one
      auto& recent = recentEyes()[stereoEye() & 1];
      const auto& log = gx2::thread_change_log();
      // this draw's fetch shader, looked up once if a state's fetch shader registers differ
      bool fetchLooked = false;
      LatteFetchShader* fetch = nullptr;
      uint64_t fetchKey = 0;
      auto holds = [&](const ContinuedDraw& e) {
        bool fetchMoved = false;
        if (!(e.valid && e.frame == R.frame && e.partEpoch == part_epoch() && e.surfaceEpoch == g_surface_epoch &&
              e.prim == prim && e.aoMode == ao_mode() && e.aoHires == ao_hires_enabled() && e.aniso == aniso_enabled() &&
              log.unchanged_since(e.logPos, r, kRecentChangeLimit, mmSQ_PGM_START_FS, 2, &fetchMoved)))
          return false;
        if (!fetchMoved) return true;
        if (!fetchLooked) {
          fetchLooked = true;
          fetch = vk::get_fetch_shader(r, &fetchKey, R.frame);
        }
        return fetch && fetch == e.fs && fetchKey == e.fsKey;
      };
      ContinuedDraw* found = holds(continued) ? &continued : nullptr;
      for (uint32_t back = 1; back <= kRecentStates && !found; ++back) {
        ContinuedDraw& e = recent.states[(recent.next + kRecentStates - back) % kRecentStates];
        if (holds(e)) found = &e;
      }
      if (found) {
        if (found != &continued) std::swap(continued, *found);
        continued.stateVersion = gx2::full_state_version();  // (its registers are this draw's: continued from here)
        continued.logPos = log.pos;
        reason = kContinued;
        fromRecent = true;
      } else {
        recent.states[recent.next] = continued;
        recent.next = (recent.next + 1) % kRecentStates;
      }
    }
    if (reason == kContinued) {
      // guest writes to the reused textures (write tracking, GX2Invalidate) upload here; an upload
      // can change lookups, so it ends the continuation
      for (const auto& stage : continued.stages)
        for (uint32_t i = 0; i < stage.count; ++i) upload_surface(stage.textures[i].guest);
      if (g_surface_epoch != continued.surfaceEpoch) reason = kUpload;
      // (texture packs) a replacement bound again only while its guest texture still takes it (changed contents and
      // a budget release also move the epoch; a texture the GPU has drawn into since does not), and noted as bound
      if (g_texture_packs_on && reason == kContinued) [[unlikely]]
        for (const auto& stage : continued.stages)
          for (uint32_t i = 0; i < stage.count; ++i)
            if (const auto& t = stage.textures[i]; t.guest != t.surface) {
              draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());
              if (replacement_for(t.guest) != t.surface) reason = kUpload;
            }
    }
    if (reason == kContinued && fastPath.verifyEvery) {
      static thread_local uint64_t verifyCounter = 0;
      if (++verifyCounter % fastPath.verifyEvery == 0) {
        ++continuedStats().verified;
        ++continuedStats().totalVerified;
        if (!verify_continued(r, prim)) reason = kVerifyMismatch;
      }
    }
    fast = reason == kContinued;
    if (fastPath.stats) {
      ++continuedStats().draws;
      ++continuedStats().reasons[reason];
      continuedStats().recent += fast && fromRecent;
      if (reason == kRegisters)
        for (int g = 0; g < gx2::kSgCount; ++g)
          if ((dirtyGroups & ~gx2::kSgContinuedMask) >> g & 1) ++continuedStats().groups[g];
    }
    // a full draw records a new state below; exceptions leave none
    continued.valid = false;
    continued.invalidReason = kNoState;
  }
  uint64_t fsKey = 0;
  LatteFetchShader* fs;
  vk::Shader *vs, *ps;
  if (fast) {
    fs = continued.fs;
    fsKey = continued.fsKey;
    vs = continued.vs;
    ps = continued.ps;
  } else if (shadersIn) {
    fs = shadersIn->fs;
    fsKey = shadersIn->fsKey;
    vs = shadersIn->vs;
    ps = shadersIn->ps;
    static const bool verifyReplay=std::getenv("WWHD_VK_STEREO_REPLAY_VERIFY")!=nullptr;
    if(verifyReplay && !backdropReplay) {
      uint64_t checkedKey=0;
      auto* checkedFetch=vk::get_fetch_shader(r,&checkedKey,R.frame);
      auto* checkedVs=checkedFetch?vk::translate(r,true,checkedFetch,checkedKey,R.frame,gx2::shader_state_gen()):nullptr;
      auto* checkedPs=checkedVs?vk::translate(r,false,checkedFetch,checkedKey,R.frame,gx2::shader_state_gen(),
                                           checkedVs->ready()?checkedVs:nullptr):nullptr;
      if(fs!=checkedFetch||fsKey!=checkedKey||vs!=checkedVs||ps!=checkedPs)
        exception_report::raise("stereo shader reuse differs from independent second-eye resolution");
      static thread_local uint64_t verified=0;
      if(++verified%100000==0)LOG("[stereo verify] %llu second-eye shader resolutions matched",(unsigned long long)verified);
    }
  } else {
    fs = vk::get_fetch_shader(r, &fsKey, R.frame);
    if (!fs)
      exception_report::raise("missing Vulkan fetch shader");
    vs = vk::translate(r, true, fs, fsKey, R.frame, gx2::shader_state_gen());
    ps = vk::translate(r, false, fs, fsKey, R.frame, gx2::shader_state_gen(),
                           vs && vs->ready() ? vs : nullptr);
  }
  if (batch && vs == batch->base) vs = batch->variant;  // (a batched run: its vertex shader's variant)
  rprof::mark(rprof::kShader);
  if (shadersOut && vs && vs->ready() && ps && ps->ready()) {
    shadersOut->fs = fs;
    shadersOut->fsKey = fsKey;
    shadersOut->vs = vs;
    shadersOut->ps = ps;
  }
  if (!vs || !vs->ready() || !ps || !ps->ready())
    exception_report::raise("Vulkan shader translation failed: " +
                             (vs && !vs->ready() ? vs->error
                              : ps               ? ps->error
                                                 : "missing shader"));
  // the point lights' passes left out (WWHD_VK_POINT_LIGHT_EYES=none): their buffer stays as cleared, no light added
  if (ps->kind == gfx::ProgramKind::PointLight && point_light_eyes() == kPointLightNone) [[unlikely]]
    return;
  // Both eyes use the same sun-cascade atlas, rendered from the light's view.
  // All camera-space passes, including the shadow mask, still draw twice.
  if(viewIndependent)
    *viewIndependent = sun_atlas_target(r) &&
      !LatteMRT::GetActiveColorBufferMask(ps->dec,*reinterpret_cast<const LatteContextRegister*>(r));
  const bool stripRestart = indexAddr &&
      (prim == 3 || prim == 6) &&
      (r[REGADDR::VGT_MULTI_PRIM_IB_RESET_EN] & 1);
  const uint32_t restartIndex = r[REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX];
  // Native guest index bytes need no widening or temporary vector. Strip
  // pipelines always enable restart for portability, so 16-bit 0xffff is only
  // safe when it is also the guest's enabled marker. Preserve other markers
  // through the existing uint32 normalization path.
  const bool nativeIndices = indexAddr && (indexType == 0 || indexType == 1) &&
      (prim == 1 || prim == 2 || prim == 3 || prim == 4 || prim == 6) &&
      ((prim != 3 && prim != 6) ||
       (indexType == 0 ? stripRestart && restartIndex == UINT16_MAX
                       : !stripRestart || restartIndex == UINT32_MAX));
  // Draw uploads copy the converted bytes before the optional AO replay calls
  // draw again. Retain CPU capacity; queued GPU work owns separate arena slices.
  PER_THREAD_STATIC(indices, std::vector<uint32_t>);
  indices.clear();
  // Conversion emits a known number of indices. Allocate once rather than
  // repeatedly growing and copying the vector for every indexed draw.
  size_t convertedCount = indexAddr ? size_t(count) : 0;
  switch (prim) {
  case 5: convertedCount = count > 2 ? size_t(count - 2) * 3 : 0; break;
  case 0x13: convertedCount = size_t(count / 4) * 6; break;
  case 0x14: convertedCount = count >= 4 ? size_t((count - 2) / 2) * 6 : 0; break;
  case 0x12: convertedCount = size_t(count) + 1; break;
  default: break;
  }
  static const bool specializeIndices = [] {
    const char* e = std::getenv("WWHD_VK_SPECIALIZE_INDICES");
    return e && !std::strcmp(e, "1");
  }();
  // ld16/ld32 use uint32 guest-EA arithmetic. A wrapped BE range stays on
  // that original path instead of replacing it with linear host addressing.
  const uint64_t guestReads = prim == 0x12 ? std::max(count, 1u) : count;
  const uint64_t guestBytes = guestReads * (indexType == 4 ? 2 : 4);
  const bool guestWrap = indexAddr && (indexType == 4 || indexType == 9) &&
      uint64_t(indexAddr) + guestBytes > 0x100000000ull;
  VkPrimitiveTopology topology;
  // Converted (big-endian, fan, quad, loop, other restart marker) index data from the buffer cache:
  // keyed by everything the conversion reads; a hit skips the conversion.
  // (stage 2.8) held from the converted indices' lookup to their upload or check: one step for the shared cache
  std::optional<draw_threads::CacheLock> bufferLocked;
  bufcache::Entry* convertedEntry = nullptr;
  // (the converted indices' slice and size, taken under the cache's lock: the entry itself is read after the lock is
  // let go no more. Another drawing thread can copy the same entry again in between, its region dropped and replaced,
  // and the slice read from the old one crashed in buffer_cache_slice: seen in the headset, 2026-11-10, once ranges
  // were copied again each game step, Cache::stepScoped)
  UploadSlice convertedSlice{};
  uint32_t convertedBytes = 0;
  bool convertedHit = false;
  bufcache::Key convertedKey{};
  if (replay) {
    topology = replay->topology;  // eye 0 prepared the indices
  } else {
  if (indexAddr && !nativeIndices && !guestWrap && buffer_cache_enabled() &&
      guestBytes <= (8u << 20) && uint64_t(indexAddr) + guestBytes <= 0x100000000ull) {
    convertedKey = {indexAddr, bufcache::kIndexConverted,
        uint64_t(count) | uint64_t(prim & 0xff) << 32 | uint64_t(indexType & 0xff) << 40 |
            uint64_t(stripRestart) << 48 | uint64_t(specializeIndices) << 49,
        stripRestart ? restartIndex : 0};
    bufferLocked.emplace(draw_threads::buffer_mutex());
    switch (buffer_cache().lookup(convertedKey, uint32_t(guestBytes), convertedEntry)) {
    case bufcache::kHit: convertedHit = true; break;
    case bufcache::kMiss: break;  // armed: convert below, then upload
    case bufcache::kBypass: convertedEntry = nullptr; break;
    }
  }
  if (convertedHit && !buffer_cache_verify()) {
    topology = primitive_topology(prim);
  } else if (specializeIndices && !nativeIndices && !guestWrap) {
    switch (prim) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: case 0x12: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: case 5: case 0x13: case 0x14:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: exception_report::raise("unsupported Vulkan primitive");
    }
    vk::convert_indices(indexAddr ? mem::ptr(indexAddr) : nullptr, prim, count,
                        indexType, stripRestart, restartIndex, indices);
  } else {
  if (!nativeIndices) indices.reserve(convertedCount);
  auto idx = [&](uint32_t i) {
    if (!indexAddr)
      return i;
    uint32_t value;
    switch (indexType) {
    case 0:
      value = ((uint16_t *)mem::ptr(indexAddr))[i];
      break;
    case 1:
      value = ((uint32_t *)mem::ptr(indexAddr))[i];
      break;
    case 4:
      value = ld16(indexAddr + i * 2);
      break;
    case 9:
      value = ld32(indexAddr + i * 4);
      break;
    default:
      exception_report::raise("unsupported index type");
    }
    // All host index buffers use uint32, whose native restart marker differs
    // from the guest's configurable marker (including 16-bit 0xffff).
    return stripRestart && value == restartIndex ? UINT32_MAX : value;
  };
  switch (prim) {
  case 1:
    topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    break;
  case 2:
    topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    break;
  case 3:
    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    break;
  case 4:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    break;
  case 5:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (uint32_t i = 1; i + 1 < count; i++)
      indices.insert(indices.end(), {idx(0), idx(i), idx(i + 1)});
    break;
  case 6:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    break;
  case 0x13:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (uint32_t i = 0; i + 3 < count; i += 4)
      indices.insert(indices.end(), {idx(i), idx(i + 1), idx(i + 2), idx(i),
                                     idx(i + 2), idx(i + 3)});
    break;
  case 0x14:
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    for (uint32_t i = 0; i + 3 < count; i += 2)
      indices.insert(indices.end(), {idx(i), idx(i + 1), idx(i + 2), idx(i + 1),
                                     idx(i + 3), idx(i + 2)});
    break;
  case 0x12:
    topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    for (uint32_t i = 0; i < count; i++)
      indices.push_back(idx(i));
    indices.push_back(idx(0));
    break;
  default:
    exception_report::raise("unsupported Vulkan primitive");
  }
  if (!nativeIndices && indices.empty() && indexAddr) {
    indices.resize(count);
    for (uint32_t i = 0; i < count; i++)
      indices[i] = idx(i);
  }
  }
  if (convertedEntry) {
    const bool hostStrip = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                           topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    if (!convertedHit) {
      if (indices.empty() ||
          !buffer_cache().upload(*convertedEntry, indices.data(), uint32_t(indices.size() * 4))) {
        convertedEntry = nullptr;
      } else {
        rprof::add_upload(rprof::kUpIndex, indices.size() * 4);
        cached_index_extent(*convertedEntry, indices.data(), indices.size(), 4, hostStrip, 0);
      }
    } else if (buffer_cache_verify()) {
      uint32_t diff = 0;
      if (convertedEntry->outSize != indices.size() * 4 ||
          !buffer_cache().verify(*convertedEntry, indices.data(), uint32_t(indices.size() * 4), &diff))
        buffer_cache_mismatch("converted indices", *convertedEntry, uint32_t(indices.size() * 4), diff);
    }
    if (convertedEntry) {
      convertedBytes = convertedEntry->outSize;
      convertedSlice = buffer_cache_slice(*convertedEntry, convertedBytes);
    }
  }
  }
  bufferLocked.reset();
  rprof::mark(rprof::kIndices);
  DrawTargets targets;
  if(backdropReplay) targets=backdropReplay->targets;
  else if (fast) targets = continued.targets;
  else if (!(replay ? resolve_replay_targets(r, ps, targets) : resolve_targets(r, ps, targets)))
    return;
  if (auto* backdrop = backdropOn ? gamepad_backdrop_replay_target() : nullptr) {
    targets.colors[0] = backdrop;
    targets.slices[0] = 0;
  }
  // (the UI layer: drawn once, by eye 0, into its own image; eye 1's copy of a UI draw into its picture is left out)
  if (ui_layer::trace()) [[unlikely]]
    LOG("[vr ui trace] eye %d VS %08X PS %08X count %u: into %s%s (%ux%u)%s%s", stereoEye(), r[mmSQ_PGM_START_VS] << 8,
        r[mmSQ_PGM_START_PS] << 8, count, ui_layer::is(targets.target) ? "the UI image" : "target",
        ui_layer::is(targets.target) || !targets.target ? "" : " (not the UI image)",
        targets.target ? targets.target->extent.width : 0, targets.target ? targets.target->extent.height : 0,
        fast ? " continued" : "", replay ? " replay" : "");
  if (stereoEye() == 1 && ui_layer::candidate() && targets.target && !ui_layer::is(targets.target) &&
      stereo_screen_target(targets.target->width, targets.target->height))
    return;
  auto& colors = targets.colors;
  auto& slices = targets.slices;
  Surface* depth = targets.depth;
  const uint32_t depthSlice = targets.depthSlice, width = targets.width, height = targets.height;
  const float sx = targets.sx, sy = targets.sy;
  clouds_before_draw(r, ps, colors[0], slices[0], depth);  // clouds.cpp: cloud shadows
  rprof::mark(rprof::kTargets);
  // (diagnostics) WWHD_VK_TARGET_STATS=1: draws per frame by target size and eye, every 600 frames (which passes
  // cost the draws: docs/VR_PERFORMANCE.md)
  static const bool targetStats = std::getenv("WWHD_VK_TARGET_STATS") != nullptr;
  if (targetStats) {
    static thread_local std::map<uint64_t, uint64_t> n;
    static thread_local uint64_t frames0 = 0;
    ++n[uint64_t(width) << 32 | uint64_t(height) << 1 | uint64_t(stereoEye() & 1)];
    if (R.frame >= frames0 + 600) {
      std::vector<std::pair<uint64_t, uint64_t>> v(n.begin(), n.end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::string line;
      for (size_t k = 0; k < v.size() && k < 10; ++k) {
        char b[64];
        std::snprintf(b, sizeof b, " %ux%u/e%u:%.0f", unsigned(v[k].first >> 32), unsigned((v[k].first >> 1) & 0x7FFFFFFF),
                      unsigned(v[k].first & 1), double(v[k].second) / 600.0);
        line += b;
      }
      LOG("[vulkan targets] draws per frame by target:%s", line.c_str());
      n.clear();
      frames0 = R.frame;
    }
  }
  // 60 fps (generated): a HUD draw into the TV picture writes its coverage too (only while it runs;
  // a continued draw keeps the targets and pipeline it was resolved with)
  uint32_t covSlot = 0;
  // (VR fork) never in a start that tries VR (framegen.h available), nor on a draw thread (its coverage variants are
  // the render thread's): the flag first, so those draws make no call
  static const bool framegenPossible = framegen::available();
  if (framegenPossible && !fast && !replay && !backdropReplay && !aoPrivateReplay() && !draw_threads::on_draw_thread() &&
      reproj::hud_coverage_wanted())
    if (Surface* cov = reproj::hud_coverage_target(colors[0])) {
      bool alone = true;
      for (int i = 1; i < 8; i++) alone = alone && !colors[i];
      uint32_t location = 0;
      if (alone && colors[0]->fmt.kind == FormatInfo::FLOAT && cov->extent.width == width && cov->extent.height == height &&
          coverage_words(ps, location) && location > 0 && location < 8) {
        colors[location] = cov;
        slices[location] = 0;
        covSlot = location;
      } else {
        reproj::hud_coverage_failed();  // this frame's in-between one: the HUD found by comparing pictures
      }
    }
  Pipeline* pp = fast ? continued.pipeline
      : !backdropReplay && replay && replay->formats == target_formats(colors, depth) ? replay->pipeline
      : &pipeline(r, vs, ps, fs, topology, colors, depth, covSlot);
  if (covSlot) {  // its coverage variant still compiling (or failed): drawn as usual, the frame generated the old way
    if (pp->pendingCompile) resolve_pending_pipeline(*pp, false);
    if (!pp->pipeline || pp->pendingCompile) {
      colors[covSlot] = nullptr;
      covSlot = 0;
      reproj::hud_coverage_failed();
      pp = &pipeline(r, vs, ps, fs, topology, colors, depth);
    }
  }
  auto &p = *pp;
  rprof::mark(rprof::kPipeline);
  if (!p.pipeline && !p.pendingCompile) return;
  if (skip_while_made(p)) return;
  // Shader compile Fast (background_shaders.h): a draw that may be left out only waits the frame's
  // budget and is left out of this frame if its pipeline is not ready by then
  // (VR fork: not on a draw thread: the tracker is the render thread's, unlocked; a draw there waits as before)
  const bool trackTargets = bgshaders::enabled() && !aoPrivateReplay() && !draw_threads::on_draw_thread() &&
                           !(backdropOn && gamepad_backdrop_replaying());
  if (p.pendingCompile && trackTargets && !background_wait(r, p, colors, depth)) return;
  if (p.pendingCompile && !bgshaders::enabled()) bgshaders::exact(bgshaders::kOff);  // (the AO pass's private
                                                                                      // copy of a draw waits)
  // A capture is published only after its private pipeline is actually ready.
  // Deferred command counts cannot certify a future driver compilation success.
  // Never wait for it on the render thread (the phone's bottleneck): poll, and
  // while it compiles in the background this pane is not captured (edge-colour
  // fallback); a later presentation captures it. Its recipe joins the pipeline
  // database like any other, so later sessions start with it warmed.
  if (backdropOn && gamepad_backdrop_replaying() && p.pendingCompile) {
    resolve_pending_pipeline(p, false);
    if (p.pendingCompile) { gamepad_backdrop_debug("pipeline-pending", R.frame); return; }
  }
  if (p.pendingCompile && !begin_exact_preparation()) resolve_pending_pipeline(p);
  if (trackTargets) track_drawn(r, colors, depth);
  if (!p.pipeline && !p.pendingCompile) return; // terminal driver failure, not pending
  if(feedback_stats_enabled()) report_feedback_stats();
  if(vertex_window_stats_enabled()) report_vertex_window_stats();
  FeedbackStatsProbe feedbackProbe;
  auto* probe=feedback_stats_enabled()?&feedbackProbe:nullptr;
  const float cameraEye = stereo::frame.mode != stereo::Off && !stereo::layout_draw_depth() &&
      stereo_screen_target(targets.target->width,targets.target->height)
      ? stereo::eye_offset(stereoEye()) : 0.0f;
  // (docs/VR_EYE_PROJECTION.md) a layout into an eye's picture takes the eye's own frustum (stereo::draw_frustum)
  // (XR; or desktop stereo with WWHD_TEST_EYE_FRUSTUM, the test aid: else its frustums are the identity)
  stereo::t_layoutInEye = stereo::frame.mode != stereo::Off && stereo::layout_draw_depth() &&
      stereo_screen_target(targets.target->width,targets.target->height) &&
      !(stereo::frame.frustum[0].identity() && stereo::frame.frustum[1].identity());
  if (fov_diag::kOn) [[unlikely]] fov_diag::layout_draw_begin(stereo::t_layoutInEye, stereoEye(), r[mmSQ_PGM_START_VS]);
  // (the sun shadow and the point lights rebuild positions from depth: both stages adjusted for each eye, never
  // replayed from eye 0 nor drawn as multiview; the name kept from when the sun shadow was the only one)
  const bool sunShadow = ps->kind == gfx::ProgramKind::SunShadowMask ||
                         (ps->kind == gfx::ProgramKind::PointLight && point_light_eyes() != kPointLightOff);
  const gfx::ProgramKind eyeRebuild = sunShadow ? ps->kind : gfx::ProgramKind::Other;
  const bool recordState = fastPathDraw && !fast;
  if (recordState)
    for (auto& stage : continued.stages) stage = {};
  // eye 1 replays a stage of eye 0's draw at the same scale through the same pipeline
  const bool replayStages = replay && &p == replay->pipeline && replay->sx == sx && replay->sy == sy;
  StageResources vres, pres;
  // (diagnostics) WWHD_VK_TARGET_STATS=1: how eye 1's draws are prepared (replayed from eye 0, or in full)
  static thread_local uint64_t eyeStats[6] = {}, eyeStatsFrame = 0;
  if (targetStats && stereoEye() == 1) {
    ++eyeStats[0];
    eyeStats[1] += replay != nullptr;
    eyeStats[2] += replayStages;
    eyeStats[3] += replayStages && replay->stages[0].valid;
    eyeStats[4] += replayStages && replay->stages[1].valid;
  }
  if (targetStats && R.frame >= eyeStatsFrame + 600 && eyeStats[0]) {
    LOG("[vulkan stereo] eye 1 draws per frame %.0f: replayed %.0f%%, same pipeline and scale %.0f%%, vertex stage "
        "replayable %.0f%%, pixel stage %.0f%%, stage replays that failed %.0f / frame", eyeStats[0] / 600.0,
        100.0 * eyeStats[1] / eyeStats[0], 100.0 * eyeStats[2] / eyeStats[0], 100.0 * eyeStats[3] / eyeStats[0],
        100.0 * eyeStats[4] / eyeStats[0], eyeStats[5] / 600.0);
    for (auto& v : eyeStats) v = 0;
    eyeStatsFrame = R.frame;
  }
  auto replayed = [&](int stage, bool ok) { if (targetStats && !ok && replayStages && replay->stages[stage].valid) ++eyeStats[5]; return ok; };
  if (auto& ab = g_artisticBias; ab.sx != sx || ab.sy != sy) {
    ab.sx = sx;
    ab.sy = sy;
    ab.bias = ::gfx::artistic_mips::resolution_bias(sx, sy);
  }
  if(backdropReplay) {
    // Identical shaders and dynamic-uniform mode give identically defined
    // descriptor-set layouts, even though the private alpha pipeline differs.
    // Bind the already resolved sets/offsets, never replay_stage/bind_stage.
    if(!backdrop_reuse_resources(*backdropReplay,p,vs,ps,R.submissionGeneration,vres,pres))return;
  } else {
  if (!replayStages || !replay->stages[0].valid ||
      !replayed(0, replay_stage(replay->stages[0], r, vs, cameraEye, colors, depth, vres)))
#ifdef WWHD_MOBILE_GPU_PATHS
    vres = bind_stage(r, vs, p.sets[0], p.dynamicUniforms || p.partialUniforms, p.dynamicMask[0], sx, sy, colors, depth,
                      probe, cameraEye, eyeRebuild,
#else
    vres = bind_stage(r, vs, p.sets[0], p.dynamicUniforms, sx, sy, colors, depth, probe, cameraEye, eyeRebuild,
#endif
                      fast ? &continued.stages[0] : nullptr, recordState ? &continued.stages[0] : nullptr,
                      shadersOut ? &shadersOut->stages[0] : nullptr);
  if (!replayStages || !replay->stages[1].valid ||
      !replayed(1, replay_stage(replay->stages[1], r, ps, sunShadow ? cameraEye : 0, colors, depth, pres)))
#ifdef WWHD_MOBILE_GPU_PATHS
    pres = bind_stage(r, ps, p.sets[1], p.dynamicUniforms || p.partialUniforms, p.dynamicMask[1], sx, sy, colors, depth,
                      probe,
#else
    pres = bind_stage(r, ps, p.sets[1], p.dynamicUniforms, sx, sy, colors, depth, probe,
#endif
                      sunShadow ? cameraEye : 0, eyeRebuild,
                      fast ? &continued.stages[1] : nullptr, recordState ? &continued.stages[1] : nullptr,
                      shadersOut ? &shadersOut->stages[1] : nullptr);
  }
  if (shadersOut) {
    shadersOut->targets=targets;
    shadersOut->stages[0].resources = vres;
    shadersOut->stages[1].resources = pres;
    shadersOut->submission = R.submissionGeneration;
    shadersOut->pipeline = &p;
    shadersOut->topology = topology;
    shadersOut->formats = target_formats(colors, depth);
    shadersOut->sx = sx;
    shadersOut->sy = sy;
    shadersOut->width = width;
    shadersOut->height = height;
    shadersOut->vertexBindCount = 0;
  }
  // (docs/VR_MULTIVIEW.md, stage M4) both eyes in this one draw, when eye 1 would only replay it
  MultiviewDraw mv;
#ifdef WWHD_MOBILE_GPU_PATHS
  t_mvRefusedMixed = false;
#endif
  const bool mvTried = shadersOut && multiview_draws();
  if (mvTried) {
#ifdef WWHD_MOBILE_GPU_PATHS
    t_mvLastWhy = uint8_t(kMvDrawn);  // (no stale reason: kMvSkipBuilding below reads this draw's)
#endif
    multiview_draw(r, vs, ps, p, targets, vres, pres, sunShadow, *shadersOut, mv);
  }
#ifdef WWHD_MOBILE_GPU_PATHS
  // (2026-11-07) a draw into two-eye images whose two-eye pipeline is still being built is left out of this frame for
  // both eyes, as a new pipeline's draws are (skip_while_made), instead of drawn one eye at a time: that marked the
  // images per eye for frames in a row as each new shader came (a Quest 3 log: the scene's depth held per eye most of
  // the session, "pipeline building", 150-580 of its draws a frame drawn twice). Not while an image is held per eye
  // anyway (no multiview draw to mix with). WWHD_VK_MULTIVIEW_SKIP_BUILDING=0: drawn per eye as before
  static const bool kSkipBuilding = [] {
    const char* e = std::getenv("WWHD_VK_MULTIVIEW_SKIP_BUILDING");
    return !(e && e[0] == '0');
  }();
  if (kSkipBuilding && kMultiviewNoMix && mvTried && !mv.on && stereoEye() == 0 && t_mvLastWhy == uint8_t(kMvBuilding)) {
    bool twoEye = false, held = false;
    auto look = [&](const Surface* s) {
      if (!s || !s->layerTwin || s->layerTwin->image != s->image) return;
      twoEye = true;
      held = held || written_per_eye(s);
    };
    look(targets.depth);
    for (const Surface* c : targets.colors) look(c);
    // (at most 30 frames in a row for one pipeline: a twin that never comes, e.g. no SPIR-V kept for a stage, is drawn
    // per eye as before after that; relaxed atomics: the scene's threads draw with the same pipeline)
    bool capped = false;
    if (twoEye && !held) {
      std::atomic_ref<uint64_t> first(p.mvSkipFirst), last(p.mvSkipLast);
      const uint64_t prev = last.load(std::memory_order_relaxed);
      if (!prev || R.frame > prev + 2) first.store(R.frame, std::memory_order_relaxed);
      last.store(R.frame, std::memory_order_relaxed);
      capped = R.frame > first.load(std::memory_order_relaxed) + 30;
    }
    if (twoEye && !held && !capped) {
      // (nothing of this draw is in a stream yet: its sets are bound with the pass, below; the state bind_stage left
      // in `continued` is not this draw's, so the next draw resolves its own)
      continued.valid = false;
      continued.invalidReason = kNoState;
      shadersOut->multiview = true;  // (eye 1 draws nothing either: stereo_draw_one, draw_both, the eye's thread)
      continuedEyes()[1].valid = false;
      continuedEyes()[1].invalidReason = kNoState;
      recentEyes()[1] = {};
      ++multiviewWhy[kMvSkipBuilding];
      return;
    }
  }
  // (mixed passes) eye 0's draw into two-eye images one eye at a time: those images stay per eye for kMixHoldFrames
  if (kMultiviewNoMix && shadersOut && multiview_draws() && !mv.on && !t_mvRefusedMixed && stereoEye() == 0) {
    // (diagnostics: what the draw holding it per eye was, two byte stores: listed every 10 s, multiview_log)
    const uint8_t why = t_mvLastWhy, kind = uint8_t(ps->kind);
    auto mark = [why, kind](Surface* s) {
      if (s && s->layerTwin && s->layerTwin->image == s->image) {
        // (the last 8 frames' marks, bit 0 this frame's: written_per_eye's long hold)
        const uint64_t gap = s->perEyeFrame ? R.frame - s->perEyeFrame : 8;
        if (gap) s->perEyeHistory = gap >= 8 ? uint8_t(1) : uint8_t((unsigned(s->perEyeHistory) << gap) | 1u);
        s->perEyeFrame = R.frame;
        s->perEyeWhy = why;
        s->perEyeKind = kind;
      }
    };
    mark(targets.depth);
    for (Surface* c : targets.colors) mark(c);
  }
#endif
  rprof::mark(rprof::kMultiview);  // (multiview_draw's sets and its end; the pass's own work is "pass", below)
  if (fastPathDraw) {
    // the resolved state of this draw: continued by the next draws while nothing breaking changes
    auto& c = continued;
    c.valid = fast || (c.stages[0].continuable && c.stages[1].continuable);
    c.invalidReason = kNotContinuable;
    if (!fast) {
      c.stateVersion = gx2::full_state_version();
      c.logPos = gx2::thread_change_log().pos;
      c.surfaceEpoch = g_surface_epoch;
      c.frame = R.frame;
      c.partEpoch = part_epoch();
      c.prim = prim;
      c.topology = topology;
      c.aoMode = ao_mode();
      c.aoHires = ao_hires_enabled();
      c.aniso = aniso_enabled();
      c.fs = fs;
      c.fsKey = fsKey;
      c.vs = vs;
      c.ps = ps;
      c.targets = targets;
      c.scissorBR = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
      c.pipeline = &p;
    }
  }
  // Texture uploads, feedback copies, and sampling transitions above may have
  // ended the previous pass. Reuse only its exact active attachment set.
  bool reusePass = pass_state().rendering && pass_state().tracked && pass_state().colors == colors &&
                   std::equal(std::begin(slices), std::end(slices),
                              pass_state().slices.begin()) &&
                   pass_state().depth == depth && pass_state().depthSlice == depthSlice &&
                   pass_state().width == width && pass_state().height == height &&
                   pass_state().multiview == mv.on;
  // (the second drawing thread's: the layouts its span starts with, docs/VR_SCENE_THREADS.md "S6: B's reads deferred")
  const auto layoutOf = [](const Surface* s) {
    return switches::kSceneTwo && scene_thread::on_scene_b() ? VkImageLayout(scene_thread::b_layout(s)) : s->layout;
  };
  for (auto *color : colors)
    if (color && layoutOf(color) != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
      reusePass = false;
  if (depth && layoutOf(depth) != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    reusePass = false;
  for (auto *twin : mv.twins)  // (a multiview pass: eye 1's layers too)
    if (twin && layoutOf(twin) != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
      reusePass = false;
  if (mv.depthTwin && layoutOf(mv.depthTwin) != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    reusePass = false;
  struct DrawStateCache {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
    DescriptorBindProbe descriptorProbe;
    struct VertexBind { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceSize offset=0; bool valid=false; };
    std::array<VertexBind,16> vertexBinds{};
    VkCommandBuffer vertexCommand=VK_NULL_HANDLE;
    uint64_t vertexGeneration=0;
    bool vertex_bind_matches(VkCommandBuffer command,uint64_t generation,
                             uint32_t binding,VkBuffer buffer,VkDeviceSize offset) {
      if(vertexCommand!=command || vertexGeneration!=generation) {
        vertexBinds={};vertexCommand=command;vertexGeneration=generation;
      }
      if(binding>=vertexBinds.size()) return false;
      auto& old=vertexBinds[binding];
      const bool match=old.valid && old.buffer==buffer && old.offset==offset;
      old={buffer,offset,true};
      return match;
    }
    VkViewport viewport{};
    VkRect2D scissor{};
    std::array<uint32_t, 4> blend{};
    uint32_t stencilFront = 0, stencilBack = 0;
    bool viewportValid = false, scissorValid = false;
    bool blendValid = false, stencilValid = false;
    uint64_t partEpoch = 0;  // (docs/VR_SCENE_THREADS.md, stage S1)
  };
  // per eye: while stereo is on, each eye's pass stays open in its own command stream. Per thread too (stage 2.5):
  // each thread records its own streams. Within a stream it is right: it is cleared at every new pass, and every
  // switch between streams ends their passes (stereo_atlas_hold, stereo_atlas_emit, stereo_flush)
  PER_THREAD_STATIC(states, std::array<DrawStateCache, 2>);
  auto& state = states[stereoEye()];
  // A tracked active pass can only contain our draw commands. Every pass end,
  // including submit/fence/reset, makes reusePass false on the next draw.
  // Reset conservatively on a new pass so external command recording cannot
  // leave this cache claiming dynamic state that was never set in this buffer.
  if (!reusePass || state.partEpoch != part_epoch()) state = {};  // (stage S1: a scene part binds everything itself)
  state.partEpoch = part_epoch();
  auto cmd = command_buffer();
  auto bindIndex = [&](VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    if (state.indexBuffer == buffer && state.indexOffset == offset && state.indexType == type) return;
    record::vkCmdBindIndexBuffer(cmd, buffer, offset, type);
    state.indexBuffer = buffer; state.indexOffset = offset; state.indexType = type;
  };
  if (!reusePass) {
    end_encoder();
#ifdef WWHD_MOBILE_GPU_PATHS
    mix_barrier(colors, depth, mv.on);
#endif
    std::array<VkRenderingAttachmentInfo, 8> attachments{};
    uint32_t ncolor = 0;
    for (int i = 0; i < 8; i++) {
      attachments[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      attachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      if (colors[i]) {
        for (Surface* s : {colors[i], mv.twins[i]}) {  // (a multiview pass: eye 1's layer too)
          if (!s) continue;
          upload_surface(s);
          transition_image(s, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
        attachments[i].imageView = mv.on ? multiview_view(colors[i]) : layer_view(colors[i], slices[i]);
        ncolor = i + 1;
      }
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
      for (Surface* s : {depth, mv.depthTwin}) {
        if (!s) continue;
        upload_surface(s);
        transition_image(s, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
      }
      da.imageView = mv.on ? multiview_view(depth) : layer_view(depth, depthSlice);
      da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = {width, height};
    ri.layerCount = 1;
    ri.viewMask = mv.on ? 0b11 : 0;  // (stage M4) both eyes' layers
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachments = attachments.data();
    ri.pDepthAttachment = depth ? &da : nullptr;
    ri.pStencilAttachment = depth && depth->fmt.stencil ? &da : nullptr;
    if(R.gpuPassTimestampsEnabled) gpu_begin_render_scope(colors,depth,slices.data(),depthSlice,width,height);
    trace_pass(colors, ncolor, depth, depthSlice, width, height);
    begin_pass(cmd, ri, [&] {
      PassBegin pass; pass.formats.colorCount = ncolor;
      for (uint32_t i = 0; i < ncolor; ++i) if (colors[i]) {
        pass.formats.colors[i] = colors[i]->fmt.pixel; pass.layouts[i] = colors[i]->layout;
      }
      if (depth) {
        pass.formats.depth = depth->fmt.pixel; pass.formats.stencil = depth->fmt.stencil;
        pass.layouts[8] = depth->layout;
      }
      return pass;
    });
    ++draw_counters().renderPassCount;
    if (scene_stats::kOn) [[unlikely]] scene_stats::pass_begin(colors.data(), depth, width, height, mv.on);
    pass_state().rendering = true;
    pass_state().tracked = true;
    pass_state().colors = colors;
    std::copy(std::begin(slices), std::end(slices), pass_state().slices.begin());
    pass_state().depth = depth;
    pass_state().depthSlice = depthSlice;
    pass_state().width = width;
    pass_state().height = height;
    pass_state().multiview = mv.on;
    if (switches::kSceneHooks) [[unlikely]] scene_split_pass_begin(mv.on);  // (may hold the pass's draws: stage S2)
  }
  rprof::mark(rprof::kPass);
  if (scene_stats::kOn) [[unlikely]] scene_stats::draw();
  if (switches::kSceneHooks) [[unlikely]] scene_split_draw();
  if (!p.pipeline && !p.pendingCompile) return;
  const bool deferredPipelineDraw = bool(p.pendingCompile);
  if (p.pendingCompile) {
    auto* ownedPipeline = &p;
    record::emit_owned([ownedPipeline,cmd]() {
      resolve_pending_pipeline(*ownedPipeline);
      if(ownedPipeline->pipeline)
        record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ownedPipeline->pipeline);
    });
    state.pipeline = VK_NULL_HANDLE;
  } else if (const VkPipeline bound = mv.on ? mv.pipeline : p.pipeline; state.pipeline != bound) {
    record::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
    state.pipeline = bound;
  }
  VkDescriptorSet sets[] = {vres.set, pres.set};
  std::array<uint32_t, 34> dynamicOffsets{};
  std::copy_n(vres.dynamicOffsets.begin(), vres.dynamicOffsetCount, dynamicOffsets.begin());
  std::copy_n(pres.dynamicOffsets.begin(), pres.dynamicOffsetCount,
              dynamicOffsets.begin() + vres.dynamicOffsetCount);
  const bool preparationStats=preparation_stats_enabled();
  if (mv.on) {  // (stage M4) the multiview pipeline's layout, sets and offsets (a per-view block's twice)
    sets[0] = mv.sets[0];
    sets[1] = mv.sets[1];
    record::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mv.layout, 0, 2, sets, mv.offsetCount,
                                    mv.offsets.data());
    state.descriptorProbe = {};
  } else {
  static const bool skipRedundantBinds = [] {
    const char* e=std::getenv("WWHD_VK_SKIP_REDUNDANT_BINDS");
    return e && !std::strcmp(e,"1");
  }();
  DescriptorBindProbe::Matches matches;
  if(skipRedundantBinds || preparationStats)
    matches=state.descriptorProbe.observe(cmd,R.submissionGeneration,p.layout,sets,
        dynamicOffsets.data(),vres.dynamicOffsetCount,pres.dynamicOffsetCount);
  const auto bind=DescriptorBindProbe::bind_range(matches,skipRedundantBinds,
      vres.dynamicOffsetCount,pres.dynamicOffsetCount);
  if(preparationStats) {
    ++draw_counters().cpuPreparation.descriptorBindCandidates;
    draw_counters().cpuPreparation.descriptorWholeMatches+=matches.whole;
    for(size_t stage=0;stage<2;++stage)draw_counters().cpuPreparation.descriptorStageMatches[stage]+=matches.stage[stage];
    draw_counters().cpuPreparation.descriptorBindCalls+=bind.count!=0;
    draw_counters().cpuPreparation.descriptorSkippedSets+=2-bind.count;
  }
  if(bind.count)
    record::vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p.layout,
        bind.first,bind.count,sets+bind.first,bind.offsetCount,
        dynamicOffsets.data()+bind.offsetBegin);
  }
  VkViewport vp;
  VkRect2D sc;
  const bool reuseRaster = replay && replay->rasterReady && replay->sx == sx && replay->sy == sy &&
      replay->width == width && replay->height == height;
  if (reuseRaster) {
    vp = replay->viewport;
    sc = replay->scissor;
  } else {
  float xs = f32(r[REGADDR::PA_CL_VPORT_XSCALE]),
        xo = f32(r[REGADDR::PA_CL_VPORT_XOFFSET]),
        ys = f32(r[REGADDR::PA_CL_VPORT_YSCALE]),
        yo = f32(r[REGADDR::PA_CL_VPORT_YOFFSET]),
        zs = f32(r[REGADDR::PA_CL_VPORT_ZSCALE]),
        zo = f32(r[REGADDR::PA_CL_VPORT_ZOFFSET]);
  LATTE_PA_CL_CLIP_CNTL clip;
  std::memcpy(&clip, r + REGADDR::PA_CL_CLIP_CNTL, 4);
  vp = {(xo - xs) * sx,
                (yo - ys) * sy,
                xs * 2 * sx,
                ys * 2 * sy,
                clip.get_DX_CLIP_SPACE_DEF() ? zo : zo - zs,
                zo + zs};
  uint32_t tl = r[REGADDR::PA_SC_GENERIC_SCISSOR_TL],
           br = r[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
  // As the Metal renderer: the scaled edges are rounded with a tolerance, not truncated. 720 * (1200/720)
  // is just below 1200 in float (16:10 at 1.5x and 3x), which left the bottom row of every full-screen
  // pass undrawn, and its stale contents spread through the shadow mask's softening.
  auto lo = [](uint32_t v, float k, uint32_t limit) {
    return uint32_t(std::clamp(std::floor(double(v) * k + 0.01), 0.0, double(limit)));
  };
  auto hi = [](uint32_t v, float k, uint32_t limit) {
    return uint32_t(std::clamp(std::ceil(double(v) * k - 0.01), 0.0, double(limit)));
  };
  uint32_t x = lo(tl & 0x7fff, sx, width),
           y = lo((tl >> 16) & 0x7fff, sy, height),
           ex = hi(br & 0x7fff, sx, width),
           ey = hi((br >> 16) & 0x7fff, sy, height);
  float ax, ay;
  // (the UI layer's image: the factors its layouts were placed with, gx2_core.cpp OP_SET_PROJ_REGS)
  const bool uiImage = ui_layer::is(colors[0]);
  auto layoutFactors = [&] {
    if (!target_aspect_factors(r[mmCB_COLOR0_TILE] & 0xFFFF, r[mmCB_COLOR0_FRAG], ax, ay)) return false;
    if (uiImage) stereo::ui_layer_factors(aspect::game(), ax, ay);
    return true;
  };
  if (aspect::content_clip() && layoutFactors())
    aspect::panes::clip(width, height, ax, ay, x, y, ex, ey);
#ifdef WWHD_VR
  // VR text and menus drawn smaller (vr::layout_scale, gx2_core.cpp): layouts are clipped to the shrunk native screen
  // (moved like the text by vr::layout_offset_y), so what the game parks just outside the screen (a "GAME OVER", a
  // minigame timer) stays out of view instead of shrinking into it
  if (const float s = vr::layout_scale();
      s < 1.0f && stereo::layout_draw_depth() && layoutFactors()) {
    // (in the UI layer's image, square pixels over the symmetric field: the shrunk screen keeps its 16:9; the eye picture's
    // height factor cut the menus' top and bottom)
    const float rw = float(width) * s / ax,
                rh = ui_layer::is(colors[0]) ? rw * 9.0f / 16.0f : float(height) * s / ay;
    const float cx = float(width) * 0.5f * (1.0f + vr::layout_offset_x()), cy = float(height) * 0.5f * (1.0f - vr::layout_offset_y());
    auto px = [](float v, uint32_t limit) { return uint32_t(std::clamp(double(std::floor(v + 0.01f)), 0.0, double(limit))); };
    float x0 = cx - rw * 0.5f, x1 = cx + rw * 0.5f, y0 = cy - rh * 0.5f, y1 = cy + rh * 0.5f;
    // (docs/VR_EYE_PROJECTION.md) in an eye drawn with its own frustum the layout is where that frustum puts it
    // (not in the UI layer's image: drawn for the symmetric field, no eye's frustum)
    if (stereo::t_layoutInEye && !ui_layer::is(colors[0])) {
      const stereo::EyeFrustum f = stereo::frame.frustum[stereoEye() ? 1 : 0];
      auto mx = [&](float v) { return ((f.sx * (2.0f * v / float(width) - 1.0f) - f.ox) + 1.0f) * 0.5f * float(width); };
      auto my = [&](float v) { return (1.0f - (f.sy * (1.0f - 2.0f * v / float(height)) - f.oy)) * 0.5f * float(height); };
      x0 = mx(x0), x1 = mx(x1), y0 = my(y0), y1 = my(y1);
    }
    x = std::max(x, px(x0, width));
    ex = std::min(ex, px(x1 + 0.99f, width));
    y = std::max(y, px(y0, height));
    ey = std::min(ey, px(y1 + 0.99f, height));
  }
#endif
  if (ui_layer::trace()) [[unlikely]]
    LOG("[vr ui trace]   viewport %.0f,%.0f %.0fx%.0f, scissor %u,%u..%u,%u of %ux%u (sx %.3f sy %.3f)%s", vp.x, vp.y,
        vp.width, vp.height, x, y, ex, ey, width, height, sx, sy, ex <= x || ey <= y ? ": empty, not drawn" : "");
  if (ex <= x || ey <= y) {
    end_encoder();
    return;
  }
  sc = {{int32_t(x), int32_t(y)}, {ex - x, ey - y}};
  // (docs/VR_DLSS.md, stage D3) DLSS runs: a depth-tested draw into an eye's screen-size target is moved by the frame's
  // sub-pixel jitter (its viewport: the same shift as a jittered projection, for every vertex). Eye 1's replay keeps
  // eye 0's viewport (above), the motion pass the draw's
  if (float jx, jy; depth && (r[REGADDR::DB_DEPTH_CONTROL] & 2) && stereo_screen_target(width, height) &&
                    motion_pass::jitter(jx, jy)) [[unlikely]] {
    vp.x += jx;
    vp.y += jy;
  }
  }
  if (shadersOut) {
    shadersOut->viewport = vp;
    shadersOut->scissor = sc;
    shadersOut->rasterReady = true;
  }
  // Bitwise comparison preserves distinct signed zeros and exact float state.
  if (!state.viewportValid || std::memcmp(&state.viewport, &vp, sizeof(vp))) {
    record::vkCmdSetViewport(cmd, 0, 1, &vp);
    state.viewport = vp;
    state.viewportValid = true;
  }
  if (!state.scissorValid || state.scissor.offset.x != sc.offset.x ||
      state.scissor.offset.y != sc.offset.y ||
      state.scissor.extent.width != sc.extent.width ||
      state.scissor.extent.height != sc.extent.height) {
    record::vkCmdSetScissor(cmd, 0, 1, &sc);
    state.scissor = sc;
    state.scissorValid = true;
  }
  std::array<uint32_t, 4> blendBits;
  std::copy_n(r + REGADDR::CB_BLEND_RED, blendBits.size(), blendBits.begin());
  if (!state.blendValid || state.blend != blendBits) {
    record::vkCmdSetBlendConstants(
        cmd, reinterpret_cast<const float *>(r + REGADDR::CB_BLEND_RED));
    state.blend = blendBits;
    state.blendValid = true;
  }
  uint32_t stencilFront = r[REGADDR::DB_STENCILREFMASK] & 255;
  uint32_t stencilBack = r[REGADDR::DB_STENCILREFMASK_BF] & 255;
  if (!state.stencilValid || state.stencilFront != stencilFront)
    record::vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencilFront);
  if (!state.stencilValid || state.stencilBack != stencilBack)
    record::vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, stencilBack);
  state.stencilFront = stencilFront;
  state.stencilBack = stencilBack;
  state.stencilValid = true;
  rprof::mark(rprof::kRecord);
  rprof::UploadKind indexUploads(rprof::kUpIndex);
  if (replay) {
    static const bool skipVertexBinds = [] {
      const char* e = std::getenv("WWHD_VK_SKIP_VERTEX_BINDS");
      return e && !std::strcmp(e, "1");
    }();
    for (uint32_t i = 0; i < replay->vertexBindCount; ++i) {
      const auto& v = replay->vertexBinds[i];
      const bool skip = skipVertexBinds && !R.vertexDiagnostics.noVertexReuse &&
          state.vertex_bind_matches(cmd, R.submissionGeneration,
          v.binding, v.buffer, v.offset);
      if (!skip) record::vkCmdBindVertexBuffers(cmd, v.binding, 1, &v.buffer, &v.offset);
      if (preparationStats) {
        draw_counters().vertexBindCalls += !skip;
        draw_counters().vertexBindSkips += skip;
      }
    }
    rprof::mark(rprof::kVertex);
    if (replay->indexed) {
      bindIndex(replay->indexBuffer, replay->indexOffset, replay->indexType);
      record::vkCmdDrawIndexed(cmd, replay->drawCount, instances, 0, int32_t(baseVertex), 0);
    } else record::vkCmdDraw(cmd, replay->drawCount, instances, baseVertex, 0);
  } else {
  // eye 0: the draw eye 1 replays
  auto recordDraw = [&](bool indexed, VkBuffer buffer, VkDeviceSize offset, VkIndexType type, size_t drawCount) {
    if (!shadersOut) return;
    shadersOut->indexed = indexed;
    shadersOut->indexBuffer = buffer;
    shadersOut->indexOffset = offset;
    shadersOut->indexType = type;
    shadersOut->drawCount = uint32_t(drawCount);
    shadersOut->replay = !deferredPipelineDraw && drawCount <= UINT32_MAX &&
        shadersOut->vertexBindCount <= shadersOut->vertexBinds.size() &&
        shadersOut->submission == R.submissionGeneration;
  };
  // Scan the actual immutable index snapshot, never a second guest read: the bytes of the buffer
  // cache entry's shadow, or of a CPU copy that the upload slice is written from. Never the slice's
  // mapped memory: upload memory is uncached or write-combined on discrete GPUs (issue #44). Unless it
  // is host-cached (R.uploadReadsDirect): then the slice's mapped bytes are scanned, as fast as a copy.
  PER_THREAD_STATIC(nativeIndexCopy, std::vector<uint8_t>);  // (per thread: docs/VR_ATLAS_THREAD.md, stage 2.2)
  UploadSlice nativeIndexSlice{};
  const bool hostRestart = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                           topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VertexExtent vertexExtent;
  const void* nativeIndexData = nullptr;  // the bytes of nativeIndexSlice, on the CPU
  if (nativeIndices) {
    const uint32_t indexBytes = indexType == 0 ? 2 : 4;
    bufcache::Entry* indexEntry = nullptr;
    // (the entry's shadow and extent read under the cache's lock, with its lookup: another drawing thread can copy the
    // entry again meanwhile, 2026-11-10; the lock is recursive)
    draw_threads::CacheLock indexLocked(draw_threads::buffer_mutex());
    if (buffer_cache_enabled() && uint64_t(count) * indexBytes <= (8u << 20) &&
        cached_native_indices(indexAddr, count * indexBytes, nativeIndexSlice, indexEntry)) {
      nativeIndexData = indexEntry->shadow.data();
      vertexExtent = cached_index_extent(*indexEntry, nativeIndexData, count, indexBytes,
                                         hostRestart, int32_t(baseVertex));
    } else {
      const size_t bytes = size_t(count) * indexBytes;
      const auto* guest = static_cast<const uint8_t*>(mem::ptr(indexAddr));
      if (R.uploadReadsDirect) {
        nativeIndexSlice = snapshot(guest, bytes, 4);
        nativeIndexData = nativeIndexSlice.mapped;
      } else {
        nativeIndexCopy.assign(guest, guest + bytes);
        nativeIndexSlice = snapshot(nativeIndexCopy.data(), bytes, 4);
        nativeIndexData = nativeIndexCopy.data();
      }
      vertexExtent = indexed_vertex_extent(nativeIndexData, count,
                                           indexBytes, hostRestart,
                                           int32_t(baseVertex));
    }
    rprof::guest_read(rprof::kUpIndex, indexAddr, uint64_t(count) * indexBytes);
  } else if (convertedEntry) {
    draw_threads::CacheLock extentLocked(draw_threads::buffer_mutex());  // (the entry's memo: under the lock, as above)
    vertexExtent = cached_index_extent(*convertedEntry, nullptr, convertedBytes / 4, 4,
                                       hostRestart, int32_t(baseVertex));
  } else if (!indices.empty()) {
    vertexExtent = indexed_vertex_extent(indices.data(), indices.size(), 4,
                                         hostRestart, int32_t(baseVertex));
  } else {
    const uint64_t maximum = uint64_t(baseVertex) + count - 1;
    vertexExtent = {maximum <= UINT32_MAX, uint32_t(maximum)};
  }
  VertexWindowExtent windowExtent;
  const bool windowStats=vertex_window_stats_enabled();
  if(windowStats && vertex_copy_window_enabled()) {
    windowExtent={vertexExtent.valid,vertexExtent.minimum,vertexExtent.maximum};
  } else if(windowStats) {
    if(nativeIndices) {
      windowExtent=indexType==0
        ? vertex_window_extent<uint16_t>(nativeIndexData,count,hostRestart,int32_t(baseVertex))
        : vertex_window_extent<uint32_t>(nativeIndexData,count,hostRestart,int32_t(baseVertex));
    } else if(!indices.empty()) {
      windowExtent=vertex_window_extent<uint32_t>(indices.data(),indices.size(),hostRestart,int32_t(baseVertex));
    } else if(count && uint64_t(baseVertex)+count-1<=UINT32_MAX) {
      windowExtent={true,baseVertex,uint32_t(uint64_t(baseVertex)+count-1)};
    }
  }
  rprof::g_upload_kind() = rprof::kUpVertex;
  const bool cachedTrims = p.trimFetch == fs && p.trimVertex == vs &&
                           p.bindingTrims.size() == fs->bufferGroups.size();
  size_t trimGroup = 0;
  for (auto &g : fs->bufferGroups) {
    uint32_t addr =
        r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7];
    uint32_t size =
        r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 1] + 1;
    if (!addr || !size)
      exception_report::raise("missing vertex buffer");
    const BindingTrimMetadata trim = cachedTrims
        ? p.bindingTrims[trimGroup] : binding_trim_metadata(r, vs, g);
    ++trimGroup;
    const auto attributeEnd = trim.attributeEnd;
    const auto stride = trim.stride;
    const auto supported = trim.supported;
    const auto rate = trim.rate;
    VertexExtent extent = vertexExtent;
    if (rate && *rate == LatteConst::VertexFetchType2::INSTANCE_DATA)
      extent = {true, instances - 1};
    const uint32_t copied = supported
        ? vertex_prefix_size(size, stride, attributeEnd, extent) : size;
    if(windowStats) {
      auto window=windowExtent;
      if(rate && *rate==LatteConst::VertexFetchType2::INSTANCE_DATA)
        window={instances!=0,0,instances-1};
      const bool eligible=supported && attributeEnd && stride && window.valid &&
          uint64_t(window.maximum)*stride+attributeEnd<=copied;
      const uint32_t unused=vertex_window_unused(copied,stride,attributeEnd,window,supported);
      auto& t=vertexWindowStats();
      ++t.bindings;t.eligible+=eligible;t.fallback+=!eligible;t.zeroStride+=stride==0;
      t.prefix+=copied;t.window+=copied-unused;
    }
    draw_counters().vertexDeclaredBytes += size;
    draw_counters().vertexCopiedBytes += copied;
    rprof::guest_read(rprof::kUpVertex, addr, copied);
    const uint64_t windowBegin=uint64_t(extent.minimum)*stride;
    const uint64_t windowEnd=uint64_t(extent.maximum)*stride+attributeEnd;
    const bool copyWindow=vertex_copy_window_enabled() &&
        (nativeIndices || !indices.empty()) && supported && rate &&
        attributeEnd && stride && extent.valid && vertexExtent.valid &&
        windowEnd<=copied && windowBegin<windowEnd && windowBegin!=0;
    UploadSlice b;
    const uint32_t cpuCount = vk::cpu_vertex_count(g, R.vertexDiagnostics.cpuClasses);
    if (cpuCount) {
      const auto converted = cpu_vertex_stream(g, addr, size, stride,
          supported && attributeEnd && extent.valid && vertexExtent.valid ? uint64_t(extent.maximum) + 1 : 0);
      const uint32_t binding = vk::cpu_vertex_binding(*fs, g, R.vertexDiagnostics.cpuClasses,
          R.properties.limits.maxVertexInputBindings);
      if (binding >= R.properties.limits.maxVertexInputBindings)
        exception_report::raise("CPU vertex classes exceed device vertex binding limit");
      record::vkCmdBindVertexBuffers(cmd, binding, 1, &converted.buffer, &converted.offset);
      if (shadersOut) shadersOut->add_vertex_bind(binding, converted.buffer, converted.offset);
      // The extra bind bypasses the normal bind cache.
      state.vertexBinds = {};
      if (preparation_stats_enabled()) ++draw_counters().vertexBindCalls;
      if (cpuCount == uint32_t(g.attribCount)) continue;
    }
    bool vertexCached = false;
    if (R.vertexDiagnostics.noVertexReuse)
      b = snapshot(mem::ptr(addr), size, 4);  // the whole declared buffer, fresh every draw
    else if (!(vertexCached = buffer_cache_enabled() && cached_guest_range(addr, copied, rprof::kUpVertex, b)))
      b = copyWindow
        ? vertex_window_snapshot(g.attributeBufferIndex,addr,copied,
                                 uint32_t(windowBegin),uint32_t(windowEnd-windowBegin))
        : vertex_snapshot(g.attributeBufferIndex, addr, copied,
                          supported && attributeEnd && extent.valid && vertexExtent.valid);
    // (docs/VR_DLSS.md, stage D2b 2b) the vertex data this draw read, for its next frame's motion: a buffer the
    // buffer cache serves (a mesh that stays where it is) by its address; one uploaded fresh (written again every
    // frame, its address moving) as a copy of the bytes read
    if (motion_records()) [[unlikely]] {
      if (motion::DrawData* d = motion_store().slot(current_draw_index(), stereoEye())) {
        // a cached mesh whose address moved since the previous frame (the game alternating between buffers) is
        // copied too: its next frame then has these bytes, where its old address may hold others by then
        const motion::DrawData* before = motion_store().previous(current_draw_index(), stereoEye());
        const motion::DrawData::Piece* was =
            before ? before->find(motion::Part::VertexBuffer, uint8_t(g.attributeBufferIndex)) : nullptr;
        const bool moved = was && was->guestAddr != addr;
        if (vertexCached && !moved) d->add_ref(motion::Part::VertexBuffer, uint8_t(g.attributeBufferIndex), addr, copied);
        else d->add(motion::Part::VertexBuffer, uint8_t(g.attributeBufferIndex), mem::ptr(addr), copied, addr);
      }
    }
    VkDeviceSize offset = b.offset;
    static const bool skipVertexBinds = [] {
      const char* e=std::getenv("WWHD_VK_SKIP_VERTEX_BINDS");
      return e && !std::strcmp(e,"1");
    }();
    // Snapshot preparation above stays fresh even when its immutable slice is
    // unchanged. Compare host binding identity, never guest addresses alone.
    const bool skip=skipVertexBinds && !R.vertexDiagnostics.noVertexReuse && state.vertex_bind_matches(cmd,
        R.submissionGeneration,g.attributeBufferIndex,b.buffer,offset);
    if(preparation_stats_enabled()) {
      draw_counters().vertexBindCalls+=!skip;
      draw_counters().vertexBindSkips+=skip;
    }
    if (shadersOut) shadersOut->add_vertex_bind(g.attributeBufferIndex, b.buffer, offset);
    if(!skip)
      record::vkCmdBindVertexBuffers(cmd,g.attributeBufferIndex,1,&b.buffer,&offset);
  }
#ifdef __ANDROID__
  static thread_local std::set<std::tuple<uint32_t, uint32_t, bool, bool, bool, bool>> seenIndexPaths;
  const auto indexDiagnostic = std::make_tuple(prim, indexType, nativeIndices, bool(indexAddr), baseVertex != 0, stripRestart);
  if (seenIndexPaths.size() < 32 && seenIndexPaths.insert(indexDiagnostic).second)
    LOG("[vulkan index observed] guestPrimitive=0x%x guestIndexType=%u indexed=%u native=%u hostType=%s stripRestart=%u nonzeroBaseVertex=%u",
        prim, indexType, bool(indexAddr), nativeIndices,
        nativeIndices ? (indexType == 0 ? "uint16" : "uint32") : (convertedEntry || !indices.empty() ? "uint32-converted" : "none"),
        stripRestart, baseVertex != 0);
#endif
  rprof::g_upload_kind() = rprof::kUpIndex;
  rprof::mark(rprof::kVertex);
  if (nativeIndices) {
    bindIndex(nativeIndexSlice.buffer, nativeIndexSlice.offset,
                         indexType == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    recordDraw(true, nativeIndexSlice.buffer, nativeIndexSlice.offset,
               indexType == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32, count);
    if(deferredPipelineDraw) { auto* pipeline=&p; record::emit_owned([=] { if(pipeline->pipeline) gfxvk::vkCmdDrawIndexed(cmd, count, instances, 0, int32_t(baseVertex), 0); }); }
    else record::vkCmdDrawIndexed(cmd, count, instances, 0, int32_t(baseVertex), 0);
  } else if (convertedEntry) {
    const uint32_t converted = convertedBytes / 4;
    const auto b = convertedSlice;  // (taken under the lock, above)
    rprof::guest_read(rprof::kUpIndex, indexAddr, guestBytes);
    bindIndex(b.buffer, b.offset, VK_INDEX_TYPE_UINT32);
    recordDraw(true, b.buffer, b.offset, VK_INDEX_TYPE_UINT32, converted);
    if(deferredPipelineDraw) { auto* pipeline=&p; record::emit_owned([=] { if(pipeline->pipeline) gfxvk::vkCmdDrawIndexed(cmd, converted, instances, 0, int32_t(baseVertex), 0); }); }
    else record::vkCmdDrawIndexed(cmd, converted, instances, 0, int32_t(baseVertex), 0);
  } else if (indices.empty()) {
    recordDraw(false, VK_NULL_HANDLE, 0, VK_INDEX_TYPE_UINT32, count);
    if(deferredPipelineDraw) { auto* pipeline=&p; record::emit_owned([=] { if(pipeline->pipeline) gfxvk::vkCmdDraw(cmd, count, instances, baseVertex, 0); }); }
    else record::vkCmdDraw(cmd, count, instances, baseVertex, 0);
  } else {
    auto b = snapshot(indices.data(), indices.size() * 4, 4);
    if (indexAddr) rprof::guest_read(rprof::kUpIndex, indexAddr, guestBytes);
    bindIndex(b.buffer, b.offset, VK_INDEX_TYPE_UINT32);
    recordDraw(true, b.buffer, b.offset, VK_INDEX_TYPE_UINT32, indices.size());
    const auto drawCount=indices.size(); if(deferredPipelineDraw) { auto* pipeline=&p; record::emit_owned([=] { if(pipeline->pipeline) gfxvk::vkCmdDrawIndexed(cmd, drawCount, instances, 0, int32_t(baseVertex), 0); }); }
    else record::vkCmdDrawIndexed(cmd, indices.size(), instances, 0, int32_t(baseVertex), 0);
  }
  }
  if(R.gpuPassTimestampsEnabled) gpu_count_render_draw();
  for (auto *color : colors)
    if (color) {
      overlap::before_change(color, "its contents, a colour target");  // its contents (atlas_overlap.h)
      mark_draw_target_written(color);
    }
  if (depth) overlap::before_change(depth, "its contents, a depth target");  // its contents (atlas_overlap.h)
  if (depth) mark_draw_target_written(depth);
  if (mv.on) {  // (stage M4) eye 1's targets were drawn too; eye 1 draws nothing (draw_stereo)
    for (auto* twin : mv.twins)
      if (twin) {
        overlap::before_change(twin, "its contents, a colour target");
        mark_draw_target_written(twin);
      }
    if (mv.depthTwin) {
      overlap::before_change(mv.depthTwin, "its contents, a depth target");
      mark_draw_target_written(mv.depthTwin);
    }
    shadersOut->multiview = true;
    continuedEyes()[1].valid = false;  // (eye 1's last own draw is not the one before its next)
    continuedEyes()[1].invalidReason = kNoState;
    recentEyes()[1] = {};
  }
  draw_counters().drawCount++;
  // (docs/VR_DLSS.md, stage D2b 2d) a depth-writing draw, again in its eye's motion pass (motion_pass.h): eye 0's, a
  // multiview draw's second view against eye 1's layer of the depth, and eye 1's replay of eye 0's draw
  if (motion_pass::active() && !aoPrivateReplay() && !batch_draw()) [[unlikely]] {
    const auto packetStarted = std::chrono::steady_clock::now();
    struct PacketTime {
      std::chrono::steady_clock::time_point t;
      ~PacketTime() { motion_pass::note_packet_ns(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count())); }
    } packetTime{packetStarted};
    if (shadersOut && stereoEye() == 0) {
      MotionShare share;  // (eye 0's uploads, reused by eye 1's packet)
#ifdef WWHD_MOBILE_GPU_PATHS
      // (mobile GPUs) a multiview draw against both eyes' layers of one depth: one packet for both eyes (motion_both)
      const auto twin = shadersOut->pipeline ? shadersOut->pipeline->motionTwin : nullptr;
      if (mv.on && mv.depthTwin && mv.depthTwin->layerOwner == depth && twin && motion_both_eyes_on()) {
        MotionBuilt built[2];
        motion_packet(r, *shadersOut, 0, depth, depthSlice, instances, baseVertex, &share, &built[0]);
        motion_packet(r, *shadersOut, 1, mv.depthTwin, depthSlice, instances, baseVertex, &share, &built[1]);
        motion_both(built, *twin);
      } else
#endif
      {
        motion_packet(r, *shadersOut, 0, depth, depthSlice, instances, baseVertex, &share);
        if (mv.on && mv.depthTwin) motion_packet(r, *shadersOut, 1, mv.depthTwin, depthSlice, instances, baseVertex, &share);
      }
    } else if (replay && stereoEye() == 1) {
      motion_packet(r, *replay, 1, depth, depthSlice, instances, baseVertex);
    }
  }
  if (aoPrivateReplay()) {
    ao_private().frame = R.frame;
    return;
  }
  if (ao_hires_enabled() && colors[0] &&
      ps->kind == gfx::ProgramKind::DepthDownsample) {
    struct ReplayGuard {
      ReplayGuard() { aoPrivateReplay() = true; ao_private().frame = ~0ull; }
      ~ReplayGuard() { aoPrivateReplay() = false; }
    } guard;
    draw(r, prim, count, indexType, indexAddr, baseVertex, instances);
    // the replay changes later lookups (aoPrivateSource/aoPrivateFrame, private images)
    continued.valid = false;
    continued.invalidReason = kAoReplay;
    recentEyes()[stereoEye() & 1] = {};
  }
  static const uint32_t drawBatch = parse_draw_batch(std::getenv("WWHD_VK_DRAW_BATCH"));
  static const uint32_t drawBatchCap =
      parse_draw_batch_cap(std::getenv("WWHD_VK_DRAW_BATCH_CAP"));
  // (never on the atlas's thread: it never submits)
  // (nor while it runs: a submission would join the threads, and the batch is only for the GPU to start sooner)
  // (eye 1's thread may run: the submission joins it, a couple of times a frame)
  if (!draw_threads::on_draw_thread() && !atlas_thread::running() &&
      drawBatchState.after_draw(R.frame, drawBatch, drawBatchCap, mv.on ? 2 : 1)) {
    // Submit only after this draw owns all its upload slices and deferred
    // resources. The next draw reopens attachments with LOAD and rebinds state.
    flush_async();
    ++drawBatchSubmissions;
  }
  rprof::mark(rprof::kSubmit);
  // Original draw is complete. Replay exactly this pane draw with its own color
  // attachment; no main-target pixels are copied and no target state escapes.
  if(!backdropOn||stereoEye()||!gamepad_backdrop_capture_active()) return;
  const auto captureBlend=backdrop_blend(r[REGADDR::CB_COLOR_CONTROL],r[REGADDR::CB_BLEND0_CONTROL],r[REGADDR::CB_TARGET_MASK]);
  bool compatible = shadersOut && shadersOut->replay && shadersOut->rasterReady &&
      shadersOut->submission==R.submissionGeneration && captureBlend.supported && colors[0] && slices[0] == 0 && !(r[REGADDR::DB_DEPTH_CONTROL] & 7);
  for (unsigned i=1;i<8;++i) compatible &= colors[i] == nullptr;
  const uint64_t snapshotFlags=uint64_t(bool(shadersOut))|(uint64_t(shadersOut&&shadersOut->replay)<<1)|
      (uint64_t(shadersOut&&shadersOut->rasterReady)<<2)|(uint64_t(shadersOut&&shadersOut->submission==R.submissionGeneration)<<3)|
      (uint64_t(captureBlend.supported)<<4)|(uint64_t(bool(colors[0]))<<5)|(uint64_t(slices[0]==0)<<6)|(uint64_t(compatible)<<7);
  gamepad_backdrop_debug("draw-gate",snapshotFlags,(uint64_t(r[REGADDR::CB_BLEND0_CONTROL])<<32)|r[REGADDR::CB_COLOR_CONTROL],
      (uint64_t(r[REGADDR::DB_DEPTH_CONTROL])<<32)|r[REGADDR::CB_TARGET_MASK],R.submissionGeneration);
  if (gamepad_backdrop_begin_replay(colors[0],compatible,captureBlend.opaque)) {
    const auto before=draw_counters().drawCount;
    // The coverage-only alpha override is part of the ordinary pipeline key: the
    // three capture registers of the pane's own register file (the gx2 file the
    // scope was opened with) are patched for the replay only and restored on
    // every exit, so guest/native draw state is unchanged. Any other register
    // file gets one reused private copy (never expected).
    uint32_t* regs=const_cast<uint32_t*>(r);
    if(!gamepad_backdrop_scope_registers(r)) {
      static std::vector<uint32_t> privateRegs;
      privateRegs.assign(r,r+gx2::kNumRegs);regs=privateRegs.data();
    }
    BackdropRegisterPatch patch(regs,REGADDR::CB_BLEND0_CONTROL,REGADDR::CB_COLOR_CONTROL,REGADDR::CB_TARGET_MASK,captureBlend);
    continued.valid=false;
    backdropShadersIn=shadersOut;
    try { draw(regs,prim,count,indexType,indexAddr,baseVertex,instances,nullptr); }
    catch (...) { backdropShadersIn=nullptr;gamepad_backdrop_end_replay(false);continued.valid=false;throw; }
    backdropShadersIn=nullptr;
    gamepad_backdrop_end_replay(draw_counters().drawCount!=before);
    continued.valid=false;
  }
}
// Setup's Vulkan preparation (headstart.cpp, the head start's Metal pipeline recipes): the recipe of the pipeline a
// draw with these registers would make, by the live derivation: fetch shader, vertex and pixel translation, active
// targets, pipeline key and state as draw() finds them. colorFormats/depthFormat: GX2 surface formats of the targets
// (0: none). nullptr with *why when the state does not make a pipeline here.
std::shared_ptr<PipelineRecipe> derive_guest_pipeline_recipe(const uint32_t* r, const uint32_t colorFormats[8],
    uint32_t depthFormat, const char** why) {
  auto fail = [&](const char* reason) { *why = reason; return std::shared_ptr<PipelineRecipe>{}; };
  const uint32_t prim = r[REGADDR::VGT_PRIMITIVE_TYPE] & 0x3F;
  VkPrimitiveTopology topology;
  switch (prim) {
  case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
  case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
  case 3: case 0x12: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
  case 4: case 5: case 0x13: case 0x14: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
  case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
  default: return fail("primitive");
  }
  uint64_t fsKey = 0;
  auto* fs = vk::get_fetch_shader(r, &fsKey, ~uint64_t{0});
  if (!fs) return fail("fetch shader");
  vk::Shader* vs = vk::translate(r, true, fs, fsKey, ~uint64_t{0}, ~uint64_t{0});
  if (!vs || !vs->ready()) return fail("vertex shader");
  vk::Shader* ps = vk::translate(r, false, fs, fsKey, ~uint64_t{0}, ~uint64_t{0}, vs);
  if (!ps || !ps->ready()) return fail("pixel shader");
  const auto& lcr = *reinterpret_cast<const LatteContextRegister*>(r);
  const auto mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
  static std::array<Surface, 9> stubs;
  std::array<Surface*, 8> colors{};
  for (int i = 0; i < 8; ++i) {
    if (!(mask & (1u << i))) continue;
    if (!colorFormats[i]) return fail("targets");  // the draw has a target the recipe does not name
    stubs[i] = Surface{};
    stubs[i].format = colorFormats[i]; stubs[i].fmt = format_info(colorFormats[i], false);
    colors[i] = &stubs[i];
  }
  Surface* depth = nullptr;
  if (LatteMRT::GetActiveDepthBufferMask(lcr) && depthFormat) {
    stubs[8] = Surface{};
    stubs[8].format = depthFormat; stubs[8].isDepth = true; stubs[8].fmt = format_info(depthFormat, true);
    depth = &stubs[8];
  }
  if (!colors[0] && !depth) {
    bool any = false;
    for (auto* c : colors) any |= c != nullptr;
    if (!any) return fail("targets");
  }
  std::vector<std::shared_ptr<PipelineRecipe>> sink;
  g_recipeSink = &sink;
  try { pipeline(r, vs, ps, fs, topology, colors, depth); }
  catch (...) { g_recipeSink = nullptr; return fail("pipeline state"); }
  g_recipeSink = nullptr;
  if (sink.empty()) return fail("shader binaries");
  return sink.front();
}
} // namespace gfxvk

namespace gfxvk {
Surface* ui_layer_image() { return ui_layer_drawn(); }  // (stereo.h)
Surface* ui_layer_drawn_last() { return ui_layer_last(); }  // (stereo.h)
}  // namespace gfxvk
