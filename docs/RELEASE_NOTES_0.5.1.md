# VR-WakerHD 0.5.1

VR mode by VR-WakerHD (https://github.com/VR-WakerHD/)

0.5.1 is a stability and smoothness update. The big themes are Quest 3 / 3S performance, crash fixes, smoother motion in the headset, and a long list of hand, weapon and item fixes.

As always, releases contain no game code: setup builds the game on your own machine from your own copy.

## Highlights

**Quest 3 / 3S: faster and sharper**
- Foveated rendering by shading-rate maps: the edges of the view are drawn at lower detail.
- The GPU now runs at a higher clock level.
- Field of view slider,  lowering FOV helps increase FPS and overall smoothness, so try it first.
- New Quest defaults: 90 Hz with Capped 30 FPS, headset resolution 0.70, slightly shorter draw distance, lens flare off. If it doesn't feel smooth enough try 72 Hz with 36 fps and frame syn on. 
- Sharpening (CAS) with built-in edge smoothing (FXAA) is the Quest's default upscaler, so there are fewer jagged edges.
- Menus, dialogue and the HUD are drawn at the headset's full sharpness, even at 0.70 resolution.
- Frame synthesis auto by default, but turn off if you are experience jitter. 

**Crash fixes**
- Quest: safeguards against the recurring driver crash during drawing, and crash logs now record the last draw.
- PC: fixed a crash when the GamePad/menu panel was recreated.
- Fixed a crash when dropping arrows.

**Smoother motion**
- Steady hands while turning (on by default): Link's hands, sword and shield stay steady while the world turns.
- Smooth Camera while Turning, including over streamed headsets such as Virtual Desktop.
- New, experimental: **Head movement smoothing** (off by default). At 45 or 36 fps, near objects no longer step back and forth as you look around. It works on any headset, wired or streamed.
- Fewer mid-game hitches.

**Headsets and controllers**
- Choosing 90 or 120 Hz now really switches the headset to that refresh rate; the game asks again until it does.
- Quest 3 and Quest 3S have their own controller profiles and calibration.
- Hands are measured from a clean idle pose, fixing hands rotated wrong on some headsets.
- The launcher sets the PS VR2 Sense controller binding in SteamVR. On the Index, the menu is on the trackpad and System button.
- Steam Frame - Native gets the Quest's tuned settings: foveation, edge smoothing, 8x texture filtering.

**Controls and comfort**
- New **Move where you look** option
- Shield: free rotation and front-facing blocks. Fixed the sword grip flipping.
- Bow:
  - better follows your hand;
  - the arrow sits square on the bow;
  - the bow is raised again after a hit or roll.
- Ropes:
  - the swing direction follows Link in first person;
  - the left grip holds the rope;
  - the rope you just let go of no longer catches you again.
- The Wind Waker: conducting the beat is easier
- After a menu, a conversation or a cutscene, the boat returns to the view you were sailing in. B returns to Link from command mode.

**Visual fixes**
- The telescope:
  - its eyepiece is sized like the Picto Box viewfinder;
  - draw distance matches the game's setting;
  - fixed black fill.
- Fade and picture transitions no longer show a double image.
- The night sky has stars all around you, and the sun and moon no longer turn with your head.
- Fixed black and rainbow-coloured hands on the boat, and a black figurehead at the Tower of the Gods.

**Setup and bug reports**
- Releases are installers: setup builds the game on your machine, then opens the VR launcher.
- On Quest, the app asks to extract the game disc when game files are missing.
- New bug report form on GitHub. Please attach your `wwhd.log`; the form shows where to find it on each platform.

## Known issues
- Streaming Headset API (VDXR, Meta Link) make sure to turn on Streaming optimal and depth for VD in Graphics settings, try 90 Hz 45 fps capped mode first and for best results use a wired connection, WiFi is working better now based on some testers but is still considered experimental. 
- Head movement smoothing is new and hasn't had much headset testing yet. Edges behind near objects, and water or smoke, may shimmer slightly.
- On Quest, frame synthesis can look smeary or jittery when the headset can't keep up. If it does, set VR frame synthesis to Off, or use 72 Hz.

## Special thanks
Special thanks to [legaiaflame](https://www.youtube.com/@legaiaflame) for all the help testing and finding bugs.
