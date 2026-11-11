// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The graphics queue's wait for the texture-pack upload thread's copies (texture_pack_vk.cpp): replacement images are
// filled on a queue family of their own and signal a timeline semaphore; once the host has seen a value reached, the
// render thread attaches that value's images to their guest textures and raises `value` here. Every graphics
// submission (backend.cpp submit, the record worker's submit opcode in pod_commands.cpp) then waits for the newest
// value, at every stage (ALL_COMMANDS), which makes the copies and layout changes visible to it (a wait the
// GPU never stalls on: the value has been reached when it is published). Header-only, so every program linking the
// submission code links this too; nothing set (no pack, nothing published yet): no submission changes.
#pragma once
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>

namespace gfxvk::transfer_wait {

// the upload thread's timeline semaphore: set once, before `value` first becomes nonzero, and never destroyed (a
// submission may wait on it until the process ends)
inline VkSemaphore g_semaphore = VK_NULL_HANDLE;
// the newest value whose images guest textures hold (only grows; 0: none yet)
inline std::atomic<uint64_t> g_value{0};

// adds the wait to `si`, whose pWaitSemaphores / pWaitDstStageMask are `waits` / `stages`, with room for one more;
// `values` (as large) and `timeline` must outlive the vkQueueSubmit. Returns whether it added one.
inline bool add(VkSubmitInfo& si, VkSemaphore* waits, VkPipelineStageFlags* stages, uint64_t* values,
                VkTimelineSemaphoreSubmitInfo& timeline) {
    const uint64_t v = g_value.load(std::memory_order_acquire);
    if (!v) [[likely]] return false;
    for (uint32_t i = 0; i < si.waitSemaphoreCount; ++i) values[i] = 0;  // (binary semaphores: ignored)
    values[si.waitSemaphoreCount] = v;
    // every stage: a replacement read by a transfer or compute is covered too (no stall: the value has been reached)
    stages[si.waitSemaphoreCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    waits[si.waitSemaphoreCount++] = g_semaphore;
    timeline = VkTimelineSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.pNext = si.pNext;
    timeline.waitSemaphoreValueCount = si.waitSemaphoreCount;
    timeline.pWaitSemaphoreValues = values;
    si.pNext = &timeline;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    return true;
}

}  // namespace gfxvk::transfer_wait
