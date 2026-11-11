// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The launcher's "Install game" tab (docs/VR_LAUNCHER.md, "Install game"): the decisions, without SDL or ImGui, so
// they are tested on their own (runtime/tools/launcher_install_test.cpp). The extraction itself is wwhd-extract's
// (tools/wudextract): the tab runs it (extract_runner.h) and reads what it prints. Nothing here reads a disc image or
// an archive.
//
// - what `wwhd-extract info` printed: the title id (a string, leading zeros kept), and for a Cemu archive its title
//   folders, of which the game itself is chosen (title type 00050000, not an update or DLC);
// - the game's name from its own meta/meta.xml (longname_en), never from the file name;
// - wwhd-extract's exit codes in plain words;
// - the destination: can it be written, is it empty, does it already hold this game, how much space is free;
// - the extracted game checked as the game checks it (code/cking.rpx) and against its title id;
// - the launcher's own vr-launcher.ini: the game folder is its launcher.gameDir (the launcher starts the game with
//   --game and that folder). settings.ini holds no game folder. Updated in place, every other line kept, a backup
//   first, written atomically, read back.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "vr/vr_settings_file.h"

namespace install {

namespace fs = std::filesystem;

inline fs::path u8path(const std::string& s) { return fs::u8path(s); }
inline std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
inline bool is_hex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

// ---- the game's versions (tools/recomp/builds.py, tools/installer/setup.py TITLE_IDS)

struct KnownTitle { const char* id; const char* region; bool supported; };
inline constexpr KnownTitle kKnownTitles[] = {
    {"0005000010143500", "USA", true},
    {"0005000010143600", "Europe", true},
    {"0005000010143400", "Japan", false},
};
inline const KnownTitle* known_title(const std::string& id) {
    for (const auto& k : kKnownTitles)
        if (lower(id) == k.id) return &k;
    return nullptr;
}
// "00050000-10143500" as the setup writes them
inline std::string title_text(const std::string& id) {
    return is_hex16(id) ? id.substr(0, 8) + "-" + id.substr(8) : id;
}

// ---- wwhd-extract info

struct ArchiveTitle {
    std::string id, folder;
    unsigned version = 0;
    uint64_t files = 0, bytes = 0;
};
struct SourceInfo {
    bool archive = false;            // "format wua": a Cemu archive (no keys)
    std::string titleId;             // the selected title's id (disc: the game partition's)
    std::string folder;              // the archive's title folder used (0005000010143500_v0)
    unsigned version = 0;
    uint64_t files = 0, bytes = 0;   // what is extracted
    std::vector<ArchiveTitle> titles;
};
// key value lines ("title_id 0005000010143500", "title ID VERSION FOLDER FILES BYTES", "format wua"); anything else
// (an "error: ..." line) is left alone
inline SourceInfo parse_info(const std::string& out) {
    SourceInfo s;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream l(line);
        std::string key;
        l >> key;
        if (key == "format") {
            std::string v;
            l >> v;
            s.archive = v == "wua";
        } else if (key == "title") {
            ArchiveTitle t;
            if (l >> t.id >> t.version >> t.folder >> t.files >> t.bytes) {
                t.id = lower(t.id);
                s.titles.push_back(t);
            }
        } else if (key == "title_id") {
            l >> s.titleId;
            s.titleId = lower(s.titleId);
        } else if (key == "selected") {
            l >> s.folder;
        } else if (key == "version") {
            l >> s.version;
        } else if (key == "files") {
            l >> s.files;
        } else if (key == "bytes") {
            l >> s.bytes;
        }
    }
    return s;
}
// the archive's title the game is extracted from: the game itself (00050000-...), a supported version of it first;
// not an update (0005000e) or DLC (0005000c). Empty with `why` when there is none or no clear one
inline const ArchiveTitle* choose_archive_title(const std::vector<ArchiveTitle>& titles, std::string& why) {
    const ArchiveTitle* best = nullptr;
    int games = 0;
    for (const auto& t : titles) {
        if (t.id.compare(0, 8, "00050000") != 0) continue;
        ++games;
        const KnownTitle* k = known_title(t.id);
        const bool better = !best || (k && k->supported && !(known_title(best->id) && known_title(best->id)->supported)) ||
                            (best->id == t.id && t.version < best->version);
        if (better) best = &t;
    }
    if (!best) {
        bool update = false;
        for (const auto& t : titles) update = update || t.id.compare(0, 8, "0005000e") == 0;
        why = titles.empty() ? "The archive holds no Wii U titles."
              : update       ? "The archive holds only an update or downloadable content, not the game itself. In Cemu, "
                               "make the archive with the game included."
                             : "The archive holds no game (only other kinds of Wii U titles).";
        return nullptr;
    }
    if (games > 1 && !(known_title(best->id) && known_title(best->id)->supported)) {
        why = "The archive holds several games and none of them is The Wind Waker HD.";
        return nullptr;
    }
    return best;
}

