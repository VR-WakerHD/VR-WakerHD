// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// A shader's multiview variant: multiview_glsl.h.
#include "multiview_glsl.h"

#include <cctype>
#include <vector>

namespace gfxvk::vk::multiview {
namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// the first occurrence of word (a whole identifier) at or after from; npos if none
size_t find_word(const std::string& s, const std::string& word, size_t from = 0) {
    for (size_t at = s.find(word, from); at != std::string::npos; at = s.find(word, at + 1)) {
        const bool before = at == 0 || !ident_char(s[at - 1]);
        const bool after = at + word.size() >= s.size() || !ident_char(s[at + word.size()]);
        if (before && after) return at;
    }
    return std::string::npos;
}

bool fail(std::string* why, const std::string& what) {
    if (why) *why = what;
    return false;
}

// a guest uniform buffer's block: `uniform uniformBlockVS<i> { vec4 uf_blockVS<i>[n]; };`
bool rewrite_block(std::string& s, const char* stage, uint32_t i, std::string* why) {
    const std::string block = std::string("uniformBlock") + stage + std::to_string(i);
    const std::string var = std::string("uf_block") + stage + std::to_string(i);
    const size_t decl = find_word(s, block);
    if (decl == std::string::npos) return fail(why, "no declaration of " + block);
    const size_t member = find_word(s, var, decl);
    const size_t close = s.find("};", decl);
    if (member == std::string::npos || close == std::string::npos || member > close)
        return fail(why, "unexpected layout of " + block);
    s.insert(member + var.size(), "_mv");
    const size_t closeNow = s.find("};", member);
    const std::string tail = "} " + var + "_blk[2];\n#define " + var + " " + var + "_blk[gl_ViewIndex]." + var + "_mv\n";
    s.replace(closeNow, 2, tail);
    return true;
}

// the support block: `layout(...) uniform ufBlock { uniform <type> <name>[...]; ... };`, its members referred to by
// name in the code
bool rewrite_support_block(std::string& s, std::string* why) {
    const size_t decl = find_word(s, "ufBlock");
    if (decl == std::string::npos) return fail(why, "no support block");
    const size_t open = s.find('{', decl);
    const size_t close = open == std::string::npos ? std::string::npos : s.find("};", open);
    if (close == std::string::npos) return fail(why, "unexpected layout of the support block");
    std::string body = s.substr(open + 1, close - open - 1);
    std::vector<std::string> names;
    std::string renamed;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find(';', pos);
        if (end == std::string::npos) {
            renamed += body.substr(pos);
            break;
        }
        std::string decl1 = body.substr(pos, end - pos);
        // the member's name: the last identifier before an array size, if any
        size_t limit = decl1.find('[');
        if (limit == std::string::npos) limit = decl1.size();
        size_t nameEnd = limit;
        while (nameEnd > 0 && !ident_char(decl1[nameEnd - 1])) --nameEnd;
        size_t nameStart = nameEnd;
        while (nameStart > 0 && ident_char(decl1[nameStart - 1])) --nameStart;
        if (nameStart == nameEnd) return fail(why, "a support block member without a name");
        const std::string name = decl1.substr(nameStart, nameEnd - nameStart);
        names.push_back(name);
        decl1.insert(nameEnd, "_mv");
        renamed += decl1 + ";";
        pos = end + 1;
    }
    if (names.empty()) return fail(why, "an empty support block");
    std::string tail = "} ufBlock_mv[2];\n";
    for (const auto& n : names) tail += "#define " + n + " ufBlock_mv[gl_ViewIndex]." + n + "_mv\n";
    s.replace(open + 1, close - open + 1, renamed + tail);
    return true;
}

// an eye-specific texture: `TEXTURE_LAYOUT(...) uniform sampler2D textureUnitPS<u>;`
bool rewrite_texture(std::string& s, const char* stage, uint32_t u, std::string* why) {
    const std::string name = std::string("textureUnit") + stage + std::to_string(u);
    const size_t at = s.find(" " + name + ";");
    if (at == std::string::npos) return fail(why, "no declaration of " + name);
    s.replace(at + 1, name.size() + 1, name + "_mv[2];\n#define " + name + " " + name + "_mv[gl_ViewIndex]");
    return true;
}

}  // namespace

bool rewrite(const std::string& glsl, const Variant& v, std::string& out, std::string* why) {
    out = glsl;
    const char* stage = v.vertex ? "VS" : "PS";
    // the extension, after #version (an extension directive must precede the code)
    const size_t version = out.find("#version");
    size_t line = version == std::string::npos ? 0 : out.find('\n', version);
    line = line == std::string::npos ? out.size() : line + 1;
    if (version == std::string::npos) line = 0;
    out.insert(line, "#extension GL_EXT_multiview : require\n");
    for (uint32_t i = 0; i < 32; ++i)
        if ((v.blocks >> i & 1) && !rewrite_block(out, stage, i, why)) return false;
    if (v.supportBlock && !rewrite_support_block(out, why)) return false;
    for (uint32_t u = 0; u < 32; ++u)
        if ((v.textures >> u & 1) && !rewrite_texture(out, stage, u, why)) return false;
    return true;
}

}  // namespace gfxvk::vk::multiview
