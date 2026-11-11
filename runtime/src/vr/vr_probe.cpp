// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// --vr-refresh-probe (docs/VR_FRAME_RATE.md, "Detect Headset"): the headset's refresh rate for the VR launcher's
// Detect Headset button. OpenXR tells a refresh rate only to a session: the rate it runs (XR_FB_display_refresh_rate,
// else the frame period xrWaitFrame predicts, which is how SteamVR tells it) and the rates the game may ask for (the
// extension only). So this starts a short session of its own, the way the game's starts (the same Session, on a small
// Vulkan device the runtime creates), lets it run 30 frames with nothing drawn, prints one line and ends it:
//   vr-refresh-probe: current=<Hz> offered=<Hz,Hz,...> choosable=<0|1> streamed=<0|1>
// or "vr-refresh-probe: error=<why>". The headset shows its runtime's empty scene for that second or two.
#include "openxr_session.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vr {
namespace {
int fail(const std::string& why) {
    std::printf("vr-refresh-probe: error=%s\n", why.c_str());
    std::fflush(stdout);
    return 2;
}
PFN_vkGetInstanceProcAddr vulkan_loader() {
#ifdef _WIN32
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    return m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
#else
    void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m) m = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr")) : nullptr;
#endif
}
}  // namespace

int refresh_probe_main() {
    const PFN_vkGetInstanceProcAddr gipa = vulkan_loader();
    if (!gipa) return fail("the Vulkan loader was not found");
    xr::Session s;
    if (!s.init("The Wind Waker HD (refresh rate)")) return fail(s.why());
    const auto req = s.requirements();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "The Wind Waker HD (refresh rate)";
    app.apiVersion = req.minApi > VK_API_VERSION_1_1 ? req.minApi : VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (s.create_vulkan_instance(ici, gipa, &instance) != VK_SUCCESS) return fail("the runtime could not create a Vulkan instance");
    const VkPhysicalDevice physical = s.vulkan_physical_device(instance);
    auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    if (!physical || !families) {
        if (destroyInstance) destroyInstance(instance, nullptr);
        return fail("the runtime named no GPU for the headset");
    }
    uint32_t n = 0;
    families(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> props(n);
    families(physical, &n, props.data());
    uint32_t family = 0;
    while (family < n && !(props[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    VkDevice device = VK_NULL_HANDLE;
    if (family >= n || s.create_vulkan_device(physical, dci, gipa, &device) != VK_SUCCESS) {
        destroyInstance(instance, nullptr);
        return fail("the runtime could not create a Vulkan device");
    }
    auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gipa(instance, "vkDestroyDevice"));
    const auto done = [&](int code) {
        s.shutdown();
        if (destroyDevice) destroyDevice(device, nullptr);
        if (destroyInstance) destroyInstance(instance, nullptr);
        return code;
    };
    // (small swapchains: nothing is drawn into them)
    if (!s.begin(instance, physical, device, family, 0, 0.25f)) return done(fail(s.why()));
    // until the runtime starts the session and 30 frames have run (the frame period settles), 15 s at most: SteamVR may
    // be starting
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int frames = 0;
    while (frames < 30 && std::chrono::steady_clock::now() < until) {
        s.poll_events();
        if (s.state() == xr::State::Lost || s.state() == xr::State::Unavailable) break;
        bool render = false;
        if (s.running() && s.begin_frame(render)) {
            s.end_frame(xr::Views{}, false);
            ++frames;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (frames < 30) return done(fail(s.running() ? "the session did not run its frames" : "the runtime did not start the session (is the headset awake?)"));
    std::string offered;
    for (float r : s.refresh_rates()) {
        char b[16];
        std::snprintf(b, sizeof b, "%s%.2f", offered.empty() ? "" : ",", double(r));
        offered += b;
    }
    // (streamed: the headset is streamed, by the runtime's and the headset's names, openxr_session.cpp; the launcher's
    // Detect Headset sets Streaming optimal from it, docs/VR_STREAMING.md)
    std::printf("vr-refresh-probe: current=%.2f offered=%s choosable=%d streamed=%d\n", double(s.refresh_rate()),
                offered.c_str(), s.refresh_rate_choosable() ? 1 : 0, s.streamed() ? 1 : 0);
    std::fflush(stdout);
    return done(0);
}

}  // namespace vr