// ---- the game's name, from its meta/meta.xml

inline std::string xml_text(std::string s) {
    auto replace = [&](const std::string& from, const std::string& to) {
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    };
    replace("&lt;", "<");
    replace("&gt;", ">");
    replace("&quot;", "\"");
    replace("&apos;", "'");
    replace("&amp;", "&");
    std::string out;
    bool space = false;
    for (char c : s) {  // its lines (the name is split over two on the console's menu) as one
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}
// the element's text, or empty
inline std::string xml_element(const std::string& xml, const std::string& name) {
    const size_t open = xml.find("<" + name);
    if (open == std::string::npos) return {};
    const size_t start = xml.find('>', open);
    const size_t end = xml.find("</" + name + ">", open);
    if (start == std::string::npos || end == std::string::npos || end < start) return {};
    return xml.substr(start + 1, end - start - 1);
}
struct Meta {
    std::string title;    // longname_en (else shortname_en); empty: not known
    std::string titleId;  // its title_id element, lower case; empty: not there
};
inline Meta parse_meta(const std::string& xml) {
    Meta m;
    if (xml.find("<menu") == std::string::npos && xml.find("<title_id") == std::string::npos) return m;  // not a meta.xml
    m.title = xml_text(xml_element(xml, "longname_en"));
    if (m.title.empty()) m.title = xml_text(xml_element(xml, "shortname_en"));
    const std::string id = lower(xml_text(xml_element(xml, "title_id")));
    if (is_hex16(id)) m.titleId = id;
    return m;
}
inline std::string read_file(const fs::path& p, size_t limit = 1 << 20) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s(limit, '\0');
    in.read(&s[0], std::streamsize(limit));
    s.resize(size_t(in.gcount()));
    return s;
}

// ---- wwhd-extract's exit codes (tools/wudextract/wwhd_extract.cpp)

inline std::string extract_error(int code, const std::string& detail, bool archive) {
    std::string s;
    switch (code) {
    case 2: s = "The extractor was started with arguments it does not accept (a launcher error)."; break;
    case 3: s = "The disc image's own key is missing or not readable. It is the .key file of your dump (by default the "
                "image's name with .key, next to it): choose it under \"Disc key file\"."; break;
    case 4: s = "The disc key does not belong to this disc image: choose the .key file made with this dump."; break;
    case 5: s = "The Wii U common key is missing or not readable. It comes from your own console: choose its file under "
                "\"Wii U common key file\" (or put common.key next to the image)."; break;
    case 6: s = "The Wii U common key is not the right one: check that it is the common key from your console."; break;
    case 7: s = archive ? "The selected file could not be read as a Cemu Wii U archive (.wua)."
                        : "The selected file could not be recognized as a supported Wii U game (a .wud or .wux disc "
                          "image, or a Cemu .wua archive)."; break;
    case 8: s = "The file is damaged: its checksums do not match (an incomplete download or copy, or a disk error). "
                "Make a new copy of it."; break;
    case 9: s = "The files could not be written: the destination folder is not writable or the disk is full."; break;
    case 10: s = "The archive does not hold the game itself."; break;
    default: s = "The extractor stopped with an error (exit code " + std::to_string(code) + ")."; break;
    }
    if (!detail.empty()) s += "\nDetails: " + detail;
    return s;
}
// the "error: ..." lines the extractor printed (its own words, kept as details)
inline std::string error_detail(const std::string& out) {
    std::istringstream in(out);
    std::string line, d;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("error: ", 0) == 0) d += (d.empty() ? "" : "; ") + line.substr(7);
    }
    return d;
}

