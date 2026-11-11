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
