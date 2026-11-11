// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The GPU-free rules of replacement texture packs ("textures" mod packages written by ZeldaWWHD Texture Studio):
// the index.json reader, which GX2 format may be replaced by which DDS format, the check of a loaded DDS file, the
// LOD shift a replacement needs, and the rule that decides whether a draw may bind the replacement. The loader
// thread, the replacement images and the binding build on these; everything here is pure so it can be tested
// without a device.
//
// A texture is identified by its Key: GX2 surface format, size and the XXH3-64 of its level-0 guest bytes. An
// index maps Keys to a DDS file whose level 0 is `scale` times larger and whose format keeps the original's
// meaning (BC1/2/3 -> BC7, BC4 -> BC4, BC5 -> BC5, RGBA8 -> BC7 or RGBA8, each with the original's sRGB-ness or
// signedness), so the guest's component selectors and sampling state apply unchanged. Any mismatch means the
// original texture keeps drawing: parse_index skips a bad entry with a warning, check_dds says no.
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

#include "mods/mod_json.h"

namespace gfx::texture_pack {

struct Key {
    uint32_t format, width, height;
    uint64_t hash;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        // the hash is already well mixed; fold the rest in with a 64-bit multiply
        uint64_t h = k.hash ^ (uint64_t(k.format) << 40) ^ (uint64_t(k.width) << 20) ^ k.height;
        return size_t(h * 0x9E3779B97F4A7C15ull);
    }
};

// `dxgi` 0 means "take the format from the DDS" (check_dds accepts any format allowed_dxgi permits for the key);
// nonzero (an optional "dxgi" in the index) must also equal the file's.
// `bad` marks an entry whose file failed to load or check, so it is reported once and the original keeps drawing.
struct Entry {
    std::filesystem::path file;
    uint32_t scale;
    uint32_t dxgi;
    bool bad = false;
};

// a std::string key looked up by std::string_view without a copy
struct PathHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

struct Index {
    std::unordered_map<Key, Entry, KeyHash> entries;
    // normalized game path (normalize_game_path) of an archive -> the keys of the textures it holds, in entry order:
    // the optional "archives" of the entries (the disc's top-level files, relative to /vol/content/)
    std::unordered_map<std::string, std::vector<Key>, PathHash, std::equal_to<>> archives;
    std::vector<std::string> warnings;
};

// A game file's path as the archive index names it: a leading /vol/content (the game's cwd: a relative path is the
// same file) and any leading slashes or "./" stripped, '\' as '/', no double slashes, ASCII lowercase (Wii U paths are
// case-insensitive). Written to `out`; returns its length, 0 for a path outside /vol/content (another volume), an
// empty one, or one longer than `out`. No allocation: the file-open hook calls it with a stack buffer.
inline size_t normalize_game_path(std::string_view in, std::span<char> out) {
    auto slash = [](char c) { return c == '/' || c == '\\'; };
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c == '\\' ? '/' : c; };
    if (!in.empty() && slash(in[0])) {
        constexpr std::string_view kContent = "/vol/content";
        if (in.size() < kContent.size()) return 0;
        for (size_t i = 0; i < kContent.size(); ++i)
            if (lower(in[i]) != kContent[i]) return 0;
        in.remove_prefix(kContent.size());
        if (!in.empty() && !slash(in[0])) return 0;  // (/vol/contentx)
    }
    for (;;) {
        if (!in.empty() && slash(in[0])) in.remove_prefix(1);
        else if (in.size() >= 2 && in[0] == '.' && slash(in[1])) in.remove_prefix(2);
        else break;
    }
    size_t n = 0;
    char prev = 0;
    for (char c : in) {
        c = lower(c);
        if (c == '/' && prev == '/') continue;
        if (n == out.size()) return 0;
        out[n++] = prev = c;
    }
    return n;
}