// ---- extract --progress

struct Progress {
    std::string phase;  // "verify" (an archive's checksum), "extract"
    uint64_t done = 0, total = 0;
};
// one line of its output; false when the line is not progress
inline bool parse_progress(const std::string& line, Progress& p) {
    std::istringstream l(line);
    std::string key;
    l >> key;
    if (key == "phase") {
        l >> p.phase;
        p.done = p.total = 0;
        return true;
    }
    if (key == "progress") {
        uint64_t d = 0, t = 0;
        if (l >> d >> t) {
            p.done = d;
            p.total = t;
            if (p.phase.empty()) p.phase = "extract";  // a disc image prints no phase
            return true;
        }
    }
    return false;
}

// ---- the destination

struct Destination {
    bool valid = false;        // a folder can be extracted to here
    std::string problem;       // why not
    bool exists = false, empty = true;
    bool hasGame = false;      // code/cking.rpx is there already
    std::string gameTitleId;   // its meta/meta.xml title id
    bool spaceKnown = false;
    uint64_t freeBytes = 0;
};
// the game's file the game itself loads first (runtime/src/main.cpp: game_dir + "/code/cking.rpx")
inline fs::path game_rpx(const fs::path& dir) { return dir / "code" / "cking.rpx"; }

// `appDir`: the launcher's own folder, which is never chosen silently
inline Destination check_destination(const std::string& path, const std::string& appDir = {}) {
    Destination d;
    if (path.empty()) {
        d.problem = "Choose a folder to extract the game to.";
        return d;
    }
    const fs::path p = u8path(path);
    std::error_code ec;
    if (!p.is_absolute()) {
        d.problem = "Use a full folder path (for example one chosen with Browse).";
        return d;
    }
    if (!appDir.empty() && fs::equivalent(p, u8path(appDir), ec)) {
        d.problem = "This is the launcher's own folder: choose another one, for example a new folder for the game.";
        return d;
    }
    d.exists = fs::exists(p, ec);
    if (d.exists && !fs::is_directory(p, ec)) {
        d.problem = "This is a file, not a folder.";
        return d;
    }
    // where it is created, or its nearest existing parent
    fs::path probe = p;
    while (!probe.empty() && !fs::exists(probe, ec)) {
        if (probe == probe.parent_path()) break;
        probe = probe.parent_path();
    }
    if (d.exists) {
        d.empty = fs::directory_iterator(p, ec) == fs::directory_iterator();
        if (ec) {
            d.problem = "The folder cannot be read: " + ec.message();
            return d;
        }
        d.hasGame = fs::is_regular_file(game_rpx(p), ec);
        if (d.hasGame) d.gameTitleId = parse_meta(read_file(p / "meta" / "meta.xml")).titleId;
    }
    // writable: a test file in it (or in its nearest existing parent, where the folder is created)
    {
        const fs::path test = probe / ".wwhd-write-test";
        std::ofstream o(test, std::ios::binary);
        const bool ok = bool(o);
        o.close();
        fs::remove(test, ec);
        if (!ok) {
            d.problem = "The destination folder is not writable (" + utf8(probe) + "): choose another one.";
            return d;
        }
    }
    const fs::space_info sp = fs::space(probe, ec);
    if (!ec && sp.available != static_cast<std::uintmax_t>(-1)) {
        d.spaceKnown = true;
        d.freeBytes = sp.available;
    }
    d.valid = true;
    return d;
}

