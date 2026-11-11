// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// scene_stats.h: the scene's shape for docs/VR_SCENE_THREADS.md (stage S0). Render thread only; draw threads (the sun
// atlas's) are not counted.
#include "scene_stats.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend.h"
#include "draw_threads.h"
#include "runtime.h"

namespace gfxvk::scene_stats {
namespace {
struct Pass {
    uint32_t draws = 0, layoutChanges = 0, eye1Flushes = 0;
    uint32_t width = 0, height = 0, colorFormat = 0;
    bool depth = false, multiview = false;
    std::vector<int> reads;         // earlier passes of this frame whose targets it samples
    std::vector<Surface*> sampledSurfaces, laidOut;
};
std::vector<Pass> g_passes;
std::unordered_map<Surface*, int> g_writtenBy;  // a target's last pass in this frame
uint32_t g_drawsBeforePass = 0, g_frameFlushesBeforePass = 0;
// the 600-frame window's sums
struct Window {
    uint64_t frames = 0, passes = 0, draws = 0, layoutChanges = 0, eye1Flushes = 0;
    uint64_t passesReading = 0, readEdges = 0, readDistance = 0;
    uint64_t splitCrossSampled = 0, splitCrossLaidOut = 0, splitDrawsSecond = 0, splitPassIndex = 0;
    uint32_t maxPasses = 0;
} W;
uint64_t g_frame = 0;
bool render_thread() { return !draw_threads::on_draw_thread(); }

void log_frame(const std::vector<Pass>& passes) {
    LOG("[scene stats] one frame's passes (%zu): index, size, colour format, depth, multiview, draws, layout changes, "
        "eye 1 flushes, earlier passes sampled", passes.size());
    for (size_t i = 0; i < passes.size(); ++i) {
        const Pass& p = passes[i];
        std::string reads;
        for (int r : p.reads) reads += (reads.empty() ? "" : ",") + std::to_string(r);
        LOG("[scene stats]   %2zu  %4ux%-4u f%-3u %s %s draws %4u  layouts %3u  flushes %u  reads [%s]", i, p.width,
            p.height, p.colorFormat, p.depth ? "D" : "-", p.multiview ? "MV" : "--", p.draws, p.layoutChanges,
            p.eye1Flushes, reads.c_str());
    }
}
}  // namespace

void pass_begin(Surface* const* colors, Surface* depth, uint32_t width, uint32_t height, bool multiview) {
    if (!render_thread()) return;
    Pass p;
    p.width = width;
    p.height = height;
    p.depth = depth != nullptr;
    p.multiview = multiview;
    const int index = int(g_passes.size());
    for (int i = 0; i < 8; ++i)
        if (colors[i]) {
            if (!p.colorFormat) p.colorFormat = colors[i]->format;
            g_writtenBy[colors[i]] = index;
        }
    if (depth) g_writtenBy[depth] = index;
    g_passes.push_back(std::move(p));
}
void draw() {
    if (!render_thread()) return;
    if (g_passes.empty()) ++g_drawsBeforePass;
    else ++g_passes.back().draws;
}
void sampled(Surface* s) {
    if (!render_thread() || g_passes.empty() || !s) return;
    Pass& p = g_passes.back();
    if (std::find(p.sampledSurfaces.begin(), p.sampledSurfaces.end(), s) == p.sampledSurfaces.end())
        p.sampledSurfaces.push_back(s);
    auto w = g_writtenBy.find(s);
    if (w == g_writtenBy.end() || w->second == int(g_passes.size()) - 1) return;
    if (std::find(p.reads.begin(), p.reads.end(), w->second) == p.reads.end()) p.reads.push_back(w->second);
}
void layout_change(Surface* s) {
    if (!render_thread() || g_passes.empty()) return;
    Pass& p = g_passes.back();
    ++p.layoutChanges;
    if (std::find(p.laidOut.begin(), p.laidOut.end(), s) == p.laidOut.end()) p.laidOut.push_back(s);
}
void eye1_flush() {
    if (!render_thread()) return;
    if (g_passes.empty()) ++g_frameFlushesBeforePass;
    else ++g_passes.back().eye1Flushes;
}
void frame_end() {
    if (!render_thread()) return;
    ++g_frame;
    if (!g_passes.empty()) {
        ++W.frames;
        W.passes += g_passes.size();
        W.maxPasses = std::max<uint32_t>(W.maxPasses, uint32_t(g_passes.size()));
        uint64_t draws = 0;
        for (size_t i = 0; i < g_passes.size(); ++i) {
            const Pass& p = g_passes[i];
            draws += p.draws;
            W.layoutChanges += p.layoutChanges;
            W.eye1Flushes += p.eye1Flushes;
            if (!p.reads.empty()) ++W.passesReading;
            W.readEdges += p.reads.size();
            for (int r : p.reads) W.readDistance += i - size_t(r);
        }
        W.draws += draws;
        // the split between two passes nearest half the frame's draws: the second part's surfaces that the first wrote
        size_t split = 0;
        uint64_t before = 0;
        while (split < g_passes.size() && 2 * (before + g_passes[split].draws) <= draws) before += g_passes[split++].draws;
        std::unordered_map<Surface*, bool> firstWrote;
        for (auto& [s, pass] : g_writtenBy)
            if (pass < int(split)) firstWrote[s] = true;
        // (a target written in both parts counts as the first part's: the second part needs its state from it)
        uint64_t crossSampled = 0, crossLaidOut = 0;
        std::vector<Surface*> seenS, seenL;
        for (size_t i = split; i < g_passes.size(); ++i) {
            for (Surface* s : g_passes[i].sampledSurfaces)
                if (firstWrote.count(s) && std::find(seenS.begin(), seenS.end(), s) == seenS.end()) seenS.push_back(s), ++crossSampled;
            for (Surface* s : g_passes[i].laidOut)
                if (firstWrote.count(s) && std::find(seenL.begin(), seenL.end(), s) == seenL.end()) seenL.push_back(s), ++crossLaidOut;
        }
        W.splitCrossSampled += crossSampled;
        W.splitCrossLaidOut += crossLaidOut;
        W.splitDrawsSecond += draws - before;
        W.splitPassIndex += split;
        if (g_frame % 1800 == 0) log_frame(g_passes);
    }
    if (W.frames >= 600) {
        const double f = double(W.frames);
        LOG("[scene stats] per frame: %.1f passes (at most %u), %.0f draws, %.1f layout changes, %.1f eye 1 flushes; "
            "%.1f passes sample an earlier pass's target (%.1f such reads, %.1f passes back on average)",
            W.passes / f, W.maxPasses, W.draws / f, W.layoutChanges / f, W.eye1Flushes / f, W.passesReading / f,
            W.readEdges / f, W.readEdges ? double(W.readDistance) / double(W.readEdges) : 0.0);
        LOG("[scene stats] the half-draws split: before pass %.1f, %.0f draws after it; the second part samples %.1f and "
            "lays out %.1f surfaces the first part wrote", W.splitPassIndex / f, W.splitDrawsSecond / f,
            W.splitCrossSampled / f, W.splitCrossLaidOut / f);
        W = {};
    }
    g_passes.clear();
    g_writtenBy.clear();
    g_drawsBeforePass = g_frameFlushesBeforePass = 0;
}
}  // namespace gfxvk::scene_stats
