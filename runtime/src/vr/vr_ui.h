// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The VR view's UI (docs/VR_UI.md): the one place that decides what of the game's 2D UI the VR view shows, and where.
// In first-person VR the game's gameplay HUD is not shown; the VR view's own wrist HUD (vr_wrist_hud.h: hearts, item
// buttons, rupees on the wrists) takes its place. Third person shows the game's HUD and no wrist HUD. Of the game's TV layouts the VR view keeps only the action prompt and the
// dialogue (its box at its 16:9 place while the layouts are drawn shrunk), and everything that is not gameplay HUD
// (menus, the title, the telescope's eyepiece, text). aspect.cpp calculates the layouts' panes and carries out the
// decision for each one; the layout mode (drawn smaller in events and menus, the telescope's eyepiece) is chosen in
// vr_game.cpp's frame update and applied by the renderer (gx2_core.cpp, vulkan/draw.cpp).
//
// classify and action are pure (vr_test.cpp); state reads the live conditions.
#pragma once
#include <cstdint>
#include <string_view>

namespace vr::ui {

// what a pane is to the VR view, from itself and its parents up to the layout's root
enum class Area {
    Other,          // not the gameplay HUD: menus, the title, dialogue text, the telescope's eyepiece
    Hud,            // the gameplay HUD: a pane with an aspect HUD rule (aspect_panes.h: the elements at the screen's
                    // edges, hearts, rupees, item buttons, keys, timers, gauges, the place name) or one inside such a pane
    ActionPrompt,   // the action prompt at the bottom ("Speak", "Open", "Climb": CommandGuide_00's L_CommandA_00), or
                    // inside it
    MessageAnchor,  // the dialogue windows' N_Position_00 (MsgWindowMain_00, MsgWindowEvent_00), or inside it
    Telescope,      // the telescope's overlay (Telescope_00: its round mask W_Wipe_00, the zoom gauge, the stick prompts;
                    // N_All_00 with N_Mode_00 in it, a child of the root)
};
// one pane on the way from a pane up to its layout's root: its name, whether its parent is the root, and whether it
// has an aspect HUD rule
struct Link { std::string_view name; bool parentIsRoot = false, hudRule = false; };
// the pane's area from it and its parents (`chain[0]` the pane itself): the telescope's overlay (`telescope`: the
// layout's child of the root is Telescope_00's N_All_00), then the nearest kept pane (the action prompt, the
// dialogue's anchor), then any HUD pane
inline Area classify(const Link* chain, int count, bool telescope = false) {
    if (telescope) return Area::Telescope;
    bool hud = false;
    for (int i = 0; i < count; ++i) {
        const Link& l = chain[i];
        if (l.parentIsRoot && l.name == "L_CommandA_00") return Area::ActionPrompt;
        if (l.parentIsRoot && l.name == "N_Position_00") return Area::MessageAnchor;
        hud = hud || l.hudRule;
    }
    return hud ? Area::Hud : Area::Other;
}

// the conditions the decisions depend on
struct State {
    bool vr = false;      // a VR session runs
    bool firstPerson = false;  // and its view is first person (third person shows the game's HUD)
    bool shrunk = false;  // the TV's layouts are drawn shrunk into the native screen (events, transitions, menu
                          // scenes; not the telescope): vr::layouts_shrunk
    bool telescope = false;  // Link looks through the telescope
    // not normal play: an event or cutscene runs (NPC conversations are events), a message or dialogue is open, or the
    // player does not control Link (aspect::gameplay_or_conducting: the game's own event, message and demo state, the
    // same the portable saves check). Conducting the Wind Waker counts as play. Independent of the text's zoom setting
    bool event = false;
    bool noHeatHaze = false;  // vr.heatHaze off: the projection group is not drawn in play either
    bool bombInHand = false;  // first person, a bomb in the hand (thrown with it: its "A Throw" prompt not shown)
    bool boatView = false;    // one of the boat's views, the Red Lion's or the Free Roam Camera: none of the game's HUD,
                              // not even the action prompt (asked for, 2026-11-06); the Free Roam Camera's label is the
                              // VR view's own (vr_wrist_hud.h)
};
enum class Action {
    Game,         // as the game calculates it (its place, its aspect rule)
    Transparent,  // calculated with alpha 0 (the game's alpha again after): not seen, nothing else changes
    NativePlace,  // without its aspect rule: at its 16:9 place
};
// what the VR view does with a pane of `area`. `self`: it is the chain's first pane (only the anchor itself is moved)
inline Action action(Area area, bool anchorItself, const State& s) {
    if (!s.vr) return Action::Game;
    switch (area) {
    // the wrist HUD replaces it in first person; third person shows the game's own HUD, as on the TV
    case Area::Hud: return s.firstPerson || s.boatView ? Action::Transparent : Action::Game;
    // the telescope's overlay only while looking through it: the game fades it out over its camera's return (about
    // 2.6 s), drawn shrunk in the middle of the view then (seen in the headset: the cut-out stayed after putting the
    // telescope away)
    case Area::Telescope: return s.telescope ? Action::Game : Action::Transparent;
    case Area::MessageAnchor:  // the dialogue's box stays inside what the shrunk layouts' clip keeps
        return s.shrunk && anchorItself ? Action::NativePlace : Action::Game;
    // the action prompt, except "A Throw" with a bomb in the hand: the hand throws it (asked for in the headset)
    case Area::ActionPrompt: return s.bombInHand || s.boatView ? Action::Transparent : Action::Game;
    case Area::Other: break;
    }
    return Action::Game;
}

// whether a particle group of the game's particle manager (dPa_control_c::draw) is drawn: not the 2D groups in VR
// (2Dfore 7, 2Dback 8: the HUD's sparkles, a rupee's or a button's twinkle, screen-space flashes). Drawn in the
// picture's 2D space, they floated in the view as stars where the game's HUD was (seen in the headset), in first
// person and in third person too, where the HUD itself is shown (seen in the headset: stars flashing in the UI). The
// world's particles and the menus' (2DmenuFore 9, 2DmenuBack 10, and HD's two after) are drawn.
// Nor, outside normal play (State::event: dialogue, cutscenes, events), the projection group (4): the particles that
// bend the picture behind them, the heat haze over lava and hot ground (o kagerou00, 0x4004, set in group 4 at
// 0239C464). The dialogue's box is drawn small in the middle of the view, where the haze is, and its text wavered with
// it (reported in the headset). Not drawn at all, rather than drawn weaker: nothing of it reaches the view. The
// particles keep running (only their draw is skipped), so the haze is back as it was the first frame play resumes.
// Normal play keeps the haze, unless Disable heat haze (vr.heatHaze off, docs/VR_SCREEN_EFFECTS.md): then it is not drawn
// in play either (in a headset it shimmers fixed to the head, and its draws cost frames in Dragon Roost and lava rooms)
constexpr uint32_t kProjectionGroup = 4;
inline bool draws_particle_group(uint32_t group, const State& s) {
    if (!s.vr) return true;
    if (group == 7 || group == 8) return false;
    return !((s.event || s.noHeatHaze) && group == kProjectionGroup);
}

// the live conditions (game thread)
State state();

}  // namespace vr::ui