namespace detail {
// a JSON number that is a whole value in [lo, hi]
inline bool whole(const mods::json::Value& v, double lo, double hi, uint32_t& out) {
    if (v.type != mods::json::Value::Number) return false;
    const double n = v.number;
    // the range first (NaN fails it too): only then is the conversion defined (lo >= 0, hi < 2^32)
    if (!(n >= lo && n <= hi)) return false;
    if (n != static_cast<double>(static_cast<uint64_t>(n))) return false;
    out = static_cast<uint32_t>(n);
    return true;
}
inline bool hex16(const std::string& s, uint64_t& out) {
    if (s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | uint64_t(d);
    }
    out = v;
    return true;
}
inline bool valid_scale(uint32_t s) { return s == 2 || s == 4 || s == 8 || s == 16 || s == 32; }
} // namespace detail

// The largest index.json read: a package of a few thousand entries is a megabyte or more (about 210 bytes an entry),
// beyond the 1 MiB of manifests
inline constexpr size_t kMaxIndexBytes = 64ull * 1024 * 1024;

// Merges the entries of one index.json into `into`. `file` paths are relative to `root` (the package's textures
// directory) and may not leave it. A key already present is kept (the first pack wins) with a warning; an entry
// with a bad field is skipped with a warning. A document that is not a JSON object, or whose format_version is not
// 1, returns false with `error` set.
inline bool parse_index(std::string_view json, const std::filesystem::path& root, Index& into, std::string& error) {
    using mods::json::Value;
    Value doc;
    try {
        doc = mods::json::parse(std::string(json), kMaxIndexBytes);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (doc.type != Value::Object) { error = "index is not a JSON object"; return false; }
    uint32_t version = 0;
    if (!detail::whole(doc.get("format_version"), 0, 1e9, version) || version != 1) {
        error = "unsupported index format_version";
        return false;
    }
    const Value& list = doc.get("entries");
    if (list.type != Value::Array) { error = "index has no entries array"; return false; }

    size_t n = 0;
    for (const Value& e : list.array) {
        std::string at = "entry " + std::to_string(n++);
        Key key{};
        uint32_t scale = 0, dxgi = 0;
        uint64_t hash = 0;
        const char* bad = nullptr;
        if (e.type != Value::Object) bad = "not an object";
        else if (!detail::whole(e.get("format"), 0, 0xFFFF, key.format)) bad = "bad format";
        else if (!detail::whole(e.get("width"), 1, 16384, key.width)) bad = "bad width";
        else if (!detail::whole(e.get("height"), 1, 16384, key.height)) bad = "bad height";
        else if (e.get("hash").type != Value::String || !detail::hex16(e.get("hash").text, hash)) bad = "bad hash";
        else if (!detail::whole(e.get("scale"), 2, 32, scale) || !detail::valid_scale(scale)) bad = "bad scale";
        else if (e.get("file").type != Value::String || e.get("file").text.empty()) bad = "missing file";
        else if (e.object.contains("dxgi") && !detail::whole(e.get("dxgi"), 0, 1000, dxgi)) bad = "bad dxgi";
        std::filesystem::path rel;
        if (!bad) {
            rel = std::filesystem::path(std::u8string(e.get("file").text.begin(), e.get("file").text.end()));
            bool escapes = rel.is_absolute() || rel.has_root_name() || rel.has_root_directory();
            for (const auto& part : rel) if (part == "..") escapes = true;
            if (escapes) bad = "file leaves the package";
        }
        if (bad) {
            into.warnings.push_back(at + " skipped: " + bad);
            continue;
        }
        key.hash = hash;
        if (into.entries.contains(key)) {
            into.warnings.push_back(at + " skipped: texture already replaced by an earlier entry or pack (" + e.get("file").text + ")");
            continue;
        }
        into.entries.emplace(key, Entry{root / rel, scale, dxgi});
        // the optional archives: relative game paths under /vol/content, no "..", as `file`; a bad one is skipped
        // with a warning (the entry still replaces its texture, as one without archives does)
        if (!e.object.contains("archives")) continue;
        const Value& archives = e.get("archives");
        if (archives.type != Value::Array) { into.warnings.push_back(at + ": archives skipped: not an array"); continue; }
        std::vector<std::string> names;
        for (const Value& a : archives.array) {
            const char* badArchive = nullptr;
            std::string name;
            if (a.type != Value::String || a.text.empty()) badArchive = "not a path";
            else if (a.text[0] == '/' || a.text[0] == '\\' || a.text.find(':') != std::string::npos) badArchive = "not relative";
            else {
                std::string_view rest = a.text;
                for (size_t from = 0; from <= rest.size();) {
                    size_t to = rest.find_first_of("/\\", from);
                    if (to == std::string_view::npos) to = rest.size();
                    if (rest.substr(from, to - from) == "..") badArchive = "leaves the game's content";
                    from = to + 1;
                }
                name.resize(a.text.size());
                name.resize(normalize_game_path(a.text, name));
                if (!badArchive && name.empty()) badArchive = "not a path";
            }
            if (badArchive) { into.warnings.push_back(at + ": an archive skipped: " + badArchive); continue; }
            if (std::find(names.begin(), names.end(), name) != names.end()) continue;  // (listed twice)
            names.push_back(name);
            into.archives[std::move(name)].push_back(key);
        }
    }
    return true;
}

// The DXGI format a replacement of GX2 format `gx2Format` may use, or 0. The pair must be in the spec's table:
// BC1/BC2/BC3 -> BC7 of the same sRGB-ness, BC4/BC5 -> the same format and signedness, RGBA8 -> BC7 or RGBA8 of
// the same colour space. (GX2: UNORM 0x31.., SRGB 0x431.., SNORM 0x234/0x235.)
inline uint32_t allowed_dxgi(uint32_t gx2Format, uint32_t dxgi) {
    constexpr uint32_t BC7 = 98, BC7s = 99, BC4 = 80, BC4s = 81, BC5 = 83, BC5s = 84, RGBA8 = 28, RGBA8s = 29;
    switch (gx2Format) {
    case 0x31: case 0x32: case 0x33: return dxgi == BC7 ? dxgi : 0;
    case 0x431: case 0x432: case 0x433: return dxgi == BC7s ? dxgi : 0;
    case 0x34: return dxgi == BC4 ? dxgi : 0;
    case 0x234: return dxgi == BC4s ? dxgi : 0;
    case 0x35: return dxgi == BC5 ? dxgi : 0;
    case 0x235: return dxgi == BC5s ? dxgi : 0;
    case 0x1A: return dxgi == BC7 || dxgi == RGBA8 ? dxgi : 0;
    case 0x41A: return dxgi == BC7s || dxgi == RGBA8s ? dxgi : 0;
    default: return 0;
    }
}

struct DdsLevels {
    uint32_t width, height, dxgi;
    std::vector<std::span<const uint8_t>> levels;
};

namespace detail {
inline uint32_t rd32(std::span<const uint8_t> f, size_t o) {
    return uint32_t(f[o]) | uint32_t(f[o + 1]) << 8 | uint32_t(f[o + 2]) << 16 | uint32_t(f[o + 3]) << 24;
}
// bytes of one level of a DXGI format (block-compressed: 4x4 blocks), 0 for a format not in the table
inline uint64_t level_bytes(uint32_t dxgi, uint32_t w, uint32_t h) {
    switch (dxgi) {
    case 80: case 81: return uint64_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    case 83: case 84: case 98: case 99: return uint64_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    case 28: case 29: return uint64_t(w) * h * 4;
    default: return 0;
    }
}
} // namespace detail

// Checks a whole DDS file (magic, 124-byte header, DX10 extension, then the levels largest first) against the index
// entry `e` of texture `key`, and slices it into levels. `maxDim` is the device's largest image side.
inline bool check_dds(std::span<const uint8_t> file, const Key& key, const Entry& e, uint32_t maxDim, DdsLevels& out, std::string& error) {
    using detail::rd32;
    constexpr size_t kData = 4 + 124 + 20;
    auto fail = [&](const char* m) { error = m; return false; };
    if (file.size() < kData) return fail("file too short for a DDS header");
    if (rd32(file, 0) != 0x20534444u) return fail("not a DDS file");
    if (rd32(file, 4) != 124 || rd32(file, 76) != 32) return fail("bad DDS header size");
    if (rd32(file, 84) != 0x30315844u) return fail("DDS has no DX10 header");
    uint32_t height = rd32(file, 12), width = rd32(file, 16), mips = rd32(file, 28);
    uint32_t dxgi = rd32(file, 128), dimension = rd32(file, 132), misc = rd32(file, 136), arraySize = rd32(file, 140);
    if (dimension != 3 || (misc & 4) || arraySize != 1) return fail("DDS is not a single 2D image");
    if ((e.dxgi != 0 && dxgi != e.dxgi) || !allowed_dxgi(key.format, dxgi)) return fail("DDS format not allowed for this texture");
    if (!detail::valid_scale(e.scale)) return fail("bad scale");
    if (uint64_t(key.width) * e.scale != width || uint64_t(key.height) * e.scale != height) return fail("DDS size is not texture size times scale");
    if (width > maxDim || height > maxDim) return fail("DDS larger than the device limit");
    if (mips < 1 || mips > 15 || mips > 1 + std::bit_width(std::max(width, height)) - 1) return fail("bad mip count");
    DdsLevels r{width, height, dxgi, {}};
    uint64_t at = kData;
    for (uint32_t i = 0; i < mips; i++) {
        uint64_t size = detail::level_bytes(dxgi, std::max(1u, width >> i), std::max(1u, height >> i));
        if (at + size > file.size()) return fail("DDS truncated");
        r.levels.push_back(file.subspan(size_t(at), size_t(size)));
        at += size;
    }
    out = std::move(r);
    return true;
}

struct LodRange {
    float minLod, maxLod;
    bool operator==(const LodRange&) const = default;
};
// The sampler's LOD range for a replacement with `offset` more levels than the original: the same picture
// at every distance. Without a mip filter the sampler reads level 0 only, so nothing changes.
inline LodRange replacement_lods(float minLod, float maxLod, bool mipFilter, uint32_t offset) {
    if (!mipFilter) return {minLod, maxLod};
    return {minLod > 0 ? minLod + float(offset) : 0.f, maxLod + float(offset)};
}

// The levels a replacement of `scale` adds in front of the guest's: guest level 0 is replacement level log2(scale).
// 0 for anything that is not a valid scale (the replacement is never made for those).
inline uint32_t lod_offset(uint32_t scale) {
    return detail::valid_scale(scale) ? uint32_t(std::countr_zero(scale)) : 0;
}

// The replacement images' default VRAM budget from the device-local heap's VK_EXT_memory_budget figures (read once
// after the VR session has started, so the compositor and the game's own images are counted): half the heap's
// budget, but no more than is still free, and at least 1 GiB (a nearly full heap still gets a working set; the
// driver pages out before it fails, and a failed image creation releases replacements).
inline uint64_t default_budget(uint64_t heapBudget, uint64_t heapUsage) {
    constexpr uint64_t kMin = 1ull << 30;
    uint64_t free = heapBudget > heapUsage ? heapBudget - heapUsage : 0;
    return std::max(kMin, std::min(heapBudget / 2, free));
}

// The VRAM budget is a cap: a finished load whose image would take the replacements' bytes over it is made only after
// replacements not bound for kPinFrames frames have been released to make room (texture_pack_vk.cpp release_over,
// down to release_target); if that cannot make room, it is not made, the original keeps drawing, and the key is asked
// for again only when there is room (may_retry), or kPinFrames later to try a release once more. `budget` 0: none set
// yet, no cap. `need` is the load's level bytes (the image's own size, give or take its alignment).
inline bool admits(uint64_t held, uint64_t need, uint64_t budget) {
    return budget == 0 || (need <= budget && held <= budget - need);
}
// the held bytes to release down to so that `need` fits; 0 when nothing would help (the load alone is over the budget)
inline uint64_t release_target(uint64_t budget, uint64_t need) { return need < budget ? budget - need : 0; }
// a key refused for the budget at `refusedFrame`: asked for again when it fits now, or `pin` frames on (the
// replacements pinned at the refusal may be released by then); never earlier, so a texture drawn every frame does not
// read its DDS every frame only to be refused again
inline bool may_retry(uint64_t refusedFrame, uint64_t frame, uint64_t pin, bool fitsNow) {
    return fitsNow || frame >= refusedFrame + pin;
}

// Replacement images are made on the render thread at the frame's end; a burst of new textures must not cost a VR
// frame (11.1 ms at 90 Hz). Per frame at most kCreateBytesPerFrame of level data and kCreateNsPerFrame of the render
// thread's time, whichever comes first, checked before each next replacement; the first of a frame is always made
// (one larger than the byte limit, 4096 px and up, then goes alone and is never starved).
constexpr uint64_t kCreateBytesPerFrame = 16ull << 20;
constexpr uint64_t kCreateNsPerFrame = 1000000;
inline bool may_start_another(uint64_t bytesSoFar, uint64_t nsSoFar, uint64_t nextBytes, uint32_t made) {
    if (made == 0) return true;
    return nsSoFar < kCreateNsPerFrame && bytesSoFar + nextBytes <= kCreateBytesPerFrame;
}

// The upload thread (texture_pack_vk.cpp, where the device has both): replacement images are made, filled and
// submitted on their own thread and a queue family of the device's own that copies but does not draw, so the render
// thread only attaches finished ones. The limit above then does not apply; the upload thread's bound is its staging
// ring (StagingRing below: at most kStagingRingBytes of level data in flight; a larger load goes alone).
constexpr uint64_t kStagingRingBytes = 64ull << 20;

// Vulkan's VkQueueFlagBits, as far as transfer_family needs them (this header stays free of vulkan.h)
constexpr uint32_t kQueueGraphics = 0x1, kQueueCompute = 0x2, kQueueTransfer = 0x4;
// The queue family the upload thread copies on: one with no graphics (the graphics family's own queue is the
// renderer's), preferring a copy-only one (no compute: the copy engine) over a compute family (which copies too);
// ~0u when the device has none. `flags[i]`, `counts[i]`: family i's VkQueueFamilyProperties queueFlags and queueCount.
inline uint32_t transfer_family(std::span<const uint32_t> flags, std::span<const uint32_t> counts, uint32_t graphicsFamily) {
    uint32_t compute = ~0u;
    for (uint32_t i = 0; i < flags.size() && i < counts.size(); ++i) {
        if (i == graphicsFamily || !counts[i] || (flags[i] & kQueueGraphics)) continue;
        if (flags[i] & kQueueCompute) { if (compute == ~0u) compute = i; continue; }
        if (flags[i] & kQueueTransfer) return i;
    }
    return compute;
}

// The upload thread's staging buffer, used as a ring: each batch's level data is placed after the previous batch's,
// and freed when that batch's copies have finished (in submission order, as the timeline semaphore's values come).
// Positions only grow (the buffer offset is a position modulo the capacity), so a batch's end is its mark.
// alloc: `size` bytes at a multiple of `align` (a power of two that divides the capacity) that never wrap past the
// buffer's end; the offset in the buffer, or kFull when the live data leaves no room (or size is 0 or over the
// capacity: such a load gets a buffer of its own). An empty ring starts again at a capacity boundary, so any load of
// at most the capacity fits it.
struct StagingRing {
    static constexpr uint64_t kFull = ~0ull;
    uint64_t capacity = 0, head = 0, tail = 0;
    uint64_t alloc(uint64_t size, uint64_t align) {
        if (!size || size > capacity || !align || (align & (align - 1)) || capacity % align) return kFull;
        if (head == tail) head = tail = (head + capacity - 1) / capacity * capacity;
        uint64_t at = (head + align - 1) & ~(align - 1);
        if (at % capacity + size > capacity) at = (at / capacity + 1) * capacity;   // to the buffer's start
        if (at + size - tail > capacity) return kFull;
        head = at + size;
        return at % capacity;
    }
    uint64_t mark() const { return head; }
    // the oldest live batch finished: everything up to its mark is free
    void release_to(uint64_t m) { tail = std::max(tail, std::min(m, head)); }
    bool empty() const { return head == tail; }
    uint64_t used() const { return head - tail; }
};

// What decides whether a draw binds the replacement: the guest surface must still be what the key was made from
// (keyCurrent), and the draw must use the texture as plain sampled colour.
struct BindFacts {
    bool hasReplacement, keyCurrent, gpuWritten, boundAsTarget, needsGuestLevels, artisticMips;
};
inline bool may_bind(const BindFacts& f) {
    return f.hasReplacement && f.keyCurrent && !f.gpuWritten && !f.boundAsTarget && !f.needsGuestLevels && !f.artisticMips;
}

} // namespace gfx::texture_pack
