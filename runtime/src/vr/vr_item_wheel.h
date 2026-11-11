// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The VR item wheel (docs/VR_ITEM_WHEEL.md): reach over the off hand's shoulder, choose an item with the right stick,
// bring the hand forward to put it on an item button.
//
// Pure parts here (unit-tested in runtime/tools/vr_test.cpp): the inventory read from the save's bytes (WheelSave ->
// wheel_pages), the gesture and navigation (ItemWheel), and the wheel's layout. vr_game.cpp reads the save and the
// controllers and assigns the item the way the game's own item menu does; vr_item_wheel_draw.cpp draws the picture.
//
// What the wheel offers is what the game's item buttons can hold (the save keys dComIfGs_getItem reads,
// d_item_local.h getItem_inl), and only what the save has:
//   - the inventory's 21 places (keys 0x00..0x14) except the three bags themselves (containers: their contents are
//     offered on their own pages), with bottles as their contents;
//   - the Spoils Bag's spoils (keys 0x18..0x1F), the Bait Bag's baits (0x24..0x2B) and the Delivery Bag's items
//     (0x30..0x37), each page only while the save has that bag.
// Swords, shields, the Power Bracelets (equipment, not on buttons), the quest items, charts and songs are never
// offered. Selecting puts the item on a button; it is used by that button, as the game's own item menu leaves it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "vr_math.h"

