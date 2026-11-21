// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// (2026-11-18) The OpenXR runtime on a Vulkan queue of its own (vr_vulkan.cpp, "The runtime's own queue"): the game's
// graphics submissions and the runtime's queue ordered by two timeline semaphores instead of one queue and one lock.
// - acquired: signalled on the runtime's queue after each swapchain image's acquire (xrWaitSwapchainImage put its
//   barriers there); every graphics submission waits for the newest value, as for the texture packs' copies
//   (transfer_wait.h), so the game's writes come after the runtime's acquire barriers.
// - written: signalled by the graphics submission that carries a frame's writes into the swapchain images (the one
//   whose command buffer is `armed`); the runtime's queue waits for that value before the images are released and
//   the frame ends, so its release barriers and compositing come after them.
// Header-only, as transfer_wait.h: every program linking the submission code links this too. Nothing set (the runtime
// on the game's queue, or no VR): no submission changes.
#pragma once
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>

namespace gfxvk::xr_queue_sync {

inline VkSemaphore g_acquired = VK_NULL_HANDLE, g_written = VK_NULL_HANDLE;  // (made once, never destroyed)
inline std::atomic<uint64_t> g_acquiredValue{0};
// the command buffer whose submission signals `written` with g_armedValue (once; VK_NULL_HANDLE: none)
inline std::atomic<VkCommandBuffer> g_armed{VK_NULL_HANDLE};
inline std::atomic<uint64_t> g_armedValue{0};

// to `si` (waits / stages / waitValues with room for one more, signals / signalValues as well, `timeline` chained by
// transfer_wait::add or not yet): the wait for the newest acquire, and the signal when `cmd` is the armed one. The
// arrays and `timeline` must outlive the vkQueueSubmit
inline void add(VkSubmitInfo& si, VkCommandBuffer cmd, VkSemaphore* waits, VkPipelineStageFlags* stages,
                uint64_t* waitValues, VkSemaphore* signals, uint64_t* signalValues, VkTimelineSemaphoreSubmitInfo& timeline) {
    if (!g_written) [[likely]] return;
    const uint64_t acquired = g_acquired ? g_acquiredValue.load(std::memory_order_acquire) : 0;
    uint64_t written = 0;
    if (cmd && g_armed.load(std::memory_order_acquire) == cmd) {
        written = g_armedValue.load(std::memory_order_relaxed);
        g_armed.store(VK_NULL_HANDLE, std::memory_order_release);
    }
    if (!acquired && !written) return;
    const bool chained = timeline.sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    if (!chained) {
        for (uint32_t i = 0; i < si.waitSemaphoreCount; ++i) waitValues[i] = 0;  // (binary semaphores: ignored)
        timeline = VkTimelineSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.pNext = si.pNext;
        si.pNext = &timeline;
    }
    if (acquired) {
        waitValues[si.waitSemaphoreCount] = acquired;
        stages[si.waitSemaphoreCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        waits[si.waitSemaphoreCount++] = g_acquired;
        si.pWaitSemaphores = waits;
        si.pWaitDstStageMask = stages;
    }
    timeline.waitSemaphoreValueCount = si.waitSemaphoreCount;
    timeline.pWaitSemaphoreValues = waitValues;
    if (written) {
        for (uint32_t i = 0; i < si.signalSemaphoreCount; ++i) signalValues[i] = 0;
        signalValues[si.signalSemaphoreCount] = written;
        signals[si.signalSemaphoreCount++] = g_written;
        si.pSignalSemaphores = signals;
        timeline.signalSemaphoreValueCount = si.signalSemaphoreCount;
        timeline.pSignalSemaphoreValues = signalValues;
    }
}

}  // namespace gfxvk::xr_queue_sync
