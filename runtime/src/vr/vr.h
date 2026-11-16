// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR mode (docs/VR_ARCHITECTURE.md): the state shared by the game thread (vr_game.cpp: camera, input,
// sword) and the render thread (gfx/vulkan/vr_vulkan.cpp: Vulkan binding, swapchains, frame loop).
//
// The game draws each frame with one located set of views (eye poses and fields of view). Those views are
// kept in a small ring under an id; the id travels with the frame's swap (gx2 OP_VR_VIEWS), so the frame
// is submitted to the compositor with exactly the poses it was rendered with, and the runtime's
// reprojection corrects only the difference to the head's newest pose.
#pragma once
#include <cstdint>
#include <string>

#include "vr_bindings.h"
#include "vr_config.h"
#include "vr_math.h"

#ifdef WWHD_VR
#include "openxr_session.h"
#endif

namespace vr {

// command line: --vr (1) / --no-vr (0) override the saved "vr.enabled" for this start (-1: the setting)
void set_launch_request(int request);
// whether this start tries VR (setting or command line, WWHD_VR=0/1 overrides both)
bool wanted();

Config config();
// vr.desktopView / vr.desktopViewFill (vr_config.h) for the renderer's desktop window, each frame without the settings'
// lock: 0 left eye, 1 right eye, 2 both side by side, 3 off, 4 / 5 both left / right dominant; `fill`: the picture fills the window (cropped)
int desktop_view(bool& fill);
namespace game {
// the telescope's magnification for a main-camera projection with the game's field of view (degrees): 1 outside it
float telescope_magnification(float gameFovyDegrees);
// the Picto Box's viewfinder size (vr.pictoBoxSize) while one is aimed in VR, else 1: the photo's frame shrinks with it
float picto_frame_scale();
// (which of the game's UI the VR view shows, and where: vr_ui.h)
// the VR view is the third-person one (the right stick's click toggles it in normal play)
bool third_person_view();
// the VR view is one of the boat's views, the Red Lion's or the Free Roam Camera (vr_sail_view.h): the game's HUD is
// not shown (the Free Roam Camera shows its "Free Roam" label instead)
bool boat_view();
bool bomb_in_hand();  // (vr_game.h) first-person play with a bomb in the hand
// Link conducts with the Wind Waker in VR: its bar of notes and beat is drawn in the middle of the view (aspect.cpp)
bool conducting_view();
// Link looks through the telescope
bool telescope_looking();
// Link looks through the telescope
bool telescope_looking();
// The game's edge HUD in normal play (game thread, at its layout calculation; docs/VR_HUD.md): the share of the way
// from the view's centre to the picture's edges it is drawn at (vr.hudInset) and its size (vr.hudSize). false (1, 1)
// everywhere else.
bool hud_layout(float& inset, float& size);
}
// game units per real metre in use: matched to Link's arm (vr.matchLinkBody) or vr.unitsPerMetre
float world_scale();
// (turn smoothing) how fast the view turns, in radians a second of its heading in the game (the game heading is the
// mapping's heading plus the head's yaw in the tracking space), as the game thread measured it at its last camera; 0 not
// turning (or a snap, a cut). The render thread turns a frame shown again by it (vr_vulkan.cpp headset_frame)
void set_view_turn_rate(float radiansPerSecond, int64_t forDisplayTime = 0);
float view_turn_rate();
// (2026-11-12) the rate only if it was set for the frame of these views (their display time): a frame drawn with the game's
// own camera (an event, a transition, Link seen from behind) sets none, and the last first-person rate, left standing,
// turned the frames made from it while its own picture did not turn (a captured sequence: the generated frames ran ahead
// and the game's frames jumped back 0.5 to 2.2 degrees)
float view_turn_rate_for(int64_t displayTime);
// (movement smoothing) how fast the view moves through the game, as metres a second in the tracking space (the way a
// point fixed in the game moves against the view, negated: the camera's own velocity), measured with the turn rate; 0
// not moving (or a jump, a cut, the boat)
void set_view_velocity(Vec3 metresPerSecond);
// the display time of the last real frame begun (the game's own frames, vr_vulkan.cpp frame_begin), ns; 0: none yet.
// Frames shown again or generated also wait for the runtime and move its last prediction on: the game's camera is
// predicted from this one only (vr_game.cpp display_time)
// `swap`: the swap that frame belongs to (gx2::swaps_executed() + 1 in frame_begin; 0: not known). The two are kept
// together (one is never read with the other's older value): display_time counts the swaps after that one, not the
// swaps not yet executed, which still counted this one for a moment after its display time was set (the camera
// predicted a refresh too far: docs/VR_STREAMING_AUDIT.md, finding 2)
void set_real_frame_display(int64_t displayTime, uint64_t swap = 0);
// (docs/VR_HANDS_LAYER_PLAN.md) the hands layer on this frame (the renderer decides: the setting, 30/45 fps); read by
// the game's side to mark Link's draw lists
void set_hands_layer_on(bool on);
bool hands_layer_on();
// (diagnostics, 2026-11-14, the Tower of the Gods' black/teal figurehead: one run that tries steady hands' parts in turn,
// WWHD_VR_HANDS_AB=<seconds a phase>) the phase now: -1 off; 0 steady hands as in play, 1 all of it off, 2 off but Link's
// lists still split and bracketed, 3 on but Link drawn into the eye's picture (no hands image)
void set_hands_ab_mode(int mode);
int hands_ab_mode();
int64_t real_frame_display(uint64_t* swap = nullptr);
Vec3 view_velocity();
// (generated frames) where the stick's turn turns the view about, in the tracking space: the game turns the mapping's
// heading, which swings the head's offset from the room-scale origin (held within `limit` metres sideways,
// gameplay_head) about that origin, not about the head itself (vr_game.cpp first-person view)
void set_turn_pivot(Vec3 origin, float limit);
bool turn_pivot(Vec3& origin, float& limit);
void set_link_arm_length(float units);  // measured from his skeleton (vr_game.cpp)
void set_config(const Config& c);  // applies at once and saves the vr.* settings
void load_settings();              // the saved vr.* and vr.bind.* settings (hostui); defaults when missing or unreadable
Bindings bindings();               // which GamePad button each controller action presses
void set_bindings(const Bindings& b);  // applies at once and saves vr.bind.*

// Why VR is not running ("" while it runs, or before it was tried): the settings overlay and log say it.
std::string status();
void set_status(const std::string& s);

// true while a session runs and frames go to the headset (any thread)
bool active();
void set_active(bool on);
// (2026-11-07) the headset taken off a standalone headset (Quest, Lepton): the app sent to the background (Android's
// lifecycle) or the session out of focus (OpenXR: synchronized, visible, idle, stopping). While away the game is held,
// as with the VR menu (vr_game.cpp menu_pause_pass), and the renderer's record worker is stopped (backend.cpp), started
// again once the headset is back: a Quest 3 crashed in the Adreno driver, in a draw the record worker replayed, as the
// headset came back on. Steam Frame - Native too, from the session's focus (2026-11-08). Always false elsewhere
void set_app_background(bool on);
void set_xr_unfocused(bool on);
// the session idle (it ended or has not begun): the next one holds the game only once it has been focused
void set_xr_session_idle();
bool headset_away();

#ifdef WWHD_VR
xr::Session& session();

// ---- views ring (game thread writes, render thread reads)
constexpr uint32_t kViewRing = 8;
// store the views a frame is drawn with; returns their id (never 0)
// gameTime: the game time the frame shows (logic steps, with the frame interpolation's fraction; diagnostics)
uint32_t store_views(const xr::Views& v, double gameTime = -1);
// the views stored under id (false when the id is 0 or already overwritten); *gameTime when given
bool views(uint32_t id, xr::Views& out, double* gameTime = nullptr);
// game thread: the views the frame being built is drawn with (camera_draw), and at its swap the id to send
// with it (read once: a frame drawn without the game camera sends 0)
void set_frame_views(uint32_t id);
uint32_t take_frame_views();
// render thread: the id that came with the swap being presented
void set_render_views(uint32_t id);
uint32_t render_views();
// The views the renderer draws for located views (stereo::XR): both eyes with the head's orientation and
// the smallest symmetric field of view containing both, each moved sideways from the head by its own
// measured distance (parallel cameras). These are what the frame is submitted with; eyeX receives the
// eyes' sideways offsets (metres, tracking space units).
xr::Views as_rendered(const xr::Views& located, float eyeX[2]);
// the symmetric field of view the game's camera draws (both eyes' union, with fov_margin)
Fov drawn_fov(const xr::Views& located);
// (docs/VR_EYE_PROJECTION.md) each eye drawn with its own field of view (default; WWHD_VR_EYE_PROJECTION=shared: the symmetric one)
bool own_eye_projection();
// the eye pictures' shape (width / height) the render targets take: each eye's own field's with own_eye_projection,
// else 0 (the symmetric field's: projection)
float eye_image_aspect();
void set_eye_image_aspect(float a);
// (docs/VR_SETTINGS.md, "Headset resolution", 2026-11-08) the size each eye's picture is drawn at: the runtime's
// recommended eye image times vr.renderScale, set at the session's start (0 x 0 before it and after it ends). The
// renderer sizes its screen targets to it (gfx/vulkan/surfaces.cpp latch_headset_resolution) while each eye has its
// own projection (eye_image_aspect > 0): copied 1:1 into the headset's image, or upscaled into it on a standalone headset
void set_picture_size(uint32_t width, uint32_t height);
bool picture_size(uint32_t& width, uint32_t& height);
// each eye's own field of view, the one its picture covers (with own_eye_projection; set each frame by the renderer):
// false without one. The UI layer covers projection()'s field, so a layout's place in an eye's picture is off-centre
// across (the Picto Box's black and photo: vr_vulkan.cpp eye_part, aspect.cpp picto_crop)
bool eye_field(int eye, Fov& out);
void set_eye_fields(const Fov& left, const Fov& right, bool on);
// (radians) how far beyond the eyes' fields of view each eye is drawn and submitted, so that a frame the runtime
// shows again, turned to the head (timewarp: at 30 frames/s on a 72 or 90 Hz headset every frame is), has picture
// where it moves in instead of its edge pixels stretched (a Quest 3 report: "stretched pixels on the edges of the
// screen, like the bottom and the right"). 5 degrees on Android, none elsewhere; WWHD_VR_FOV_MARGIN=<degrees>
float fov_margin();
// (docs/VR_EYE_PROJECTION.md "Field of view") the share of each eye's field (its tangents) drawn this session:
// vr.fieldOfView in 0.05 steps, 0.75 .. 1, latched at the session's start (latch_fov_scale); 1 in PC VR
float fov_scale();
void latch_fov_scale();
#endif

// Menus (game thread sets, render thread reads): the size the TV's layouts are drawn at (1: as the game draws
// them; menu scenes 1 / vr.menuZoomOut)
float layout_scale();
void set_layout_scale(float s);
// ... and moved vertically (share of the half view height, + up)
float layout_offset_y();
// the TV's layouts drawn shrunk into the native 16:9 screen and clipped to it (conversations, cutscenes, menu
// scenes; not the telescope's eyepiece): aspect.cpp then places their panes as on a 16:9 screen
bool layouts_shrunk();
void set_layout_offset_y(float y);
// ... and horizontally (share of the half view width, + right): a conversation's text held in the world in first person
float layout_offset_x();
void set_layout_offset_x(float x);
// (docs/VR_CUTSCENE_TEXT.md, "Dialogue in the UI layer") a conversation's text held in the world by the UI layer's quad:
// the direction it is held in, in the headset's tracking space (game thread sets, render thread reads)
void set_talk_text_dir(bool on, Vec3 dir = {});
bool talk_text_dir(Vec3& dir);
// the telescope's eyepiece: the share of the eye picture (width, height, centred) that is shown; the rest is black
// (0, 0: no eyepiece). Game thread sets it with the layouts, the render thread reads it for the eye copies
void set_eyepiece(float width, float height);
bool eyepiece(float& width, float& height);

// Per-eye rendering for the renderer (render thread; latched at the frame boundary with stereo::latch):
// the eye's horizontal offset from the centre camera in view space (game units, - left / + right).
struct EyeOffsets {
    float x[2] = {0, 0};
};
EyeOffsets eye_offsets();
void set_eye_offsets(const EyeOffsets& e);
// The field of view the game's camera uses in VR (symmetric, both eyes): the game thread puts it into the
// camera (fovy, aspect) and the culling frustum (aspect.cpp), the renderer draws each eye with it.
bool projection(Fov& out);
void set_projection(const Fov& f, bool on);

// The VR work on the game thread (docs/VR_PERFORMANCE.md), by part, timed in each entry point of vr_game.cpp and
// reported per frame shown with `[vr] frame times` (vr_vulkan.cpp): the game thread's own frame time is close to the
// 11.1 ms budget, and this is what VR adds to it. Two clock reads per call, no allocation
enum class GameWork : int { Camera, Pad, Execute, Arms, Draw, Count };
void add_game_work(GameWork part, uint64_t ns);
// the nanoseconds of each part since the last call, and starts again
void take_game_work(uint64_t (&ns)[int(GameWork::Count)]);
// (diagnostics, docs/VR_SCENE_THREADS.md "S5b in the headset") the camera's pose prediction: of the camera frames since
// the last call, how many were predicted 0 to 4 frames ahead (vr_game.cpp display_time), and how many changed that
// count from the camera frame before; starts again
namespace game { void take_camera_ahead(uint64_t (&ahead)[5], uint64_t& changes); }  // (vr_game.cpp)
// times the scope as `part`
class GameWorkTimer {
public:
    explicit GameWorkTimer(GameWork part);
    ~GameWorkTimer();
    GameWorkTimer(const GameWorkTimer&) = delete;
    GameWorkTimer& operator=(const GameWorkTimer&) = delete;

private:
    GameWork part_;
    uint64_t start_;
};

}  // namespace vr