namespace vr {

// ---- items

// the save's bytes the wheel reads (dSv_player_c: zeldaret/tww d_save.h, unchanged in WWHD as used here)
struct WheelSave {
    uint8_t items[21];    // inventory place -> item number (0xFF: empty)
    uint8_t got[21];      // "got" bits per place (the bow: 1 fire and ice arrows, 2 light arrows)
    uint8_t beast[8];     // the Spoils Bag (item numbers, 0xFF empty)
    uint8_t bait[8];      // the Bait Bag
    uint8_t reserve[8];   // the Delivery Bag
    uint8_t beastNum[8];  // spoils counts by dBeastIdx (the bag's order)
    uint8_t baitNum[8];   // bait counts per place
    uint8_t arrows = 0, bombs = 0, magic = 0;
    uint8_t buttons[3];   // the save keys on X, Y, R (0xFF none)
    WheelSave() {
        std::fill(std::begin(items), std::end(items), 0xFF);
        std::fill(std::begin(got), std::end(got), 0);
        std::fill(std::begin(beast), std::end(beast), 0xFF);
        std::fill(std::begin(bait), std::end(bait), 0xFF);
        std::fill(std::begin(reserve), std::end(reserve), 0xFF);
        std::fill(std::begin(beastNum), std::end(beastNum), 0);
        std::fill(std::begin(baitNum), std::end(baitNum), 0);
        std::fill(std::begin(buttons), std::end(buttons), 0xFF);
    }
};

constexpr uint8_t kNoItem = 0xFF;
constexpr uint8_t kSpoilsBag = 0x24, kBaitBag = 0x2C, kDeliveryBag = 0x30;
constexpr uint8_t kBombs = 0x31;
constexpr int kSpoilsPlace = 4, kBaitPlace = 11, kDeliveryPlace = 18;
constexpr uint8_t kFirstBeastKey = 0x18, kFirstBaitKey = 0x24, kFirstReserveKey = 0x30;

inline bool bow_item(uint8_t n) { return n == 0x27 || n == 0x35 || n == 0x36; }
// swords (0x38..0x3E) and shields (0x3B / 0x3C): never in the item wheel (the physical sword owns the sword)
inline bool sword_or_shield(uint8_t n) { return n >= 0x38 && n <= 0x3E; }
inline bool bag_item(uint8_t n) { return n == kSpoilsBag || n == kBaitBag || n == kDeliveryBag; }

// English names (the wheel's caption; the game's own names are in its message archives)
inline const char* item_name(uint8_t n) {
    switch (n) {
    case 0x1F: return "Joy Pendant";
    case 0x20: return "Telescope";
    case 0x21: return "Tingle Bottle";
    case 0x22: return "Wind Waker";
    case 0x23: return "Picto Box";
    case 0x24: return "Spoils Bag";
    case 0x25: return "Grappling Hook";
    case 0x26: return "Deluxe Picto Box";
    case 0x27: return "Hero's Bow";
    case 0x29: return "Iron Boots";
    case 0x2A: return "Magic Armor";
    case 0x2C: return "Bait Bag";
    case 0x2D: return "Boomerang";
    case 0x2F: return "Hookshot";
    case 0x30: return "Delivery Bag";
    case 0x31: return "Bombs";
    case 0x33: return "Skull Hammer";
    case 0x34: return "Deku Leaf";
    case 0x35: return "Hero's Bow";  // with Fire and Ice Arrows (the caption's second line says which)
    case 0x36: return "Hero's Bow";  // with Light Arrows
    case 0x45: return "Skull Necklace";
    case 0x46: return "Boko Baba Seed";
    case 0x47: return "Golden Feather";
    case 0x48: return "Knight's Crest";
    case 0x49: return "Red Chu Jelly";
    case 0x4A: return "Green Chu Jelly";
    case 0x4B: return "Blue Chu Jelly";
    case 0x50: return "Empty Bottle";
    case 0x51: return "Red Potion";
    case 0x52: return "Green Potion";
    case 0x53: return "Blue Potion";
    case 0x54: return "Elixir Soup (half)";
    case 0x55: return "Elixir Soup";
    case 0x56: return "Water";
    case 0x57: return "Fairy";
    case 0x58: return "Forest Firefly";
    case 0x59: return "Forest Water";
    case 0x77: return "Swift Sail";
    case 0x78: return "Sail";
    case 0x82: return "All-Purpose Bait";
    case 0x83: return "Hyoi Pear";
    case 0x8C: return "Town Flower";
    case 0x8D: return "Sea Flower";
    case 0x8E: return "Exotic Flower";
    case 0x8F: return "Hero's Flag";
    case 0x90: return "Big Catch Flag";
    case 0x91: return "Big Sale Flag";
    case 0x92: return "Pinwheel";
    case 0x93: return "Sickle Moon Flag";
    case 0x94: return "Skull Tower Idol";
    case 0x95: return "Fountain Idol";
    case 0x96: return "Postman Statue";
    case 0x97: return "Shop Guru Statue";
    case 0x98: return "Father's Letter";
    case 0x99: return "Note to Mom";
    case 0x9A: return "Maggie's Letter";
    case 0x9B: return "Moblin's Letter";
    case 0x9C: return "Cabana Deed";
    case 0x9D: return "Complimentary ID";
    case 0x9E: return "Fill-Up Coupon";
    }
    if (n >= 0x50 && n <= 0x5F) return "Bottle";
    if (n >= 0x80 && n <= 0x8B) return "Bait";
    return "Item";
}

// what the wheel shows of an item
enum class WheelPage : uint8_t { Items, Spoils, Bait, Delivery };
inline const char* page_name(WheelPage p) {
    switch (p) {
    case WheelPage::Items: return "Items";
    case WheelPage::Spoils: return "Spoils Bag";
    case WheelPage::Bait: return "Bait Bag";
    case WheelPage::Delivery: return "Delivery Bag";
    }
    return "";
}
struct WheelEntry {
    uint8_t key = kNoItem;   // the save key a button holds (the inventory place, or the bag's key)
    uint8_t item = kNoItem;  // the item number there
    int count = -1;          // ammunition, bombs or how many in the bag (-1: none shown)
    bool empty = false;      // no bombs or arrows left: the game refuses to use it (it can still go on a button)
    int button = -1;         // the item button (0 X, 1 Y, 2 R) it is on now, -1 none
    std::string detail;      // the caption's second line (arrow types, "needs magic")
};
struct WheelPageItems {
    WheelPage page = WheelPage::Items;
    std::vector<WheelEntry> entries;
};

inline int button_of(const WheelSave& s, uint8_t key) {
    for (int b = 0; b < 3; ++b)
        if (s.buttons[b] == key) return b;
    return -1;
}

// the pages the save has (in order: items, spoils, bait, delivery), each with what it holds now; empty pages and
// pages of bags the save does not have are left out
inline std::vector<WheelPageItems> wheel_pages(const WheelSave& s) {
    std::vector<WheelPageItems> pages;
    WheelPageItems items{WheelPage::Items, {}};
    for (int place = 0; place < 21; ++place) {
        const uint8_t n = s.items[place];
        // (not the sails, 0x77 / 0x78: asked for in the headset, taken off the wheel; the game's own menu still has them)
        if (n == kNoItem || bag_item(n) || sword_or_shield(n) || n == 0x28 /* the Power Bracelets */ || n == 0x77 ||
            n == 0x78)
            continue;
        WheelEntry e;
        e.key = uint8_t(place);
        e.item = n;
        e.button = button_of(s, e.key);
        if (bow_item(n)) {
            e.count = s.arrows;
            e.empty = s.arrows == 0;
            const bool fire = n == 0x35 || n == 0x36 || (s.got[place] & 1), light = n == 0x36 || (s.got[place] & 2);
            e.detail = light ? "Fire, Ice and Light Arrows" : fire ? "Fire and Ice Arrows" : "Arrows";
        } else if (n == kBombs) {
            e.count = s.bombs;
            e.empty = s.bombs == 0;
        } else if (n == 0x34 || n == 0x2A) {
            e.detail = s.magic ? "Uses magic" : "Needs magic";
        }
        items.entries.push_back(e);
    }
    if (!items.entries.empty()) pages.push_back(std::move(items));
    auto bag = [&](WheelPage page, int place, const uint8_t* slots, uint8_t firstKey, const uint8_t* counts) {
        if (s.items[place] == kNoItem) return;  // the save has no such bag
        WheelPageItems p{page, {}};
        for (int i = 0; i < 8; ++i) {
            if (slots[i] == kNoItem) continue;
            WheelEntry e;
            e.key = uint8_t(firstKey + i);
            e.item = slots[i];
            e.button = button_of(s, e.key);
            if (counts) e.count = counts[i];
            p.entries.push_back(e);
        }
        if (!p.entries.empty()) pages.push_back(std::move(p));
    };
    bag(WheelPage::Spoils, kSpoilsPlace, s.beast, kFirstBeastKey, s.beastNum);
    bag(WheelPage::Bait, kBaitPlace, s.bait, kFirstBaitKey, s.baitNum);
    bag(WheelPage::Delivery, kDeliveryPlace, s.reserve, kFirstReserveKey, nullptr);
    return pages;
}

// The assignment the game's item menu makes (hdIngameUi_c::assignItemSlot, as story_item_buttons_test.cpp
// records it): the key goes on `button`; a button that held it already gets the button's old key (a swap).
// Returns the new keys of X, Y, R.
struct ButtonKeys { uint8_t keys[3]; };
inline ButtonKeys assign_button(const uint8_t before[3], int button, uint8_t key) {
    ButtonKeys out{{before[0], before[1], before[2]}};
    if (button < 0 || button > 2) return out;
    for (int b = 0; b < 3; ++b)
        if (b != button && before[b] == key) out.keys[b] = before[button];
    out.keys[button] = key;
    return out;
}

// The forward pull also presses the button, so Link takes the item out at once (asked for in the headset: no press of
// X needed), except for what the press would use up or start: a filled bottle (drunk, let out), bait (thrown), the
// Tingle Bottle (its event), and the bags' spoils and delivery items (shown as "can't use"). Those only go on the
// button (daPy_lk_c::checkNewItemChange decides what a press does).
inline bool wheel_presses_button(uint8_t item) {
    if (item >= 0x51 && item <= 0x5F) return false;  // a bottle with something in it
    if (item >= 0x80 && item <= 0x8B) return false;  // bait
    if ((item >= 0x45 && item <= 0x4B) || item == 0x1F) return false;  // spoils
    if (item >= 0x8C && item <= 0x9E) return false;  // delivery items
    return item != 0x21;                             // the Tingle Bottle
}

// ---- the gesture and the navigation

enum class WheelPhase : uint8_t { Idle, Reaching, Selecting, Committing, Cooldown };
inline const char* wheel_phase_name(WheelPhase p) {
    switch (p) {
    case WheelPhase::Idle: return "idle";
    case WheelPhase::Reaching: return "reaching";
    case WheelPhase::Selecting: return "selecting";
    case WheelPhase::Committing: return "committing";
    case WheelPhase::Cooldown: return "cooldown";
    }
    return "?";
}

// one update's facts (the off hand in the body's frame from the head: x towards the off hand's side... see below)
struct WheelInput {
    bool allowed = false;     // first-person play, nothing blocking (vr_game.cpp wheel_allowed)
    bool tracked = false;     // the off hand is tracked now (not lost, not held through a gap)
    // the off hand from the head, in the body's frame (metres: x right, y up, z back)
    Vec3 hand;
    int side = -1;            // the off hand's shoulder: -1 left, +1 right
    bool swordReaching = false;  // the sword hand is at its own shoulder (the draw): the wheel does not open
    float stickX = 0, stickY = 0;  // the right controller's stick
    bool cancel = false;      // the right stick clicked (a deliberate cancel)
    // the off hand's grip is held: it opens the wheel at the shoulder and keeps it open there (asked for in the headset:
    // the hand over the holder alone opened it); once the arm pulls forward it no longer matters
    bool grip = false;
    // the sword hand's trigger, while Link holds nothing (asked for in the headset): a press opens the wheel, the right
    // stick chooses, letting go takes the choice out. triggerHand: that controller is tracked; emptyHanded: Link holds
    // nothing (no sword, item or carried thing out), needed to open it
    bool trigger = false, triggerHand = false, emptyHanded = false;
    float dt = 0;
};
struct WheelEvent {
    bool opened = false, committed = false, cancelled = false;
    const char* why = "";     // the cancel's reason (log)
};

class ItemWheel {
public:
    // the shoulder: as the sword's draw (vr_sword.h ShoulderReach) on the off hand's side; inside within kEnter of the
    // point, out again beyond kExit
    static constexpr float kEnter = 0.28f, kExit = 0.34f;
    static constexpr float kOpenDwell = 0.12f;   // s inside before the wheel opens (passing through does not)
    static constexpr float kCommitWindow = 0.8f; // s from leaving the shoulder to reaching the front
    static constexpr float kCooldown = 0.4f;
    // the front of the torso: this far ahead of the head (body frame), not below the waist, not far to a side
    static constexpr float kFrontAhead = 0.18f, kFrontLow = 0.75f, kFrontSide = 0.5f;
    // the stick: pushed beyond kPush along one axis to step, back within kRelease for the next step; held, it repeats
    static constexpr float kPush = 0.55f, kRelease = 0.3f, kRepeatFirst = 0.4f, kRepeat = 0.16f;

