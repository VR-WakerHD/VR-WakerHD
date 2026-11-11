# VR: the item wheel

## How it works

**With the sword hand's trigger** (asked for in the headset, 2026-10-31): while Link holds nothing (no sword, item or
carried pot out), **hold the sword hand's trigger**: the wheel opens. Choose with the **right stick**, then **let go
of the trigger**: the item goes on `vr.itemWheelButton` and is taken out, as below. Clicking the right stick cancels.
The trigger presses no button of its own (A is the sword hand's primary button); with an aimed item out it fires it,
as before. Or by the shoulder:

1. **Open:** reach over the shoulder of your **shield hand** (the off hand: the left one, or the right one with
   `vr.leftHanded=1`) and **hold its grip**, for a moment. The hand over the shoulder alone opens nothing (changed
   after the headset: it opened by accident). The wheel opens in front of you, 0.65 m
   away, a little below your eyes and to the shield hand's side, facing you (it was 0.42 m away beside that shoulder).
2. **Choose:** use the **right stick**.
   - Left / right moves around the ring.
   - Up / down changes the page: Items, Spoils Bag, Bait Bag, Delivery Bag.
   - Holding the stick repeats. Letting it go keeps the choice.
3. **Take it out:** **pull the arm forward** in front of you. The grip may be let go on the way. The chosen item
   goes on the item button `vr.itemWheelButton` (X by default), and that button is pressed for you, so Link takes it
   out at once, as if you had pressed X (asked for in the headset).
   - **Only put on the button, not pressed:** what a press would use up or start, namely a filled bottle (drunk, let
     out), bait (thrown), the Tingle Bottle (its event), and the bags' spoils and delivery items.
   - Iron Boots and Magic Armor are put on, as a press does.

**Cancel** by letting go of the grip while the hand is still at the shoulder, by clicking the right stick, or by
lowering the hand to your side instead of pulling it forward. It also closes, without a choice, when:
- the shield hand loses tracking;
- the controllers stop sending input;
- play stops allowing it (an event or cutscene, a GamePad menu, the telescope, third person, Link falls);
- you take the headset off and the session stops.

It works in first-person play only. In third person the controllers play as on the GamePad, and the game's own item
menu (on the GamePad panel) works as usual everywhere.

## What it offers

What the save has, read from the save each time it opens and every 0.25 s while it is open (`wheel_pages`,
`vr_item_wheel.h`). These are the keys the game's item buttons can hold (`dComIfGs_getItem`):

| Page | What | Shown with it |
|---|---|---|
| Items | the inventory's 21 places, except the three bags | the bow: arrows left and which arrows it has; bombs: how many (dimmed at 0); Deku Leaf, Magic Armor: magic |
| Spoils Bag | its spoils (only with the bag) | how many of each |
| Bait Bag | its baits (only with the bag) | how many |
| Delivery Bag | its letters, statues, flags and other trading items (only with the bag) | |

**The Items page:** Telescope, Sail (Swift Sail in HD), Wind Waker, Grappling Hook, Boomerang, Deku Leaf, Tingle
Bottle, Picto Box / Deluxe Picto Box, Iron Boots, Magic Armor, Hero's Bow, Bombs, Hookshot, Skull Hammer, and the
four bottles.

**Bottles** show as what they hold: an empty bottle, Red, Green or Blue Potion, Elixir Soup (whole or half), Water,
Forest Water, Fairy, Forest Firefly. Pressing the button afterwards does what the game does with that bottle:
- drink a potion or soup;
- let a fairy out;
- swing an empty bottle.

**Not offered** (the game does not put them on item buttons):
- swords: the physical sword owns them;
- shields;
- the Power Bracelets;
- the bags themselves (their contents are offered instead);
- songs, charts, pearls, Triforce pieces and other quest items.

**Icons:** the game's own icons (`BtnItemIcon_00`'s `Icon128_nn`), read from your own 2D pack when the wheel first
opens (`item_icons::game_icons`, on a worker thread). Each item's icon frame comes from the game's own table
(hdMenuItem `itemForSlot`, 57 entries at 100E7AB8). The Wind Waker is not in that table, and none of the button
icons is it. Its picture is the Quest screen's (`BtnCollectIcon_00`'s `CollectIcon118_07`), kept after the game's frames
([VR_WIND_WAKER.md](VR_WIND_WAKER.md)). The sails are not in the table either; they show their name in the circle
instead.

The names are English (`item_name`). The game's own names are in its message archives, which the port does not read
for this.

## Putting the item on the button: the game's assignment

`wheel_assign` (`vr_game.cpp`) does what the game's item menu does (`hdIngameUi_c::assignItemSlot`, as
`story_item_buttons_test.cpp` records it):
- The save key goes on the button (`dSv_player_status_a_c::mSelectItem`, save + 0x29).
- A button that held that key already takes the button's old key (a swap).
- The play's item numbers follow (`dComIfGp_setSelectItem`, play + 0x5BBB): the item for the key, or none. A key
  that holds nothing is cleared.
- A mod item or the story notebook on that button comes off it, as after the game's own menu
  (`story_item_buttons::game_assigned`).

The HUD's X / Y / R icons read the save key, so they follow at once, on the wrist and on the screen.

## The gesture (`ItemWheel`, `vr_item_wheel.h`)

