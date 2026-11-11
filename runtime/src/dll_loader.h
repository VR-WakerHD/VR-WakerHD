// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
#pragma once
// Extra native libraries loaded into the game at start (docs/dll-loader.md): any DLL (.so on Linux), not only code
// mods with the wwhd_mod_init_v1 entry point (mods/packages.h). Loaded once, in this order, before the renderer
// starts (so a library that hooks Vulkan, OpenXR or SDL sees them created), and kept loaded until the game exits:
//   1. every *.dll in "dlls" next to the executable, then in "dlls" in the data folder, by file name;
//   2. the DLLs chosen in Graphics > Extra DLLs (F1 and the VR launcher; settings.ini "extraDlls"), from where they are;
//   3. WWHD_LOAD_DLLS: paths separated by ';' (absolute, or relative to the data folder);
//   4. --load-dll <path>, any number of times.
// --no-dlls or WWHD_NO_DLLS=1 loads none of them. A library that cannot be loaded is logged and skipped, never fatal.
#include <string>
#include <vector>

namespace dll_loader {
// settings.ini: the chosen DLLs' full paths, separated by '|' (a character no Windows path has)
constexpr const char* kSettingKey = "extraDlls";
inline std::vector<std::string> split_setting(const std::string& value) {
    std::vector<std::string> out;
    for (size_t start = 0; start <= value.size();) {
        size_t end = value.find('|', start);
        if (end == std::string::npos) end = value.size();
        if (end > start) out.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
inline std::string join_setting(const std::vector<std::string>& paths) {
    std::string out;
    for (const std::string& p : paths) out += (out.empty() ? "" : "|") + p;
    return out;
}

// `chosen`: the saved Graphics > Extra DLLs list (split_setting of kSettingKey)
void load_all(int argc, char** argv, const std::vector<std::string>& chosen);
// one DLL now (F1 > Graphics > Load now); false and why when it cannot be loaded
bool load_one(const std::string& path, std::string& error);
// what this start did with a path: "loaded", "already loaded", "failed: <why>", or "" when it was not tried
std::string result(const std::string& path);
}  // namespace dll_loader
