// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A shader's motion variant: motion_glsl.h.
#include "motion_glsl.h"

#include <cctype>
#include <vector>

namespace gfxvk::vk::motion {
namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

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

// the array of each block: its size and how a use picks the element
struct Arrays {
    uint32_t count;
    std::string index;
};

// a guest uniform buffer's block: `uniform uniformBlockVS<i> { vec4 uf_blockVS<i>[n]; };`
bool rewrite_block(std::string& s, uint32_t i, const Arrays& a, std::string* why) {
    const std::string block = "uniformBlockVS" + std::to_string(i);
    const std::string var = "uf_blockVS" + std::to_string(i);
    const size_t decl = find_word(s, block);
    if (decl == std::string::npos) return fail(why, "no declaration of " + block);
    const size_t member = find_word(s, var, decl);
    const size_t close = s.find("};", decl);
    if (member == std::string::npos || close == std::string::npos || member > close)
        return fail(why, "unexpected layout of " + block);
    s.insert(member + var.size(), "_mo");
    const size_t closeNow = s.find("};", member);
    s.replace(closeNow, 2, "} " + var + "_blk[" + std::to_string(a.count) + "];\n#define " + var + " " + var + "_blk[" +
                               a.index + "]." + var + "_mo\n");
    return true;
}

// the support block: `layout(...) uniform ufBlock { uniform <type> <name>[...]; ... };`
bool rewrite_support_block(std::string& s, const Arrays& a, std::string* why) {
    const size_t decl = find_word(s, "ufBlock");
    if (decl == std::string::npos) return fail(why, "no support block");
    const size_t open = s.find('{', decl);
    const size_t close = open == std::string::npos ? std::string::npos : s.find("};", open);
    if (close == std::string::npos) return fail(why, "unexpected layout of the support block");
    const std::string body = s.substr(open + 1, close - open - 1);
    std::vector<std::string> names;
    std::string renamed;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t end = body.find(';', pos);
        if (end == std::string::npos) {
            renamed += body.substr(pos);
            break;
        }
        std::string member = body.substr(pos, end - pos);
        size_t limit = member.find('[');
        if (limit == std::string::npos) limit = member.size();
        size_t nameEnd = limit;
        while (nameEnd > 0 && !ident_char(member[nameEnd - 1])) --nameEnd;
        size_t nameStart = nameEnd;
        while (nameStart > 0 && ident_char(member[nameStart - 1])) --nameStart;
        if (nameStart == nameEnd) return fail(why, "a support block member without a name");
        names.push_back(member.substr(nameStart, nameEnd - nameStart));
        member.insert(nameEnd, "_mo");
        renamed += member + ";";
        pos = end + 1;
    }
    if (names.empty()) return fail(why, "an empty support block");
    std::string tail = "} ufBlock_mo[" + std::to_string(a.count) + "];\n";
    for (const auto& n : names) tail += "#define " + n + " ufBlock_mo[" + a.index + "]." + n + "_mo\n";
    s.replace(open + 1, close - open + 1, renamed + tail);
    return true;
}

// every vertex input: `ATTR_LAYOUT(<n>) in <type> <name>;` -> the current one renamed, the previous one at n + 16
bool rewrite_inputs(std::string& s, std::string* why) {
    const std::string tag = "ATTR_LAYOUT(";
    size_t at = 0;
    int inputs = 0;
    while ((at = s.find(tag, at)) != std::string::npos) {
        // the macro's own #define lines are not inputs
        const size_t lineStart = s.rfind('\n', at) == std::string::npos ? 0 : s.rfind('\n', at) + 1;
        if (s.compare(lineStart, 7, "#define") == 0) {
            at += tag.size();
            continue;
        }
        const size_t numEnd = s.find(')', at);
        const size_t semi = s.find(';', at);
        if (numEnd == std::string::npos || semi == std::string::npos) return fail(why, "unexpected vertex input");
        const uint32_t location = uint32_t(std::stoul(s.substr(at + tag.size(), numEnd - at - tag.size())));
        if (location >= kPrevAttributeOffset) return fail(why, "a vertex input at location 16 or above");
        const std::string decl = s.substr(numEnd + 1, semi - numEnd - 1);  // " in uvec4 attrDataSem0"
        size_t nameEnd = decl.size();
        while (nameEnd > 0 && !ident_char(decl[nameEnd - 1])) --nameEnd;
        size_t nameStart = nameEnd;
        while (nameStart > 0 && ident_char(decl[nameStart - 1])) --nameStart;
        if (nameStart == nameEnd || decl.find(" in ") == std::string::npos) return fail(why, "unexpected vertex input");
        const std::string name = decl.substr(nameStart, nameEnd - nameStart);
        const std::string head = decl.substr(0, nameStart);  // " in uvec4 "
        const std::string repl = tag + std::to_string(location) + ")" + head + name + "_cur;\n" + tag +
                                 std::to_string(location + kPrevAttributeOffset) + ")" + head + name + "_prev;\n" +
                                 "#define " + name + " (wwhd_mframe == 1 ? " + name + "_prev : " + name + "_cur)";
        s.replace(at, semi + 1 - at, repl);  // (the declaration's semicolon too: the macro line must not end in one)
        at += repl.size();
        ++inputs;
    }
    (void)inputs;  // a shader without inputs (positions from gl_VertexIndex) moves only by its uniforms
    return true;
}