| Phase | Enters when | Leaves |
|---|---|---|
| Idle | | the off hand comes within 0.28 m of its shoulder point with its grip held, play allows it, and the hand has left the shoulder since the last time |
| Reaching | | 0.12 s there with the grip held: Selecting (opened). Out again or the grip let go sooner: Idle (passing through opens nothing) |
| Selecting | opened | the grip let go at the shoulder: cancelled. The hand leaves the shoulder (beyond 0.34 m): Committing (from here the grip no longer matters) |
| Committing | | in front (0.18 m ahead of the head, above the waist, within 0.5 m to the side): committed. Back at the shoulder: Selecting. Not in front within 0.8 s: cancelled |
| Cooldown | committed or cancelled | 0.4 s, and it opens again only once the hand has left the shoulder |

**The shoulder point** is the sword draw's (`ShoulderReach`, `vr_sword.h`), on the off hand's side. It is 0.15 m to
the side, 0.08 m below and 0.12 m behind the head. It is measured in the body's frame: the head's yaw, followed
slowly as for the shoulders. So turning, room-scale walking and seated play move it with you, and no fixed spot in
the room is used. The 0.06 m between entering and leaving keeps tracking jitter from opening and closing it.

**Arbitration with the sword:**
- The wheel does not open while the sword hand is at its own shoulder (the draw).
- The draw is not taken while the wheel is open.
- The two hands' shoulders are on opposite sides.

**While the wheel is open:**
- The right stick neither turns nor reaches the game.
- The shield hand's X and Y and the crouch (right stick down) are held back, so nothing is used by accident while
  choosing.
- The click that cancels it never also toggles the camera view.

**The stick:**
- A push beyond 0.55 along one axis is one step.
- It must come back within 0.3 before the same push counts again.
- Held, it repeats after 0.4 s, then every 0.16 s.
- Light haptics on the right controller mark each step. The off hand gets a pulse when the wheel opens and a firmer
  one on a choice.

## The picture

**Placement:** a 0.32 m square, 0.42 m ahead of the head, 0.17 m to the off hand's side and 0.12 m below the eyes. It
is turned to the body's yaw at opening and faces the head.
- It follows the head's position smoothly (10/s), so it does not wobble with tracking noise.
- It keeps its turn, so you can look away from it.
- It never comes nearer than its distance.

**Drawing:**
- An OpenXR quad layer of its own (`Session::kWheel`, `end_frame`), blended by its alpha. One picture is seen by both
  eyes at its place in the room, so it is in stereo by construction and drawn once.
- The picture (768 x 768) is drawn on the CPU (`vr_item_wheel_draw.cpp`) only when what it shows changes (page,
  selection, inventory). It is copied into the swapchain image each frame it is shown.
- The captions use a system font (Segoe UI or Arial on Windows, DejaVu or Noto on Linux) through stb_truetype (the
  copy bundled with Dear ImGui).

The game's HUD and the wrist displays are untouched: the wheel is a layer of its own.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `vr.itemWheel` | 1 | the item wheel on or off |
| `vr.itemWheelButton` | x | the button the choice goes on: `x`, `y` or `r` |

Both are in the launcher (Controls > Sword and arms) and in the game's settings overlay (F1 > VR headset).

## Tests

- **`vr_test` `test_item_wheel`:**
  - **Inventory:** only owned items; no swords; bags as pages, and only with the bag; bottles as their contents;
    arrow and bomb counts (empty bombs dimmed); which button an item is on.
  - **Assignment:** the swap between buttons.
  - **Gesture:**
    - passing through the shoulder does not open it, holding there does;
    - stick steps, repeats and the dead zone;
    - commit in front;
    - cancels: by timeout, by lost tracking, when play no longer allows it, by the stick click;
    - the cooldown, and re-arming once the hand has left the shoulder;
    - no opening while the sword hand reaches;
    - the left-handed shoulder.
- **`item_icons_test`:**
  - **Decoding:** RGBA8 untiling round trip in every tile mode and swizzle, the .bflim reader, the icon archive's
    reader.
  - **The real pack:** with `WWHD_TEST_2D_PACK` set to the player's pack, all 55 game icons decode
    (`permanent_2d_UsEnglish.pack`: 55 of 55).
- **Picture preview:** drawn offline with the game's real icons and the item → icon table read from the game's RPX.
  The ring, the highlight, counts, button badges, page captions and the no-icon names were checked.
- **In the game, without a headset** (desktop, `--no-vr`, a copy of the player's last session):
  - `WWHD_VR_TEST_WHEEL=step[:page:index[:button]]` logs the wheel's pages as read from the save, then runs
    `wheel_assign` at that step.
  - **The save read:** the Telescope, save key 00, icon frame 10.
  - **The assignment:** before, Telescope on Y (play item Y 0x20). After, keys X 00 / Y FF, play items X 0x20 / Y
    none. That is the game's swap, and it held for the next 10 s.
  - **The HUD:** it showed the Telescope on X.
  - **Using it:** a scripted X press brought up the Telescope's view.

**Not yet checked in the headset:**
- the gesture's feel and thresholds;
- the picture's size, place and readability;
- the right stick's feel;
- the quad layer on the runtime (SteamVR).

## Limits

- **Arrows:** the arrow type is chosen as in the game (the bow's own arrow switching). The wheel puts the bow on a
  button, not a particular arrow.
- **Captions:** English names, in a system font, not the game's.
- **A held item:** not checked: what Link does with an item already in his hands when the wheel changes its button.
  The HD game changes buttons live from the GamePad as well.
- **The Swift Sail:** item number 0x77 is named the Swift Sail because the game treats 0x77 with the sail (0x78) in
  checkNewItemChange; it was not seen in a save.
