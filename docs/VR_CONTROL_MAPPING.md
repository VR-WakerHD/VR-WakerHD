# VR control mapping

How the VR controllers drive the game. The game itself is unchanged: the controllers become the Wii U GamePad's
buttons and sticks (`runtime/src/vr/vr_game.cpp`, at the end of the pad read), so every action works exactly as the
game decides (combos, targeting, context buttons, menus).

## Hands

The **sword hand** is the right controller, the **shield hand** the left. `vr.leftHanded=1` swaps them (the sword in
the left hand). Link's arms follow the controller on their own side (left arm, left controller) in both modes; the
sticks keep their roles.

## Fixed inputs

| Input | Does |
|---|---|
| Left stick | movement, through the game's movement code (radial dead zone `vr.stickDeadZone`); forward is where you look (`vr.moveDirection=hand`: where the left controller points) |
| Right stick left/right | snap or smooth turning (`vr.turn`) |
| Right stick while aiming the bow, hookshot or boomerang | aims, as on the GamePad (turning pauses) |
| A swing of the sword-hand controller | presses B for two logic steps: the game chooses the cut; the sword's hit capsules follow the swept blade |
| Both stick clicks held 0.6 s | recentre (not sent to the game) |

## Bindable buttons (defaults)

Each controller input presses a GamePad button; change it in the launcher (Controllers tab), or with
`vr.bind.<action>=<button>` in `settings.ini`.

| Action (`vr.bind.`) | Controller input | Default button | In the game |
|---|---|---|---|
| `mainTrigger` | sword-hand trigger | (none) | with nothing in Link's hands, opens the item wheel (held: choose, let go: take it out); fires aimed items. A until settings format 10 |
| `mainPrimary` | sword-hand A / Cross | A | talk, open, pick up, climb |
| `mainSecondary` | sword-hand B / Circle | B | sword (also without a swing) |
| `mainGrip` | sword-hand grip | R | the item on R |
| `mainStickClick` | sword-hand stick click | Minus | Minus |
| `offTrigger` | shield-hand trigger | ZL | target lock (L-targeting) |
| `offGrip` | shield-hand grip | (none) | closes Link's hand; over the shield hand's shoulder it opens the item wheel |
| `rightStickDown` | right stick down (held) | ZR | shield, crouch |
| `offPrimary` | shield-hand X / Square | X | the item on X |
| `offSecondary` | shield-hand Y / Triangle | Y | the item on Y |
| `offStickClick` | shield-hand stick click | LeftStickClick | — |
| `menu` | menu button (left controller) | Plus | pause menu (on the GamePad panel) |

Buttons: `A B X Y L R ZL ZR Plus Minus Home Up Down Left Right LeftStickClick RightStickClick`, or empty for nothing.
Holding a button does not repeat its action: the game reacts to the press.

The controller side comes from the OpenXR runtime: the game suggests bindings for the Khronos simple controller,
Oculus Touch (Quest, Rift; PS VR2 Sense through SteamVR), Valve Index, HTC Vive and Windows Mixed Reality; the
runtime's binding editor (SteamVR: Controllers > Manage controller bindings) can move any physical input. The Index
has no menu button: bind "Menu" there.

## Supported and not supported

| Wanted | Status |
|---|---|
| Movement, turning | supported (fixed sticks) |
| Pause / menu | supported (menu button; the menu shows on the GamePad panel) |
| Inventory, map, item assignment | through the pause/GamePad menus (as on the Wii U, where they are on the GamePad screen); no separate shortcut yet |
| Interaction | supported (A: triggers / primary) |
| Primary / secondary actions, items | supported (X, Y, R bindings) |
| Sword | supported (swing, or the B binding) |
| Shield | supported (ZR: the shield-hand grip) |
| Target lock | supported (ZL: the shield-hand trigger) |
| Recenter | supported (both stick clicks; Settings > VR > Recentre) |
| Touching the GamePad panel | not supported yet |
| Aiming items with the controller's direction | not supported yet (right stick) |
