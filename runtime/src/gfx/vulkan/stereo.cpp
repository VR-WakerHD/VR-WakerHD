#include "stereo.h"
#include "atlas_thread.h"
#include "eye_thread.h"
#include "api.h"
#include "deferred_dispatch.h"
#include "record_worker.h"
#include "../hdr.h"
#include <string_view>
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "overlay/hostui.h"
#include "runtime.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include "draw_threads.h"
#include "atlas_overlap.h"
#include "scene_stats.h"
namespace gfxvk {
// (the selected eye, per thread, and eye 1's surfaces: backend.h)
Screen stereoRight;
namespace {
// eye 1's linear targets and main depth (eye 0's: R.linearTargets, R.mainDepthAddr). Each eye keeps its own:
// selecting an eye no longer swaps them (stage 2.6)
std::vector<Surface*> otherLinearTargets;
uint32_t otherMainDepth = 0;
// A render pass not in R.pass now: the unselected eye's, or the atlas's held stream's
struct EyePass {
    PassState pass;
    uint32_t scope = UINT32_MAX; // GPU pass timestamp scope
} otherPass;
// Eye 1's held commands. Unbounded: a limit would emit them in place, inside eye 0's pass.
DeferredCommands eyeCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                      std::numeric_limits<size_t>::max()}};
// ... as POD bytes (no allocation per command), replayed in one run per flush.
PodCommands eyePodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                               std::numeric_limits<size_t>::max()}};
int inOrderScopes = 0;
// (docs/VR_SCENE_THREADS.md, stage S5) the scene's thread's own in-order operation (a clear or copy it records)
constinit thread_local int t_inOrderScopes = 0;
bool streaming() { return stereo::frame.mode != stereo::Off && !inOrderScopes && !t_inOrderScopes; }
void swap_pass(EyePass& other = otherPass) {
    auto& slot = R.submissions[R.activeSubmission];
    std::swap(R.pass, other.pass);
    std::swap(slot.activeRenderScope, other.scope);
}
// The shared sun atlas's draws in a held stream of their own (stereo_atlas_hold; docs/VR_PERFORMANCE.md, roadmap 2b,
// stage 1). While it holds, it continues the renderer's stream: everything recorded goes into it in order, with the
// atlas's render pass, and it is emitted whole before anything else is recorded. Unbounded, as eye 1's.
EyePass atlasPass;
DeferredCommands atlasCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                        std::numeric_limits<size_t>::max()}};
PodCommands atlasPodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                 std::numeric_limits<size_t>::max()}};
bool atlasHolding = false;
// The atlas's block recorded on its own thread (atlas_thread.h): the render thread records none of it, and nothing
// into the atlas's stream
bool atlasThreaded = false;
// (stage 3.2) the scene's eye 0 commands while the atlas's thread catches up: held, and emitted after the atlas's
bool sceneHolding = false;
DeferredCommands sceneCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                        std::numeric_limits<size_t>::max()}};
PodCommands scenePodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                 std::numeric_limits<size_t>::max()}};
// (stage 3.2) the render thread's uploads and layouts of game textures the atlas's thread has not used yet in this
// overlap, while the scene holds: emitted before the atlas's stream (stereo_before_atlas)
DeferredCommands beforeAtlasCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                              std::numeric_limits<size_t>::max()}};
PodCommands beforeAtlasPodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                       std::numeric_limits<size_t>::max()}};
DeferredCommands* before_atlas_stream() {
    if (!beforeAtlasCommands.capturing()) {
        if (!beforeAtlasCommands.command_count()) beforeAtlasCommands.hold_in(&beforeAtlasPodCommands);
        beforeAtlasCommands.start_detached_capture([] {
            throw std::logic_error("the commands held before the sun atlas's cannot be emitted out of order");
        });
    }
    return &beforeAtlasCommands;
}
// (docs/VR_SCENE_THREADS.md, stage S1) the scene's part: eye 0's commands from a draw inside a large pass on, held and
// emitted whole when that pass ends (stereo_part_hold, stereo_part_join)
bool partHolding = false, partJoining = false;
// (stage S5b) the part is recorded into the renderer's own stream: nothing is held for it, on either thread
bool partDirect = false;
DeferredCommands partCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                       std::numeric_limits<size_t>::max()}};
PodCommands partPodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                std::numeric_limits<size_t>::max()}};
DeferredCommands* part_stream() {
    if (!partCommands.capturing()) {
        if (!partCommands.command_count()) partCommands.hold_in(&partPodCommands);
        partCommands.start_detached_capture([] {
            throw std::logic_error("the scene part's held commands cannot be emitted out of order");
        });
    }
    return &partCommands;
}
// (stage S4) the eye 1 halves the scene's thread draws in a part: moved into eye 1's held stream when the threads join
DeferredCommands partEye1Commands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                           std::numeric_limits<size_t>::max()}};
PodCommands partEye1PodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                    std::numeric_limits<size_t>::max()}};
DeferredCommands* part_eye1_stream() {
    if (!partEye1Commands.capturing()) {
        if (!partEye1Commands.command_count()) partEye1Commands.hold_in(&partEye1PodCommands);
        partEye1Commands.start_detached_capture([] {
            throw std::logic_error("the scene part's eye 1 commands cannot be emitted out of order");
        });
    }
    return &partEye1Commands;
}
// (stage S6) the second drawing thread's span: its eye 0 commands and its eye 1 halves, held while it records beside
// the scene's thread, and emitted after that thread's (stereo_part_emit_b)
DeferredCommands partBCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                        std::numeric_limits<size_t>::max()}};
PodCommands partBPodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                 std::numeric_limits<size_t>::max()}};
DeferredCommands partBEye1Commands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                            std::numeric_limits<size_t>::max()}};
PodCommands partBEye1PodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                     std::numeric_limits<size_t>::max()}};
DeferredCommands partBPreCommands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                           std::numeric_limits<size_t>::max()}};
PodCommands partBPrePodCommands{PodCommands::Limits{std::numeric_limits<size_t>::max(),
                                                    std::numeric_limits<size_t>::max()}};