    WheelPhase phase() const { return phase_; }
    bool open() const { return phase_ == WheelPhase::Selecting || phase_ == WheelPhase::Committing; }
    bool consumes_stick() const { return phase_ != WheelPhase::Idle && phase_ != WheelPhase::Cooldown; }
    int page() const { return page_; }
    int index() const { return index_; }
    // the pages and their sizes changed (the inventory read again): the selection stays on the same key when it can
    void set_pages(const std::vector<int>& sizes) {
        sizes_ = sizes;
        clamp();
    }
    void select(int page, int index) {
        page_ = page;
        index_ = index;
        clamp();
    }
    static Vec3 shoulder(int side) { return {0.15f * float(side), -0.08f, 0.12f}; }
    static bool at_front(Vec3 h) { return h.z < -kFrontAhead && h.y > -kFrontLow && std::fabs(h.x) < kFrontSide; }

    WheelEvent update(const WheelInput& in) {
        WheelEvent e;
        const float d = length(in.hand - shoulder(in.side));
        inside_ = in.tracked && (inside_ ? d < kExit : d < kEnter);
        auto cancel = [&](const char* why) {
            e.cancelled = true;
            e.why = why;
            phase_ = WheelPhase::Cooldown;
            timer_ = 0;
        };
        const bool triggerPress = in.trigger && !triggerWas_;
        triggerWas_ = in.trigger;
        if (byTrigger_ && open()) {
            if (!in.allowed) { cancel("play no longer allows it"); return e; }
            if (!in.triggerHand) { cancel("the sword hand lost tracking"); return e; }
            if (in.cancel) { cancel("the right stick clicked"); return e; }
            navigate(in);
            if (!in.trigger) commit(e);  // let go: the choice taken out
            return e;
        }
        if (consumes_stick() && phase_ != WheelPhase::Reaching) {
            if (!in.allowed) { cancel("play no longer allows it"); return e; }
            if (!in.tracked) { cancel("the off hand lost tracking"); return e; }
            if (in.cancel) { cancel("the right stick clicked"); return e; }
        }
        timer_ += in.dt;
        if (phase_ == WheelPhase::Idle && triggerPress && in.triggerHand && in.emptyHanded && in.allowed) {
            phase_ = WheelPhase::Selecting;
            byTrigger_ = true;
            e.opened = true;
            resetStick();
            return e;
        }
        if (phase_ == WheelPhase::Idle || phase_ == WheelPhase::Reaching) byTrigger_ = false;
        switch (phase_) {
        case WheelPhase::Idle:
            if (inside_ && in.grip && in.allowed && armed_ && !in.swordReaching) {
                phase_ = WheelPhase::Reaching;
                timer_ = 0;
            }
            if (!inside_) armed_ = true;
            break;
        case WheelPhase::Reaching:
            if (!inside_ || !in.grip || !in.allowed || in.swordReaching) {
                phase_ = WheelPhase::Idle;  // passed through: nothing opened
            } else if (timer_ >= kOpenDwell) {
                phase_ = WheelPhase::Selecting;
                armed_ = false;
                e.opened = true;
                resetStick();
            }
            break;
        case WheelPhase::Selecting:
            if (inside_ && !in.grip) { cancel("the grip was let go at the shoulder"); return e; }
            navigate(in);
            if (!inside_) {
                phase_ = WheelPhase::Committing;
                timer_ = 0;
                if (at_front(in.hand)) commit(e);
            }
            break;
        case WheelPhase::Committing:
            navigate(in);
            if (inside_) {
                if (!in.grip) { cancel("the grip was let go at the shoulder"); return e; }
                phase_ = WheelPhase::Selecting;  // back at the shoulder: still choosing
            } else if (at_front(in.hand)) {
                commit(e);
            } else if (timer_ > kCommitWindow) {
                cancel("the hand did not come to the front");
            }
            break;
        case WheelPhase::Cooldown:
            if (!inside_) armed_ = true;
            if (timer_ >= kCooldown) phase_ = WheelPhase::Idle;
            break;
        }
        return e;
    }
    void reset() {
        phase_ = WheelPhase::Idle;
        inside_ = false;
        armed_ = true;
        byTrigger_ = false;
        timer_ = 0;
        resetStick();
    }
    bool by_trigger() const { return byTrigger_ && open(); }

private:
    void commit(WheelEvent& e) {
        e.committed = !sizes_.empty();
        if (!e.committed) e.cancelled = true, e.why = "nothing to choose";
        phase_ = WheelPhase::Cooldown;
        timer_ = 0;
    }
    void clamp() {
        if (sizes_.empty()) { page_ = index_ = 0; return; }
        page_ = std::clamp(page_, 0, int(sizes_.size()) - 1);
        index_ = sizes_[page_] > 0 ? std::clamp(index_, 0, sizes_[page_] - 1) : 0;
    }
    void resetStick() { held_ = 0; repeat_ = 0; }
    // the stick's left / right steps through the page's items (around the ring), up / down through the pages
    void navigate(const WheelInput& in) {
        if (sizes_.empty()) return;
        const float ax = std::fabs(in.stickX), ay = std::fabs(in.stickY);
        int dir = 0;  // 1 next, -1 previous; 2 next page, -2 previous page
        if (std::max(ax, ay) >= kPush) dir = ax >= ay ? (in.stickX > 0 ? 1 : -1) : (in.stickY < 0 ? 2 : -2);
        if (held_ && std::max(ax, ay) < kRelease) held_ = 0;
        if (!dir) return;
        if (held_ != dir) {  // a new push (or another direction): one step now, then repeats while held
            held_ = dir;
            repeat_ = kRepeatFirst;
            step(dir);
            return;
        }
        repeat_ -= in.dt;
        if (repeat_ <= 0) {
            repeat_ += kRepeat;
            step(dir);
        }
    }
    void step(int dir) {
        const int pages = int(sizes_.size());
        if (dir == 2 || dir == -2) {
            if (pages > 1) {
                page_ = (page_ + (dir > 0 ? 1 : pages - 1)) % pages;
                index_ = std::min(index_, std::max(0, sizes_[page_] - 1));
            }
            return;
        }
        const int n = sizes_[page_];
        if (n > 0) index_ = (index_ + (dir > 0 ? 1 : n - 1)) % n;
    }

