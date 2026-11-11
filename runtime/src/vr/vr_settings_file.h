// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR settings in the game's settings file (docs/VR_SETTINGS.md), for programs other than the game: the VR
// launcher (tools/vr_launcher) reads and writes them here, the game through hostui. Both use the definitions of
// vr_config.h and vr_bindings.h, so there is one list of keys, defaults and ranges.
//
// The file is the game's `settings.ini`: `key=value` lines, `#` comments. Only `vr.*` keys are written; every other
// line (other settings, comments, unknown keys, malformed lines) is kept as it was, in its place. A key that is
// there more than once takes the last value, as the game reads it; writing a key leaves one line for it. Saving
// writes a temporary file and replaces the settings file with it (never a half-written file).
#pragma once
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "vr_bindings.h"
#include "vr_config.h"

namespace vr::settings {

struct File {
    std::vector<std::string> lines;

    static size_t eq(const std::string& line) {
        if (line.empty() || line[0] == '#') return std::string::npos;
        return line.find('=');
    }
    // A missing file is an empty one (all defaults). False with `error` when it exists but cannot be read.
    bool load(const std::string& path, std::string& error) {
        lines.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (f) {
                std::fclose(f);
                error = "cannot read " + path;
                return false;
            }
            return true;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        return true;
    }
    bool get(const std::string& key, std::string& value) const {
        bool found = false;
        for (const auto& l : lines) {
            const size_t e = eq(l);
            if (e != std::string::npos && l.compare(0, e, key) == 0 && e == key.size()) {
                value = l.substr(e + 1);
                found = true;
            }
        }
        return found;
    }
    void set(const std::string& key, const std::string& value) {
        int last = -1;
        for (int i = 0; i < int(lines.size()); ++i) {
            const size_t e = eq(lines[size_t(i)]);
            if (e == key.size() && lines[size_t(i)].compare(0, e, key) == 0) last = i;
        }
        const std::string line = key + "=" + value;
        if (last < 0) {
            lines.push_back(line);
            return;
        }
        lines[size_t(last)] = line;
        // earlier duplicates would be read first and lose anyway: drop them so the file says one thing
        for (int i = last - 1; i >= 0; --i) {
            const size_t e = eq(lines[size_t(i)]);
            if (e == key.size() && lines[size_t(i)].compare(0, e, key) == 0) lines.erase(lines.begin() + i);
        }
    }
    std::string text() const {
        std::string t;
        for (const auto& l : lines) t += l + "\n";
        return t;
    }
    // writes to `path` through a temporary file next to it; `replace(from, to)` replaces the file (the game's
    // host::replace_file in the launcher; a plain rename in tests)
    template <class Replace>
    bool save(const std::string& path, std::string& error, Replace&& replace) const {
        const std::string tmp = path + ".vr-tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) { error = "cannot write " + tmp; return false; }
            out << text();
            out.flush();
            if (!out) { error = "cannot write " + tmp + " (disk full?)"; std::remove(tmp.c_str()); return false; }
        }
        if (!replace(tmp, path)) {
            std::remove(tmp.c_str());
            error = "cannot replace " + path;
            return false;
        }
        return true;
    }
};

// what the file says (defaults for everything missing or unusable); `problems` lists the values that were kept at
// their defaults because they could not be read
inline Config read_config(const File& f, std::vector<std::string>* problems = nullptr) {
    Config c;
    for (const auto& key : keys()) {
        std::string v;
        if (f.get(key, v) && !from_kv(c, key, v) && problems) problems->push_back(key + "=" + v);
    }
    std::string format;
    migrate_config(c, f.get("vr.format", format) ? std::atoi(format.c_str()) : 1);
    validate(c);
    return c;
}
inline Bindings read_bindings(const File& f, std::vector<std::string>* problems = nullptr) {
    Bindings b;
    for (const auto& [key, unused] : Bindings{}.to_kv()) {
        std::string v;
        if (f.get(key, v) && !b.from_kv(key, v) && problems) problems->push_back(key + "=" + v);
    }
    std::string format;
    migrate_bindings(b, f.get("vr.format", format) ? std::atoi(format.c_str()) : 1);
    return b;
}
// the VR settings into the file (only vr.* keys change)
inline void write(File& f, const Config& c, const Bindings& b) {
    Config v = c;
    validate(v);
    for (const auto& [k, value] : to_kv(v)) f.set(k, value);
    for (const auto& [k, value] : b.to_kv()) f.set(k, value);
}

}  // namespace vr::settings
