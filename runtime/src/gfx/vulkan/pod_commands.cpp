#include <cstdio>
#include "pod_commands.h"
#include "crash_track.h"
#include "handoff_trace.h"
#include "render_prof.h"
#include "transfer_wait.h"
#include "xr_queue_sync.h"
#include <atomic>
#include <vector>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
namespace gfxvk {
std::mutex& graphics_queue_mutex() {
  static std::mutex m;
  return m;
}
// (standalone draw guard, 2026-11-12: a Quest 3S crashed in the Adreno driver's vkCmdDrawIndexed, address 0xf8, as
// the record worker replayed a draw) a draw replayed into a command buffer that has no graphics pipeline, or for an
// indexed draw no index buffer, bound since it began is left out and counted, not handed to the driver. Judged only
// for command buffers whose begin this thread replayed, with nothing recorded into them another way since (a call
// command, or a drain before direct recording: note_direct_recording); anything else is drawn as before, so a valid
// draw is never left out. Standalone headsets only; x64 PC builds unchanged
#if defined(__ANDROID__) || defined(__aarch64__)
constexpr bool kDrawGuard = true;
#else
constexpr bool kDrawGuard = false;
#endif
namespace {
std::atomic<uint64_t> g_directRecording{0};
std::atomic<uint64_t> g_leftOutNoPipeline{0}, g_leftOutNoIndex{0}, g_leftOutNoPass{0};
// (2026-11-13) the guard also follows the render pass: a draw with none open in its command buffer (begun and not yet
// ended, dynamic rendering or a render pass object) is left out too; and what each command buffer has bound, for the
// crash log's last replayed draw (replay_crash_note)
struct GuardedCommand {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint64_t epoch = 0;
  bool known = false, pipeline = false, index = false;
  bool pass = false, vertex = false, rateMap = false;
  uint32_t viewMask = 0, colors = 0, depth = 0;
  uint64_t pipelineHandle = 0, indexHandle = 0, vertexHandle = 0, setHandle = 0;
};
// the last draw the replay handed to the driver (or left out), for the crash log: plain atomics, written before each
// draw, read by the crash handler without locks
struct LastDraw {
  std::atomic<uint64_t> cmd{0}, pipeline{0}, index{0}, vertex{0}, set{0}, draws{0};
  std::atomic<uint32_t> flags{0}, viewMask{0}, colors{0}, count{0};
};
LastDraw g_lastDraw;
enum : uint32_t { kLdKnown = 1, kLdPass = 2, kLdPipeline = 4, kLdIndex = 8, kLdVertex = 16, kLdRateMap = 32,
                  kLdIndexed = 64, kLdDepth = 128, kLdLeftOut = 256 };
thread_local std::vector<GuardedCommand> t_guarded;
GuardedCommand* guarded(VkCommandBuffer cmd) {
  for (auto& g : t_guarded)
    if (g.cmd == cmd) return &g;
  return nullptr;
}
void guard_begin(VkCommandBuffer cmd) {
  GuardedCommand* g = guarded(cmd);
  if (!g) {
    if (t_guarded.size() >= 64) t_guarded.erase(t_guarded.begin());
    t_guarded.push_back({});
    g = &t_guarded.back();
  }
  *g = {};
  g->cmd = cmd;
  g->epoch = g_directRecording.load(std::memory_order_acquire);
  g->known = true;
}
void guard_forget(VkCommandBuffer cmd) {
  if (GuardedCommand* g = guarded(cmd)) g->known = false;
}
// true: the draw may go to the driver
bool guard_draw(VkCommandBuffer cmd, bool indexed, uint32_t count) {
  const GuardedCommand* g = guarded(cmd);
  const bool judged = g && g->known && g->epoch == g_directRecording.load(std::memory_order_acquire);
  uint32_t f = indexed ? kLdIndexed : 0;
  if (judged)
    f |= kLdKnown | (g->pass ? kLdPass : 0) | (g->pipeline ? kLdPipeline : 0) | (g->index ? kLdIndex : 0) |
         (g->vertex ? kLdVertex : 0) | (g->rateMap ? kLdRateMap : 0) | (g->depth ? kLdDepth : 0);
  const bool ok = !judged || (g->pass && g->pipeline && (!indexed || g->index));
  if (!ok) {
    f |= kLdLeftOut;
    (!g->pass ? g_leftOutNoPass : !g->pipeline ? g_leftOutNoPipeline : g_leftOutNoIndex).fetch_add(1, std::memory_order_relaxed);
  }
  g_lastDraw.cmd.store(uint64_t(reinterpret_cast<uintptr_t>(cmd)), std::memory_order_relaxed);
  g_lastDraw.pipeline.store(judged ? g->pipelineHandle : 0, std::memory_order_relaxed);
  g_lastDraw.index.store(judged ? g->indexHandle : 0, std::memory_order_relaxed);
  g_lastDraw.vertex.store(judged ? g->vertexHandle : 0, std::memory_order_relaxed);
  g_lastDraw.set.store(judged ? g->setHandle : 0, std::memory_order_relaxed);
  crash_track::note_command(indexed ? 4 : 3, uint64_t(reinterpret_cast<uintptr_t>(cmd)), count, judged ? g->pipelineHandle : 0);
  g_lastDraw.viewMask.store(judged ? g->viewMask : 0, std::memory_order_relaxed);
  g_lastDraw.colors.store(judged ? g->colors : 0, std::memory_order_relaxed);
  g_lastDraw.count.store(count, std::memory_order_relaxed);
  g_lastDraw.flags.store(f, std::memory_order_relaxed);
  g_lastDraw.draws.fetch_add(1, std::memory_order_relaxed);
  return ok;
}
}  // namespace
// (logged by the renderer, draw.cpp: this file is also built into tools without the game's log)
uint64_t draw_guard_left_out(uint64_t* noPipeline, uint64_t* noPass) {
  if (noPipeline) *noPipeline = g_leftOutNoPipeline.load(std::memory_order_relaxed);
  if (noPass) *noPass = g_leftOutNoPass.load(std::memory_order_relaxed);
  return g_leftOutNoPipeline.load(std::memory_order_relaxed) + g_leftOutNoIndex.load(std::memory_order_relaxed) +
         g_leftOutNoPass.load(std::memory_order_relaxed);
}
// (crash handler, main.cpp) the last draw the record worker replayed: what its command buffer had bound and open.
// snprintf into a static buffer only (no locks, no allocation)
void replay_crash_note(int fd, void (*out)(int, const char*, size_t)) {
  if constexpr (!kDrawGuard) return;
  static char buf[512];
  const uint32_t f = g_lastDraw.flags.load(std::memory_order_relaxed);
  const int n = std::snprintf(
      buf, sizeof buf,
      "\n--- the record worker's last replayed draw ---\n%s draw of %u, command buffer %llx (%s): render pass %s "
      "(view mask %u, %u colour attachments, depth %s, shading rate map %s), graphics pipeline %s %llx, index buffer "
      "%s %llx, vertex buffers %s%s; %llu draws replayed; left out so far: no pass %llu, no pipeline %llu, no index "
      "buffer %llu\n",
      (f & kLdIndexed) ? "indexed" : "plain", g_lastDraw.count.load(std::memory_order_relaxed),
      (unsigned long long)g_lastDraw.cmd.load(std::memory_order_relaxed), (f & kLdKnown) ? "followed" : "not followed",
      (f & kLdPass) ? "open" : "NOT open", g_lastDraw.viewMask.load(std::memory_order_relaxed),
      g_lastDraw.colors.load(std::memory_order_relaxed), (f & kLdDepth) ? "yes" : "no", (f & kLdRateMap) ? "yes" : "no",
      (f & kLdPipeline) ? "bound" : "NOT bound", (unsigned long long)g_lastDraw.pipeline.load(std::memory_order_relaxed),
      (f & kLdIndex) ? "bound" : "NOT bound", (unsigned long long)g_lastDraw.index.load(std::memory_order_relaxed),
      (f & kLdVertex) ? "bound" : "none", (f & kLdLeftOut) ? " (this draw left out)" : "",
      (unsigned long long)g_lastDraw.draws.load(std::memory_order_relaxed),
      (unsigned long long)g_leftOutNoPass.load(std::memory_order_relaxed),
      (unsigned long long)g_leftOutNoPipeline.load(std::memory_order_relaxed),
      (unsigned long long)g_leftOutNoIndex.load(std::memory_order_relaxed));
  if (n > 0) out(fd, buf, size_t(n < int(sizeof buf) ? n : int(sizeof buf) - 1));
  // (crash_track.h, WWHD_VK_CRASH_TRACK=1) the use-after-free check, the pipeline's kind, the commands before
  if (!crash_track::enabled()) return;
  auto line = [&](const char* fmt, auto... args) {
    const int k = std::snprintf(buf, sizeof buf, fmt, args...);
    if (k > 0) out(fd, buf, size_t(k < int(sizeof buf) ? k : int(sizeof buf) - 1));
  };
  const uint64_t frame = crash_track::g_frame.load(std::memory_order_relaxed);
  auto alive = [&](const char* what, uint64_t h) {
    if (!h) return;
    uint32_t kind = 0;
    const uint64_t at = crash_track::destroyed_frame(h, &kind);
    if (at == ~uint64_t{0}) line("  %s %llx: not among the last %u destroys\n", what, (unsigned long long)h, crash_track::kDestroyed);
    else line("  %s %llx: DESTROYED at frame %llu (now %llu, %llu frames ago; kind %u)\n", what, (unsigned long long)h,
              (unsigned long long)at, (unsigned long long)frame, (unsigned long long)(frame - at), kind);
  };
  line("crash tracking (frame %llu):\n", (unsigned long long)frame);
  const uint64_t p = g_lastDraw.pipeline.load(std::memory_order_relaxed);
  const uint32_t pf = crash_track::pipeline_flags(p);
  line("  the pipeline's kind: %s%s (the pass's view mask %u)\n",
       !(pf & crash_track::kRegistered) ? "not a registered two-eye pipeline (single view)" : "two-eye (multiview)",
       (pf & crash_track::kShadingRate) ? ", with the shading rate flag" : "", g_lastDraw.viewMask.load(std::memory_order_relaxed));
  alive("pipeline", p);
  alive("index buffer", g_lastDraw.index.load(std::memory_order_relaxed));
  alive("vertex buffer (first)", g_lastDraw.vertex.load(std::memory_order_relaxed));
  line("  descriptor set (first) %llx\n", (unsigned long long)g_lastDraw.set.load(std::memory_order_relaxed));
  line("  the last %u commands replayed (oldest first; 1 pipeline, 2 index buffer, 3 draw, 4 indexed draw, 6 end "
       "rendering, 18 vertex buffers, 19 descriptor sets, 24 begin rendering, 26/27 render pass, 28 begin):\n",
       crash_track::kCommands);
  const uint32_t end = crash_track::g_commandAt.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < crash_track::kCommands; ++i) {
    const crash_track::Command& c = crash_track::g_commands[(end + i) % crash_track::kCommands];
    const uint32_t op = c.opcode.load(std::memory_order_acquire);
    if (!op) continue;
    line("    op %u cmd %llx a %llx b %llx\n", op, (unsigned long long)c.cmd.load(std::memory_order_relaxed),
         (unsigned long long)c.a.load(std::memory_order_relaxed), (unsigned long long)c.b.load(std::memory_order_relaxed));
  }
}
void note_direct_recording() {
  if constexpr (kDrawGuard) g_directRecording.fetch_add(1, std::memory_order_acq_rel);
}
namespace {
struct Header {
  uint32_t opcode;
  uint32_t bytes;
};
static_assert(std::is_trivially_copyable_v<Header>);
struct Writer {
  // Most draw/state packets fit here; avoid transient heap storage and growth.
  std::array<std::byte, 256> inline_bytes;
  std::vector<std::byte> overflow;
  size_t used = 0, limit;
  bool valid = true;
  PodFailureReason failure; // assigned before every transition to invalid
  explicit Writer(size_t n) : limit(n) {}
  std::span<const std::byte> view() const {
    return {overflow.empty() ? inline_bytes.data() : overflow.data(), used};
  }
  void raw(const void *p, size_t count, size_t size) {
    if (!valid) return;
    if ((count && !p) || size == 0) {
      failure = PodFailureReason::InvalidInput; valid = false; return;
    }
    if (count > (limit - used) / size) {
      failure = PodFailureReason::ByteBudgetOrOverflow; valid = false; return;
    }
    const size_t n = count * size;
    if (!n) return;
    const size_t next = used + n;
    if (overflow.empty() && next <= inline_bytes.size()) {
      std::memcpy(inline_bytes.data() + used, p, n);
    } else {
      const bool spilling = overflow.empty();
      overflow.resize(next);
      if (spilling && used) std::memcpy(overflow.data(), inline_bytes.data(), used);
      std::memcpy(overflow.data() + used, p, n);
    }
    used = next;
  }
  template <class T> void put(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    raw(&value, 1, sizeof(T));
  }
  template <class T> void array(uint32_t count, const T *p) {
    raw(p, count, sizeof(T));
  }
};
struct Reader {
  const std::byte *bytes;
  size_t remaining;
  bool materialize = true;
  template <class T> T get() {
    static_assert(std::is_trivially_copyable_v<T>);
    if (sizeof(T) > remaining)
      throw std::runtime_error("truncated POD command");
    T value{};
    std::memcpy(&value, bytes, sizeof(T));
    bytes += sizeof(T);
    remaining -= sizeof(T);
    return value;
  }
  template <class T> std::vector<T> array(uint32_t count) {
    if (count > remaining / sizeof(T))
      throw std::runtime_error("truncated POD array");
    const size_t n = size_t(count) * sizeof(T);
    std::vector<T> value;
    if (materialize) {
      value.resize(count);
      if (n) std::memcpy(value.data(), bytes, n);
    } else if constexpr (requires(const T &v) { v.pNext; }) {
      // Preserve semantic chain validation during the allocation-free dry pass.
      // Typed memcpy decoding is still required: packet arrays may be unaligned.
      for (uint32_t i = 0; i < count; ++i) {
        T item;
        std::memcpy(&item, bytes + size_t(i) * sizeof(T), sizeof(T));
        if (item.pNext) throw std::runtime_error("invalid POD array chain");
      }
    }
    bytes += n;
    remaining -= n;
    return value;
  }
};
template <class T> const T *data(const std::vector<T> &v) {
  return v.empty() ? nullptr : v.data();
}
template <class T> bool chains(uint32_t n, const T *p, PodFailureReason& reason) {
  if (n && !p) { reason = PodFailureReason::InvalidInput; return false; }
  for (uint32_t i = 0; i < n; ++i)
    if (p[i].pNext) { reason = PodFailureReason::UnsupportedPNext; return false; }
  return true;
}
template <class T> bool chains(uint32_t n, const T* p) {
  PodFailureReason reason;
  return chains(n, p, reason); // Replay validation preserves its fatal behavior.
}
void checked(VkResult r) {
  if (r != VK_SUCCESS)
    throw std::runtime_error("POD Vulkan operation failed: " +
                             std::to_string(int(r)));
}
} // namespace
bool PodCommands::append(uint32_t opcode,
                         std::span<const std::byte> payload) {
  if (commands_ >= limits_.maxCommands)
    return reject(PodFailureReason::CommandLimit);
  if (bytes_.size() > limits_.maxPayloadBytes || payload.size() > UINT32_MAX ||
      sizeof(Header) > limits_.maxPayloadBytes - bytes_.size() ||
      payload.size() > limits_.maxPayloadBytes - bytes_.size() - sizeof(Header))
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  const Header h{opcode, uint32_t(payload.size())};
  try {
    auto old = bytes_.size();
    bytes_.resize(old + sizeof h + payload.size());
    std::memcpy(bytes_.data() + old, &h, sizeof h);
    if (!payload.empty())
      std::memcpy(bytes_.data() + old + sizeof h, payload.data(),
                  payload.size());
    ++commands_;
    return true;
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
// Fixed scalar packets only: no padded aggregate, borrowed array or pointer dereference.
template<class... T>
bool PodCommands::append_scalars(uint32_t opcode, const T&... values) {
  static_assert((std::is_trivially_copyable_v<T> && ...));
  constexpr size_t payload = (sizeof(T) + ... + 0);
  if (payload > limits_.maxPayloadBytes) return reject(PodFailureReason::ByteBudgetOrOverflow);
  if (commands_ >= limits_.maxCommands) return reject(PodFailureReason::CommandLimit);
  if (bytes_.size() > limits_.maxPayloadBytes || payload > UINT32_MAX ||
      sizeof(Header) > limits_.maxPayloadBytes - bytes_.size() ||
      payload > limits_.maxPayloadBytes - bytes_.size() - sizeof(Header))
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  try {
#ifdef WWHD_POD_SCALAR_TEST
    if (failDirectGrowth_) { failDirectGrowth_ = false; throw std::bad_alloc(); }
#endif
    const size_t old = bytes_.size();
    bytes_.resize(old + sizeof(Header) + payload); // Strong guarantee before writes.
    const Header header{opcode, uint32_t(payload)};
    std::memcpy(bytes_.data() + old, &header, sizeof(header));
    size_t cursor = old + sizeof(header);
    auto put = [&](const auto& value) {
      std::memcpy(bytes_.data() + cursor, &value, sizeof(value)); cursor += sizeof(value);
    };
    (put(values), ...); // Only nonthrowing field copies after successful growth.
    ++commands_; return true;
  } catch (const std::bad_alloc&) { return reject(PodFailureReason::AllocationFailure); }
}
bool PodCommands::vkCmdBindPipeline(VkCommandBuffer cmd,
                                    VkPipelineBindPoint point,
                                    VkPipeline pipeline) {
  if (directScalars_) return append_scalars(1, cmd, point, pipeline);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(point);
    w.put(pipeline);
    if (!w.valid) return reject(w.failure);
    return append(1, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer,
                                       VkDeviceSize offset, VkIndexType type) {
  if (directScalars_) return append_scalars(2, cmd, buffer, offset, type);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(buffer);
    w.put(offset);
    w.put(type);
    if (!w.valid) return reject(w.failure);
    return append(2, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices,
                            uint32_t instances, uint32_t firstVertex,
                            uint32_t firstInstance) {
  if (directScalars_) return append_scalars(3, cmd, vertices, instances, firstVertex, firstInstance);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(vertices);
    w.put(instances);
    w.put(firstVertex);
    w.put(firstInstance);
    if (!w.valid) return reject(w.failure);
    return append(3, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices,
                                   uint32_t instances, uint32_t firstIndex,
                                   int32_t vertexOffset,
                                   uint32_t firstInstance) {
  if (directScalars_) return append_scalars(4, cmd, indices, instances, firstIndex, vertexOffset, firstInstance);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(indices);
    w.put(instances);
    w.put(firstIndex);
    w.put(vertexOffset);
    w.put(firstInstance);
    if (!w.valid) return reject(w.failure);
    return append(4, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y,
                                uint32_t z) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(x);
    w.put(y);
    w.put(z);
    if (!w.valid) return reject(w.failure);
    return append(5, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdEndRendering(VkCommandBuffer cmd) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    if (!w.valid) return reject(w.failure);
    return append(6, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdSetStencilReference(VkCommandBuffer cmd,
                                           VkStencilFaceFlags faces,
                                           uint32_t reference) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(faces);
    w.put(reference);
    if (!w.valid) return reject(w.failure);
    return append(7, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdSetStencilWriteMask(VkCommandBuffer cmd,
                                           VkStencilFaceFlags faces,
                                           uint32_t mask) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(faces);
    w.put(mask);
    if (!w.valid) return reject(w.failure);
    return append(8, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool,
                                      uint32_t first, uint32_t count) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(pool);
    w.put(first);
    w.put(count);
    if (!w.valid) return reject(w.failure);
    return append(9, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdWriteTimestamp(VkCommandBuffer cmd,
                                      VkPipelineStageFlagBits stage,
                                      VkQueryPool pool, uint32_t query) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(stage);
    w.put(pool);
    w.put(query);
    if (!w.valid) return reject(w.failure);
    return append(10, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdSetViewport(VkCommandBuffer cmd, uint32_t first,
                                   uint32_t count, const VkViewport *values) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(first);
    w.put(count);
    w.array(count, static_cast<const VkViewport *>(values));
    if (!w.valid) return reject(w.failure);
    return append(11, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdSetScissor(VkCommandBuffer cmd, uint32_t first,
                                  uint32_t count, const VkRect2D *values) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(first);
    w.put(count);
    w.array(count, static_cast<const VkRect2D *>(values));
    if (!w.valid) return reject(w.failure);
    return append(12, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer source,
                                  VkBuffer destination, uint32_t count,
                                  const VkBufferCopy *regions) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(source);
    w.put(destination);
    w.put(count);
    w.array(count, static_cast<const VkBufferCopy *>(regions));
    if (!w.valid) return reject(w.failure);
    return append(13, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer source,
                                         VkImage destination,
                                         VkImageLayout layout, uint32_t count,
                                         const VkBufferImageCopy *regions) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(source);
    w.put(destination);
    w.put(layout);
    w.put(count);
    w.array(count, static_cast<const VkBufferImageCopy *>(regions));
    if (!w.valid) return reject(w.failure);
    return append(14, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage source,
                                         VkImageLayout layout,
                                         VkBuffer destination, uint32_t count,
                                         const VkBufferImageCopy *regions) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(source);
    w.put(layout);
    w.put(destination);
    w.put(count);
    w.array(count, static_cast<const VkBufferImageCopy *>(regions));
    if (!w.valid) return reject(w.failure);
    return append(15, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdCopyImage(VkCommandBuffer cmd, VkImage source,
                                 VkImageLayout sourceLayout,
                                 VkImage destination,
                                 VkImageLayout destinationLayout,
                                 uint32_t count, const VkImageCopy *regions) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(source);
    w.put(sourceLayout);
    w.put(destination);
    w.put(destinationLayout);
    w.put(count);
    w.array(count, static_cast<const VkImageCopy *>(regions));
    if (!w.valid) return reject(w.failure);
    return append(16, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdBlitImage(VkCommandBuffer cmd, VkImage source,
                                 VkImageLayout sourceLayout,
                                 VkImage destination,
                                 VkImageLayout destinationLayout,
                                 uint32_t count, const VkImageBlit *regions,
                                 VkFilter filter) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(source);
    w.put(sourceLayout);
    w.put(destination);
    w.put(destinationLayout);
    w.put(count);
    w.array(count, static_cast<const VkImageBlit *>(regions));
    w.put(filter);
    if (!w.valid) return reject(w.failure);
    return append(17, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first,
                                         uint32_t count,
                                         const VkBuffer *buffers,
                                         const VkDeviceSize *offsets) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(first);
    w.put(count);
    w.array(count, static_cast<const VkBuffer *>(buffers));
    w.array(count, static_cast<const VkDeviceSize *>(offsets));
    if (!w.valid) return reject(w.failure);
    return append(18, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdBindDescriptorSets(
    VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet *sets,
    uint32_t offsetCount, const uint32_t *offsets) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(point);
    w.put(layout);
    w.put(first);
    w.put(count);
    w.array(count, static_cast<const VkDescriptorSet *>(sets));
    w.put(offsetCount);
    w.array(offsetCount, static_cast<const uint32_t *>(offsets));
    if (!w.valid) return reject(w.failure);
    return append(19, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdPushConstants(VkCommandBuffer cmd,
                                     VkPipelineLayout layout,
                                     VkShaderStageFlags stages, uint32_t offset,
                                     uint32_t size, const void *values) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(layout);
    w.put(stages);
    w.put(offset);
    w.put(size);
    w.array(size, static_cast<const std::byte *>(values));
    if (!w.valid) return reject(w.failure);
    return append(20, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::call(CallFn fn, VkCommandBuffer cmd, const void* bytes, uint32_t size) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(reinterpret_cast<uintptr_t>(fn));
    w.put(cmd);
    w.put(size);
    w.array(size, static_cast<const std::byte*>(bytes));
    if (!w.valid) return reject(w.failure);
    return append(30, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdSetBlendConstants(VkCommandBuffer cmd,
                                         const float values[4]) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.array(4, static_cast<const float *>(values));
    if (!w.valid) return reject(w.failure);
    return append(21, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdClearColorImage(VkCommandBuffer cmd, VkImage image,
                                       VkImageLayout layout,
                                       const VkClearColorValue *value,
                                       uint32_t count,
                                       const VkImageSubresourceRange *ranges) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(image);
    w.put(layout);
    w.array(1, static_cast<const VkClearColorValue *>(value));
    w.put(count);
    w.array(count, static_cast<const VkImageSubresourceRange *>(ranges));
    if (!w.valid) return reject(w.failure);
    return append(22, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdClearDepthStencilImage(
    VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
    const VkClearDepthStencilValue *value, uint32_t count,
    const VkImageSubresourceRange *ranges) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(image);
    w.put(layout);
    w.array(1, static_cast<const VkClearDepthStencilValue *>(value));
    w.put(count);
    w.array(count, static_cast<const VkImageSubresourceRange *>(ranges));
    if (!w.valid) return reject(w.failure);
    return append(23, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdBeginRendering(VkCommandBuffer cmd,
                                      const VkRenderingInfo *info) {
  // Reject the complete packet before reading any nested caller array.
  if (commands_ >= limits_.maxCommands) return reject(PodFailureReason::CommandLimit);
  if (bytes_.size() > limits_.maxPayloadBytes ||
      sizeof(Header) > limits_.maxPayloadBytes - bytes_.size())
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  size_t remaining = std::min<size_t>(
      UINT32_MAX, limits_.maxPayloadBytes - bytes_.size() - sizeof(Header));
  const auto consume = [&remaining](size_t count, size_t element) {
    if (count > remaining / element)
      return false;
    remaining -= count * element;
    return true;
  };
  if (!consume(1, sizeof(cmd) + sizeof(VkRenderingInfo) + sizeof(uint32_t)))
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  if (!info) return reject(PodFailureReason::InvalidInput);
  // (foveation.cpp) the one pNext a pass may carry: its fragment shading rate map, alone
  const auto* rateMap = static_cast<const VkRenderingFragmentShadingRateAttachmentInfoKHR*>(info->pNext);
  if (rateMap && (rateMap->sType != VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR || rateMap->pNext))
    return reject(PodFailureReason::UnsupportedPNext);
  if (!consume(rateMap ? 1 : 0, sizeof(VkRenderingFragmentShadingRateAttachmentInfoKHR)))
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  if (!consume(info->colorAttachmentCount, sizeof(VkRenderingAttachmentInfo)) ||
      !consume(info->pDepthAttachment ? 1 : 0,
               sizeof(VkRenderingAttachmentInfo)) ||
      !consume(info->pStencilAttachment ? 1 : 0,
               sizeof(VkRenderingAttachmentInfo)))
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  PodFailureReason reason;
  if (!chains(info->colorAttachmentCount, info->pColorAttachments, reason)) return reject(reason);
  if ((info->pDepthAttachment && info->pDepthAttachment->pNext) ||
      (info->pStencilAttachment && info->pStencilAttachment->pNext))
    return reject(PodFailureReason::UnsupportedPNext);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    auto copy = *info;
    copy.pNext = nullptr;
    copy.pColorAttachments = nullptr;
    copy.pDepthAttachment = nullptr;
    copy.pStencilAttachment = nullptr;
    w.put(copy);
    uint32_t mask = (info->pDepthAttachment ? 1u : 0u) |
                    (info->pStencilAttachment ? 2u : 0u) |
                    ((info->pDepthAttachment &&
                      info->pDepthAttachment == info->pStencilAttachment)
                         ? 4u
                         : 0u) |
                    (rateMap ? 8u : 0u);
    w.put(mask);
    w.array(info->colorAttachmentCount, info->pColorAttachments);
    if (mask & 1)
      w.put(*info->pDepthAttachment);
    if (mask & 2)
      w.put(*info->pStencilAttachment);
    if (mask & 8)
      w.put(*rateMap);
    if (!w.valid) return reject(w.failure);
    return append(24, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdPipelineBarrier(
    VkCommandBuffer cmd, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags,
    uint32_t memoryCount, const VkMemoryBarrier *memory, uint32_t bufferCount,
    const VkBufferMemoryBarrier *buffers, uint32_t imageCount,
    const VkImageMemoryBarrier *images) {
  // Counts are budgeted together before chains() dereferences any array.
  if (commands_ >= limits_.maxCommands) return reject(PodFailureReason::CommandLimit);
  if (bytes_.size() > limits_.maxPayloadBytes ||
      sizeof(Header) > limits_.maxPayloadBytes - bytes_.size())
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  size_t remaining = std::min<size_t>(
      UINT32_MAX, limits_.maxPayloadBytes - bytes_.size() - sizeof(Header));
  const auto consume = [&remaining](size_t count, size_t element) {
    if (count > remaining / element)
      return false;
    remaining -= count * element;
    return true;
  };
  if (!consume(1, sizeof(cmd) + sizeof(source) + sizeof(destination) +
                      sizeof(flags) + sizeof(memoryCount) +
                      sizeof(bufferCount) + sizeof(imageCount)) ||
      !consume(memoryCount, sizeof(VkMemoryBarrier)) ||
      !consume(bufferCount, sizeof(VkBufferMemoryBarrier)) ||
      !consume(imageCount, sizeof(VkImageMemoryBarrier)))
    return reject(PodFailureReason::ByteBudgetOrOverflow);
  PodFailureReason reason;
  if (!chains(memoryCount, memory, reason)) return reject(reason);
  if (!chains(bufferCount, buffers, reason)) return reject(reason);
  if (!chains(imageCount, images, reason)) return reject(reason);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(source);
    w.put(destination);
    w.put(flags);
    w.put(memoryCount);
    w.array(memoryCount, static_cast<const VkMemoryBarrier *>(memory));
    w.put(bufferCount);
    w.array(bufferCount, static_cast<const VkBufferMemoryBarrier *>(buffers));
    w.put(imageCount);
    w.array(imageCount, static_cast<const VkImageMemoryBarrier *>(images));
    if (!w.valid) return reject(w.failure);
    return append(25, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdBeginRenderPass(VkCommandBuffer cmd,
                                       const VkRenderPassBeginInfo *info,
                                       VkSubpassContents contents) {
  if (!info) return reject(PodFailureReason::InvalidInput);
  if (info->pNext) return reject(PodFailureReason::UnsupportedPNext);
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    auto copy = *info;
    copy.pClearValues = nullptr;
    w.put(copy);
    w.array(info->clearValueCount, info->pClearValues);
    w.put(contents);
    if (!w.valid) return reject(w.failure);
    return append(26, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::vkCmdEndRenderPass(VkCommandBuffer cmd) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    if (!w.valid) return reject(w.failure);
    return append(27, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::beginCommandBuffer(VkCommandBuffer cmd,
                                     VkCommandBufferUsageFlags flags) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(cmd);
    w.put(flags);
    if (!w.valid) return reject(w.failure);
    return append(28, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
bool PodCommands::submit(VkDevice device, VkQueue queue, VkCommandBuffer cmd,
                         VkFence fence, VkSemaphore wait, VkSemaphore signal,
                         VkSemaphore extraWait, VkSemaphore extraSignal) {
  try {
    Writer w(limits_.maxPayloadBytes);
    w.put(device);
    w.put(queue);
    w.put(cmd);
    w.put(fence);
    w.put(wait);
    w.put(signal);
    w.put(extraWait);
    w.put(extraSignal);
    if (!w.valid) return reject(w.failure);
    return append(29, w.view());
  } catch (const std::bad_alloc &) {
    return reject(PodFailureReason::AllocationFailure);
  }
}
void PodCommands::replay(const std::vector<std::byte> &bytes) {
  const bool smallDiagnostic = handoff_trace::enabled() && bytes.size() <= 2048;
  std::array<uint32_t, 31> opcodeCounts;
  if (smallDiagnostic) opcodeCounts.fill(0);
  // Validate the entire stream before emitting its first Vulkan command.
  for (bool execute : {false, true}) {
    Reader stream{bytes.data(), bytes.size(), execute};
    while (stream.remaining) {
      auto h = stream.get<Header>();
      if (h.bytes > stream.remaining)
        throw std::runtime_error("truncated POD payload");
      Reader r{stream.bytes, h.bytes, execute};
      const bool timed = smallDiagnostic && execute;
      const uint64_t wallStart = timed ? handoff_trace::native_now_ns() : 0;
      const uint64_t cpuStart = timed ? rprof::thread_cpu_ns() : 0;
      switch (h.opcode) {
      case 1: {
        auto cmd = r.get<VkCommandBuffer>();
        auto point = r.get<VkPipelineBindPoint>();
        auto pipeline = r.get<VkPipeline>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (point == VK_PIPELINE_BIND_POINT_GRAPHICS)
              if (GuardedCommand* g = guarded(cmd)) {
                g->pipeline = pipeline != VK_NULL_HANDLE;
                g->pipelineHandle = uint64_t(reinterpret_cast<uintptr_t>(pipeline));
              }
          crash_track::note_command(1, uint64_t(reinterpret_cast<uintptr_t>(cmd)), uint64_t(reinterpret_cast<uintptr_t>(pipeline)), point);
          gfxvk::vkCmdBindPipeline(cmd, point, pipeline);
        }
        break;
      }
      case 2: {
        auto cmd = r.get<VkCommandBuffer>();
        auto buffer = r.get<VkBuffer>();
        auto offset = r.get<VkDeviceSize>();
        auto type = r.get<VkIndexType>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd)) {
              g->index = buffer != VK_NULL_HANDLE;
              g->indexHandle = uint64_t(reinterpret_cast<uintptr_t>(buffer));
            }
          crash_track::note_command(2, uint64_t(reinterpret_cast<uintptr_t>(cmd)), uint64_t(reinterpret_cast<uintptr_t>(buffer)), offset);
          gfxvk::vkCmdBindIndexBuffer(cmd, buffer, offset, type);
        }
        break;
      }
      case 3: {
        auto cmd = r.get<VkCommandBuffer>();
        auto vertices = r.get<uint32_t>();
        auto instances = r.get<uint32_t>();
        auto firstVertex = r.get<uint32_t>();
        auto firstInstance = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute && (!kDrawGuard || guard_draw(cmd, false, vertices)))
          gfxvk::vkCmdDraw(cmd, vertices, instances, firstVertex,
                           firstInstance);
        break;
      }
      case 4: {
        auto cmd = r.get<VkCommandBuffer>();
        auto indices = r.get<uint32_t>();
        auto instances = r.get<uint32_t>();
        auto firstIndex = r.get<uint32_t>();
        auto vertexOffset = r.get<int32_t>();
        auto firstInstance = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute && (!kDrawGuard || guard_draw(cmd, true, indices)))
          gfxvk::vkCmdDrawIndexed(cmd, indices, instances, firstIndex,
                                  vertexOffset, firstInstance);
        break;
      }
      case 5: {
        auto cmd = r.get<VkCommandBuffer>();
        auto x = r.get<uint32_t>();
        auto y = r.get<uint32_t>();
        auto z = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdDispatch(cmd, x, y, z);
        break;
      }
      case 6: {
        auto cmd = r.get<VkCommandBuffer>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd)) g->pass = false, g->rateMap = false;
          crash_track::note_command(6, uint64_t(reinterpret_cast<uintptr_t>(cmd)), 0, 0);
          gfxvk::vkCmdEndRendering(cmd);
        }
        break;
      }
      case 7: {
        auto cmd = r.get<VkCommandBuffer>();
        auto faces = r.get<VkStencilFaceFlags>();
        auto reference = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdSetStencilReference(cmd, faces, reference);
        break;
      }
      case 8: {
        auto cmd = r.get<VkCommandBuffer>();
        auto faces = r.get<VkStencilFaceFlags>();
        auto mask = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdSetStencilWriteMask(cmd, faces, mask);
        break;
      }
      case 9: {
        auto cmd = r.get<VkCommandBuffer>();
        auto pool = r.get<VkQueryPool>();
        auto first = r.get<uint32_t>();
        auto count = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdResetQueryPool(cmd, pool, first, count);
        break;
      }
      case 10: {
        auto cmd = r.get<VkCommandBuffer>();
        auto stage = r.get<VkPipelineStageFlagBits>();
        auto pool = r.get<VkQueryPool>();
        auto query = r.get<uint32_t>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdWriteTimestamp(cmd, stage, pool, query);
        break;
      }
      case 11: {
        auto cmd = r.get<VkCommandBuffer>();
        auto first = r.get<uint32_t>();
        auto count = r.get<uint32_t>();
        auto values = r.array<VkViewport>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdSetViewport(cmd, first, count, data(values));
        break;
      }
      case 12: {
        auto cmd = r.get<VkCommandBuffer>();
        auto first = r.get<uint32_t>();
        auto count = r.get<uint32_t>();
        auto values = r.array<VkRect2D>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdSetScissor(cmd, first, count, data(values));
        break;
      }
      case 13: {
        auto cmd = r.get<VkCommandBuffer>();
        auto source = r.get<VkBuffer>();
        auto destination = r.get<VkBuffer>();
        auto count = r.get<uint32_t>();
        auto regions = r.array<VkBufferCopy>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdCopyBuffer(cmd, source, destination, count,
                                 data(regions));
        break;
      }
      case 14: {
        auto cmd = r.get<VkCommandBuffer>();
        auto source = r.get<VkBuffer>();
        auto destination = r.get<VkImage>();
        auto layout = r.get<VkImageLayout>();
        auto count = r.get<uint32_t>();
        auto regions = r.array<VkBufferImageCopy>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdCopyBufferToImage(cmd, source, destination, layout, count,
                                        data(regions));
        break;
      }
      case 15: {
        auto cmd = r.get<VkCommandBuffer>();
        auto source = r.get<VkImage>();
        auto layout = r.get<VkImageLayout>();
        auto destination = r.get<VkBuffer>();
        auto count = r.get<uint32_t>();
        auto regions = r.array<VkBufferImageCopy>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdCopyImageToBuffer(cmd, source, layout, destination, count,
                                        data(regions));
        break;
      }
      case 16: {
        auto cmd = r.get<VkCommandBuffer>();
        auto source = r.get<VkImage>();
        auto sourceLayout = r.get<VkImageLayout>();
        auto destination = r.get<VkImage>();
        auto destinationLayout = r.get<VkImageLayout>();
        auto count = r.get<uint32_t>();
        auto regions = r.array<VkImageCopy>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdCopyImage(cmd, source, sourceLayout, destination,
                                destinationLayout, count, data(regions));
        break;
      }
      case 17: {
        auto cmd = r.get<VkCommandBuffer>();
        auto source = r.get<VkImage>();
        auto sourceLayout = r.get<VkImageLayout>();
        auto destination = r.get<VkImage>();
        auto destinationLayout = r.get<VkImageLayout>();
        auto count = r.get<uint32_t>();
        auto regions = r.array<VkImageBlit>(count);
        auto filter = r.get<VkFilter>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdBlitImage(cmd, source, sourceLayout, destination,
                                destinationLayout, count, data(regions),
                                filter);
        break;
      }
      case 18: {
        auto cmd = r.get<VkCommandBuffer>();
        auto first = r.get<uint32_t>();
        auto count = r.get<uint32_t>();
        auto buffers = r.array<VkBuffer>(count);
        auto offsets = r.array<VkDeviceSize>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd)) {
              g->vertex = count > 0;
              if (count) g->vertexHandle = uint64_t(reinterpret_cast<uintptr_t>(data(buffers)[0]));
            }
          crash_track::note_command(18, uint64_t(reinterpret_cast<uintptr_t>(cmd)),
                                    count ? uint64_t(reinterpret_cast<uintptr_t>(data(buffers)[0])) : 0, count);
          gfxvk::vkCmdBindVertexBuffers(cmd, first, count, data(buffers),
                                        data(offsets));
        }
        break;
      }
      case 19: {
        auto cmd = r.get<VkCommandBuffer>();
        auto point = r.get<VkPipelineBindPoint>();
        auto layout = r.get<VkPipelineLayout>();
        auto first = r.get<uint32_t>();
        auto count = r.get<uint32_t>();
        auto sets = r.array<VkDescriptorSet>(count);
        auto offsetCount = r.get<uint32_t>();
        auto offsets = r.array<uint32_t>(offsetCount);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd); g && count && point == VK_PIPELINE_BIND_POINT_GRAPHICS && first == 0)
              g->setHandle = uint64_t(reinterpret_cast<uintptr_t>(data(sets)[0]));
          crash_track::note_command(19, uint64_t(reinterpret_cast<uintptr_t>(cmd)),
                                    count ? uint64_t(reinterpret_cast<uintptr_t>(data(sets)[0])) : 0, first);
          gfxvk::vkCmdBindDescriptorSets(cmd, point, layout, first, count,
                                         data(sets), offsetCount,
                                         data(offsets));
        }
        break;
      }
      case 20: {
        auto cmd = r.get<VkCommandBuffer>();
        auto layout = r.get<VkPipelineLayout>();
        auto stages = r.get<VkShaderStageFlags>();
        auto offset = r.get<uint32_t>();
        auto size = r.get<uint32_t>();
        auto values = r.array<std::byte>(size);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdPushConstants(cmd, layout, stages, offset, size,
                                    data(values));
        break;
      }
      case 21: {
        auto cmd = r.get<VkCommandBuffer>();
        auto values = r.array<float>(4);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdSetBlendConstants(cmd, data(values));
        break;
      }
      case 22: {
        auto cmd = r.get<VkCommandBuffer>();
        auto image = r.get<VkImage>();
        auto layout = r.get<VkImageLayout>();
        auto value = r.array<VkClearColorValue>(1);
        auto count = r.get<uint32_t>();
        auto ranges = r.array<VkImageSubresourceRange>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdClearColorImage(cmd, image, layout, data(value), count,
                                      data(ranges));
        break;
      }
      case 23: {
        auto cmd = r.get<VkCommandBuffer>();
        auto image = r.get<VkImage>();
        auto layout = r.get<VkImageLayout>();
        auto value = r.array<VkClearDepthStencilValue>(1);
        auto count = r.get<uint32_t>();
        auto ranges = r.array<VkImageSubresourceRange>(count);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdClearDepthStencilImage(cmd, image, layout, data(value),
                                             count, data(ranges));
        break;
      }
      case 24: {
        auto cmd = r.get<VkCommandBuffer>();
        auto info = r.get<VkRenderingInfo>();
        auto mask = r.get<uint32_t>();
        if (info.pNext || info.pColorAttachments || info.pDepthAttachment ||
            info.pStencilAttachment || mask > 15 ||
            ((mask & 4) && ((mask & 3) != 3)))
          throw std::runtime_error("invalid rendering payload");
        auto colors =
            r.array<VkRenderingAttachmentInfo>(info.colorAttachmentCount);
        VkRenderingAttachmentInfo depth{}, stencil{};
        if (mask & 1)
          depth = r.get<VkRenderingAttachmentInfo>();
        if (mask & 2)
          stencil = r.get<VkRenderingAttachmentInfo>();
        VkRenderingFragmentShadingRateAttachmentInfoKHR rateMap{};
        if (mask & 8) {
          rateMap = r.get<VkRenderingFragmentShadingRateAttachmentInfoKHR>();
          if (rateMap.sType != VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR || rateMap.pNext)
            throw std::runtime_error("invalid shading rate map payload");
          info.pNext = &rateMap;
        }
        if ((execute && !chains(info.colorAttachmentCount, data(colors))) ||
            ((mask & 1) && depth.pNext) || ((mask & 2) && stencil.pNext))
          throw std::runtime_error("invalid attachment chain");
        info.pColorAttachments = data(colors);
        info.pDepthAttachment = (mask & 1) ? &depth : nullptr;
        info.pStencilAttachment =
            (mask & 4) ? &depth : ((mask & 2) ? &stencil : nullptr);
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd)) {
              g->pass = true;
              g->viewMask = info.viewMask;
              g->colors = info.colorAttachmentCount;
              g->depth = (mask & 1) ? 1u : 0u;
              g->rateMap = (mask & 8) != 0;
            }
          crash_track::note_command(24, uint64_t(reinterpret_cast<uintptr_t>(cmd)), info.viewMask,
                                    info.colorAttachmentCount | ((mask & 8) ? 0x100u : 0u) | ((mask & 1) ? 0x200u : 0u));
          gfxvk::vkCmdBeginRendering(cmd, &info);
        }
        break;
      }
      case 25: {
        auto cmd = r.get<VkCommandBuffer>();
        auto source = r.get<VkPipelineStageFlags>();
        auto destination = r.get<VkPipelineStageFlags>();
        auto flags = r.get<VkDependencyFlags>();
        auto memoryCount = r.get<uint32_t>();
        auto memory = r.array<VkMemoryBarrier>(memoryCount);
        if (execute && !chains(memoryCount, data(memory)))
          throw std::runtime_error("invalid barrier chain");
        auto bufferCount = r.get<uint32_t>();
        auto buffers = r.array<VkBufferMemoryBarrier>(bufferCount);
        if (execute && !chains(bufferCount, data(buffers)))
          throw std::runtime_error("invalid barrier chain");
        auto imageCount = r.get<uint32_t>();
        auto images = r.array<VkImageMemoryBarrier>(imageCount);
        if (execute && !chains(imageCount, data(images)))
          throw std::runtime_error("invalid barrier chain");
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute)
          gfxvk::vkCmdPipelineBarrier(cmd, source, destination, flags,
                                      memoryCount, data(memory), bufferCount,
                                      data(buffers), imageCount, data(images));
        break;
      }
      case 26: {
        auto cmd = r.get<VkCommandBuffer>();
        auto info = r.get<VkRenderPassBeginInfo>();
        if (info.pNext || info.pClearValues)
          throw std::runtime_error("invalid renderpass payload");
        auto clears = r.array<VkClearValue>(info.clearValueCount);
        info.pClearValues = data(clears);
        auto contents = r.get<VkSubpassContents>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd)) g->pass = true, g->viewMask = 0, g->rateMap = false;
          crash_track::note_command(26, uint64_t(reinterpret_cast<uintptr_t>(cmd)), 0, 0);
          gfxvk::vkCmdBeginRenderPass(cmd, &info, contents);
        }
        break;
      }
      case 27: {
        auto cmd = r.get<VkCommandBuffer>();
        if (r.remaining)
          throw std::runtime_error("trailing POD payload");
        if (execute) {
          if constexpr (kDrawGuard)
            if (GuardedCommand* g = guarded(cmd)) g->pass = false;
          crash_track::note_command(27, uint64_t(reinterpret_cast<uintptr_t>(cmd)), 0, 0);
          gfxvk::vkCmdEndRenderPass(cmd);
        }
        break;
      }
      case 28: {
        auto cmd = r.get<VkCommandBuffer>();
        auto flags = r.get<VkCommandBufferUsageFlags>();
        if (r.remaining)
          throw std::runtime_error("trailing begin payload");
        if (execute) {
          VkCommandBufferBeginInfo info{
              VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
          info.flags = flags;
          checked(gfxvk::vkBeginCommandBuffer(cmd, &info));
          if constexpr (kDrawGuard) guard_begin(cmd);
        }
        break;
      }
      case 29: {
        auto device = r.get<VkDevice>();
        auto queue = r.get<VkQueue>();
        auto cmd = r.get<VkCommandBuffer>();
        auto fence = r.get<VkFence>();
        auto wait = r.get<VkSemaphore>();
        auto signal = r.get<VkSemaphore>();
        auto extraWait = r.get<VkSemaphore>();
        auto extraSignal = r.get<VkSemaphore>();
        if (r.remaining)
          throw std::runtime_error("trailing submit payload");
        if (execute) {
          if constexpr (kDrawGuard) guard_forget(cmd);
          checked(gfxvk::vkEndCommandBuffer(cmd));
          checked(gfxvk::vkResetFences(device, 1, &fence));
          std::array<VkSemaphore, 4> waits{}, signals{};
          std::array<VkPipelineStageFlags, 4> stages{
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
          std::array<uint64_t, 4> waitValues{}, signalValues{};
          VkTimelineSemaphoreSubmitInfo timeline{};
          VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
          info.commandBufferCount = 1;
          info.pCommandBuffers = &cmd;
          if (wait)
            waits[info.waitSemaphoreCount++] = wait;
          if (extraWait)
            waits[info.waitSemaphoreCount++] = extraWait;
          if (signal)
            signals[info.signalSemaphoreCount++] = signal;
          if (extraSignal)
            signals[info.signalSemaphoreCount++] = extraSignal;
          info.pWaitSemaphores = waits.data();
          info.pSignalSemaphores = signals.data();
          info.pWaitDstStageMask = stages.data();
          // the texture-pack upload thread's copies of replacements guest textures now hold (transfer_wait.h)
          transfer_wait::add(info, waits.data(), stages.data(), waitValues.data(), timeline);
          xr_queue_sync::add(info, cmd, waits.data(), stages.data(), waitValues.data(), signals.data(), signalValues.data(),
                             timeline);
          if (handoff_trace::enabled()) handoff_trace::emit_worker(handoff_trace::Kind::QueueSubmitBegin);
          VkResult result;
          {
            // a held TV present (even frame delivery) may be issued by the producer meanwhile
            std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
            result = gfxvk::vkQueueSubmit(queue, 1, &info, fence);
          }
          if (handoff_trace::enabled()) handoff_trace::emit_worker(handoff_trace::Kind::QueueSubmitEnd, uint64_t(int64_t(result)));
          checked(result);
        }
        break;
      }
      case 30: {
        const auto fn = reinterpret_cast<CallFn>(r.get<uintptr_t>());
        auto cmd = r.get<VkCommandBuffer>();
        const auto size = r.get<uint32_t>();
        auto bytes = r.array<std::byte>(size);
        if (r.remaining)
          throw std::runtime_error("trailing call payload");
        if (execute) {
          if constexpr (kDrawGuard) guard_forget(cmd);  // (a call may bind anything)
          fn(cmd, bytes.data());
        }
        break;
      }
      default:
        throw std::runtime_error("unknown POD opcode");
      }
      if (timed) {
        const uint64_t cpuEnd = rprof::thread_cpu_ns();
        const uint64_t wallEnd = handoff_trace::native_now_ns();
        ++opcodeCounts[h.opcode];
        handoff_trace::slow_opcode(h.opcode, wallStart, wallEnd, cpuStart, cpuEnd);
      }
      stream.bytes += h.bytes;
      stream.remaining -= h.bytes;
    }
  }
  if (smallDiagnostic)
    for (uint32_t opcode = 1; opcode < opcodeCounts.size(); ++opcode)
      if (opcodeCounts[opcode]) handoff_trace::emit_worker(handoff_trace::Kind::OpcodeInventory, opcode, opcodeCounts[opcode]);
}
} // namespace gfxvk

// (main.cpp's crash handler, through a weak symbol: the game is also built without this renderer)
extern "C" void wwhd_replay_crash_note(int fd, void (*out)(int, const char*, size_t)) { gfxvk::replay_crash_note(fd, out); }
