#include <array>
#include <algorithm>
// SPDX-License-Identifier: MPL-2.0
// The GameCube player model converter (runtime/src/gcmodel) from the command line, for checks on a computer:
//   gcmodel_tool convert Link.arc permanent_3d.pack Link.szs   the converted Link.szs (the report on stdout)
//   gcmodel_tool synth permanent_3d.pack Link.arc               a synthetic test pack for headless checks: HD Link's
//       own skeleton and material names (read from the local game, nothing of it stored), a box per shape on its
//       bone, a flat colour per texture. Written where the caller says (a run folder); never part of the repository.
// After GreenNaugahyde/ZeldaWWHDRecompAndroid's tools/gcmodels/host/host_main.cpp (MPL-2.0).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "gcmodel/gcm_io.h"
#include "gcmodel/gcmodel.h"
#include "gcmodel_synth.h"

static bool load(const char* p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static bool save(const char* p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    return f && f.write((const char*)d.data(), (std::streamsize)d.size());
}

static int usage() {
    fprintf(stderr, "usage: gcmodel_tool convert Link.arc permanent_3d.pack Link.szs\n"
                    "       gcmodel_tool synth permanent_3d.pack Link.arc\n");
    return 2;
}

static int synth(const char* packPath, const char* outPath) {
    std::vector<uint8_t> pack;
    if (!load(packPath, pack)) {
        fprintf(stderr, "ERROR: cannot read %s\n", packPath);
        return 1;
    }
    try {
        const auto szs = gcmodel::link_szs(pack);
        if (szs.empty()) throw gcmodel::Error("no Link.szs in the pack");
        const auto files = gcmodel::sarc_read(gcmodel::yaz0_decode(szs));
        const auto parts = gcmodel::describe(files.at("Link.bfres"));
        const gcmodel::PartInfo* cl = nullptr;
        for (auto& p : parts)
            if (p.name == "cl") cl = &p;
        if (!cl) throw gcmodel::Error("Link.bfres without cl");
        gcmsynth::J3DModel m;
        for (auto& b : cl->bones) {
            gcmsynth::J3DJoint j;
            j.name = b.name, j.parent = b.parent;
            for (int q = 0; q < 3; q++) j.t[q] = b.world[q] - (b.parent >= 0 ? cl->bones[b.parent].world[q] : 0);
            m.joints.push_back(j);
            m.draw.push_back({{(int)m.joints.size() - 1, 1.0}});
            m.weighted.push_back(false);
        }
        std::map<std::string, int> texIndex;
        for (size_t i = 0; i < cl->materials.size(); i++) {
            m.materials.push_back(gcmodel::gc_material(cl->materials[i]));
            int t = -1;
            if (!cl->materialTextures[i].empty()) {
                const std::string& name = cl->materialTextures[i][0];
                auto it = texIndex.find(name);
                if (it == texIndex.end()) {
                    gcmsynth::J3DTexture tex;
                    tex.name = name;
                    tex.rgba.resize(8 * 8 * 4);
                    static const uint8_t colours[][3] = {{255, 0, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0}, {255, 128, 0}};
                    const uint8_t* c = colours[texIndex.size() % 5];
                    for (int k = 0; k < 64; k++) memcpy(&tex.rgba[k * 4], c, 3), tex.rgba[k * 4 + 3] = 255;
                    it = texIndex.emplace(name, (int)m.textures.size()).first;
                    m.textures.push_back(tex);
                }
                t = it->second;
            }
            m.materialTexture.push_back(t);
        }
        m.uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const double h = 3.0;
        for (auto& sh : cl->shapes) {
            gcmsynth::J3DShape s;
            s.material = sh.material;
            const int base = (int)m.pos.size();
            for (int k = 0; k < 8; k++) m.pos.push_back({k & 1 ? h : -h, k & 2 ? h : -h, k & 4 ? h : -h});
            static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
            for (auto& f : faces) {
                auto v = [&](int k, int uv) { return gcmsynth::J3DVertex{base + f[k], uv, sh.bone}; };
                s.tris.push_back({v(0, 0), v(2, 2), v(1, 1)});
                s.tris.push_back({v(0, 0), v(3, 3), v(2, 2)});
            }
            m.shapes.push_back(s);
        }
        if (!save(outPath, gcmsynth::rarc({{"cl.bdl", gcmsynth::j3d(m)}}))) throw gcmodel::Error("cannot write the output");
        printf("synthetic Link.arc: %zu joints, %zu shapes, %zu materials, %zu textures\n", m.joints.size(), m.shapes.size(),
               m.materials.size(), m.textures.size());
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
    return 0;
}

// (VR, 2026-11-15) every model of Link.bfres with its bones (bind pose, world translation), to read an item's geometry
static int parts(const char* packPath) {
    std::vector<uint8_t> pack;
    if (!load(packPath, pack)) return 1;
    const auto szs = gcmodel::link_szs(pack);
    const auto files = gcmodel::sarc_read(gcmodel::yaz0_decode(szs));
    for (auto& p : gcmodel::describe(files.at("Link.bfres"))) {
        printf("model %s: %zu bones, %zu shapes\n", p.name.c_str(), p.bones.size(), p.shapes.size());
        for (size_t i = 0; i < p.bones.size(); i++)
            printf("  bone %zu %-16s parent %2d  world %9.3f %9.3f %9.3f\n", i, p.bones[i].name.c_str(), p.bones[i].parent,
                   p.bones[i].world[0], p.bones[i].world[1], p.bones[i].world[2]);
        for (auto& sh : p.shapes) printf("  shape %-20s bone %d skin %d\n", sh.name.c_str(), sh.bone, sh.skin);
    }
    return 0;
}
namespace gcmodel {
std::vector<std::pair<std::string, std::vector<std::array<double, 3>>>> shape_points(const std::vector<uint8_t>&, const std::string&);
}
static int extents(const char* packPath, const char* model) {
    std::vector<uint8_t> pack;
    if (!load(packPath, pack)) return 1;
    const auto files = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::link_szs(pack)));
    for (auto& [sn, pts] : gcmodel::shape_points(files.at("Link.bfres"), model)) {
        double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30}, sum[3] = {0, 0, 0};
        for (auto& p : pts)
            for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]), sum[k] += p[k];
        printf("shape %s: %zu points; x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f; centre %.2f %.2f %.2f\n", sn.c_str(), pts.size(),
               lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], sum[0] / pts.size(), sum[1] / pts.size(), sum[2] / pts.size());
        // the points at the far +X end (the muzzle) and their centre
        double cx = 0, cy = 0, cz = 0; int n = 0;
        for (auto& p : pts) if (p[0] > hi[0] - 0.5) cx += p[0], cy += p[1], cz += p[2], ++n;
        if (n) printf("   +X end (within 0.5): %d points, centre %.2f %.2f %.2f\n", n, cx / n, cy / n, cz / n);
        // (WWHD_POINTS=y0,y1: every point with Y in that range)
        if (const char* pv = std::getenv("WWHD_POINTS")) {
            double a0 = 0, a1 = 0;
            std::sscanf(pv, "%lf,%lf", &a0, &a1);
            for (auto& p : pts)
                if (p[1] >= a0 && p[1] < a1) printf("   point %7.2f %7.2f %7.2f\n", p[0], p[1], p[2]);
        }
        // (WWHD_SLICE=y0,y1,step: the shape's cross-section in bands of Y: its X and Z ranges)
        if (const char* sl = std::getenv("WWHD_SLICE")) {
            double a0 = -14, a1 = 14, st = 2;
            std::sscanf(sl, "%lf,%lf,%lf", &a0, &a1, &st);
            for (double y0 = a0; y0 < a1; y0 += st) {
                double xl = 1e30, xh = -1e30, zl = 1e30, zh = -1e30;
                int m = 0;
                for (auto& p : pts)
                    if (p[1] >= y0 && p[1] < y0 + st)
                        xl = std::min(xl, p[0]), xh = std::max(xh, p[0]), zl = std::min(zl, p[2]), zh = std::max(zh, p[2]), ++m;
                if (m) printf("   y %6.2f..%6.2f: %3d points, x %6.2f..%6.2f  z %6.2f..%6.2f\n", y0, y0 + st, m, xl, xh, zl, zh);
            }
        }
    }
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "extents")) return extents(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "parts")) return parts(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "synth")) return synth(argv[2], argv[3]);
    if (argc != 5 || strcmp(argv[1], "convert")) return usage();
    std::vector<uint8_t> arc, pack, out;
    if (!load(argv[2], arc) || !load(argv[3], pack)) {
        fprintf(stderr, "ERROR: cannot read %s or %s\n", argv[2], argv[3]);
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = gcmodel::convert(arc, pack, out, [](int pc, const std::string& s) { fprintf(stderr, "\r%3d%% %-40s", pc, s.c_str()); });
    fprintf(stderr, "\n");
    printf("%s", r.report.c_str());
    if (!r.error.empty()) {
        fprintf(stderr, "ERROR: %s\n", r.error.c_str());
        return 1;
    }
    if (!save(argv[4], out)) {
        fprintf(stderr, "ERROR: cannot write %s\n", argv[4]);
        return 1;
    }
    printf("written %zu bytes in %.1f s\n", out.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