// the translator's `void main()` renamed; false if there is not exactly one
bool rename_main(std::string& s, std::string* why) {
    const size_t m = s.find("void main()");
    if (m == std::string::npos || s.find("void main()", m + 1) != std::string::npos) return fail(why, "no single main");
    s.replace(m, 11, "void wwhd_main()");
    return true;
}

// after #version and the #extension lines (globals and directives the variant adds)
size_t after_header(const std::string& s) {
    size_t line = 0;
    const size_t version = s.find("#version");
    if (version != std::string::npos) line = s.find('\n', version) + 1;
    while (s.compare(line, 10, "#extension") == 0) {
        const size_t next = s.find('\n', line);
        if (next == std::string::npos) return s.size();
        line = next + 1;
    }
    return line;
}

}  // namespace

bool rewrite(const std::string& glsl, const Variant& v, std::string& out, std::string* why) {
    out = glsl;
    if (v.views != 1 && v.views != 2) return fail(why, "views must be 1 or 2");
    if (!rename_main(out, why)) return false;
    const std::string prev = "layout(location = " + std::to_string(kPrevClipLocation) + ") ";
    const std::string cur = "layout(location = " + std::to_string(kCurClipLocation) + ") ";
    if (v.vertex) {
        const Arrays a = v.views == 2 ? Arrays{4, "wwhd_mframe * 2 + gl_ViewIndex"} : Arrays{2, "wwhd_mframe"};
        std::string head;
        if (v.views == 2 && out.find("GL_EXT_multiview") == std::string::npos) head += "#extension GL_EXT_multiview : require\n";
        head += "int wwhd_mframe = 0;  // 1 while the translator's main runs on the previous frame's data\n";
        out.insert(after_header(out), head);
        for (uint32_t i = 0; i < 32; ++i)
            if ((v.blocks >> i & 1) && !rewrite_block(out, i, a, why)) return false;
        if (v.supportBlock && !rewrite_support_block(out, a, why)) return false;
        if (!rewrite_inputs(out, why)) return false;
        out += "\n" + prev + "out vec4 wwhd_prevClip;\n" + cur + "out vec4 wwhd_curClip;\n"
               "void main()\n{\n"
               "wwhd_mframe = 1;\nwwhd_main();\nwwhd_prevClip = gl_Position;\n"
               "wwhd_mframe = 0;\nwwhd_main();\nwwhd_curClip = gl_Position;\n}\n";
        return true;
    }
    out += "\n" + prev + "in vec4 wwhd_prevClip;\n" + cur + "in vec4 wwhd_curClip;\n"
           "layout(location = " + std::to_string(v.motionLocation) + ") out vec2 wwhd_motion;\n"
           "void main()\n{\n"
           "wwhd_main();\n"
           "wwhd_motion = (wwhd_prevClip.xy / wwhd_prevClip.w - wwhd_curClip.xy / wwhd_curClip.w) * 0.5;\n}\n";
    return true;
}

std::string motion_only_pixel() {
    return "#version 430\n"
           "layout(location = " + std::to_string(kPrevClipLocation) + ") in vec4 wwhd_prevClip;\n"
           "layout(location = " + std::to_string(kCurClipLocation) + ") in vec4 wwhd_curClip;\n"
           "layout(location = 0) out vec2 wwhd_motion;\n"
           "void main()\n{\n"
           "vec2 m = (wwhd_prevClip.xy / wwhd_prevClip.w - wwhd_curClip.xy / wwhd_curClip.w) * 0.5;\n"
           // implausible motion is unknown motion: the previous position behind the camera, not a number, or more
           // than a quarter of the picture in one frame (geometry the game rebuilds around the camera, whose vertex
           // n of the previous frame is another point: the horizon's sea strip)
           "if (wwhd_prevClip.w <= 0.0 || any(isnan(m)) || any(isinf(m)) || dot(m, m) > " +
           std::to_string(kMaxMotion * kMaxMotion) + ") m = vec2(0.0);\n"
           "wwhd_motion = m;\n}\n";
}

}  // namespace gfxvk::vk::motion
