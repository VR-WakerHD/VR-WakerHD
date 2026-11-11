// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR settings (docs/VR_CONTROLS.md): saved with the other settings as "vr.*" key=value lines
// (hostui::get / hostui::set, the form motion.h uses for "gyro.*"). Every value is validated and clamped;
// an unknown key or an unreadable value keeps the default, so a damaged settings file never stops the game.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "../platform/quest_theater.h"
#include "vr_dynres.h"
#include "vr_framesynth.h"
#include "vr_upscale.h"

namespace vr {

enum class Turn : int { Snap = 0, Smooth = 1 };
enum class MoveDirection : int { Head = 0, LeftHand = 1 };
enum class Posture : int { Standing = 0, Seated = 1 };
enum class GamePadScreen : int { Menus = 0, Always = 1, Off = 2 };  // the GamePad screen as a panel in the headset
// where the menu panel goes: where the head looks when it opens (then fixed in the room), locked to the head,
// or straight ahead of the tracking space's forward direction (the recentred direction)
enum class MenuPlacement : int { Gaze = 0, Head = 1, Front = 2 };
// vr.eyeCrop (docs/VR_MENU.md, "Double vision on Linux"; "Bubble vision on Quest and VDXR"): each eye submitted as
// only the part of the symmetric image that shows its own field of view, with that part's exact field of view (the
// display's projection), or the whole symmetric image for the runtime to reproject. Auto: cropped everywhere except
// SteamVR on Windows (which reprojects the whole image right); on, off (the default): always, never. With each eye drawn
// with its own projection (the default, vr::own_eye_projection) the whole image is the eye's field: nothing to crop; it
// matters only with WWHD_VR_EYE_PROJECTION=shared
enum class EyeCrop : int { Auto = 0, On = 1, Off = 2 };
inline bool eye_crop_on(EyeCrop m, bool onLinux, bool steamvr) {  // (not "linux": a macro on Linux)
    return m == EyeCrop::On || (m == EyeCrop::Auto && (onLinux || !steamvr));
}
enum class WheelButton : int { X = 0, Y = 1, R = 2 };
// a PC VR build (Windows, desktop Linux), not a standalone headset's (Android: Quest, Steam Frame - Lepton; Linux arm64:
// Steam Frame - Native): VR upscaling, frame synthesis and dynamic resolution are off there by default (the user's
// choice, 2026-11-03), each still choosable
#if defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
inline constexpr bool kPcVrBuild = false;
#else
inline constexpr bool kPcVrBuild = true;
#endif
// a Quest (the Android build on a Meta Quest; not the Steam Frame's Android layer), at run time
inline bool quest_headset() {
#ifdef __ANDROID__
    return quest_theater::device();
#else
    return false;
#endif
}
// a Steam Frame build (Native: Linux arm64; Lepton: its Android flavor), whose defaults may differ from a Quest's
#if defined(WWHD_STEAM_FRAME_NATIVE) || defined(WWHD_STEAM_FRAME_LEPTON) || (defined(__linux__) && defined(__aarch64__) && !defined(__ANDROID__))
inline constexpr bool kSteamFrameBuild = true;
#else
inline constexpr bool kSteamFrameBuild = false;
#endif
// the frame rate drawn for the headset, by meaning (docs/VR_FRAME_RATE.md; interp_pacing.h VrFrameMode): unlocked (as
// fast as the headset shows evenly and the computer draws), capped at the refresh rate (Full: 90 fps at 90 Hz), at
// half of it (Half: 45 fps at 90 Hz), or auto (full while its frames fit, else half). Original (the game's own 30 fps)
// is read from older settings only and becomes Half. The game logic stays at 30 steps a second in each
enum class FrameRate : int { Unlocked = 0, Half = 1, Original = 2, Auto = 3, Full = 4 };  // (= pacing::VrFrameMode)
// the headset refresh rates the settings offer (Hz); the one asked for at the start where the runtime lets the game
// choose (XR_FB_display_refresh_rate), and the one the frame-rate caps are named after
inline constexpr int kRefreshRates[] = {60, 72, 80, 90, 120};
constexpr int kDefaultRefreshRate = 90;
// a refresh rate as one of kRefreshRates: within 0.6 Hz of one (runtimes report 89.9, 119.88, 59.94, 72.0: a rate
// rounded or in NTSC steps; 61 or 88 Hz are other rates), else 0
inline int supported_refresh_rate(float hz) {
    if (!(hz > 0)) return 0;
    for (int r : kRefreshRates)
        if (hz > r - 0.6f && hz < r + 0.6f) return r;
    return 0;
}

// The version of the vr.* settings ("vr.format"): written with them; older files (no key) read the same way
// 2: the shield-hand grip no longer on ZR (vr_bindings.h migrate_bindings); 3 to 5: the sword tilt's default
// -40 -> -10 -> +20 -> +45 (migrate_config); 8: the frame rate's default unlocked -> 90 fps; 9: the frame rate by
// meaning (full, half) and the refresh rate one of kRefreshRates; 10: the sword hand's trigger no longer on A
// (migrate_bindings: it opens the item wheel); 14: frame synthesis auto on a Quest (2026-11-04, a test build only);
// 15: off again there (the user's choice, the same day): a Quest's auto saved by format 14 back to off; 16: the menu
// zoom; 17: vr.renderScale the headset resolution (the eye pictures' size), not the headset images' size alone; 18:
// a standalone headset's default 0.55 -> 0.65; 19: ambient occlusion off by default on a Quest
constexpr int kSettingsFormat = 19;

// The defaults are the settings the project's VR port was tuned with in the headset (PS VR2, SteamVR; 2026-10-25)
// Headset resolution's quick choices (the VR menus' buttons beside the slider; 2026-11-09: 0.70 between the 0.65
// default and 0.75 on a Quest) and the slider's step: a value is kept to 0.05s so a controller's drag lands on one.
inline constexpr float kHeadsetResolutionPresets[] = {kPcVrBuild ? 0.75f : 0.65f, kPcVrBuild ? 1.0f : 0.70f,
                                                      kPcVrBuild ? 1.25f : 0.75f, kPcVrBuild ? 1.5f : 1.0f};
inline float snap_headset_resolution(float v) { return std::round(v * 20.0f) / 20.0f; }

struct Config {
    bool enabled = true;                // start in VR when an OpenXR runtime and headset are available
    Posture posture = Posture::Standing;
    float heightOffset = 0;             // metres added to the calibrated eye height (seated players: +)
    float unitsPerMetre = 100;          // game units per real metre (Link is about 1.2 m tall); see matchLinkBody
    // the world scaled to Link's body: game units per metre = Link's arm over yours (vr.realArmLength), so your hands
    // are drawn where they are and his arms reach as far as yours; off: vr.unitsPerMetre
    bool matchLinkBody = false;
    bool leftHanded = false;            // sword in the left hand, shield in the right
    Turn turn = Turn::Smooth;
    float snapDegrees = 30;
    float smoothDegreesPerSecond = 120;
    MoveDirection moveDirection = MoveDirection::Head;
    float moveSpeed = 1;                // scale of the left stick before the game reads it (the game runs/walks)
    // the headset resolution (docs/VR_SETTINGS.md, "Headset resolution"): the eye pictures drawn at this share of the
    // runtime's recommended eye image (on a PC the headset's images too, a 1:1 copy). 1 on a PC; 0.65 on a standalone
    // headset (the user's choice, 2026-11-08: Quest 3 1092x1144 of 1680x1760, 1.4x the pixels its 0.75x internal
    // resolution drew, sharper for some GPU time), upscaled there
    float renderScale = kPcVrBuild ? 1.0f : 0.65f;
    float maxHeadOffset = 1.0f;         // metres the head may lean / walk away from Link before it is held back
                                        // (VR fork: 1 m, was 0.6; walls hold it shorter: vr_game.cpp g_wallLean)
    // first person: metres the view sits in front of (+) or behind (-) Link's eyes (the game's eye point; 0 = his eyes)
    float eyeForward = 0;
    bool startThirdPerson = false;      // normal play starts in the third-person view (the right stick click toggles)
    // first person: Link's body moves under your head when you lean or walk in your play space (while he stands or
    // walks on the ground; docs/VR_FIRST_PERSON_CAMERA.md "The body under the head")
    bool roomScale = true;
    // first person: the view turns round with Link during the spin attack and the Hurricane Spin (docs/VR_CONTROLS.md,
    // "Spin attacks"); off keeps the view still while he spins. On by default (2026-11-04)
    bool spinCamera = true;
    // Streaming optimal (docs/VR_STREAMING.md, 2026-11-05): on a streamed headset (Virtual Desktop, Air Link, Steam
    // Link, ALVR) at 72 or 80 Hz the game draws 72 or 80 fps, a frame on every refresh, instead of 60; the stream's
    // encoder gets a frame every refresh. No effect on a wired headset. Detect Headset sets it from the runtime
    bool streamingOptimal = false;  // (off by default: the user's choice, 2026-11-05; Detect Headset turns it on for a streamed headset)
    // Depth for Virtual Desktop (docs/VR_STREAMING.md, 2026-11-05): with Streaming optimal, on Virtual Desktop's own
    // runtime (VDXR) the game sends each eye's depth (XR_KHR_composition_layer_depth), so its frame smoothing (SSW) and
    // reprojection move the picture with head translation and depth when a frame is late or the network stalls. Not
    // while text, menus, the GamePad screen or the item wheel are shown (their quads went behind the scene in VDXR's
    // composition, 2026-11-03). Other runtimes: vr.frameSynthesis as before. Applies at the next start
    bool vdxrDepth = false;  // (off by default: the user's choice, 2026-11-05)
    // the spin attack's and the Hurricane Spin's speed (docs/VR_CONTROLS.md, "Spin speed"): their animations' rate and
    // the Hurricane Spin's turn each step scaled together, so his model, his sword and the view's turn stay in step;
    // 1 the game's own (2026-11-04)
    float spinSpeed = 1.0f;
    // normal play: the game's HUD at the screen's edges drawn this share of the way from the view's centre to
    // them. The eye picture spans the headset's whole field (about 120 degrees); at its edges the HUD was outside what
    // the lenses show (docs/VR_HUD.md)
    float hudInset = 0.3f;  // 0.5 (corners about 43 / 34 degrees out) was still out of view in the headset
    float hudSize = 0.5f;   // and this share of its size: the game's size looked too large in the headset
    // sword grip: the blade's root in the controller's grip space (metres) and its tilt forward (degrees)
    float gripOffsetX = 0, gripOffsetY = 0, gripOffsetZ = -0.04f;
    // a weapon picked up from an enemy (the stick): moved in the controller's grip space (metres: +x right, +z towards
    // you) from where its handle is put at the hand; asked for in the headset, a little right and closer. Right-handed
    // play only (vr.leftHanded off), where they were set
    float stickOffsetX = 0.03f, stickOffsetY = 0, stickOffsetZ = 0.10f;  // (0.04, then 0.07: still too far out)
    // the shield moved in its own level frame (metres: +x right, +y up, +z towards you from its face) from its mirrored
    // place. Held flush with the body (settings format 11), the hand's fingers went through it (reported in the
    // headset): by default out in front of the fist, a little right and up, so its handle is in the hand (as set in the
    // headset). (Before format 12 it was in the controller's grip space, -0.13 z for the shield out of the back of the
    // hand.) Right-handed play only (vr.leftHanded off), where they were set
    float shieldOffsetX = 0.072f, shieldOffsetY = 0.016f, shieldOffsetZ = -0.06f;
    // the drawn sword moved in the sword-hand controller's grip space (metres: +x right, +z towards you, along the fist)
    // from the game's grip mirrored onto the hand: its handle had sat a little out of the fist (reported in the
    // headset). Right-handed play only (vr.leftHanded off), where the game's grip is mirrored
    float swordOffsetX = 0.0f, swordOffsetY = 0.0f, swordOffsetZ = 0.03f;
    // the blade tilted up from where the controller points (OpenXR's aim pose; the line through the fist before
    // settings format 6). From the fist (44 degrees above where Touch-style controllers point) a forward thrust stood
    // the blade up into the face (reported in the headset)
    float gripPitchDegrees = 20;
    // the sword hand's wrist turned into line with the forearm (1) or as the controller holds the sword (0); the
    // sword turns with the hand (asked for in the headset: the wrist bent up to hold the sword)
    float swordWristStraight = 1.0f;
    // the Skull Hammer turned about its shaft from the controller's hold (0: its face away from the controller's top; 180,
    // the default: turned over, asked for in the headset so its flat end faces down; docs/VR_SKULL_HAMMER.md)
    float hammerRollDegrees = 180;
    float bladeLength = 0.85f;          // metres from root to tip of the drawn sword (Hero's Sword)
    // m/s of the hand: a swing starts (TPVR: 2.2; at the tip before format 6). 2.0 left most side slices unstarted
    // (in the headset: asked to be more sensitive)
    float swingStartSpeed = 1.6f;
    float swingEndSpeed = 0.8f;         // m/s of the hand: a swing ends
    float swingMinTime = 0.05f;         // s above the start speed before it counts (no flicks)
    float swingCooldown = 0.25f;        // s after a swing before the next may start
    // the game's attack (its animation and sound) only for a full swing, down, left to right or right to left (asked
    // for in the headset): once the hand has carried this far (metres) across (a side slice) or down (a chop).
    // Measured in the headset: side slices carried the hand 0.19 to 0.43 m, chops 0.24 to 0.49 m; 0.25 for both missed
    // many slices and some chops. swingFullDegrees and swingFullDistance are no longer used
    float swingAcrossDistance = 0.12f;
    float swingDownDistance = 0.15f;
    // a thrust (asked for in the headset: the sword drawn in, then pushed out): the hand carried this far (metres)
    // forward, mostly forward and along the blade, attacks with the game's forward thrust (procCutF). 0.10 (0.15 at
    // first: asked for lower in the headset, easier for people)
    float swingThrustDistance = 0.10f;
    float swingFullDegrees = 35.0f;
    float swingFullDistance = 0.2f;
    float hapticStrength = 0.6f;        // 0 = off
    bool cutsceneFollow = true;         // events: the game's camera position with head rotation (else flat)
    bool showBody = false;              // draw Link's whole model in first person (normally his head is hidden)
    // first person: Link's body not drawn, only his hands and what they hold (docs/VR_FIRST_PERSON_CAMERA.md, "Hiding
    // the body"); wins over showBody. On by default (the user's choice, 2026-11-04)
    bool hideBody = true;
    // first person: Link's arms not drawn, his hands are (and what they hold): each arm folded into its hand (docs/
    // VR_FIRST_PERSON_CAMERA.md, "Hiding the arms"); with hideBody or on its own. On by default (2026-11-04)
    bool hideArms = true;
    // the game's screen effects in the headset (docs/VR_SCREEN_EFFECTS.md); on is the original picture, off the default
    // (asked for after the headset test, 2026-10-30). The screen blur (Swift Sail, some attacks: the last frame laid
    // over the new one, zoomed) and the bloom (the glow round bright light; the game's Bloom strength setting still
    // scales it when on)
    bool screenBlur = false;
    bool bloom = false;
    // the heat haze (Dragon Roost, lava rooms, hot ground: the particles that bend the picture behind them, draw group 4;
    // vr_ui.h draws_particle_group), off by default like the two above
    bool heatHaze = false;
    // the game's depth of field (agl DepthOfField, interp_fx.cpp hook_027824A8): its near and far blur, off in the
    // headset by default like the three above (asked for, 2026-11-06: stereo already gives the depth, and the blur stays
    // where the game focuses, not where you look). Only the blur: the same effect's colour control (the sealed Hyrule's
    // grey), vignetting (the telescope) and distortion are kept
    bool depthOfField = false;
    // the game's ambient occlusion (the soft shading under characters and in corners: PS 44BDFD00, gfx/vulkan draw.cpp
    // and shaders.cpp): on by default (asked for, 2026-11-06: a toggle, "not on by default for now"); off, its pass
    // writes no occlusion and skips its depth taps (GPU time per eye), the sun's shadows unchanged. Off by default on a
    // Quest (the user's choice, 2026-11-10: the GPU is the limit there), still choosable
    bool ambientOcclusion = !quest_headset();
    bool trackedSword = true;           // draw the sword at the tracked hand (else where Link's animation holds it)
    // TPVR's physical sword: while the sword hand swings (swingStartSpeed), the blade cuts and hits along its tracked
    // path without the game's attack animation; full swings still play the game's attack
    bool physicalSword = true;
    // the item wheel (docs/VR_ITEM_WHEEL.md): reach over the off hand's shoulder, choose with the right stick, bring the
    // hand forward to put the item on itemWheelButton (X: the off hand's primary button)
    bool itemWheel = true;
    // the body follows the head (docs/VR_BODY_FOLLOW.md): standing in first-person play, Link turns to face where you
    // look once your head has turned this far (degrees) from his body, and turns until within 5 degrees of it
    float bodyFollowDegrees = 30.0f;
    WheelButton itemWheelButton = WheelButton::X;
    bool debugCollision = false;        // VR collision visualisation (development)
    // the GamePad window on the desktop while a VR session runs (the headset shows the GamePad screen as a panel);
    // presenting it took 8.6 ms of every VR frame (docs/VR_PERFORMANCE.md)
    bool desktopGamePad = false;
    // menu scenes (title, file select): their camera this much farther from what it frames and their layouts
    // drawn this much smaller (1.5 = 50 % farther); gameplay is never affected. 3 since 2026-11-04 (format 16): at 2
    // the file select was too wide to see whole in the headset (the conversations' text has been at 3 since its own)
    float menuZoomOut = 3.0f;
    // events (cutscenes): the game's camera this much farther from what it frames (1 = as the game frames it)
    float cutsceneZoomOut = 1.0f;
    // events and conversations: the TV's text and textboxes drawn this much smaller around the view's centre (as if
    // that much farther), and moved up (+) or down (-) by this share of the half view height
    float textZoomOut = 3.0f;           // a third of the size: 1.6 and then 2 were still large in the headset
    float textHeight = 0.05f;           // near the middle of the view, slightly below it (+0.2 was too high)
    // conversations in first person (docs/VR_CUTSCENE_TEXT.md, "Conversations in first person"): the view stays at
    // Link's eyes (not the game's camera), and the text box stays in the world where you looked when it began
    bool firstPersonTalk = true;
    // the telescope's eyepiece (the game's mask) drawn at this share of its full size; the eye picture around it is
    // black (docs/VR_TELESCOPE.md). Full size put the black ring outside what the lenses show
    float telescopeSize = 0.85f;
    // the Picto Box's viewfinder (its frame, zoom gauge, count and prompts) drawn at this share of its full size,
    // centred; the photo is the part inside the frame, so it shrinks with it (docs/VR_PICTO_BOX.md). Full size put
    // the zoom gauge outside what the lenses show
    float pictoBoxSize = 0.70f;
    // the wrist HUD (docs/VR_WRIST_HUD.md): the hearts on top of the left wrist, the item buttons underneath it, the
    // magic bar on top of the right wrist, the rupees underneath it, each switched on or off; placed from the controller's grip, wristHudAlong
    // metres towards the elbow and wristHudLift metres off its axis, drawn at wristHudScale of their size
    bool armHearts = true, wristItems = true, wristRupees = true, wristMagic = true;
    float wristHudAlong = 0.07f, wristHudLift = 0.035f, wristHudScale = 1.0f;
    // the menu panel (the GamePad screen: pause, item, map and bag menus); its own settings, not the camera's
    GamePadScreen gamepadScreen = GamePadScreen::Menus;
    MenuPlacement menuPlacement = MenuPlacement::Gaze;
    float menuDistance = 1.4f;          // metres from the head to the panel (headset: 1.7 looked small, 1.1 too close)
    float menuHeight = -0.08f;          // metres above (+) or below (-) eye level
    float menuScale = 1.0f;             // times the base width of 0.8 m
    float menuCurvature = 0.0f;         // 0 flat .. 1 curved around the head (runtimes with cylinder layers)
    float refreshRate = kDefaultRefreshRate;  // Hz, one of kRefreshRates: asked of the headset (XR_FB_display_refresh_rate)
    bool autoFrameRate = true;          // switch frame interpolation on for the session if it is off
    // vr.frameRate: full (the refresh rate: 90 fps at 90 Hz), half, unlocked, auto. (VR fork) Auto Mode by default:
    // capped at the refresh rate while its frames fit, else at half (was full)
    FrameRate frameRate = FrameRate::Auto;
    // vr.upscaler (docs/VR_UPSCALING.md, vr_upscale.h): auto (the best the device offers: Quest Super Resolution, DLSS
    // on an NVIDIA RTX PC, else SGSR1 where the eye picture is upscaled), off, sgsr1, superResolution, dlss, and by
    // choice only sgsr2 and fsr1
    upscale::Mode upscaler = kPcVrBuild ? upscale::Mode::Off : upscale::Mode::Auto;  // (PC VR: off by default)
    // SGSR1's edge sharpness, 1 (soft) .. 2 (Qualcomm's default); 1.5 on the Steam Frame (2026-11-03: 2 may halo the
    // game's dark outlines; STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md section 6)
    float upscaleSharpness = kSteamFrameBuild ? 1.5f : 2.0f;
    float fsrSharpness = 0.2f;          // FSR 1's RCAS: stops of reduction, 0 (sharpest) .. 2 (AMD's samples: 0.2)
    // vr.frameSynthesis (docs/VR_FRAME_SYNTHESIS.md, vr_framesynth.h): auto (the runtime's frame synthesis on a
    // standalone headset that offers it, the game at half the refresh rate; depth for reprojection elsewhere), off, on.
    // Applies at the next start (the runtime's extensions are chosen when VR starts)
    // Off by default (the user's choices of 2026-11-03): PC VR; the Steam Frame until it is tested there (a player's
    // run with it smeared and tore, and QuakeVR's Frame port ships it off after artifacts). Off on a Quest too
    // (2026-11-04: auto there for one test build, then off again by the user's choice). Every choice stays choosable
    framesynth::Mode frameSynthesis = framesynth::Mode::Off;
    // vr.dynamicResolution (docs/VR_DYNAMIC_RESOLUTION.md, vr_dynres.h): off, quality, balanced, performance, custom;
    // Custom's range (of the internal resolution, linear), GPU target (busy %) and settle time (s). Applies at once
    dynres_vr::Mode dynamicResolution = dynres_vr::Mode::Off;  // (off everywhere by default)
    float dynresMin = 0.75f, dynresMax = 1.0f, dynresGpuTarget = 90.0f, dynresSettle = 2.0f;
    EyeCrop eyeCrop = EyeCrop::Off;  // (vr.eyeCrop, above): not needed with each eye's own projection; applies from the next frame
    float stickDeadZone = 0.15f;        // radial dead zone of the controllers' sticks
    // conducting with the Wind Waker (docs/VR_WIND_WAKER.md): the hands also play it, each where it is held from where
    // it was when conducting began (the left hand the notes, as the left stick; the right the beat, as the right stick),
    // a direction once it is batonHandReach metres out that way (held until it is back within 60 % of that)
    bool batonHands = true;
    float batonHandReach = 0.12f;
    bool armIK = true;                  // Link's arms follow the controllers
    // Link's arms reach where the controllers are: the hand target is not held to his shoulder's reach (the arm moves
    // out from the shoulder instead) and the wrist turns as far as the controller (docs/VR_ARM_ANIMATION.md)
    bool detachedArms = true;
    bool aimLaser = true;  // the red aiming laser while the bow, hookshot, boomerang or grappling hook aims (docs/VR_AIMING.md)
    float realArmLength = 0.6f;         // metres from shoulder to grip of the player (for scaling to Link's arms)
    // replacement texture packs ("textures" mod packages, gfx/texture_pack.h; docs/superpowers/specs runtime texture
    // adapter): vr.texturePacks switches them on (does anything only with such a package enabled; applies at the next
    // start), and vr.texturePackBudgetMB is the VRAM their images may take (0: automatic, half of the graphics card's
    // memory budget but no more than is free once the VR session runs, at least 1 GiB)
    bool texturePacks = true;
    float texturePackBudgetMB = 0;
};

// Link's arm, shoulder to wrist, in game units (as the game measures it in his HD model: vr_game.cpp), and the world
// scale vr.matchLinkBody gives for an arm of `realArmLength` metres: his arm over yours, within vr.unitsPerMetre's range.
// The settings show it; the game uses the arm it measures (the same in every scene so far).
constexpr float kLinkArmUnits = 35.3f;
inline float matched_world_scale(float realArmLength) {
    return realArmLength > 0 ? std::clamp(kLinkArmUnits / realArmLength, 40.0f, 250.0f) : 100.0f;
}

inline bool parse_float(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}
inline bool parse_bool(const std::string& s, bool& out) {
    if (s == "1" || s == "true" || s == "on") { out = true; return true; }
    if (s == "0" || s == "false" || s == "off") { out = false; return true; }
    return false;
}

namespace detail {
struct FloatKey { const char* key; float Config::*field; float lo, hi; };
inline const FloatKey kFloats[] = {
    {"vr.heightOffset", &Config::heightOffset, -1.0f, 1.0f},
    {"vr.unitsPerMetre", &Config::unitsPerMetre, 40.0f, 250.0f},
    {"vr.snapDegrees", &Config::snapDegrees, 10.0f, 90.0f},
    {"vr.smoothDegreesPerSecond", &Config::smoothDegreesPerSecond, 20.0f, 360.0f},
    {"vr.moveSpeed", &Config::moveSpeed, 0.2f, 1.0f},
    {"vr.spinSpeed", &Config::spinSpeed, 0.5f, 1.0f},
    {"vr.renderScale", &Config::renderScale, 0.5f, 2.0f},
    {"vr.maxHeadOffset", &Config::maxHeadOffset, 0.05f, 2.0f},
    {"vr.eyeForward", &Config::eyeForward, -0.3f, 0.3f},
    {"vr.hudInset", &Config::hudInset, 0.15f, 1.0f},
    {"vr.hudSize", &Config::hudSize, 0.2f, 1.5f},
    {"vr.gripOffsetX", &Config::gripOffsetX, -0.3f, 0.3f},
    {"vr.gripOffsetY", &Config::gripOffsetY, -0.3f, 0.3f},
    {"vr.gripOffsetZ", &Config::gripOffsetZ, -0.3f, 0.3f},
    {"vr.stickOffsetX", &Config::stickOffsetX, -0.3f, 0.3f},
    {"vr.stickOffsetY", &Config::stickOffsetY, -0.3f, 0.3f},
    {"vr.stickOffsetZ", &Config::stickOffsetZ, -0.3f, 0.3f},
    {"vr.shieldOffsetX", &Config::shieldOffsetX, -0.4f, 0.4f},
    {"vr.shieldOffsetY", &Config::shieldOffsetY, -0.4f, 0.4f},
    {"vr.shieldOffsetZ", &Config::shieldOffsetZ, -0.4f, 0.4f},
    {"vr.swordOffsetX", &Config::swordOffsetX, -0.3f, 0.3f},
    {"vr.swordOffsetY", &Config::swordOffsetY, -0.3f, 0.3f},
    {"vr.swordOffsetZ", &Config::swordOffsetZ, -0.3f, 0.3f},
    {"vr.gripPitchDegrees", &Config::gripPitchDegrees, -90.0f, 90.0f},
    {"vr.swordWristStraight", &Config::swordWristStraight, 0.0f, 1.0f},
    {"vr.hammerRollDegrees", &Config::hammerRollDegrees, -180.0f, 180.0f},
    {"vr.bladeLength", &Config::bladeLength, 0.3f, 1.5f},
    {"vr.swingStartSpeed", &Config::swingStartSpeed, 0.5f, 10.0f},
    {"vr.swingEndSpeed", &Config::swingEndSpeed, 0.2f, 8.0f},
    {"vr.swingMinTime", &Config::swingMinTime, 0.0f, 0.3f},
    {"vr.swingFullDegrees", &Config::swingFullDegrees, 20.0f, 160.0f},
    {"vr.swingFullDistance", &Config::swingFullDistance, 0.1f, 1.0f},
    {"vr.swingAcrossDistance", &Config::swingAcrossDistance, 0.05f, 0.6f},
    {"vr.bodyFollowDegrees", &Config::bodyFollowDegrees, 10.0f, 90.0f},
    {"vr.swingDownDistance", &Config::swingDownDistance, 0.05f, 0.6f},
    {"vr.swingThrustDistance", &Config::swingThrustDistance, 0.05f, 0.6f},
    {"vr.swingCooldown", &Config::swingCooldown, 0.0f, 1.5f},
    {"vr.hapticStrength", &Config::hapticStrength, 0.0f, 1.0f},
    {"vr.menuZoomOut", &Config::menuZoomOut, 1.0f, 4.0f},
    {"vr.cutsceneZoomOut", &Config::cutsceneZoomOut, 1.0f, 3.0f},
    {"vr.textZoomOut", &Config::textZoomOut, 1.0f, 3.0f},
    {"vr.textHeight", &Config::textHeight, -0.5f, 0.5f},
    {"vr.telescopeSize", &Config::telescopeSize, 0.4f, 1.4f},
    {"vr.pictoBoxSize", &Config::pictoBoxSize, 0.5f, 1.0f},
    {"vr.wristHudAlong", &Config::wristHudAlong, -0.05f, 0.2f},
    {"vr.wristHudLift", &Config::wristHudLift, 0.0f, 0.1f},
    {"vr.wristHudScale", &Config::wristHudScale, 0.5f, 2.0f},
    {"vr.menuDistance", &Config::menuDistance, 0.4f, 4.0f},
    {"vr.menuHeight", &Config::menuHeight, -0.8f, 0.8f},
    {"vr.menuScale", &Config::menuScale, 0.25f, 3.0f},
    {"vr.menuCurvature", &Config::menuCurvature, 0.0f, 1.0f},
    {"vr.refreshRate", &Config::refreshRate, 0.0f, 240.0f},
    {"vr.stickDeadZone", &Config::stickDeadZone, 0.0f, 0.6f},
    {"vr.realArmLength", &Config::realArmLength, 0.3f, 1.0f},
    {"vr.batonHandReach", &Config::batonHandReach, 0.05f, 0.4f},
    {"vr.upscaleSharpness", &Config::upscaleSharpness, 1.0f, 2.0f},
    {"vr.fsrSharpness", &Config::fsrSharpness, 0.0f, 2.0f},
    {"vr.dynresMin", &Config::dynresMin, 0.5f, 1.0f},
    {"vr.dynresMax", &Config::dynresMax, 0.5f, 1.0f},
    {"vr.dynresGpuTarget", &Config::dynresGpuTarget, 50.0f, 98.0f},
    {"vr.dynresSettle", &Config::dynresSettle, 0.5f, 32.0f},
    {"vr.texturePackBudgetMB", &Config::texturePackBudgetMB, 0.0f, 65536.0f},
};
struct BoolKey { const char* key; bool Config::*field; };
inline const BoolKey kBools[] = {
    {"vr.enabled", &Config::enabled}, {"vr.leftHanded", &Config::leftHanded},
    {"vr.cutsceneFollow", &Config::cutsceneFollow}, {"vr.showBody", &Config::showBody},
    {"vr.hideBody", &Config::hideBody},
    {"vr.hideArms", &Config::hideArms},
    {"vr.trackedSword", &Config::trackedSword}, {"vr.firstPersonTalk", &Config::firstPersonTalk}, {"vr.autoFrameRate", &Config::autoFrameRate},
    {"vr.physicalSword", &Config::physicalSword}, {"vr.itemWheel", &Config::itemWheel},
    {"vr.armIK", &Config::armIK}, {"vr.matchLinkBody", &Config::matchLinkBody},
    {"vr.detachedArms", &Config::detachedArms}, {"vr.aimLaser", &Config::aimLaser},
    {"vr.desktopGamePad", &Config::desktopGamePad}, {"vr.armHearts", &Config::armHearts},
    {"vr.wristItems", &Config::wristItems}, {"vr.wristRupees", &Config::wristRupees}, {"vr.wristMagic", &Config::wristMagic},
    {"vr.startThirdPerson", &Config::startThirdPerson},
    {"vr.roomScale", &Config::roomScale},
    {"vr.spinCamera", &Config::spinCamera},
    {"vr.streamingOptimal", &Config::streamingOptimal},
    {"vr.vdxrDepth", &Config::vdxrDepth},
    {"vr.screenBlur", &Config::screenBlur}, {"vr.bloom", &Config::bloom}, {"vr.heatHaze", &Config::heatHaze},
    {"vr.depthOfField", &Config::depthOfField},
    {"vr.ambientOcclusion", &Config::ambientOcclusion},
    {"vr.debugCollision", &Config::debugCollision}, {"vr.batonHands", &Config::batonHands},
    {"vr.texturePacks", &Config::texturePacks},
};
}  // namespace detail

// one saved key; false if the key is not a VR key or the value was not usable (the default stays)
inline bool from_kv(Config& c, const std::string& key, const std::string& value) {
    for (const auto& f : detail::kFloats)
        if (key == f.key) {
            float v;
            if (!parse_float(value, v)) return false;
            c.*f.field = std::clamp(v, f.lo, f.hi);
            return true;
        }
    for (const auto& b : detail::kBools)
        if (key == b.key) return parse_bool(value, c.*b.field);
    if (key == "vr.turn") {
        if (value == "snap") c.turn = Turn::Snap;
        else if (value == "smooth") c.turn = Turn::Smooth;
        else return false;
        return true;
    }
    if (key == "vr.moveDirection") {
        if (value == "head") c.moveDirection = MoveDirection::Head;
        else if (value == "hand") c.moveDirection = MoveDirection::LeftHand;
        else return false;
        return true;
    }
    if (key == "vr.menuPlacement") {
        if (value == "gaze") c.menuPlacement = MenuPlacement::Gaze;
        else if (value == "head") c.menuPlacement = MenuPlacement::Head;
        else if (value == "front") c.menuPlacement = MenuPlacement::Front;
        else return false;
        return true;
    }
    if (key == "vr.format") {
        float v;
        return parse_float(value, v);  // read, never stored: newer formats keep the keys this version knows
    }
    if (key == "vr.gamepadScreen") {
        if (value == "menus") c.gamepadScreen = GamePadScreen::Menus;
        else if (value == "always") c.gamepadScreen = GamePadScreen::Always;
        else if (value == "off") c.gamepadScreen = GamePadScreen::Off;
        else return false;
        return true;
    }
    if (key == "vr.eyeCrop") {
        if (value == "auto") c.eyeCrop = EyeCrop::Auto;
        else if (value == "on") c.eyeCrop = EyeCrop::On;
        else if (value == "off") c.eyeCrop = EyeCrop::Off;
        else return false;
        return true;
    }
    if (key == "vr.dynamicResolution") return dynres_vr::parse_mode(value.c_str(), c.dynamicResolution);
    if (key == "vr.frameSynthesis") {
        if (value == "auto") c.frameSynthesis = framesynth::Mode::Auto;
        else if (value == "off") c.frameSynthesis = framesynth::Mode::Off;
        else if (value == "on") c.frameSynthesis = framesynth::Mode::On;
        else return false;
        return true;
    }
    if (key == "vr.upscaler") {
        if (value == "auto") c.upscaler = upscale::Mode::Auto;
        else if (value == "off") c.upscaler = upscale::Mode::Off;
        else if (value == "sgsr1") c.upscaler = upscale::Mode::Sgsr1;
        else if (value == "superResolution") c.upscaler = upscale::Mode::SuperResolution;
        else if (value == "dlss") c.upscaler = upscale::Mode::Dlss;
        else if (value == "sgsr2") c.upscaler = upscale::Mode::Sgsr2;
        else if (value == "fsr1") c.upscaler = upscale::Mode::Fsr1;
        else return false;
        return true;
    }
    if (key == "vr.frameRate") {
        if (value == "unlocked") c.frameRate = FrameRate::Unlocked;
        else if (value == "full") c.frameRate = FrameRate::Full;
        else if (value == "half") c.frameRate = FrameRate::Half;
        else if (value == "auto") c.frameRate = FrameRate::Auto;
        // older formats: 45 (half the rate), 30 (the game's own), and the fixed 60, 90 and 120 fps of format 8 (all
        // three the full rate: each was chosen as the headset's own)
        else if (value == "45") c.frameRate = FrameRate::Half;
        else if (value == "30") c.frameRate = FrameRate::Original;
        else if (value == "60" || value == "90" || value == "120") c.frameRate = FrameRate::Full;
        else return false;
        return true;
    }
    if (key == "vr.itemWheelButton") {
        if (value == "x") c.itemWheelButton = WheelButton::X;
        else if (value == "y") c.itemWheelButton = WheelButton::Y;
        else if (value == "r") c.itemWheelButton = WheelButton::R;
        else return false;
        return true;
    }
    if (key == "vr.posture") {
        if (value == "standing") c.posture = Posture::Standing;
        else if (value == "seated") c.posture = Posture::Seated;
        else return false;
        return true;
    }
    return false;
}

// (VR fork) 120 Hz offers only Capped 60 FPS and Auto Mode for now: the game draws nowhere near 120 in VR yet, and
// Unlocked and Capped 120 there only showed uneven frames. Revisit when the frame rate allows (docs/VR_FRAME_RATE.md)
constexpr int kHalfOnlyRefreshRate = 120;
// the swing thresholds must keep their order: the end speed below the start speed
// the refresh rate is one of kRefreshRates (anything else, the runtime's choice 0 of older formats among it, the
// default), the frame rate one the menu offers (Original: Half)
inline void validate(Config& c) {
    if (c.swingEndSpeed >= c.swingStartSpeed) c.swingEndSpeed = c.swingStartSpeed * 0.5f;
    const int hz = supported_refresh_rate(c.refreshRate);
    c.refreshRate = float(hz ? hz : kDefaultRefreshRate);
    if (c.frameRate == FrameRate::Original) c.frameRate = FrameRate::Half;
    if (int(c.frameRate) < 0 || int(c.frameRate) > int(FrameRate::Full)) c.frameRate = FrameRate::Full;
    if (int(c.refreshRate) == kHalfOnlyRefreshRate && c.frameRate != FrameRate::Half)
        c.frameRate = FrameRate::Auto;  // (its two choices: Capped 60 FPS and Auto Mode)
}

// The frame-rate menu (the overlay's VR page, the launcher), from the refresh rate chosen: its four choices in order,
// each a meaning (FrameRate) with its name and its cap in fps (0: none). The one place the caps are named; a refresh
// rate change keeps the meaning, so Capped 45 FPS at 90 Hz is Capped 60 FPS at 120 Hz
struct FrameRateChoice {
    FrameRate rate;
    int fps;
    std::string name;
};
inline std::vector<FrameRateChoice> frame_rate_choices(int refreshHz) {
    const int hz = supported_refresh_rate(float(refreshHz)) ? refreshHz : kDefaultRefreshRate;
    if (hz == kHalfOnlyRefreshRate)
        return {{FrameRate::Half, hz / 2, "Capped " + std::to_string(hz / 2) + " FPS"}, {FrameRate::Auto, 0, "Auto Mode"}};
    return {{FrameRate::Unlocked, 0, "Unlocked FPS"},
            {FrameRate::Full, hz, "Capped " + std::to_string(hz) + " FPS"},
            {FrameRate::Half, hz / 2, "Capped " + std::to_string(hz / 2) + " FPS"},
            {FrameRate::Auto, 0, "Auto Mode"}};
}
inline std::string frame_rate_name(FrameRate r, int refreshHz) {
    for (const auto& c : frame_rate_choices(refreshHz))
        if (c.rate == r) return c.name;
    return frame_rate_name(FrameRate::Half, refreshHz);  // (Original: shown as what it becomes)
}
inline std::string refresh_rate_name(int hz) { return std::to_string(hz) + " Hz"; }
// a refresh rate chosen in the menu: the frame rate keeps its meaning (its cap follows the new rate)
// (VR fork) with the default frame rate, Auto Mode; it kept the meaning before
inline void choose_refresh_rate(Config& c, int hz) {
    if (supported_refresh_rate(float(hz)) != hz) return;
    if (int(c.refreshRate) != hz) c.frameRate = Config{}.frameRate;
    c.refreshRate = float(hz);
    validate(c);
}

// Detect Headset (the launcher, docs/VR_FRAME_RATE.md "Detect Headset"): what the runtime reported, and what the
// settings became. `current` the rate it runs now (Hz, 0: not known), `offered` the rates it lets the game ask for
// (XR_FB_display_refresh_rate; empty where it does not)
struct DetectedRefresh {
    bool applied = false;  // the settings now name the headset's rate, capped at it (full)
    int hz = 0;            // that rate
    std::string message;   // for the window: what was found and done, or why nothing changed
};
// The headset's current rate selected, and the frame rate capped at it (Full). A current rate the menu has no choice
// for (144 Hz, say) changes nothing unless the runtime offers one of the menu's rates, which the game then asks for at
// its start (the fastest offered); without a rate (detection failed) nothing changes
inline DetectedRefresh apply_detected_refresh(Config& c, float current, const std::vector<float>& offered) {
    DetectedRefresh d;
    char now[32];
    std::snprintf(now, sizeof now, "%.1f Hz", double(current));
    if (const int hz = supported_refresh_rate(current)) {
        c.refreshRate = float(hz);
        c.frameRate = Config{}.frameRate;  // (the default: Auto Mode)
        validate(c);
        d.applied = true;
        d.hz = hz;
        d.message = "The headset runs at " + std::string(now) + ": " + refresh_rate_name(hz) + ", " +
                    frame_rate_name(c.frameRate, hz) + ".";
        return d;
    }
    if (!(current > 0)) {
        d.message = "The runtime did not report the headset's refresh rate: the settings are unchanged.";
        return d;
    }
    int best = 0;
    for (float r : offered) best = std::max(best, supported_refresh_rate(r));
    if (best) {
        c.refreshRate = float(best);
        c.frameRate = Config{}.frameRate;  // (the default: Auto Mode)
        validate(c);
        d.applied = true;
        d.hz = best;
        d.message = "The headset runs at " + std::string(now) + ", not one of the menu's rates; it offers " +
                    refresh_rate_name(best) + ", which the game asks for at its start: " + refresh_rate_name(best) + ", " +
                    frame_rate_name(c.frameRate, best) + ".";
        return d;
    }
    d.message = "The headset runs at " + std::string(now) + ", which is not one of the menu's rates, and the runtime "
                "does not let the game choose another: the settings are unchanged (set the rate in the runtime's own "
                "settings).";
    return d;
}

inline std::vector<std::pair<std::string, std::string>> to_kv(const Config& c) {
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back("vr.format", std::to_string(kSettingsFormat));
    for (const auto& b : detail::kBools) out.emplace_back(b.key, c.*b.field ? "1" : "0");
    out.emplace_back("vr.turn", c.turn == Turn::Snap ? "snap" : "smooth");
    out.emplace_back("vr.moveDirection", c.moveDirection == MoveDirection::Head ? "head" : "hand");
    out.emplace_back("vr.posture", c.posture == Posture::Standing ? "standing" : "seated");
    out.emplace_back("vr.menuPlacement", c.menuPlacement == MenuPlacement::Gaze ? "gaze"
                                         : c.menuPlacement == MenuPlacement::Head ? "head" : "front");
    out.emplace_back("vr.gamepadScreen", c.gamepadScreen == GamePadScreen::Menus ? "menus"
                                         : c.gamepadScreen == GamePadScreen::Always ? "always" : "off");
    out.emplace_back("vr.eyeCrop", c.eyeCrop == EyeCrop::On ? "on" : c.eyeCrop == EyeCrop::Off ? "off" : "auto");
    out.emplace_back("vr.dynamicResolution", dynres_vr::mode_key(c.dynamicResolution));
    out.emplace_back("vr.frameSynthesis", c.frameSynthesis == framesynth::Mode::Off  ? "off"
                                          : c.frameSynthesis == framesynth::Mode::On ? "on"
                                                                                     : "auto");
    out.emplace_back("vr.upscaler", c.upscaler == upscale::Mode::Off               ? "off"
                                    : c.upscaler == upscale::Mode::Sgsr1           ? "sgsr1"
                                    : c.upscaler == upscale::Mode::SuperResolution ? "superResolution"
                                    : c.upscaler == upscale::Mode::Dlss            ? "dlss"
                                    : c.upscaler == upscale::Mode::Sgsr2           ? "sgsr2"
                                    : c.upscaler == upscale::Mode::Fsr1            ? "fsr1"
                                                                                   : "auto");
    out.emplace_back("vr.frameRate", c.frameRate == FrameRate::Half || c.frameRate == FrameRate::Original ? "half"
                                     : c.frameRate == FrameRate::Auto                                      ? "auto"
                                     : c.frameRate == FrameRate::Full                                      ? "full"
                                                                          : "unlocked");
    out.emplace_back("vr.itemWheelButton", c.itemWheelButton == WheelButton::X ? "x"
                                           : c.itemWheelButton == WheelButton::Y ? "y" : "r");
    for (const auto& f : detail::kFloats) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", double(c.*f.field));
        out.emplace_back(f.key, buf);
    }
    return out;
}

