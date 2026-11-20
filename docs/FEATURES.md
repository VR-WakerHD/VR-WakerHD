# Wind Waker HD VR features: first person, motion controls, physical sword

The full list of what **VR-WakerHD** adds to *The Legend of Zelda: The Wind Waker HD*: an open-source VR mode built on
the ZeldaWWHDRecomp native port, for PC VR (OpenXR) and standalone Meta Quest 3 / 3S. Features marked
*experimental* are new and less tested. Every setting is in [VR_SETTINGS.md](VR_SETTINGS.md).

## First-person VR and head tracking

- **The view at Link's eyes**, in stereo 3D, with full 6DoF head tracking. Leaning is followed, and the world is scaled
  to Link's arms. Details: [VR_FIRST_PERSON_CAMERA.md](VR_FIRST_PERSON_CAMERA.md).
- **Link turns to face where you look** when you stand still. Walking, he faces where he walks.
- **Third person** (right stick click) uses the game's own camera, turned by your head.
- **Sailing views** on the Great Sea ([VR_SAILING_VIEWS.md](VR_SAILING_VIEWS.md)).

## Motion controls

- **Your arms are Link's arms**: inverse kinematics with natural elbows and shoulders. Hands keep their pose through
  short tracking gaps ([VR_ARM_ANIMATION.md](VR_ARM_ANIMATION.md)).
- **Physical sword**: you draw it over your shoulder, and full swings play the game's attacks. The blade also cuts and
  hits along its tracked path, with hit vibration. Spin attacks turn the view with Link.
- **Combat**: lock-on with the trigger, automatic parry, and a shield that guards on its own while locked on. Grip to
  block.
- **Items in your hands** ([VR_HELD_ITEMS.md](VR_HELD_ITEMS.md)):
  - **The bow**: pull the string with your drawing hand.
  - **The Skull Hammer.**
  - **The Wind Waker**: conduct with your hand or the sticks ([VR_WIND_WAKER.md](VR_WIND_WAKER.md)).
  - **The boomerang and grappling hook**, aimed from your view.
  - **The telescope and the Picto Box**, in their own frames.
- **Item wheel at your shoulder**: reach over your shield shoulder, choose with the stick, and pull the item out
  ([VR_ITEM_WHEEL.md](VR_ITEM_WHEEL.md)).
- **Ropes, ladders, sidling and ledges** follow the first-person view.
- **Left-handed play** swaps the sword and shield hands.

## Interface in VR

- **Wrist HUD**: hearts, items, rupees and magic on Link's wrists, shown when you look at them
  ([VR_WRIST_HUD.md](VR_WRIST_HUD.md)).
- **Menus on panels**: pause, map, bag and the GamePad screen, on a panel in front of you
  ([VR_PAUSE_MENU.md](VR_PAUSE_MENU.md)).
- **Dialogue, subtitles and tutorials** in the headset.
- **In-headset settings menu** (left stick click; [VR_MENU.md](VR_MENU.md)).
- **VR launcher** on Windows, Linux, SteamOS and Quest: headset check, refresh rate, controls and graphics, then
  Play VR or Play Flat ([VR_LAUNCHER.md](VR_LAUNCHER.md)).

## Comfort

- Smooth or snap turning, and **Move where you look**.
- **Steady hands while turning**: hands, sword and shield stay steady while the world turns.
- **Smooth Camera while turning**.
- *Experimental:* **Head movement smoothing** at 45 or 36 fps.
- Depth of field, bloom and ambient occlusion can be turned off.

## Graphics and performance

- **Multiview**: both eyes drawn in one pass ([VR_MULTIVIEW.md](VR_MULTIVIEW.md)).
- **Foveated rendering** on Quest ([VR_FOVEATION.md](VR_FOVEATION.md)).
- **Headset resolution and dynamic resolution** ([VR_DYNAMIC_RESOLUTION.md](VR_DYNAMIC_RESOLUTION.md)).
- **Upscalers** ([VR_UPSCALING.md](VR_UPSCALING.md)): Quest Super Resolution, SGSR 1 and 2, FSR 1, and sharpening (CAS)
  with edge smoothing (FXAA).
- **Frame rate choices**: refresh-rate selection, Capped modes and frame synthesis
  ([VR_FRAME_RATE.md](VR_FRAME_RATE.md), [VR_FRAME_SYNTHESIS.md](VR_FRAME_SYNTHESIS.md)).
- **Streaming optimal** for wireless headsets (Virtual Desktop, Air Link), *experimental*
  ([VR_STREAMING.md](VR_STREAMING.md)).
- **Multithreaded rendering**: the scene and the sun's shadows drawn on threads of their own
  ([VR_SCENE_THREADS.md](VR_SCENE_THREADS.md)).

## From the base port

Everything ZeldaWWHDRecomp offers still works, flat-screen through **Play Flat**: 60 fps interpolation, save states,
mods, texture packs, the randomizer and more ([UPSTREAM_README.md](UPSTREAM_README.md)).

See also: [COMPATIBILITY.md](COMPATIBILITY.md) · [PLAYING.md](PLAYING.md) · [back to the README](../README.md)