// ---- the extracted game

struct Verify {
    bool ok = false;
    std::vector<std::string> problems;
    std::string titleId;  // from its meta/meta.xml
};
inline Verify verify_game(const std::string& dir, const std::string& expectedTitleId) {
    Verify v;
    const fs::path p = u8path(dir);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        v.problems.push_back("The folder is not there.");
        return v;
    }
    for (const char* sub : {"code", "content", "meta"})
        if (!fs::is_directory(p / sub, ec)) v.problems.push_back(std::string("The folder \"") + sub + "\" is missing.");
    const fs::path rpx = game_rpx(p);
    if (!fs::is_regular_file(rpx, ec)) v.problems.push_back("code/cking.rpx (the game's program) is missing.");
    else if (fs::file_size(rpx, ec) == 0 || ec) v.problems.push_back("code/cking.rpx (the game's program) is empty.");
    const fs::path meta = p / "meta" / "meta.xml";
    if (!fs::is_regular_file(meta, ec) || fs::file_size(meta, ec) == 0) {
        v.problems.push_back("meta/meta.xml (the game's description) is missing or empty.");
    } else {
        v.titleId = parse_meta(read_file(meta)).titleId;
        if (!expectedTitleId.empty() && !v.titleId.empty() && v.titleId != lower(expectedTitleId))
            v.problems.push_back("meta/meta.xml names title " + title_text(v.titleId) + ", not " +
                                 title_text(expectedTitleId) + " as the source does.");
    }
    v.ok = v.problems.empty();
    return v;
}

// ---- vr-launcher.ini (launcher.gameDir)

inline constexpr const char* kGameDirKey = "launcher.gameDir";

// a backup's name next to the file that is not taken yet: vr-launcher.ini.bak, then .bak2, .bak3 ...
inline fs::path backup_name(const fs::path& file) {
    std::error_code ec;
    fs::path b = file;
    b += ".bak";
    for (int i = 2; fs::exists(b, ec) && i < 1000; ++i) {
        b = file;
        b += ".bak" + std::to_string(i);
    }
    return b;
}
struct ConfigResult {
    bool ok = false;
    std::string error;
    std::string backup;  // the backup made (empty: the file did not exist yet)
};
// launcher.gameDir = `gameDir` in the file at `path`: every other line as it was, the file backed up first, written
// through `replace` (atomic: a temporary file renamed over it; host::replace_file), read back and checked
inline ConfigResult configure_game_dir(const std::string& path, const std::string& gameDir,
                                       const std::function<bool(const std::string&, const std::string&)>& replace) {
    ConfigResult r;
    const fs::path file = u8path(path);
    std::error_code ec;
    vr::settings::File f;
    std::string err;
    if (!f.load(path, err)) {
        r.error = err;
        return r;
    }
    fs::create_directories(file.parent_path(), ec);
    if (fs::exists(file, ec)) {
        const fs::path b = backup_name(file);
        fs::copy_file(file, b, fs::copy_options::none, ec);
        if (ec) {
            r.error = "could not back up " + path + ": " + ec.message();
            return r;
        }
        r.backup = utf8(b);
    }
    std::string format;
    if (!f.get("launcher.format", format)) f.set("launcher.format", "1");
    f.set(kGameDirKey, gameDir);
    if (!f.save(path, err, replace)) {
        r.error = err;
        return r;
    }
    vr::settings::File back;
    std::string got;
    if (!back.load(path, err) || !back.get(kGameDirKey, got) || got != gameDir) {
        r.error = "the file was written but does not read back with the new game folder";
        return r;
    }
    r.ok = true;
    return r;
}

// sizes for people: "1.2 GB"
inline std::string size_text(uint64_t bytes) {
    char b[32];
    if (bytes >= (1ull << 30)) std::snprintf(b, sizeof b, "%.1f GB", double(bytes) / double(1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(b, sizeof b, "%.0f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(b, sizeof b, "%.0f KB", double(bytes) / 1024.0);
    return b;
}

}  // namespace install