    WheelPhase phase_ = WheelPhase::Idle;
    bool inside_ = false, armed_ = true;
    bool byTrigger_ = false, triggerWas_ = false;  // opened by the sword hand's trigger (WheelInput::trigger)
    float timer_ = 0, repeat_ = 0;
    int held_ = 0;
    std::vector<int> sizes_;
    int page_ = 0, index_ = 0;
};

// ---- the wheel's layout (the picture, vr_item_wheel_draw.cpp): an item's centre on the ring, in pixels from the
// picture's centre (+x right, +y down); the first item at the top, clockwise
// The wheel's layout in its square picture of `side` pixels (asked for in the headset: the title outside the gold ring,
// the ring big, the icons as large as fit, hugging its inside, the page's number in the middle): the ring's radius, the
// circle the icons' centres lie on, and their radius (the chosen one drawn kWheelChosenScale larger, still inside)
struct WheelLayout {
    float ring = 0;     // the gold ring's radius
    float centres = 0;  // the icons' centres from the middle
    float icon = 0;     // an icon's circle radius (unchosen)
};
constexpr float kWheelChosenScale = 1.15f;
constexpr float kWheelTitleBand = 56.0f;  // pixels above the ring for the title
inline WheelLayout wheel_layout(float side, int n) {
    WheelLayout l;
    const float mid = side * 0.5f;
    l.ring = mid - kWheelTitleBand;
    const float pad = 6.0f;  // between an icon and the ring, and between neighbours
    // the largest radius r with the chosen icon (r * kWheelChosenScale) inside the ring and neighbours apart: centres at
    // ring - pad - r S, the chord between neighbours 2 c sin(pi / n) at least 2 r + pad, so
    // r <= (2 s (ring - pad) - pad) / (2 + 2 s S)
    const int k = n < 1 ? 1 : n;
    const float s = k > 1 ? std::sin(kPi / float(k)) : 1.0f;
    float r = (2.0f * s * (l.ring - pad) - pad) / (2.0f + 2.0f * s * kWheelChosenScale);
    r = std::min(r, l.ring * 0.22f);  // a few items: not one huge circle each
    l.icon = std::max(r, 12.0f);
    l.centres = l.ring - pad - l.icon * kWheelChosenScale;
    return l;
}
inline void ring_position(int i, int n, float radius, float& x, float& y) {
    const float a = n > 0 ? 2.0f * kPi * float(i) / float(n) : 0.0f;
    x = radius * std::sin(a);
    y = -radius * std::cos(a);
}

}  // namespace vr