DeferredCommands* held_stream(DeferredCommands& commands, PodCommands& pod, const char* what) {
    if (!commands.capturing()) {
        if (!commands.command_count()) commands.hold_in(&pod);
        commands.start_detached_capture([what] { throw std::logic_error(what); });
    }
    return &commands;
}
DeferredCommands* scene_stream() {
    if (!sceneCommands.capturing()) {
        if (!sceneCommands.command_count()) sceneCommands.hold_in(&scenePodCommands);
        sceneCommands.start_detached_capture([] {
            throw std::logic_error("the scene's held commands cannot be emitted out of order");
        });
    }
    return &sceneCommands;
}
DeferredCommands* atlas_stream() {
    if (!atlasCommands.capturing()) {
        if (!atlasCommands.command_count()) atlasCommands.hold_in(&atlasPodCommands);
        atlasCommands.start_detached_capture([] {
            throw std::logic_error("the sun atlas's held commands cannot be emitted out of order");
        });
    }
    return &atlasCommands;
}
// (diagnostics, WWHD_VK_TARGET_STATS) holds (one per emit) and the atlas draws they held, since the last log
uint64_t g_atlasHolds = 0, g_atlasHeldDraws = 0, g_atlasEmits[int(AtlasEmit::Count)] = {};
bool g_inOrderFlush = false;  // (diagnostics) stereo_flush called by StereoInOrder
// (docs/VR_EYE_THREAD.md, stage E2) Segmented held streams: both eyes' commands held, in segments. A flush from inside
// a draw (stereo_release_flush) cuts instead: each eye's segment ends, and the next pair begins. stereo_flush emits
// them in order, eye 0's then eye 1's for each pair: the same order as flushing at every cut. With eye 1 on a thread
// of its own the cut needs no wait for it.
struct HeldStream {
    DeferredCommands commands{DeferredCommands::Limits{std::numeric_limits<size_t>::max(),
                                                       std::numeric_limits<size_t>::max()}};
    PodCommands pods{PodCommands::Limits{std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()}};
    DeferredCommands* stream() {
        if (!commands.capturing()) {
            if (!commands.command_count()) commands.hold_in(&pods);
            commands.start_detached_capture([] {
                throw std::logic_error("an eye's held segment cannot be emitted out of order");
            });
        }
        return &commands;
    }
};
std::vector<std::unique_ptr<HeldStream>> eyeSegments[2];
size_t segment = 0;  // the pair being recorded
HeldStream& eye_segment(int eye, size_t i) {
    auto& v = eyeSegments[eye];
    while (v.size() <= i) v.push_back(std::make_unique<HeldStream>());
    return *v[i];
}
bool held_segments() {
    for (int eye = 0; eye < 2; ++eye)
        for (size_t i = 0; i <= segment && i < eyeSegments[eye].size(); ++i)
            if (eyeSegments[eye][i]->commands.command_count()) return true;
    return false;
}
DeferredCommands* eye_stream() {
    if (stereo_eye_segments()) return eye_segment(1, segment).stream();
    if (!eyeCommands.capturing()) {
        if (!eyeCommands.command_count()) eyeCommands.hold_in(&eyePodCommands);
        eyeCommands.start_detached_capture([] {
            throw std::logic_error("stereo eye commands cannot be emitted out of order");
        });
    }
    return &eyeCommands;
}
// Eye 0's stream in the current pair of segments. The first pair's is eye 0's usual one (the renderer's own stream, or
// the scene's held stream while the atlas's thread runs): eye 0 is held only after a cut, which is rare. Holding it from
// the start copied all its commands once more, and handed the record worker one large burst per flush instead of a
// steady stream (docs/VR_EYE_THREAD.md: the eye thread's cost)
DeferredCommands* eye0_segment_stream() {
    if (segment > 0) return eye_segment(0, segment).stream();
    return sceneHolding ? scene_stream() : nullptr;
}
void attach_eye_stream() {
    eye_deferred_commands() = atlasHolding && !atlasThreaded                       ? atlas_stream()
                            : stereoEye() && streaming()                            ? eye_stream()
                            : stereo_eye_segments() && streaming() && segment > 0 ? eye_segment(0, segment).stream()
                            : partHolding && streaming() && !partDirect           ? part_stream()
                            : sceneHolding                                        ? scene_stream()
                                                                                  : nullptr;
}
}
std::vector<Surface*>& selected_linear_targets() { return stereoEye() ? otherLinearTargets : R.linearTargets; }
uint32_t& selected_main_depth() { return stereoEye() ? otherMainDepth : R.mainDepthAddr; }
namespace overlap {
namespace { std::atomic<bool> g_disabled{false}; }
void set_atlas_thread() { atlasThreadTls = true; }
void join(const char*) {}  // (stage 3: waits for the atlas's thread)
void disable(const char* why) {
    if (!g_disabled.exchange(true)) LOG("[vulkan stereo] sun atlas thread: off for this session (%s)", why);
}
bool disabled() { return g_disabled.load(); }
void conflict(const Surface* s) {
    if (!g_disabled.load())
        LOG("[vulkan stereo] sun atlas thread: surface %08X (%ux%u, %u slices, format %X%s%s) used after the render thread "
            "changed it (%s)", s->addr, s->width, s->height, s->slices, s->format, s->isDepth ? ", depth" : "",
            s->gpuWritten ? ", rendered" : "", s->sceneChangeWhat ? s->sceneChangeWhat : "?");
    disable("the atlas's thread uses a surface the render thread changed in this overlap");
}
}  // namespace overlap
bool stereo_atlas_holding() { return atlasHolding; }
DeferredCommands* stereo_atlas_thread_stream() { return atlas_stream(); }
namespace {
uint32_t g_timelineDraw = 0;
uint64_t g_timelineFrame = ~0ull;
std::string g_timeline;
bool g_timelineSampled = false;
}
static bool timeline_frame() { return R.frame == 2400 || R.frame == 3000; }
void stereo_timeline_draw() {
    if (g_timelineFrame != R.frame) {
        if (g_timelineFrame != ~0ull && (g_timelineFrame == 2400 || g_timelineFrame == 3000))
            LOG("[vulkan stereo] frame %llu timeline (%u draws):%s", (unsigned long long)g_timelineFrame, g_timelineDraw,
                g_timeline.c_str());
        g_timelineFrame = R.frame;
        g_timelineDraw = 0;
        g_timeline.clear();
        g_timelineSampled = false;
    }
    ++g_timelineDraw;
}
void stereo_timeline_event(const char* what) {
    if (!timeline_frame()) return;
    g_timeline += " ";
    g_timeline += what;
    g_timeline += "@" + std::to_string(g_timelineDraw);
}
void stereo_timeline_texture(const Surface* s, bool atlasDraw) {
    // per frame: the atlas's draws' textures, and how many of them the scene's draws (any other) bind as well
    static uint64_t frame = ~0ull;
    static std::unordered_set<const Surface*> atlasTextures, sceneTextures;
    static uint64_t atlasBinds = 0, sceneBinds = 0;
    if (frame != R.frame) {
        if (frame == 2400 || frame == 3000) {
            size_t shared = 0;
            for (auto* t : atlasTextures) shared += sceneTextures.count(t);
            LOG("[vulkan stereo] frame %llu textures: the atlas's draws bound %llu times %zu textures, the scene's draws "
                "%llu times %zu; %zu of the atlas's also bound by the scene",
                (unsigned long long)frame, (unsigned long long)atlasBinds, atlasTextures.size(),
                (unsigned long long)sceneBinds, sceneTextures.size(), shared);
        }
        frame = R.frame;
        atlasTextures.clear();
        sceneTextures.clear();
        atlasBinds = sceneBinds = 0;
    }
    (atlasDraw ? atlasTextures : sceneTextures).insert(s);
    ++(atlasDraw ? atlasBinds : sceneBinds);
}
void stereo_timeline_layout(const Surface* s, VkImageLayout layout) {
    if (g_timelineSampled || layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        !stereo_light_atlas(s->width, s->height, s->slices, s->isDepth))
        return;
    g_timelineSampled = true;
    stereo_timeline_event("atlas-sampled");
}
void stereo_atlas_hold() {
    if (atlasHolding || !stereo_atlas_stream_enabled() || !streaming() || stereoEye() != 0) return;
    if (atlas_thread::enabled()) {
        // the atlas's draws on its own thread (atlas_thread.h): it takes the atlas's pass, stream, arena and pool
        atlasHolding = atlasThreaded = true;
        ++g_atlasHolds;
        if (stereo_timeline_on()) stereo_timeline_event("hold(thread)");
        atlas_thread::begin();
        return;
    }
    // the renderer's own pass ends first: the held commands are emitted after it, never inside it
    end_encoder();
    swap_pass(atlasPass);
    atlasHolding = true;
    ++g_atlasHolds;
    if (stereo_timeline_on()) stereo_timeline_event("hold");
    attach_eye_stream();
    set_thread_upload_blocks(&R.atlasUploadBlocks);  // its own upload arena (stage 2.3)
    set_thread_descriptor_pool(&R.atlasDescriptorPool);  // and descriptor pool (stage 2.5)
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG("[vulkan stereo] sun atlas held stream: its own upload arena, %s",
            R.atlasDescriptorPool && descriptor_pool() == R.atlasDescriptorPool ? "its own descriptor pool"
                                                                                : "the renderer's descriptor pool (none of its own)");
    }
}
void stereo_atlas_held_draw() { ++g_atlasHeldDraws; }
namespace {
// a held stream into the renderer's, as stereo_flush emits eye 1's: queued after the renderer's stream as POD, or
// replayed through its recorder (either way after everything emitted before it)
void emit_held(DeferredCommands& held) {
    if (!held.command_count() || held.publish_held()) return;
    auto commands = held.take();
    if (auto* recorder = active_deferred_commands; recorder && recorder->pod_mode()) drain_record_commands();
    for (auto& command : commands) record::emit_owned(std::move(command));
}
void timeline_emit(AtlasEmit why) {
    if (!stereo_timeline_on()) return;
    static const char* const names[] = {"emit(draw)", "emit(submission/swap)", "emit(clear/copy)", "emit(eye)",
                                        "emit(other)", "join(atlas-sampled)", "join(invalidate)", "join(idle)"};
    stereo_timeline_event(names[int(why)]);
}
// (stage 3.2) the threads join: the atlas's thread finishes, the scene's eye 0 pass ends at the end of its held
// commands, and both streams go into the renderer's, the atlas's first
void join_atlas_thread(AtlasEmit why) {
    ++g_atlasEmits[int(why)];
    timeline_emit(why);
    // (with the eyes' segments: eye 0's pass is in the scene's stream only in the first pair; after a cut a cut ended it)
    if (sceneHolding && (!stereo_eye_segments() || segment == 0)) {
        if (stereoEye()) swap_pass();  // (eye 0's pass is the unselected one)
        eye_deferred_commands() = scene_stream();
        end_encoder();
        if (stereoEye()) swap_pass();
    }
    eye_deferred_commands() = nullptr;
    const bool scene = sceneHolding;
    atlasHolding = atlasThreaded = sceneHolding = false;
    try {
        atlas_thread::finish();
        emit_held(beforeAtlasCommands);
        emit_held(atlasCommands);
        if (scene) emit_held(sceneCommands);
    } catch (...) { attach_eye_stream(); throw; }
    attach_eye_stream();
}
}
void stereo_atlas_emit_held(AtlasEmit why) {
    if (atlas_thread::running()) {
        // (stage 3.2) the block's end: the render thread goes on with the scene, its eye 0 commands held, while the
        // atlas's thread catches up. The scene's own draws and eye switches need no join (a draw that may sample the
        // atlas joins first: draw_stereo); everything else does
        if (atlasHolding && why == AtlasEmit::Draw) {
            ++g_atlasEmits[int(why)];
            timeline_emit(why);
            atlasHolding = false;
            sceneHolding = true;
            attach_eye_stream();
            return;
        }
        if (sceneHolding && (why == AtlasEmit::Draw || why == AtlasEmit::Eye)) return;
        join_atlas_thread(why);
        return;
    }
    if (!atlasHolding) return;
    ++g_atlasEmits[int(why)];
    timeline_emit(why);
    // the atlas's pass ends at the end of its held commands
    end_encoder();
    swap_pass(atlasPass);
    atlasHolding = false;
    attach_eye_stream();
    set_thread_upload_blocks(nullptr);
    set_thread_descriptor_pool(nullptr);
    emit_held(atlasCommands);
}
void stereo_atlas_join_held(AtlasEmit why) {
    if (atlas_thread::running()) join_atlas_thread(why);
    else stereo_atlas_emit_held(why);
}
namespace {
// (diagnostics) parts, and why each was emitted: its pass ended, or AtlasEmit's reasons (Flush, InOrder, Eye, Other, ...)
uint64_t g_partHolds = 0, g_partJoins[int(AtlasEmit::Count) + 1] = {}, g_partRecordingJoins = 0;
constexpr int kPartPassEnd = int(AtlasEmit::Count);
// the part goes into the renderer's stream, after everything recorded before it began
// (diagnostics, the scene split's log line) how the parts went into the renderer's stream: queued to the record worker
// whole (published), or replayed here after draining it (exact); and the time the joins took, waiting and emitting
uint64_t g_partPublished = 0, g_partExact = 0, g_partWaitNs = 0, g_partEmitNs = 0, g_partBusyDuringJoinNs = 0;
uint64_t g_partFrameWaitNs = 0;  // (the automatic tail, draw.cpp) this frame's waits at the joins
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void finish_part() {
    partHolding = false;
    eye_deferred_commands() = nullptr;
    const uint64_t t0 = now_ns();
    try {
        if (switches::kSceneFront) scene_thread::front_join();  // (stage S7: the second drawing thread's span first)
        if (partCommands.command_count()) {
            if (partCommands.publish_held()) ++g_partPublished;
            else {
                ++g_partExact;
                auto commands = partCommands.take();
                if (auto* recorder = active_deferred_commands; recorder && recorder->pod_mode()) drain_record_commands();
                for (auto& command : commands) record::emit_owned(std::move(command));
            }
        }
    } catch (...) { partJoining = false; attach_eye_stream(); throw; }
    g_partEmitNs += now_ns() - t0;
    partJoining = false;
    attach_eye_stream();
}
}  // namespace
uint64_t scene_split_frame_draws();  // (draw.cpp)
namespace { uint64_t g_partStartDraws = 0; }
void stereo_part_trace_start() { g_partStartDraws = scene_split_frame_draws(); }
bool stereo_part_holding() { return partHolding; }
DeferredCommands* stereo_part_b_stream() {
    return held_stream(partBCommands, partBPodCommands, "the second drawing thread's commands cannot be emitted out of order");
}
DeferredCommands* stereo_part_b_eye1_stream() {
    return held_stream(partBEye1Commands, partBEye1PodCommands,
                       "the second drawing thread's eye 1 commands cannot be emitted out of order");
}
DeferredCommands* stereo_part_b_pre_stream() {
    return held_stream(partBPreCommands, partBPrePodCommands,
                       "the second drawing thread's starting layouts cannot be emitted out of order");
}
bool stereo_part_b_eye1_held() { return partBEye1Commands.command_count() != 0; }
bool stereo_part_b_append_eye1() {
    ++scene_thread::b_eye_generation();
    if (stereo_part_b_stream()->append_held(partBEye1Commands)) return true;
    auto held = partBEye1Commands.take();
    for (auto& command : held) record::emit_owned(std::move(command));  // (into its own stream: the attached one)
    return false;
}
bool stereo_part_emit_b() {
    // (the scene's thread, the second drawing thread done) its commands after the scene's thread's span: to the record
    // worker after the renderer's (S5b: the scene's thread records there directly), as finish_part sends a held part;
    // or after the part's held ones (S5)
    bool whole = true;
    // ("S6: B's reads deferred") the layouts its span starts with first, then its commands
    for (DeferredCommands* held : {&partBPreCommands, &partBCommands}) {
        if (!held->command_count()) continue;
        if (partDirect ? held->publish_held() : part_stream()->append_held(*held)) {
        } else {
            whole = false;
            auto commands = held->take();
            if (partDirect)
                if (auto* recorder = active_deferred_commands; recorder && recorder->pod_mode()) drain_record_commands();
            for (auto& command : commands) record::emit_owned(std::move(command));
        }
    }
    // its eye 1 halves after the scene's thread's, in the part's eye 1 stream
    if (partBEye1Commands.command_count() && !part_eye1_stream()->append_held(partBEye1Commands)) {
        whole = false;
        auto held = partBEye1Commands.take();
        DeferredCommands* const previous = eye_deferred_commands();
        eye_deferred_commands() = part_eye1_stream();
        struct Back { DeferredCommands* p; ~Back() { eye_deferred_commands() = p; } } back{previous};
        for (auto& command : held) record::emit_owned(std::move(command));
    }
    return whole;
}
void stereo_front_emit_b() {
    // (stage S7) the second drawing thread's span, its eye 1 halves after its eye 0 commands (kEndB), into the renderer's
    // stream: queued to the record worker whole, as finish_part sends the part after it, or replayed here
    for (DeferredCommands* held : {&partBCommands, &partBEye1Commands}) {
        if (!held->command_count() || held->publish_held()) continue;
        auto commands = held->take();
        if (auto* recorder = active_deferred_commands; recorder && recorder->pod_mode()) drain_record_commands();
        for (auto& command : commands) record::emit_owned(std::move(command));
    }
}
bool stereo_part_eye1_held() { return partEye1Commands.command_count() != 0; }
void stereo_thread_in_order(bool on) { t_inOrderScopes += on ? 1 : -1; }
bool stereo_part_append_eye1() {
    // (the scene's thread) the part's eye 1 commands after its eye 0 ones, as stereo_flush emits eye 1's held commands
    ++stereoFlushGeneration;
    // (stage S5b) the part is the renderer's stream: they go to the record worker after it, as stereo_flush sends eye
    // 1's held commands
    if (partDirect) {
        if (partEye1Commands.publish_held()) return true;
        auto held = partEye1Commands.take();
        if (auto* recorder = active_deferred_commands; recorder && recorder->pod_mode()) drain_record_commands();
        for (auto& command : held) record::emit_owned(std::move(command));
        return false;
    }
    if (part_stream()->append_held(partEye1Commands)) return true;
    auto held = partEye1Commands.take();
    for (auto& command : held) record::emit_owned(std::move(command));  // (into the part's stream: the attached one)
    return false;
}
void scene_part_recording() {
    // (the render thread only: the scene's thread records the part; a join's own emission records too)
    if (partJoining || draw_threads::on_draw_thread()) return;
    // (stage S7) the render thread records again: the second drawing thread's span ends here, its later draws are the
    // render thread's, in the part (after the span's on the GPU, as in the guest's order)
    if (switches::kSceneFront) {
        scene_thread::front_end_span();
        return;
    }
    ++g_partRecordingJoins;
    stereo_part_join_at(PartJoinSite::Recording);
}
namespace { uint64_t g_partSites[int(PartJoinSite::Count)] = {}; }
// (diagnostics) one frame in 1,800: each join's site, the frame's draws before it and the part's draws
namespace {
std::string g_joinTrace;
uint64_t g_joinTraceFrame = ~0ull;
}
void stereo_part_join_at(PartJoinSite site) {
    // (the tail drawn here ends where a part would have been joined: any join site, as without it)
    if (!draw_threads::on_draw_thread()) scene_tail_flush();
    if (!partHolding || partJoining || draw_threads::on_draw_thread()) return;
    ++g_partSites[int(site)];
    stereo_part_join(site == PartJoinSite::Flush ? AtlasEmit::Flush : site == PartJoinSite::InOrder ? AtlasEmit::InOrder
                     : site == PartJoinSite::Idle ? AtlasEmit::Idle : AtlasEmit::Other);
    if (R.frame % 1800 == 0) {  // (after the join: the scene's thread has stopped counting)
        if (g_joinTraceFrame != R.frame) {
            if (!g_joinTrace.empty()) LOG("[vulkan scene thread] frame %llu joins:%s", (unsigned long long)g_joinTraceFrame, g_joinTrace.c_str());
            g_joinTrace.clear();
            g_joinTraceFrame = R.frame;
        }
        static const char* const names[] = {"flush", "in-order", "pass-end", "eye1", "recording", "clouds", "idle"};
        char b[96];
        std::snprintf(b, sizeof b, " %s@%llu (part from %llu)", names[int(site)],
                      (unsigned long long)scene_split_frame_draws(), (unsigned long long)g_partStartDraws);
        g_joinTrace += b;
    }
}
void stereo_part_site_stats(uint64_t (&sites)[int(PartJoinSite::Count)]) {
    for (int i = 0; i < int(PartJoinSite::Count); ++i) sites[i] = g_partSites[i], g_partSites[i] = 0;
}
void stereo_part_hold() {
    // only from the renderer's own stream: nothing else held, no redirect active, eye 0 selected
    if (partHolding || !streaming() || stereoEye() != 0 || atlasHolding || sceneHolding || stereo_eye_segments() ||
        eye_thread::running() || draw_threads::on_draw_thread() || eye_deferred_commands() != nullptr)
        return;
    // (stage S3) the scene's thread: only while eye 1 holds nothing (its held stream is the render thread's)
    const bool threaded = scene_thread::enabled();
    if (threaded && eyeCommands.command_count()) return;
    partHolding = true;
    partDirect = threaded && switches::kSceneDirect;
    ++g_partHolds;
    stereo_part_trace_start();
    ++g_partEpoch;  // (a second thread starts with its own caches: the part records from empty ones)
    attach_eye_stream();
    if (threaded) scene_thread::begin(partDirect ? nullptr : part_stream(), part_eye1_stream());
}
void stereo_part_join(AtlasEmit why) {
    if (!partHolding || partJoining || draw_threads::on_draw_thread()) return;
    partJoining = true;
    ++g_partJoins[int(why)];
    if (scene_thread::running()) {
        // (stage S3) the scene's thread ends eye 0's pass at the end of the part; the render thread's copy of that pass
        // (the unselected one while eye 1 is selected) is no longer open
        g_partOverlapping = false;
        try {
            const uint64_t t0 = now_ns();
            const uint64_t busyBefore = scene_thread::busy_ns();
            scene_thread::finish();
            const uint64_t t1 = now_ns(), waited = t1 - t0;
            g_partWaitNs += waited;
            g_partFrameWaitNs += waited;
            scene_tail_note_join(t0, t1);
            g_partBusyDuringJoinNs += scene_thread::busy_ns() - busyBefore;
        } catch (...) { partHolding = false; partJoining = false; attach_eye_stream(); throw; }
        if (stereoEye()) swap_pass();
        pass_state().rendering = false;
        pass_state().tracked = false;
        if (stereoEye()) swap_pass();
        // (stage S4) the eye 1 halves it drew go after eye 1's held commands (none: a part starts only without them),
        // as if drawn here; eye 1's pass ended at their end
        if (partEye1Commands.command_count()) {
            DeferredCommands* const previous = eye_deferred_commands();
            eye_deferred_commands() = eye_stream();
            try {
                auto held = partEye1Commands.take();
                for (auto& command : held) record::emit_owned(std::move(command));
            } catch (...) { eye_deferred_commands() = previous; partHolding = false; partJoining = false; attach_eye_stream(); throw; }
            eye_deferred_commands() = previous;
        }
        finish_part();
        return;
    }
    // eye 0's pass ends at the end of the part's commands (the unselected pass while eye 1 is selected)
    if (stereoEye()) swap_pass();
    eye_deferred_commands() = part_stream();
    try {
        end_encoder();
    } catch (...) { if (stereoEye()) swap_pass(); partHolding = false; partJoining = false; attach_eye_stream(); throw; }
    if (stereoEye()) swap_pass();
    finish_part();
}
void stereo_part_pass_ended() {
    // (the scene's thread's own pass ends inside the part: the part goes on)
    // (stage S7: the render thread's own passes go on in the part; it joins at a draw outside the pass or a flush)
    if (switches::kSceneFront) return;
    if (!partHolding || partJoining || stereoEye() != 0 || eye_deferred_commands() != &partCommands ||
        draw_threads::on_draw_thread())
        return;
    partJoining = true;
    ++g_partJoins[kPartPassEnd];
    finish_part();
}
uint64_t stereo_part_take_frame_wait_ns() {
    const uint64_t ns = g_partFrameWaitNs;
    g_partFrameWaitNs = 0;
    return ns;
}
double stereo_part_busy_during_join_ms() {
    const double ms = double(g_partBusyDuringJoinNs) / 1e6;
    g_partBusyDuringJoinNs = 0;
    return ms;
}
void stereo_part_join_stats(uint64_t& published, uint64_t& exact, double& waitMs, double& emitMs) {
    published = g_partPublished;
    exact = g_partExact;
    waitMs = double(g_partWaitNs) / 1e6;
    emitMs = double(g_partEmitNs) / 1e6;
    g_partPublished = g_partExact = g_partWaitNs = g_partEmitNs = 0;
}
void stereo_part_stats(uint64_t& holds, uint64_t& passEnds, uint64_t& flushes, uint64_t& others) {
    holds = g_partHolds;
    passEnds = g_partJoins[kPartPassEnd];
    flushes = g_partJoins[int(AtlasEmit::Flush)] + g_partJoins[int(AtlasEmit::InOrder)];
    others = 0;
    for (int i = 0; i < int(AtlasEmit::Count); ++i) others += g_partJoins[i];
    others -= flushes;
    g_partHolds = 0;
    for (auto& n : g_partJoins) n = 0;
}
bool stereo_scene_holding() { return sceneHolding; }
// (diagnostics) the stereo streams and passes as they are now, for a log line (backend.cpp, the frame repeated while a
// shader or pipeline is made)
std::string stereo_debug_state() {
    const DeferredCommands* e = eye_deferred_commands();
    const char* stream = !e ? "renderer" : e == &eyeCommands ? "eye1" : e == &sceneCommands ? "scene"
                       : e == &atlasCommands ? "atlas" : "other";
    char s[320];
    snprintf(s, sizeof s,
             "eye %d, pass open %d (other eye's %d, atlas's %d), stream %s, eye 1 held %zu, scene held %d (%zu), atlas held "
             "%d (threaded %d, thread %d), part held %d, segment %zu, in-order %d",
             stereoEye(), int(R.pass.rendering), int(otherPass.pass.rendering), int(atlasPass.pass.rendering), stream,
             eyeCommands.command_count(), int(sceneHolding), sceneCommands.command_count(), int(atlasHolding),
             int(atlasThreaded), int(atlas_thread::running()), int(partHolding), segment, inOrderScopes);
    return s;
}
bool stereo_before_atlas_held(const Surface* s) {
    return sceneHolding && stereoEye() == 0 && !s->gpuWritten && s->atlasUse != overlap::id() &&
           !overlap::on_atlas_thread();
}
DeferredCommands* stereo_before_atlas_stream() { return before_atlas_stream(); }
BeforeAtlas::BeforeAtlas(const Surface* s) : on(stereo_before_atlas(s)), previous(eye_deferred_commands()) {
    if (!on) return;
    end_encoder();  // (the scene's pass ends in the scene's stream)
    eye_deferred_commands() = before_atlas_stream();
}
BeforeAtlas::~BeforeAtlas() noexcept(false) {
    if (!on) return;
    // (the attached stream again, not the one held when the scope began: a cut may have moved it on meanwhile)
    struct Restore {
        ~Restore() { attach_eye_stream(); }
    } restore;
    end_encoder();  // a pass its commands opened ends where they are
}
void stereo_atlas_stats(uint64_t& holds, uint64_t& draws, uint64_t (&why)[int(AtlasEmit::Count)]) {
    holds = g_atlasHolds;
    draws = g_atlasHeldDraws;
    g_atlasHolds = g_atlasHeldDraws = 0;
    for (int i = 0; i < int(AtlasEmit::Count); ++i) why[i] = g_atlasEmits[i], g_atlasEmits[i] = 0;
}
bool stereo_light_atlas(uint32_t width,uint32_t height,uint32_t layers,bool depth) {
    // WWHD's three 1024-square sun cascades are rendered in light space.
    // Scene depth, AO, the screen-space shadow mask and bloom remain per-eye.
    return depth && width==1024 && height==1024 && layers==3;
}
decltype(Renderer::surfaces)& stereo_surface_cache(const SurfaceDesc& desc) {
    if(stereoEye() && stereo_light_atlas(desc.width,desc.height,desc.slices,desc.isDepth))
        return surfaces_of(0); // canonical light-space atlas belongs to eye 0
    return selected_surfaces();
}
decltype(Renderer::surfaces)& stereo_copy_cache(uint32_t addr,uint32_t width,uint32_t height,uint32_t layers) {
    if(stereoEye() && stereo_light_atlas(width,height,layers,true)) {
        auto range=surfaces_of(0).equal_range(addr);
        for(auto it=range.first;it!=range.second;++it) {
            const auto& s=*it->second;
            if(s.isDepth && s.width==width && s.height==height && s.slices==layers)
                return surfaces_of(0);
        }
    }
    return selected_surfaces();
}
void stereo_invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    // (stage 3.2) in the scene while the atlas's thread catches up: the atlas's draws come before it. In the atlas's
    // block it is applied at once, as before the thread (the scene that follows sees it)
    if (atlas_thread::running() && !atlasHolding && !overlap::on_atlas_thread())
        join_atlas_thread(AtlasEmit::Invalidate);
    // (docs/VR_SCENE_THREADS.md, stage S7) applied at once beside the second drawing thread, as beside the scene's thread
    // in S5 and S6 (pictures byte-identical there): it marks ranges and surfaces to be read again, under the surface
    // lock, and the guest wrote them before the draws that follow; a draw before it the thread has not drawn yet may
    // read them again too. The game invalidates between the pass's draws, so a join here ended every span at once
    draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());  // (stage 2.6b: the surface maps are shared)
    draw_threads::lock_site(14);
    if (stereo_timeline_on() && !overlap::on_atlas_thread())
        stereo_timeline_event(atlasHolding ? "invalidate(in-atlas)" : "invalidate");
    // Invalidation changes CPU cache metadata only. Keep the active render
    // pass open and invalidate the shared buffer cache just once.
    invalidate(flags,addr,size);
    // (the unselected eye's; with eye 1 on its thread, there, in order with its draws: stage E4)
    if (eye_thread::running() && stereoEye() == 0) eye_thread::invalidate(flags, addr, size);
    else invalidate_surface_cache(surfaces_of(1-stereoEye()),flags,addr,size);
}
void stereo_select_eye(int eye) {
    if (stereoEye() == eye) return;
    stereo_atlas_emit(AtlasEmit::Eye);  // (the atlas is eye 0's: held only while eye 0 is selected)
    // One command stream (stereo off, StereoInOrder): the pass cannot stay open across a switch.
    if (!streaming()) end_encoder();
    swap_pass();
    stereoEye() = eye;  // (each eye keeps its own surfaces: surfaces_of)
    attach_eye_stream();
}
uint64_t stereoFlushGeneration = 1;
bool stereo_shared_texture(const Surface* s) {
    // (docs/VR_SCENE_THREADS.md, "the drawing threads' locks") a texture its own state rules out needs no lock; under
    // it, the same checks again (an upload on another thread may have been under way)
    const auto ruled_out = [s] {
        if (!streaming() || s->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return true;
        const bool photograph = stereo::layout_photograph() && s->gpuWritten && !s->isDepth;
        return !photograph && (s->gpuWritten || s->dirty || s->lastCheckedFrame != R.frame);
    };
    if (draw_threads::second_thread_active() && ruled_out()) return false;
    draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());  // (stage 2.6b: the surface maps are shared)
    draw_threads::lock_site(15);
    if (ruled_out()) return false;
    const bool photograph = stereo::layout_photograph() && s->gpuWritten && !s->isDepth;
    auto& eye0 = surfaces_of(0);
    auto& eye1 = surfaces_of(1);
    if (!photograph && eye1.count(s->addr)) return false;
    bool found = false;
    auto range = eye0.equal_range(s->addr);
    for (auto it = range.first; it != range.second; ++it) {
        if (!photograph && it->second->gpuWritten) return false;
        found |= it->second.get() == s;
    }
    return found;
}
Surface* stereo_eye0_texture(const SurfaceDesc& d) {
    if (stereoEye() != 1 || !streaming()) return nullptr;
    draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());  // (stage 2.6b: the surface maps are shared)
    draw_threads::lock_site(16);
    // PF_Pictograph_00 is a photograph, not a second live camera. Its GPU
    // preview must match the canonical image subsequently saved by the guest.
    // Eye 0's draw has already resolved format aliases and transitioned it;
    // the normal held-texture lease orders any later rewrite after this read.
    if (stereo::layout_photograph() && !d.isDepth) {
        Surface* photo = nullptr;
        auto range = surfaces_of(0).equal_range(d.addr);
        for (auto it = range.first; it != range.second; ++it) {
            auto* s = it->second.get();
            if (s->gpuWritten && !s->isDepth && s->width == d.width && s->height == d.height &&
                (s->format & 0x3f) == (d.format & 0x3f) && s->slices == d.slices &&
                s->dim == d.dim && stereo_shared_texture(s) && (!photo || s->writeSeq > photo->writeSeq))
                photo = s;
        }
        if (photo) { stereo_hold_texture(photo); return photo; }
    }
    if (selected_surfaces().count(d.addr)) return nullptr;
    // find_or_create_surface's sampled match among textures nobody rendered
    const bool volume = static_cast<Latte::E_DIM>(d.dim) == Latte::E_DIM::DIM_3D;
    Surface* exact = nullptr;
    auto range = surfaces_of(0).equal_range(d.addr);
    for (auto it = range.first; it != range.second; ++it) {
        auto* s = it->second.get();
        if (s->gpuWritten) return nullptr;
        if (s->isDepth == d.isDepth && s->width == d.width && s->height == d.height && s->format == d.format &&
            s->slices == d.slices && (s->imageType == VK_IMAGE_TYPE_3D) == volume && s->mips >= d.mips &&
            (!exact || s->writeSeq > exact->writeSeq))
            exact = s;
    }
    if (!exact || !stereo_shared_texture(exact)) return nullptr;
    stereo_hold_texture(exact);
    return exact;
}
void stereo_hold_texture(Surface* s) {
    if (scene_thread::on_scene_b()) [[unlikely]] {  // (docs/VR_SCENE_THREADS.md, stage S6: its own generation)
        s->eyeHeldB = scene_thread::b_eye_generation();
        return;
    }
    if (eye_thread::on_eye_thread()) eye_thread::check(s);  // (stage E5: eye 0's texture as of this draw)
    s->eyeHeld = stereoFlushGeneration;
}
namespace {
double g_flushMs = 0;  // (diagnostics) time in stereo_flush with held commands, since the last stereo_flush_ms(true)
}
double stereo_flush_ms(bool reset) {
    const double v = g_flushMs;
    if (reset) g_flushMs = 0;
    return v;
}
namespace {
// (stage E2) each eye's pass ends at the end of its segment: eye 1's, then eye 0's
void end_segment_passes() {
    if (eye_thread::running()) {
        // (stage E4) eye 1's pass is its thread's: a cut or its finish ends it there
        if (stereoEye()) swap_pass();
        eye_deferred_commands() = eye0_segment_stream();
        end_encoder();
        if (stereoEye()) swap_pass();
        return;
    }
    if (stereoEye() == 0) swap_pass();
    eye_deferred_commands() = eye_segment(1, segment).stream();
    end_encoder();
    swap_pass();
    eye_deferred_commands() = eye0_segment_stream();
    end_encoder();
    if (stereoEye() == 1) swap_pass();
}
uint64_t g_cuts = 0;  // (diagnostics)
}
bool stereo_streaming() { return streaming(); }
bool stereo_eye1_holding() { return eyeCommands.command_count() != 0; }
DeferredCommands* stereo_eye1_segment() {
    // the render thread's own eye 1 pass (it drew eye 1 since the last flush) ends in the segment first: eye 1's
    // thread opens its own there
    if (stereoEye() == 0 && !eye_thread::running()) {
        swap_pass();
        DeferredCommands* const previous = eye_deferred_commands();
        eye_deferred_commands() = eye_segment(1, segment).stream();
        end_encoder();
        eye_deferred_commands() = previous;
        swap_pass();
    }
    return eye_segment(1, segment).stream();
}
uint64_t stereo_take_cuts() { const uint64_t c = g_cuts; g_cuts = 0; return c; }
void stereo_cut() {
    // (stage E2) eye 1's commands so far go before eye 0's from here on, without emitting anything. A redirect that is
    // active (a scope recording into another stream) stays: only the attached stream moves to the next segment
    DeferredCommands* const previous = eye_deferred_commands();
    const bool attached = previous == (stereoEye() ? eye_segment(1, segment).stream() : eye0_segment_stream());
    ++stereoFlushGeneration;
    end_segment_passes();
    ++segment;
    ++g_cuts;
    if (eye_thread::running()) eye_thread::cut(eye_segment(1, segment).stream());  // (stage E4)
    if (attached) attach_eye_stream();
    else eye_deferred_commands() = previous;
}
void stereo_release_flush() {
    // (the scene's thread flushes the eye 1 halves it holds, into its part: docs/VR_SCENE_THREADS.md, stage S5)
    if (scene_thread::on_scene_thread()) { scene_thread::flush_eye1(); return; }
    // (a draw thread: the atlas's stream goes before eye 1's held commands anyway; eye 1's thread's own commands and
    // changes are in one stream, in order)
    if (draw_threads::on_draw_thread()) return;
    if (stereo_eye_segments() && streaming()) {
        stereo_cut();
        return;
    }
    // (stage S7) beside the second drawing thread, maybe inside a cache lock: no join; into the part, after eye 0's
    if (switches::kSceneFront && partHolding && !sceneHolding) {
        if (!eyeCommands.command_count()) return;
        ++stereoFlushGeneration;
        if (stereoEye() == 0) swap_pass();
        eye_deferred_commands() = &eyeCommands;
        end_encoder();
        swap_pass();
        eye_deferred_commands() = part_stream();
        end_encoder();
        if (stereoEye() == 1) swap_pass();
        try {
            auto held = eyeCommands.take();
            for (auto& command : held) record::emit_owned(std::move(command));  // (into the part)
        } catch (...) { attach_eye_stream(); throw; }
        attach_eye_stream();
        return;
    }
    if (!sceneHolding) { stereo_flush(); return; }
    // (stage 3.2) inside a scene draw while the atlas's thread catches up, maybe inside a cache lock: no join. Eye 1's
    // held commands continue the scene's held stream instead, after eye 0's: both go after the atlas's at the join
    if (!eyeCommands.command_count()) return;
    ++stereoFlushGeneration;
    if (stereoEye() == 0) swap_pass();
    eye_deferred_commands() = &eyeCommands;
    end_encoder();
    swap_pass();
    eye_deferred_commands() = scene_stream();
    end_encoder();
    if (stereoEye() == 1) swap_pass();
    try {
        auto held = eyeCommands.take();
        for (auto& command : held) record::emit_owned(std::move(command));  // (into the scene's stream)
    } catch (...) { attach_eye_stream(); throw; }
    attach_eye_stream();
}
void stereo_flush() {
    if (scene_stats::kOn) [[unlikely]] scene_stats::eye1_flush();
    // the sun atlas's held commands first: they continue the renderer's stream (stereo_atlas_hold)
    if (stereo_timeline_on() && !atlasHolding) stereo_timeline_event(g_inOrderFlush ? "in-order" : eyeCommands.command_count() ? "flush" : "flush(empty)");
    stereo_atlas_emit(g_inOrderFlush ? AtlasEmit::InOrder : AtlasEmit::Flush);
    if (stereo_eye_segments()) {
        static bool logged = false;
        if (!logged) { logged = true; LOG("[vulkan stereo] eyes' held segments: on (docs/VR_EYE_THREAD.md, stage E2)"); }
        // (stage E4) eye 1's thread finishes first: its pass ends at the end of its segment
        eye_thread::finish();
        // (stage E2) every held pair, in order: eye 0's segment, then eye 1's
        if (!held_segments()) { segment = 0; return; }
        ++stereoFlushGeneration;
        end_segment_passes();
        eye_deferred_commands() = nullptr;
        try {
            // (the first pair's eye 0 commands are in the renderer's stream already, or in the scene's, emitted with the
            // atlas's)
            for (size_t i = 0; i <= segment; ++i)
                for (int eye = i ? 0 : 1; eye < 2; ++eye)
                    if (i < eyeSegments[eye].size()) emit_held(eyeSegments[eye][i]->commands);
        } catch (...) { segment = 0; attach_eye_stream(); throw; }
        segment = 0;
        attach_eye_stream();
        return;
    }
    // Eye 1's pass is open only while its begin is still held.
    if (!eyeCommands.command_count()) return;
    static const bool timed = std::getenv("WWHD_VK_TARGET_STATS") != nullptr;
    struct Timer {
        bool on;
        std::chrono::steady_clock::time_point t0 = on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        ~Timer() { if (on) g_flushMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } timer{timed};
    ++stereoFlushGeneration;  // the held commands below no longer wait for any texture
    // End eye 1's pass at the end of its held commands, then eye 0's in the renderer's stream.
    if (stereoEye() == 0) swap_pass();
    eye_deferred_commands() = &eyeCommands;
    end_encoder();
    swap_pass();
    eye_deferred_commands() = nullptr;
    end_encoder();
    if (stereoEye() == 1) swap_pass();
    try {
        // Both eye streams refer to the same submission-owned resources. Queue
        // a pure POD eye after eye 0 rather than draining and replaying its Vulkan
        // calls on the producer. Custom owned commands retain the exact path.
        if (eyeCommands.publish_held()) { attach_eye_stream(); return; }
        auto held = eyeCommands.take();
        // The record worker owns the renderer's stream; held commands follow its queued work.
        if (auto* recorder = active_deferred_commands; recorder && recorder->pod_mode())
            drain_record_commands();
        // Through the renderer's recorder, so they stay ordered behind a capture it holds.
        for (auto& command : held) record::emit_owned(std::move(command));
    } catch (...) { attach_eye_stream(); throw; }
    attach_eye_stream();
}
StereoInOrder::StereoInOrder() {
    g_inOrderFlush = true;
    stereo_flush();
    g_inOrderFlush = false;
    ++inOrderScopes;
    eye_deferred_commands() = nullptr;
}
StereoInOrder::~StereoInOrder() {
    // The selected eye's pass may be open in the renderer's stream: eye 1's must not stay open
    // there once its commands are held again.
    if (stereoEye()) end_encoder();
    --inOrderScopes;
    attach_eye_stream();
}
void stereo_latch() {
    draw_threads::CacheLock surfacesLocked(draw_threads::surface_mutex());  // (stage 2.6b: the surface maps are shared)
    draw_threads::lock_site(17);
    // Frame boundary: the swap submitted eye 1's commands; nothing held may outlive it.
    stereo_flush();
    const bool wasOff=stereo::frame.mode==stereo::Off;
    const int wasMode=stereo::frame.mode;
    stereo::latch();
    if (stereo::frame.mode!=wasMode) {
        static const char* const names[]={"off","half side by side","full side by side","top and bottom","VR headset"};
        const int m=stereo::frame.mode;
        LOG("[stereo] 3D output: %s", m>=0 && m<=stereo::XR ? names[m] : "?");
    }
    if (stereo::frame.mode==stereo::Off) {
        if (!wasOff) {
            stereo_select_eye(0);
            // Retire images through the renderer's submission queue so any
            // pending GPU reads finish before their allocations are freed.
            for (auto& [addr,s]:surfaces_of(1)) destroy_surface_image(s.get());
            surfaces_of(1).clear();
            ++g_surface_set_gen;
            otherLinearTargets.clear();
            ++g_surface_epoch; // eye 1's fast-path continuation names released surfaces (backend.h)
            otherMainDepth=0;
            stereo_release_ao();
            destroy_surface_image(stereoRight.scan.get());
            destroy_surface_image(stereoRight.buildingScan.get());
            stereoRight.scan.reset();
            stereoRight.buildingScan.reset();
            stereoRight.buildingScanDirty=false;
            stereoRight.scanDisplayExtent=stereoRight.buildingScanDisplayExtent={};
            stereoRight.buildingScanSrgb=stereoRight.scanSrgb=false;
        }
        return;
    }
    if (!wasOff) return;
    // Switching on during play must preserve GPU-only caches (e.g. shadows
    // which the guest drew once). Guest RAM need not contain those pixels.
    std::vector<Surface*> seeds;
    for (auto& [addr,s]:surfaces_of(0))
        if(s->gpuWritten && s->image && s->imageType==VK_IMAGE_TYPE_2D && s->mips==1)
            seeds.push_back(s.get());
    // The copies read eye 0's images: they must not wait in eye 1's held commands.
    StereoInOrder ordered;
    stereo_select_eye(1);
    try {
        for (auto* s:seeds) {
            if(stereo_light_atlas(s->width,s->height,s->slices,s->isDepth)) continue;
            SurfaceDesc d;
            d.addr=s->addr;d.mipAddr=s->mipAddr;d.width=s->width;d.height=s->height;
            d.slices=s->slices;d.pitch=s->pitch;d.format=s->format;d.dim=s->dim;
            d.tileMode=s->tileMode;d.swizzle=s->swizzle;d.isDepth=s->isDepth;
            auto* dst=find_or_create_surface(d,true);
            resample(s,dst,s->arrayLayers);
            mark_gpu_written(dst);
            dst->writeSeq=s->writeSeq;
        }
    } catch (...) { stereo_select_eye(0);throw; }
    stereo_select_eye(0);
}
void load_saved_stereo_hdr() {
    std::string v;
    auto num=[&](const char* key) { return ::hostui::get(key,v) ? std::optional<float>(float(atof(v.c_str()))) : std::nullopt; };
    if(auto x=num("stereoMode"))stereo::set_mode(stereo::launchMono ? stereo::Off : int(*x));  // (--no-stereo: flat)
    if(auto x=num("stereoDepth"))stereo::set_depth(*x);
    if(auto x=num("stereoConvergence"))stereo::set_convergence(*x);
    if(auto x=num("stereoSwap"))stereo::requestedSwap=*x!=0;
#ifdef WWHD_HDR_OUTPUT
    if(auto x=num("hdrEnabled"))hdr::enabled=*x!=0;
#endif
    hdr::sdrCurve=0;hdr::format=0;
    if(::hostui::get("hdrSdrCurve",v))hdr::sdrCurve=hdr::setting(std::string_view(v));
    if(::hostui::get("hdrFormat",v))hdr::format=hdr::setting(std::string_view(v));
    if(auto x=num("hdrPaperWhite"))hdr::set(hdr::paperWhite,*x,80,500);
    const auto savedPeak=num("hdrPeak");
    if(savedPeak)hdr::set(hdr::peak,*savedPeak,200,4000);
    const bool hasPeakMode=::hostui::get("hdrPeakAuto",v);
    hdr::peakAuto=hdr::automatic_peak_setting(hasPeakMode,v,savedPeak.has_value());
    if(auto x=num("hdrExposure"))hdr::set(hdr::exposure,*x,-3,3);
    if(auto x=num("hdrHighlights"))hdr::set(hdr::highlights,*x,0,1);
    if(auto x=num("hdrMenuWhite"))hdr::set(hdr::menuWhite,*x,80,500);
    stereo::latch();
}
}
