#pragma once
#include "gamepad_backdrop.h"
#include "../overlay/hostui.h"
#include <string>

namespace gfx::gamepad_backdrop {
inline constexpr const char* kSettingKey = "gamepad-bar-background";
inline void load_options() {
    std::string saved;
    // (VR fork) off unless chosen ("1"), and never in a VR start
    enabled.store(possible() && hostui::get(kSettingKey, saved) && saved == "1", std::memory_order_relaxed);
}
inline void set_option(bool on) {
    enabled.store(on && possible(), std::memory_order_relaxed);
    hostui::set(kSettingKey, on ? "1" : "0");
}
} // namespace gfx::gamepad_backdrop
