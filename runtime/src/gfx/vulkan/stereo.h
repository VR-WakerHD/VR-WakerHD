#pragma once
#include "stereo_switches.h"
#include "backend.h"
#include "deferred_commands.h"
#include "../stereo.h"
namespace gfxvk {
// (stereoEye, the selected eye per thread: backend.h)
extern Screen stereoRight;
bool stereo_light_atlas(uint32_t width, uint32_t height, uint32_t layers, bool depth);
decltype(Renderer::surfaces)& stereo_surface_cache(const SurfaceDesc& desc);
decltype(Renderer::surfaces)& stereo_copy_cache(uint32_t addr,uint32_t width,uint32_t height,uint32_t layers);
// While stereo is on, each eye records into its own command stream: eye 0 into the renderer's,
// eye 1 into a held one (eye_deferred_commands). Selecting an eye keeps both render passes
// open; on a tile-based GPU ending a pass stores and reloads its attachments, so ending it at
// every switch cost several times the frame (issue #248). Eye 1's commands are emitted as one
// run after eye 0's pass, at every submission and wherever the eyes must stay ordered
// (stereo_flush). The eyes share only buffers (never overwritten while recorded commands use
// them), the sun atlas, which eye 0 writes and transitions first, captured photographs in
// their layout pane, and the textures neither eye renders (below).
void stereo_select_eye(int eye);
// Eye 1 samples eye 0's image of a texture the guest uploads and neither eye renders: one image
// and one upload for both eyes, and eye 1 can bind eye 0's descriptor set (draw.cpp). Shared while
// eye 1 has no surface at its address, eye 0 no render target there, and it is uploaded for this
// frame in the sampling layout, so eye 1's held commands record no transition of it.
bool stereo_shared_texture(const Surface* s);
// A captured-photo pane also shares its GPU image: eye 0 resolved and transitioned it first.
Surface* stereo_eye0_texture(const SurfaceDesc& d);  // eye 1 selected: the shared texture, else null
void stereo_hold_texture(Surface* s);  // eye 1's held commands sample s
void stereo_flush();
// The shared sun atlas's draws in a held stream of their own, on the same thread (WWHD_VK_ATLAS_STREAM=1;
// docs/VR_PERFORMANCE.md, roadmap 2b, stage 1: the step before recording them on a thread of their own). Each
// frame's atlas draws come as one block before the scene's. stereo_atlas_hold before an atlas draw (eye 0, stereo
// on) starts holding: the renderer's pass ends, the atlas's pass and stream take over. stereo_atlas_emit ends the
// atlas's pass and emits the stream whole into the renderer's, before the next other draw, every stereo_flush
// (submissions, in-order operations, the swap), an eye switch, and the operations that bypass those. Nothing is
// recorded into the renderer's stream while it holds, so the GPU gets the same commands in the same order.
inline bool stereo_atlas_stream_enabled() { return switches::kAtlasStream; }
// the atlas's held stream, its capture started, for the atlas's thread to record into (atlas_thread.h)
DeferredCommands* stereo_atlas_thread_stream();
bool stereo_atlas_holding();
void stereo_atlas_hold();
void stereo_atlas_held_draw();  // (diagnostics) one atlas draw recorded while holding
// why the held commands were emitted (diagnostics): the next other draw, a stereo_flush (a submission or the swap), an
// in-order operation (a clear or copy), an eye switch, or an operation that bypasses those (peek_z, write-back)
// With the atlas's thread (stage 3.2) also why the threads joined in the scene: a draw that may sample the atlas, an
// invalidate, the render thread out of commands.
enum class AtlasEmit : int { Draw, Flush, InOrder, Eye, Other, Sampled, Invalidate, Idle, Count };
void stereo_atlas_emit_held(AtlasEmit why);
void stereo_atlas_join_held(AtlasEmit why);
// The scene's part (docs/VR_SCENE_THREADS.md, stage S1; WWHD_VK_SCENE_SPLIT=1, off by default): from a draw inside the
// frame's largest two-eye pass, eye 0's commands go into a held stream of their own while the pass stays open, recorded
// as a second thread would: from an empty draw-state cache and no continued draw (g_partEpoch). The part is emitted
// whole into the renderer's stream, eye 0's pass ending in it, when that pass ends and before anything that must come
// after it: a stereo_flush (eye 1's held commands, submissions, in-order operations, the atlas's block, the swap) and
// the operations that bypass those. An eye switch does not end it (stage S2): eye 1's draws go into eye 1's held stream
// as always, and eye 0's continue the part when eye 0 is selected again. On one thread the GPU gets the same draws in
// the same order, with more binds. Off, nothing is held and each hook is one constant test.
inline uint64_t g_partEpoch = 0;  // moves at each part's start: draw-state caches and continued draws begin again
// (stage S6) a drawing thread's own moves: its next draw binds everything itself (the second drawing thread's commands
// went between, or a draw of its own was undone)
inline thread_local uint64_t t_partEpochSalt = 0;
inline uint64_t part_epoch() { return g_partEpoch + (t_partEpochSalt << 40); }
bool stereo_part_holding();
void stereo_part_hold();               // before a draw: holds from here if nothing else is held or redirected
void stereo_part_join(AtlasEmit why);  // emits the part, eye 0's pass ending in it first
// (diagnostics, stage S5) where the threads joined: counted, then stereo_part_join
enum class PartJoinSite : int { Flush, InOrder, EndEncoder, Eye1, Recording, DrawState, Idle, Count };
void stereo_part_join_at(PartJoinSite site);
void stereo_part_site_stats(uint64_t (&sites)[int(PartJoinSite::Count)]);  // since the last call
void stereo_part_pass_ended();         // (end_encoder) eye 0's pass ended in the part's stream: emits it
bool stereo_part_eye1_held();          // (stage S4, the scene's thread) the part's eye 1 stream holds commands
// (stage S5, the scene's thread) its in-order operation's scope, as StereoInOrder's on the render thread; the part's eye 1
// commands moved after its eye 0 ones (false: some were not POD, and went in as owned commands)
void stereo_thread_in_order(bool on);
bool stereo_part_append_eye1();
// (diagnostics) parts held, and why they were emitted (their pass ended, a stereo_flush, anything else), since the last call
void stereo_part_stats(uint64_t& holds, uint64_t& passEnds, uint64_t& flushes, uint64_t& others);
// (diagnostics) parts queued to the record worker whole or replayed after draining it, and the joins' wait and emit time
void stereo_part_join_stats(uint64_t& published, uint64_t& exact, double& waitMs, double& emitMs);
// (render thread, the automatic tail) this frame's time waiting at the parts' joins, then 0
uint64_t stereo_part_take_frame_wait_ns();
// (render thread, the tail's rule, draw.cpp) a part's join with the scene's thread, its start and end (steady clock
// ns); a join site reached (before its join): the tail drawn here ends there
void scene_tail_note_join(uint64_t t0, uint64_t t1);
void scene_tail_flush();
double stereo_part_busy_during_join_ms();  // (diagnostics) the scene thread's draw time while the render thread waited
// (draw.cpp) the swap: the next frame's split point, from this frame's passes; every 600 frames a log line
void scene_split_frame_end();
// (docs/VR_SCENE_THREADS.md, stages S3 and S4; WWHD_VK_SCENE_THREAD=1, off by default; draw.cpp) the scene's parts
// recorded on a draw thread of their own, beside the render thread: it hands each draw over and goes on (S4). A part
// starts only while eye 1 holds no commands. The thread draws a draw not drawn for both eyes as two, its eye 1 half
// into the part's eye 1 stream (moved into eye 1's held stream when the threads join); the threads then join before the
// next draw, so the part's later draws are drawn for both eyes again. The render thread records nothing meanwhile:
// anything it would record joins the threads first (deferred_commands.h g_partOverlapping).
namespace scene_thread {
bool enabled();          // the switch, and no eye 1 thread (whose upload arena and descriptor pool it takes)
bool running();          // (render thread) a part is being recorded there
bool on_scene_thread();  // the calling thread is it
// (render thread) eye 0's open pass and the registers to it, the part's stream and its eye 1 stream
void begin(DeferredCommands* stream, DeferredCommands* eye1Stream);
void finish();           // (render thread) its pass ended at the end of the part, every draw done
bool hand_state(int kind, const uint32_t* words, uint32_t n);  // (stage S5) render::hand_draw_state
// (stage S5, the scene's thread) eye 1's held halves into the part, as stereo_flush would: eye 1's pass ends, eye 0's
// pass ends, eye 1's commands follow eye 0's
void flush_eye1();
uint64_t busy_ns();  // (diagnostics) its draws' time so far
// (stage S5, render thread) a clear or copy recorded there in order, from copies of its guest structs: false if no part runs
bool clear_color(const uint32_t* colorBuffer, const float* rgba);
bool clear_depth_stencil(const uint32_t* depthBuffer, float depth, uint32_t stencil, uint32_t flags);
bool copy_surface(const uint32_t* src, uint32_t srcMip, uint32_t srcSlice, const uint32_t* dst, uint32_t dstMip,
                  uint32_t dstSlice);
bool copy_to_scan(const uint32_t* colorBuffer, uint32_t target);
// (stage S6) the second drawing thread
bool on_scene_b();       // the calling thread is it
bool eye1_held();        // (either drawing thread) its own eye 1 halves are held
uint64_t& b_eye_generation();  // its eye 1 holds' generation (Surface::eyeHeldB)
// (the second drawing thread) a surface's state is about to change (atlas_overlap.h before_change): it waits for the
// scene's thread's span before it first, or its draw is undone and drawn again after it
void b_before_change(Surface* s, const char* what);
// (docs/VR_SCENE_THREADS.md, "S6: B's reads deferred"; the second drawing thread) a layout change of a surface, before
// A1's end: true if it is left out, the surface made so (read, or drawn into) for the span's start by the scene's thread
// instead, between the threads' commands, once both are done. A later other change of it makes it so first
bool b_defer_layout(Surface* s, int layout, uint32_t stage, uint32_t access);
int b_layout(const Surface* s);  // the layout its commands find it in: the deferred one, else its own
void b_commit_reads();  // (the scene's thread at A1's end, the second drawing thread done)
void b_commit_read(Surface* s);
// a sampled surface's copy from a newer alias (surfaces.cpp adopt_newer_alias): true if it is left out, made at A1's end
bool b_defer_adopt(Surface* s);
// its rendered mip chain built again (surfaces.cpp with_mip_chain): true if it is left out, built at A1's end
bool b_defer_chain(Surface* s, const SurfaceDesc& d, uint32_t maxLevels);  // (the second drawing thread) one of them now, before it changes it otherwise
// (render thread, frame end) the last frame's main geometry run, in the frame's scene draws: [first, end)
void set_main_run(uint32_t first, uint32_t end);
void log_b(double frames, double runDraws);  // (diagnostics, every 600 frames)
// (the second drawing thread, S6 tuning) a texture its draws sampled: made ready at the next part's start, on the
// render thread before the scene's thread's span, so it finds it settled and need not wait for that span
void note_texture(const uint32_t* words, bool isDepthSampler, uint32_t maxLevels, Surface* s);
// (stage S7, WWHD_VK_SCENE_FRONT=1) the second drawing thread draws the front of the frame's largest two-eye pass (its
// span, from last frame's), while the render thread reads on; at the span's end the render thread draws the rest of the
// pass itself, into the part (stereo_part_hold), beside the thread. The GPU gets the thread's commands, then the
// part's: the guest's order. The two overlap as the sun atlas's thread and the scene do (atlas_overlap.h): the thread
// comes first on the GPU, so the render thread marks what it uses and changes, the thread marks what it uses, and a
// change one makes to what the other has used in this overlap is a conflict (S7 off for the session, logged). The
// threads join (front_join, from the part's emission) at a draw into other targets or sampling the pass's, a
// submission, an in-order operation, an invalidate, the swap
bool front_draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                uint32_t baseVertex, uint32_t instances);  // (render thread) true: handed to the thread
bool front_span();        // (render thread) the thread's span runs: the render thread records nothing
bool front_overlapping(); // (render thread) the thread's span ran in this part: not joined yet
void front_end_span();    // (render thread) the span ends here (the render thread records from now on, into the part)
void front_join();        // (render thread, the part's emission) wait for the thread, its commands first
void front_used(Surface* s);                     // a surface a draw uses (either thread)
void front_change(Surface* s, const char* what); // a surface's state about to change (either thread)
void front_frame_end(uint32_t draws);  // (frame end) next frame's split: this frame's longest stretch into the same targets
void front_log(double frames);  // (diagnostics, every 600 frames)
}  // namespace scene_thread
// (stage S6, render thread) the second drawing thread's held streams, and their emission after the scene's thread's span
// (S6.1: not used yet). False if a stream had to be emitted command by command (not wholly POD)
DeferredCommands* stereo_part_b_stream();
// ("S6: B's reads deferred") the layouts the second drawing thread's span starts with, made by it after A1's end: emitted
// before its commands
DeferredCommands* stereo_part_b_pre_stream();
void adopt_newer_alias_now(Surface* s);  // (surfaces.cpp) the copy from a newer alias, from its eye's map
void with_mip_chain_now(Surface* s, const SurfaceDesc& d, uint32_t maxLevels);  // (surfaces.cpp) its eye's levels
DeferredCommands* stereo_part_b_eye1_stream();
bool stereo_part_emit_b();
bool stereo_part_b_eye1_held();
bool stereo_part_b_append_eye1();  // (the second drawing thread) its eye 1 halves after its eye 0 commands
void stereo_front_emit_b();        // (stage S7, render thread) its commands into the renderer's stream, before the part
// (nothing to do unless the atlas's held stream is on: one constant test, every draw)
inline void stereo_atlas_emit(AtlasEmit why = AtlasEmit::Other) {
    if (switches::kSceneHooks && why != AtlasEmit::Draw && why != AtlasEmit::Eye)
        stereo_part_join_at(why == AtlasEmit::InOrder ? PartJoinSite::InOrder : PartJoinSite::Flush);
    if (switches::kAtlasStream) stereo_atlas_emit_held(why);
}
// the held commands emitted whatever the reason: with the atlas's thread the threads join (never inside a cache lock)
inline void stereo_atlas_join(AtlasEmit why) {
    if (switches::kSceneHooks && why != AtlasEmit::Draw && why != AtlasEmit::Eye)
        stereo_part_join_at(why == AtlasEmit::Idle ? PartJoinSite::Idle : PartJoinSite::Flush);
    if (switches::kAtlasStream) stereo_atlas_join_held(why);
}
// (stage 3.2) the scene's eye 0 commands are held while the atlas's thread catches up
bool stereo_scene_holding();
// (diagnostics) the stereo streams and passes now, as one log line's text
std::string stereo_debug_state();
// (stage 3.2) while the scene holds, the render thread's commands that upload a game texture of eye 0 or change its
// layout, before the atlas's thread has used it in this overlap, go into a stream emitted before the atlas's: where
// the atlas's own first use would have put them (its draws come first in the guest's order), so its thread finds it
// ready. A texture the atlas's thread has used stays in the scene's stream, after the atlas's.
bool stereo_before_atlas_held(const Surface* s);
inline bool stereo_before_atlas(const Surface* s) { return switches::kAtlasStream && stereo_before_atlas_held(s); }
DeferredCommands* stereo_before_atlas_stream();
// (render thread) a scope recording into that stream when stereo_before_atlas(s)
struct BeforeAtlas {
    explicit BeforeAtlas(const Surface* s);
    ~BeforeAtlas() noexcept(false);
    BeforeAtlas(const BeforeAtlas&) = delete;
    BeforeAtlas& operator=(const BeforeAtlas&) = delete;
    const bool on;
    DeferredCommands* const previous;
};
// (diagnostics) holds, the atlas draws they held, and the emits by reason since the last call
void stereo_atlas_stats(uint64_t& holds, uint64_t& draws, uint64_t (&why)[int(AtlasEmit::Count)]);
// (diagnostics, WWHD_VK_ATLAS_TIMELINE=1) one frame's events by draw number, logged at TV frames 2400 and 3000: the
// atlas's holds and emits (with why), and the first time the atlas is made ready for sampling (the scene's first use
// of it). Where the threads would have to join decides how much of the atlas's work can overlap the scene's
inline bool stereo_timeline_on() { return switches::kAtlasTimeline; }
void stereo_timeline_draw();  // one draw (draw_stereo)
void stereo_timeline_event(const char* what);
void stereo_timeline_layout(const Surface* s, VkImageLayout layout);  // transition_image
void stereo_timeline_texture(const Surface* s, bool atlasDraw);    // a texture bound by a draw (bind_stage)
extern uint64_t stereoFlushGeneration;  // advanced by every stereo_flush that emits commands
// Before s changes (a layout transition): eye 1's held commands that sample it run first.
void stereo_release_flush();  // stereo_flush, but no join with the atlas's thread (stereo.cpp)
// (the eyes' held segments) eye 1's commands so far go before eye 0's from here on: a cut, no wait (stage E2)
void stereo_cut();
// (docs/VR_EYE_THREAD.md, stage E2; WWHD_VK_EYE_SEGMENTS=1) both eyes' commands held in segments, cut by in-draw
// flushes and emitted in order by stereo_flush
inline bool stereo_eye_segments() { return switches::kEyeSegments; }
bool stereo_streaming();  // both eyes' commands are held per eye now (stereo on, no in-order scope)
// (docs/VR_MULTIVIEW.md, stage M4) eye 1's held stream has commands (not with the held segments): a multiview draw,
// recorded in eye 0's stream for both eyes, comes after them, so stereo_flush emits them first
bool stereo_eye1_holding();
DeferredCommands* stereo_eye1_segment();  // eye 1's current held segment, its capture started (eye_thread.h)
uint64_t stereo_take_cuts();  // (diagnostics) cuts since the last call
inline void stereo_release_texture(Surface* s) {
    if (scene_thread::on_scene_b()) [[unlikely]] {  // (stage S6: its own holds)
        if (s->eyeHeldB == scene_thread::b_eye_generation()) stereo_release_flush();
        return;
    }
    if (s->eyeHeld == stereoFlushGeneration) stereo_release_flush();
}
void stereo_invalidate(uint32_t flags, uint32_t addr, uint32_t size);
// Operations other than draws keep the earlier interleaved order: eye 1's held commands are
// emitted first, then each eye runs in turn in the renderer's stream, ending its pass.
struct StereoInOrder {
    StereoInOrder();
    ~StereoInOrder();
    StereoInOrder(const StereoInOrder&) = delete;
    StereoInOrder& operator=(const StereoInOrder&) = delete;
};
// Wrap complete guest operations, including clears/copies, so intermediate
// color, depth, history and alias images stay in the corresponding eye's graph.
template<class F> void stereo_pair(F&& f, bool force = false) {
    if (stereo::frame.mode == stereo::Off && !force) {
        stereo_select_eye(0);
        f();
        return;
    }
    // Clears and copies may reach the shared sun atlas (stereo_copy_cache).
    // Canonical guest reads explicitly select the left eye.
    StereoInOrder ordered;
    try {
        stereo_select_eye(0);
        f();
        stereo_select_eye(1);
        f();
        stereo_select_eye(0);
    } catch (...) { stereo_select_eye(0); throw; }
}
// Renderer-owned commands (command buffer begin, submission) go to the renderer's stream
// even while eye 1 is selected.
struct RendererCommands {
    DeferredCommands* eye = eye_deferred_commands();
    RendererCommands() { eye_deferred_commands() = nullptr; }
    ~RendererCommands() { eye_deferred_commands() = eye; }
    RendererCommands(const RendererCommands&) = delete;
    RendererCommands& operator=(const RendererCommands&) = delete;
};
void stereo_release_ao(); // retire only eye 1's scratch images
bool stereo_screen_target(uint32_t width, uint32_t height);
// (docs/VR_EYE_PROJECTION.md) the game's UI drawn this frame into its own image (each eye with its own projection: the
// quad layer shows it), or null (draw.cpp ui_layer)
Surface* ui_layer_image();
// (the frame dumps) the UI layer's image as the last frame with UI left it, or null
Surface* ui_layer_drawn_last();
// (diagnostics, WWHD_VK_TARGET_STATS) ms spent flushing eye 1's held commands since the last reset
double stereo_flush_ms(bool reset);
// s's allocation is a Half SBS (halfWidth) or top-and-bottom (halfHeight) eye of the guest's size;
// false for both when its format cannot be scaled on this device (each eye then keeps full size)
void stereo_target_halves(const Surface* s, bool& halfWidth, bool& halfHeight);
// renderer smoke test: eye-sized targets again after a test of the full-size fallback
void stereo_reset_eye_sized_targets();
// advanced when eye-sized targets are switched off (or on): targets looked up before then may
// still have the other size, so a draw resolves its attachments again (draw.cpp)
uint32_t stereo_eye_size_generation();
void stereo_latch();
void load_saved_stereo_hdr(); // start-up, both hosts: 3D and HDR settings saved by the overlay
}
