// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Extra native libraries loaded at start (dll_loader.h, docs/dll-loader.md).
#include "dll_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "platform/host.h"
#include "platform/start_folder.h"
#include "runtime.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dll_loader {
namespace fs = std::filesystem;

static std::string utf8(const fs::path& p) {
    const auto bytes = p.u8string();
    return std::string(bytes.begin(), bytes.end());
}

#ifdef _WIN32
static constexpr const char* kExtension = ".dll";
static std::string last_error_text() {
    const DWORD code = GetLastError();
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, (char*)&text, 0, nullptr);
    std::string out = text ? text : "";
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out + " (error " + std::to_string(code) + ")";
}
// LOAD_WITH_ALTERED_SEARCH_PATH: the library's own dependencies are found next to it first (a mod folder with its
// helper DLLs), then in the usual places
static bool load(const fs::path& path, std::string& error) {
    if (LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return true;
    error = last_error_text();
    return false;
}
#else
static constexpr const char* kExtension = ".so";
static bool load(const fs::path& path, std::string& error) {
    if (dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) return true;
    const char* reason = dlerror();
    error = reason ? reason : "cannot load";
    return false;
}
#endif

// the same file reached by two routes (the executable's folder is the data folder) is loaded once
static std::string identity(const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    std::string key = utf8(ec ? path.lexically_normal() : p);
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
#endif
    return key;
}

static bool has_extension(const fs::path& path) {
    std::string ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == kExtension;
}

// the folder's libraries by file name (so 00_first.dll, 10_second.dll set an order)
static void add_folder(const fs::path& folder, std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && has_extension(it->path())) found.push_back(it->path());
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    out.insert(out.end(), found.begin(), found.end());
}

// what each path's load did (keyed by identity), for F1 > Graphics; the menu reads it from its own thread
static std::mutex g_mutex;
static std::map<std::string, std::string> g_results;

// one path: loaded once, its result kept and logged
static bool load_path(const fs::path& path, std::string& error) {
    const std::string key = identity(path);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto it = g_results.find(key); it != g_results.end() && it->second == "loaded") return true;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) error = "not found";
    else if (load(path, error)) {
        g_results[key] = "loaded";
        LOG("[dlls] loaded %s", utf8(path).c_str());
        return true;
    }
    g_results[key] = "failed: " + error;
    LOG("[dlls] %s: cannot load: %s", utf8(path).c_str(), error.c_str());
    return false;
}

bool load_one(const std::string& path, std::string& error) { return load_path(start_folder::utf8_path(path), error); }

std::string result(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_results.find(identity(start_folder::utf8_path(path)));
    return it == g_results.end() ? std::string() : it->second;
}

void load_all(int argc, char** argv, const std::vector<std::string>& chosen) {
    std::vector<std::string> named;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-dlls")) {
            LOG("[dlls] --no-dlls: no extra libraries loaded");
            return;
        }
        if (!strcmp(argv[i], "--load-dll") && i + 1 < argc) named.push_back(argv[++i]);
    }
    if (const char* off = getenv("WWHD_NO_DLLS"); off && !strcmp(off, "1")) {
        LOG("[dlls] WWHD_NO_DLLS=1: no extra libraries loaded");
        return;
    }
    std::error_code ec;
    const fs::path data = fs::current_path(ec);
    std::vector<fs::path> paths;
    if (const std::string exe = host::exe_dir(); !exe.empty()) add_folder(start_folder::utf8_path(exe) / "dlls", paths);
    if (!ec) add_folder(data / "dlls", paths);
    std::vector<std::string> listed = chosen;  // Graphics > Extra DLLs, then WWHD_LOAD_DLLS, then --load-dll
    if (const char* list = getenv("WWHD_LOAD_DLLS"); list && *list) {
        std::string all = list;
        for (size_t start = 0; start <= all.size();) {
            size_t end = all.find(';', start);
            if (end == std::string::npos) end = all.size();
            std::string entry = all.substr(start, end - start);
            while (!entry.empty() && entry.back() == ' ') entry.pop_back();
            while (!entry.empty() && entry.front() == ' ') entry.erase(0, 1);
            if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') entry = entry.substr(1, entry.size() - 2);
            if (!entry.empty()) listed.push_back(entry);
            start = end + 1;
        }
    }
    listed.insert(listed.end(), named.begin(), named.end());
    for (const std::string& entry : listed) {
        fs::path p = start_folder::utf8_path(entry);
        paths.push_back(p.is_relative() && !ec ? data / p : p);
    }
    if (paths.empty()) return;
    std::set<std::string> seen;
    unsigned loaded = 0, failed = 0;
    for (const fs::path& path : paths) {
        if (!seen.insert(identity(path)).second) continue;
        std::string error;
        (load_path(path, error) ? loaded : failed)++;
    }
    LOG("[dlls] %u loaded, %u failed", loaded, failed);
}
}  // namespace dll_loader
