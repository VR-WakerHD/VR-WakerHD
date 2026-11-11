#pragma once
#include "loader.h"
#include "per_thread.h"
#include <cstddef>
#include <functional>
#include <new>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <vector>

namespace gfxvk {
class PodCommands;
class DeferredCommands;
// Guest recording and cross-thread checkpoint/quit hold GX2's execution mutex.
inline DeferredCommands* active_deferred_commands=nullptr;
// Stereo 3D (stereo.cpp): while the second eye records, its commands are held here and later
// emitted as one run, so neither eye's render pass ends at every eye switch. Never the active
// recorder: flushes and submissions still find the renderer's own one above.
// Per thread (docs/VR_ATLAS_THREAD.md, stage 2.7): the sun atlas's thread records into its own held
// stream whatever stream the render thread attaches meanwhile, its barriers and pass ends included.
// (per_thread.h: a plain global while no draw thread is possible, as before the threads; other threads that record
// either bypass it, presentation through isolated_command_dispatch, or never read it)
PER_THREAD(DeferredCommands*, eye_deferred_commands, nullptr)
// Isolated presentation owns a separate command pool/queue outside that mutex.
// Bypass before reading the global pointer, avoiding both capture and a data race.
inline thread_local bool isolated_command_dispatch=false;
struct IsolatedCommandDispatch {
    bool previous=isolated_command_dispatch;
    IsolatedCommandDispatch() { isolated_command_dispatch=true; }
    ~IsolatedCommandDispatch() { isolated_command_dispatch=previous; }
};
// (docs/VR_SCENE_THREADS.md, stage S4) while the scene's thread records a part beside the render thread, anything the
// render thread records would land out of order: the threads join first (stereo.cpp scene_part_recording; the scene's
// thread itself returns there at once)
inline bool g_partOverlapping = false;
void scene_part_recording();
inline DeferredCommands* dispatch_recorder() {
    if (isolated_command_dispatch) return nullptr;
    if (g_partOverlapping) [[unlikely]] scene_part_recording();
    return eye_deferred_commands() ? eye_deferred_commands() : active_deferred_commands;
}
// Owns CPU command arguments, not Vulkan handles or pointed-to GPU resources.
// Caller retains the active submission pools/uploads/retirement lists until emission.
// One renderer-global recorder; caller serializes access with the execution mutex.
// (stage D4) a call in the command stream: fn(cmd, a copy of the bytes), by the thread that records cmd
using CommandCallFn = void (*)(VkCommandBuffer cmd, const void* bytes);
class DeferredCommands {
public:
    struct Limits { size_t maxCommands=65536, maxPayloadBytes=4*1024*1024; };
    DeferredCommands() = default;
    ~DeferredCommands() { if(active_deferred_commands==this)active_deferred_commands=nullptr; }
    explicit DeferredCommands(Limits limits): limits_(limits) {}
    DeferredCommands(const DeferredCommands&) = delete;
    DeferredCommands& operator=(const DeferredCommands&) = delete;
    // Destruction releases CPU copies only. GPU emission must be explicit.
    void start_capture(std::function<void()> beforeExact = {});
    // Capture without becoming active_deferred_commands (the stereo eye stream). Its
    // owner emits the tail in order with take(); beforeExact must not return, because a
    // detached tail cannot be emitted in place of the commands recorded before it.
    void start_detached_capture(std::function<void()> beforeExact);
    // The unemitted tail, in order; capture continues with an empty tail.
    std::vector<std::function<void()>> take();
    // A wholly POD detached tail can follow the worker's prefix directly.
    // Mixed/custom commands stay owned here and use take()'s exact fallback.
    bool publish_held();
    // (docs/VR_SCENE_THREADS.md, stage S5) another detached stream's commands after this one's, both wholly POD: its
    // held bytes appended to this one's. False (nothing moved) otherwise
    bool append_held(DeferredCommands& from);
    // (stage S6.3) a draw recorded and then undone: the stream's size before it, and back to it. Only while nothing
    // since was sealed or owned (held bytes only); false (nothing changed) otherwise
    struct Mark { size_t commands = 0, heldBytes = 0, heldCommands = 0, payload = 0; };
    Mark mark() const;
    bool can_rollback(const Mark& m) const;
    bool rollback(const Mark& m);
    // Detached capture: hold the commands this POD encoder takes as its bytes, without an
    // allocation per command; take() returns each run of them as one command, in order.
    void hold_in(PodCommands* held);
    // Startup/shutdown only: reuse the existing dispatch pointer without adding
    // an extra branch to the default immediate command path.
    void set_pod_commands(PodCommands* commands);
    bool pod_mode() const { return pod_ != nullptr; }
    bool capturing() const { return capturing_; }
    size_t command_count() const { return commands_.size()-emitted_+held_command_count(); }
    size_t payload_bytes() const { return payloadBytes_; }
    void emit_exact();
    void finish_exact() { force_exact(); }
    // Allocate before sealing submission ownership: fallback can resolve/emit
    // older work without observing an unsubmitted slot lacking its action.
    template<class Seal, class Action>
    void owned_boundary(Seal seal, Action action) {
        static_assert(std::is_nothrow_invocable_v<Seal>);
        if(!capturing_) { seal(); action(); return; }
        const size_t payload=sizeof(Action);
        if(commands_.size()>=limits_.maxCommands ||
           payload>limits_.maxPayloadBytes-payloadBytes_) {
            force_exact(); seal(); action(); return;
        }
        std::function<void()> owned;
        try {
            seal_held();
            owned=action;
            commands_.reserve(commands_.size()+1);
        } catch(const std::bad_alloc&) {
            force_exact(); seal(); action(); return;
        }
        seal();
        commands_.emplace_back(std::move(owned)); // reserved; function move is noexcept
        payloadBytes_+=payload;
    }
    // Custom bindings/draws own their pending worker state by value. They are
    // emitted exactly once, in the same ordered stream as native commands.
    template<class Action> void emit_owned(Action action) {
        owned_boundary([]() noexcept {},std::move(action));
    }
    void reset(); // Rejects discarding an unemitted tail.
    void vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline);
    void vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
    void vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance);
    void vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
    void vkCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z);
    void vkCmdEndRendering(VkCommandBuffer cmd);
    void vkCmdSetStencilReference(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t reference);
    void vkCmdSetStencilWriteMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t mask);
    void vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first, uint32_t count);
    void vkCmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query);
    void vkCmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* values);
    void vkCmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D* values);
    void vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer source, VkBuffer destination, uint32_t count, const VkBufferCopy* regions);
    void vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer source, VkImage destination, VkImageLayout layout, uint32_t count, const VkBufferImageCopy* regions);
    void vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage source, VkImageLayout layout, VkBuffer destination, uint32_t count, const VkBufferImageCopy* regions);
    void vkCmdCopyImage(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, uint32_t count, const VkImageCopy* regions);
    void vkCmdBlitImage(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, uint32_t count, const VkImageBlit* regions, VkFilter filter);
    void vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets);
    void vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t offsetCount, const uint32_t* offsets);
    void vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* values);
    void vkCmdSetBlendConstants(VkCommandBuffer cmd, const float values[4]);
    void vkCmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const VkClearColorValue* value, uint32_t count, const VkImageSubresourceRange* ranges);
    void vkCmdClearDepthStencilImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue* value, uint32_t count, const VkImageSubresourceRange* ranges);
    void vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info);
    void vkCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier* memory, uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, uint32_t imageCount, const VkImageMemoryBarrier* images);
    void vkCmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents);
    void vkCmdEndRenderPass(VkCommandBuffer cmd);
    // (stage D4) fn(cmd, a copy of the bytes) in order with the other commands: in the record worker's stream when it
    // runs (it records the command buffer: PodCommands::call), else held or run now
    void call(CommandCallFn fn, VkCommandBuffer cmd, const void* bytes, uint32_t size);
private:
    PodCommands* pod_ = nullptr;
    PodCommands* held_ = nullptr;  // hold_in
    size_t held_command_count() const;
    template<class Encode, class... Args> bool hold(Encode encode, Args&&... args);
    void seal_held();  // the held bytes, as one command at the end of the tail
    Limits limits_{};
    bool capturing_=false;
    size_t payloadBytes_=0, emitted_=0;
    std::function<void()> beforeExact_;
    std::vector<std::function<void()>> commands_;
    void force_exact();
    template<class Factory, class Immediate>
    void record(size_t payload, bool supported, Factory make, Immediate immediate) {
        if(!capturing_) { immediate(); return; }
        if(!supported || commands_.size()>=limits_.maxCommands ||
           payload>limits_.maxPayloadBytes-payloadBytes_) {
            force_exact(); immediate(); return;
        }
        try {
            seal_held();
            auto command=make();
            commands_.emplace_back(std::move(command));
            payloadBytes_+=payload;
        } catch(const std::bad_alloc&) {
            // Original arguments still belong to the caller here.
            force_exact(); immediate();
        }
    }
};
DeferredCommands& deferred_commands();
} // namespace gfxvk
