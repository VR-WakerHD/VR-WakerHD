// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The motion variant's GLSL (runtime/src/gfx/vulkan/motion_glsl.h) on shaders laid out as the translator writes them:
// blocks per frame (and per view), vertex inputs doubled at location + 16, the translator's main run twice, the pixel
// shader's motion output, and refusals of sources not laid out that way. Compiling the variants of the game's shaders is
// the game's own check (WWHD_VK_MOTION_VALIDATE=1).
#include "gfx/vulkan/motion_glsl.cpp"  // (the renderer's sources build with the Cemu shim; this one needs nothing)
#include <cstdio>
#include <string>

using namespace gfxvk::vk::motion;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static size_t count(const std::string& s, const char* part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) ++n;
    return n;
}

static const char* kVertex = R"(#version 430
#extension GL_ARB_texture_gather : enable
#extension GL_ARB_separate_shader_objects : enable
#ifdef VULKAN
#define ATTR_LAYOUT(__location) layout(location = __location)
#define UNIFORM_BUFFER_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(set = __vkSet, binding = __vkLocation, std140)
#define SET_POSITION(_v) gl_Position = _v; gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0
#else
#define ATTR_LAYOUT(__location) layout(location = __location)
#define SET_POSITION(_v) gl_Position = _v
#endif
#ifdef VULKAN
layout(set = 0, binding = 0) uniform ufBlock
{
uniform ivec4 uf_remappedVS[30];
uniform vec4 uf_windowSpaceToClipSpaceTransform;
};
#else
uniform ivec4 uf_remappedVS[30];
#endif
UNIFORM_BUFFER_LAYOUT(1, 0, 1) uniform uniformBlockVS1
{
vec4 uf_blockVS1[4096];
};
ATTR_LAYOUT(0) in uvec4 attrDataSem0;
ATTR_LAYOUT(2) in uvec4 attrDataSem6;
out gl_PerVertex
{
	vec4 gl_Position;
};
layout(location = 0) out vec4 passParameterSem2;
void main()
{
vec4 R0f = intBitsToFloat(ivec4(attrDataSem0)) + uf_blockVS1[0] + intBitsToFloat(uf_remappedVS[1]);
passParameterSem2 = vec4(attrDataSem6);
SET_POSITION(R0f);
}
)";

static const char* kPixel = R"(#version 430
#extension GL_ARB_texture_gather : enable
layout(location = 0) in vec4 passParameterSem2;
layout(location = 0) out vec4 passPixelColor0;
layout(location = 1) out vec4 passPixelColor1;
void main()
{
if (passParameterSem2.w < 0.5) discard;
passPixelColor0 = passParameterSem2;
passPixelColor1 = passParameterSem2;
}
)";

