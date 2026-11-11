// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Replacement texture packs in the Vulkan renderer (texture_pack_vk.cpp): the calls the rest of the renderer makes.
// texture_pack_step and replacement_for are declared in backend.h; the registry and loader are gfx/texture_pack.h.
#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#include "backend.h"

namespace gfxvk {

// texture_pack::active() as of the renderer's start (set there and at shutdown only): the hooks' one test, a plain
// load the branch predictor learns, no call
extern bool g_texture_packs_on;
// (draw.cpp's binding, under the surface lock) units that kept the original because the shader needs the guest's own
// levels or size there: the stats line's guest-levels, since the last line
extern uint64_t g_stat_texpack_guest_levels;
// (diagnostics) draw.cpp's pack_replacement calls and their time, lock wait included, since the last line; any
// drawing thread, hence atomic
extern std::atomic<uint64_t> g_stat_texpack_bind_calls, g_stat_texpack_bind_ns;
// (texture_pack_step, under the surface lock) finished loads not made because the VRAM budget had no room: the stats
// line's refused, since the last line
extern uint64_t g_stat_texpack_refused;

// (render thread; the smoke reads it) level bytes the render thread itself has copied into replacement images since
// the start: the path without the upload thread's (texture_pack_vk.cpp); with it, always 0
extern std::atomic<uint64_t> g_stat_texpack_render_copy_bytes;
// (diagnostics) replacement images the upload thread has filled on its own queue since the start
extern std::atomic<uint64_t> g_stat_texpack_uploaded;

// device creation (backend.cpp, before vkCreateDevice): the queue family the texture packs' upload thread copies on,
// when a pack will be loaded (an enabled "textures" package, vr.texturePacks on, BC formats; or the renderer smoke),
// the device has timeline semaphores and a family without graphics (texture_pack_core.h transfer_family); the caller
// then asks for one queue of it and enables timelineSemaphore. ~0u otherwise: replacements are made on the render
// thread, as before (texture_pack_start logs why). WWHD_TEXTURE_PACK_RENDER_UPLOADS=1 keeps the render thread's path.
uint32_t texture_pack_transfer_family(VkPhysicalDevice physical, bool timelineSemaphores, bool bcFormats);
// after vkCreateDevice: that family's queue and the timeline semaphore entry points
void texture_pack_device_ready();
// (any thread, before vkDeviceWaitIdle) the upload thread's queue held: it is not in vkQueueSubmit or
// vkQueueWaitIdle while the lock is held (vkDeviceWaitIdle needs every queue of the device externally synchronized).
// Always lockable (an unused mutex without a pack). Take no other lock that the upload thread waits for meanwhile:
// it holds this one only for those two calls.
std::unique_lock<std::mutex> texture_pack_queue_lock();
// (main.cpp, --renderer-smoke, before the renderer starts) the upload thread's queue is asked for without a package
void texture_pack_want_uploads_for_test();
// whether replacements are made on the upload thread (its queue and entry points exist); the smoke reads it
bool texture_pack_uploads_available();
// TEST ONLY (renderer smoke): replacements made on the render thread even where the upload thread could run (takes
// effect at the next start of the upload thread: set it while none runs)
void texture_pack_set_render_uploads_for_test(bool on);

// renderer start (backend.cpp, after the device): index warnings to the Mods tab (packages::note_texture_pack), then
// the enabled "textures" packages' indexes (packages::texture_roots) when vr.texturePacks is on. Nothing else
// starts: no package, or the setting off, leaves texture_pack::active() false and every hook below idle.
void texture_pack_start();
// renderer shutdown (after the graphics queue is idle): the upload thread and the loader thread stop; the images
// still on their way are freed
void texture_pack_shutdown();
// upload_surface, after a real upload of `s` (its contents changed; s->level0Hash is current), only while
// g_texture_packs_on: drops a replacement made for other contents, asks for one if the new key is in a pack (or
// attaches the one already made by a prefetch of its archive: pointer work). Under the surface lock.
void texture_pack_after_upload(Surface* s);
// destroy_surface_image of a guest surface with a replacement: the replacement goes with the guest's image (freed at
// the next frame's start, so no draw of this frame still holds it). Under the surface lock.
void texture_pack_drop(Surface* s);
// the stats line (backend.cpp, beside the texture lines); nothing without a pack
void texture_pack_report();
// the F1 overlay's line: the replacement images' VRAM used / budget; empty without a pack. Any thread.
std::string texture_pack_status();
// TEST ONLY (renderer smoke): the VRAM budget as vr.texturePackBudgetMB would set it, in bytes (0: automatic again)
void texture_pack_set_budget_for_test(uint64_t bytes);
// TEST ONLY (renderer smoke, render thread): prefetched replacements made and attached to no guest yet
size_t texture_pack_unattached_for_test();
// TEST ONLY (renderer smoke, render thread): prefetched loads the VRAM budget had no room for since the start (their
// data freed on the upload thread)
uint64_t texture_pack_prefetch_full_for_test();
// (surfaces.cpp) a staging slice from the submission's upload arena for `size` bytes of `s`'s texels, or an empty
// slice: the caller makes its own staging buffer
UploadSlice texture_staging_slice(const Surface* s, size_t size);

}  // namespace gfxvk