// the documented range of a numeric setting (the launcher's and the overlay's sliders use it); false if unknown
inline bool float_range(const std::string& key, float& lo, float& hi) {
    for (const auto& f : detail::kFloats)
        if (key == f.key) { lo = f.lo; hi = f.hi; return true; }
    return false;
}

// every vr.* key known (the settings layer loads exactly these)
inline std::vector<std::string> keys() {
    std::vector<std::string> k;
    for (const auto& kv : to_kv(Config{})) k.push_back(kv.first);
    return k;
}

// settings saved by an older format: values that were that format's defaults move to the new ones (a value chosen
// since stays)
inline void migrate_config(Config& c, int savedFormat) {
    if (savedFormat < 5 && (c.gripPitchDegrees == -40.0f || c.gripPitchDegrees == -10.0f || c.gripPitchDegrees == 20.0f))
        c.gripPitchDegrees = 45.0f;
    // format 6: the sword's tilt is from where the controller points, not the fist (a different angle: the new
    // default), and the swing speeds are the hand's, not the tip's (the old defaults become the new ones)
    if (savedFormat < 6) {
        c.gripPitchDegrees = Config{}.gripPitchDegrees;
        if (c.swingStartSpeed == 2.6f) c.swingStartSpeed = Config{}.swingStartSpeed;
        if (c.swingEndSpeed == 1.2f) c.swingEndSpeed = Config{}.swingEndSpeed;
    }
    // format 7: the hand's speeds lowered for sensitivity (2.0 / 1.0 were the format 6 defaults)
    if (savedFormat < 7) {
        if (c.swingStartSpeed == 2.0f) c.swingStartSpeed = Config{}.swingStartSpeed;
        if (c.swingEndSpeed == 1.0f) c.swingEndSpeed = Config{}.swingEndSpeed;
    }
    // format 8: capped at the refresh rate the default frame rate (unlocked was the format 7 default)
    if (savedFormat < 8 && c.frameRate == FrameRate::Unlocked) c.frameRate = Config{}.frameRate;
    // format 9: the refresh rate one of kRefreshRates (validate: 0, the runtime's choice, and others the default)
    // format 11: the shield held flush with the body, at the hand: the earlier default offset (-0.13 z) would push it
    // out in front of the hand
    if (savedFormat < 11 && c.shieldOffsetX == 0.0f && c.shieldOffsetY == 0.0f && c.shieldOffsetZ == -0.13f) {
        c.shieldOffsetX = c.shieldOffsetY = 0.0f;
        c.shieldOffsetZ = 0.0f;  // (format 11's default; format 12 below moves it on)
    }
    // format 12: the shield's offset in its own level frame, out in front of the fist by default (format 11's 0 put the
    // fingers through it)
    if (savedFormat < 12 && c.shieldOffsetX == 0.0f && c.shieldOffsetY == 0.0f && c.shieldOffsetZ == 0.0f) {
        c.shieldOffsetX = Config{}.shieldOffsetX;
        c.shieldOffsetY = Config{}.shieldOffsetY;
        c.shieldOffsetZ = Config{}.shieldOffsetZ;
    }
    // format 13: a thrust from 0.10 m of the hand's travel (0.15, the first default, saved by format 12)
    if (savedFormat < 13 && c.swingThrustDistance == 0.15f) c.swingThrustDistance = Config{}.swingThrustDistance;
    // format 15: frame synthesis off by default on a Quest again; format 14 (one test build) had moved a Quest's off to
    // auto, which goes back (a Quest's settings only: elsewhere format 14 left it as it was)
#ifdef __ANDROID__
    if (savedFormat == 14 && c.frameSynthesis == framesynth::Mode::Auto && quest_theater::device())
        c.frameSynthesis = framesynth::Mode::Off;
#endif
    // format 16: the title and file select 3x farther and a third of their size (the old default 2 goes to the new)
    if (savedFormat < 16 && c.menuZoomOut == 2.0f) c.menuZoomOut = Config{}.menuZoomOut;
    // format 17 (2026-11-08): vr.renderScale is the headset resolution, the eye pictures' size. On a standalone headset
    // the old value sized only its images (the pictures came from the internal resolution and were stretched into
    // them): it goes to the new default, a saved 1 would draw 3.3x the pixels. On a PC the images keep their size and
    // the pictures now match them
    if (savedFormat < 17 && !kPcVrBuild) c.renderScale = Config{}.renderScale;
    // format 18: a standalone headset's default 0.55 (format 17's, one test build) to 0.65 (a value chosen since stays)
    if (savedFormat == 17 && !kPcVrBuild && c.renderScale == 0.55f) c.renderScale = Config{}.renderScale;
    // format 19: ambient occlusion off by default on a Quest; a Quest's settings saved before (AO on, the old default)
    // take it
    if (savedFormat < 19 && quest_headset()) c.ambientOcclusion = Config{}.ambientOcclusion;
}

inline Config from_saved(const std::vector<std::pair<std::string, std::string>>& saved) {
    Config c;
    int format = 1;
    for (const auto& [k, v] : saved) {
        from_kv(c, k, v);
        if (k == "vr.format") format = std::atoi(v.c_str());
    }
    migrate_config(c, format);
    validate(c);
    return c;
}

}  // namespace vr