int main() {
    // vertex, one view
    {
        Variant v;
        v.vertex = true;
        v.blocks = 1u << 1;
        v.supportBlock = true;
        std::string out, why;
        CHECK(rewrite(kVertex, v, out, &why));
        CHECK(why.empty());
        // the frame selector after the header, before any declaration that uses it
        CHECK(out.find("int wwhd_mframe = 0;") < out.find("uniform ufBlock"));
        CHECK(out.find("#extension GL_ARB_separate_shader_objects") < out.find("int wwhd_mframe"));
        // blocks: arrays of two, indexed by the frame
        CHECK(has(out, "} uf_blockVS1_blk[2];"));
        CHECK(has(out, "#define uf_blockVS1 uf_blockVS1_blk[wwhd_mframe].uf_blockVS1_mo"));
        CHECK(has(out, "} ufBlock_mo[2];"));
        CHECK(has(out, "#define uf_remappedVS ufBlock_mo[wwhd_mframe].uf_remappedVS_mo"));
        CHECK(has(out, "#define uf_windowSpaceToClipSpaceTransform ufBlock_mo[wwhd_mframe].uf_windowSpaceToClipSpaceTransform_mo"));
        // inputs: current renamed, previous at + 16, the name a choice between them; the macro lines left alone
        CHECK(has(out, "ATTR_LAYOUT(0) in uvec4 attrDataSem0_cur;"));
        CHECK(has(out, "ATTR_LAYOUT(16) in uvec4 attrDataSem0_prev;"));
        CHECK(has(out, "ATTR_LAYOUT(18) in uvec4 attrDataSem6_prev;"));
        CHECK(has(out, "#define attrDataSem6 (wwhd_mframe == 1 ? attrDataSem6_prev : attrDataSem6_cur)"));
        CHECK(count(out, "#define ATTR_LAYOUT(__location)") == 2);
        CHECK(!has(out, "attrDataSem0_cur);") && !has(out, "attrDataSem6_cur);"));  // no semicolon after a macro
        // the translator's code untouched, its main renamed and run twice, previous frame first
        CHECK(has(out, "vec4 R0f = intBitsToFloat(ivec4(attrDataSem0)) + uf_blockVS1[0] + intBitsToFloat(uf_remappedVS[1]);"));
        CHECK(count(out, "void main()") == 1 && has(out, "void wwhd_main()"));
        const size_t prevRun = out.find("wwhd_mframe = 1;\nwwhd_main();\nwwhd_prevClip = gl_Position;");
        const size_t curRun = out.find("wwhd_mframe = 0;\nwwhd_main();\nwwhd_curClip = gl_Position;");
        CHECK(prevRun != std::string::npos && curRun != std::string::npos && prevRun < curRun);
        CHECK(has(out, "layout(location = 30) out vec4 wwhd_prevClip;"));
        CHECK(has(out, "layout(location = 31) out vec4 wwhd_curClip;"));
        CHECK(!has(out, "GL_EXT_multiview"));
    }
    // vertex, two views (a multiview draw): four of each block, frame and view
    {
        Variant v;
        v.vertex = true;
        v.views = 2;
        v.blocks = 1u << 1;
        v.supportBlock = true;
        std::string out;
        CHECK(rewrite(kVertex, v, out));
        CHECK(has(out, "#extension GL_EXT_multiview : require"));
        CHECK(has(out, "} uf_blockVS1_blk[4];"));
        CHECK(has(out, "uf_blockVS1_blk[wwhd_mframe * 2 + gl_ViewIndex]"));
        CHECK(has(out, "ufBlock_mo[wwhd_mframe * 2 + gl_ViewIndex]"));
        // the multiview extension already there (the multiview variant first): not added twice
        std::string again;
        CHECK(rewrite("#version 430\n#extension GL_EXT_multiview : require\n" + std::string(kVertex).substr(13), v, again));
        CHECK(count(again, "GL_EXT_multiview") == 1);
    }
    // pixel: the motion output after its colours; previous minus current, halved; the original main first
    {
        Variant v;
        v.motionLocation = 2;
        std::string out;
        CHECK(rewrite(kPixel, v, out));
        CHECK(has(out, "layout(location = 2) out vec2 wwhd_motion;"));
        CHECK(has(out, "layout(location = 30) in vec4 wwhd_prevClip;"));
        CHECK(has(out, "layout(location = 31) in vec4 wwhd_curClip;"));
        CHECK(out.find("wwhd_main();\nwwhd_motion = ") != std::string::npos);
        CHECK(has(out, "(wwhd_prevClip.xy / wwhd_prevClip.w - wwhd_curClip.xy / wwhd_curClip.w) * 0.5"));
        CHECK(has(out, "if (passParameterSem2.w < 0.5) discard;"));  // a discarded pixel writes no motion
    }
    // refusals
    {
        Variant v;
        v.vertex = true;
        std::string out, why;
        CHECK(!rewrite("#version 430\nvoid other() {}\n", v, out, &why) && !why.empty());           // no main
        CHECK(!rewrite("#version 430\nvoid main() {}\nvoid main() {}\n", v, out, &why));              // two mains
        v.blocks = 1u << 3;
        CHECK(!rewrite(kVertex, v, out, &why) && why.find("uniformBlockVS3") != std::string::npos);  // missing block
        v.blocks = 0;
        CHECK(!rewrite("#version 430\nATTR_LAYOUT(16) in uvec4 a;\nvoid main() {}\n", v, out, &why));  // no room at + 16
        v.views = 3;
        CHECK(!rewrite(kVertex, v, out, &why));
    }
    // the motion pass's own pixel shader: both positions in, the motion out at location 0, nothing else
    {
        const std::string ps = motion_only_pixel();
        CHECK(has(ps, "#version 430\n"));
        CHECK(has(ps, "layout(location = 30) in vec4 wwhd_prevClip;"));
        CHECK(has(ps, "layout(location = 31) in vec4 wwhd_curClip;"));
        CHECK(has(ps, "layout(location = 0) out vec2 wwhd_motion;"));
        CHECK(has(ps, "(wwhd_prevClip.xy / wwhd_prevClip.w - wwhd_curClip.xy / wwhd_curClip.w) * 0.5"));
        CHECK(count(ps, "void main()") == 1 && !has(ps, "uniform") && !has(ps, "discard"));
        // implausible motion written as zero (behind the camera, not a number, past a quarter of the picture)
        CHECK(has(ps, "wwhd_prevClip.w <= 0.0") && has(ps, "isnan(m)") && has(ps, "dot(m, m) > 0.0625"));
        CHECK(has(ps, "m = vec2(0.0);") && has(ps, "wwhd_motion = m;"));
    }
    if (failures) {
        std::fprintf(stderr, "motion_glsl_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("motion_glsl_test: ok\n");
    return 0;
}
