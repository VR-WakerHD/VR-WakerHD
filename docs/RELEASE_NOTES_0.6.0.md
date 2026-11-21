# VR-WakerHD 0.6.0

VR mode by VR-WakerHD (https://github.com/VR-WakerHD/)

**The Legend of Zelda: The Wind Waker HD, in VR.** VR-WakerHD is an unofficial VR version of *The Wind Waker HD*
built on the [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) native port. You play in first
person at Link's eyes, with head tracking, motion controls and a physical sword, on PC VR (OpenXR) and standalone on
Meta Quest 3 / 3S.

> **No game is included.** You need your own, legally obtained copy of the Wii U game. Setup builds the game on your
> device from your own files; these downloads contain no game code, game data or keys.

0.6.0 brings much better sword play, arrow swapping on the bow made simple, and a new **True size** mode that lets you play at your own height, plus a long list of fixes.

## Downloads

| Platform | File |
|---|---|
| Windows PC (x86-64) | `VR-WakerHD-Windows-0.6.0-x86_64.zip` |
| Linux PC (x86-64, glibc 2.35+) | `VR-WakerHD-Linux-0.6.0-x86_64.AppImage` |
| Meta Quest 3 / 3S (standalone) | `VR-WakerHD-Quest3-0.6.0.apk` |
| Steam Frame (SteamOS, arm64) | `VR-WakerHD-SteamOS-aarch64-0.6.0.AppImage` |
| What to install on Linux, per distribution | `required_packages.txt` |

New here? [How to install](https://github.com/VR-WakerHD/VR-WakerHD/blob/main/docs/INSTALLATION.md) ·
[Compatibility](https://github.com/VR-WakerHD/VR-WakerHD/blob/main/docs/COMPATIBILITY.md) ·
[Quest guide](https://github.com/VR-WakerHD/VR-WakerHD/blob/main/docs/QUEST.md) ·
[Tutorial video](https://youtu.be/a98maOC3x24)

## Highlights

**Better sword play**
- Every swing is yours: in first person the blade cuts exactly where you swing it, locked on too, without the game
  taking over with its own attack animation. B attacks, parries and the spin attack stay on their buttons.
- Only real swings count: the blade has to move fast enough and far enough, so moving your arm around doesn't attack.
  Thrusts (pushing the sword tip first) work too.
- Hits follow the blade's whole arc, not just where it ends up, and the controller's vibration follows how hard you hit.
- Link yells only on hard swings. Tune it in Settings > VR > Sword and items: "Swing speed for Link's yell".

**Arrow swapping on the bow**
- Fire, ice and light arrows can now be chosen in first person. With no arrow on the bow, press the **right trigger** to
  pick the next arrow type; while aiming, **push the right stick down** to change the arrow on the bow.
- All arrow types aim and fly exactly like normal arrows.

**New: True size**
- Play at your own height instead of Link's. Your eyes are at your real height in his world, so you stand taller than
  him, and his hands move freely with your controllers. Your height comes from the headset when it tracks the floor,
  or from the new "Your height" setting.
- Turn it on in Settings > VR (or the launcher's Camera and comfort tab): "True size (your own height)". Recentre after
  turning it on.

## Fixes

- On the boat, drawing the bow no longer takes out the cannon or the grappling hook, and the view turns while you draw.
- With the Deku Leaf out, pushing the right stick down no longer brings out the shield.
- The Skull Hammer's free hand no longer flips around when you move and turn.
- The sun's glow and rays no longer turn with your head (the moon was fixed in 0.5.1).
- The Master Sword's sheath is hidden in first person.
- All the boat's views work in the Tower of the Gods, as on the Great Sea.
- Link's head comes back in cutscenes that hold him still.
- In cutscenes, moving your head moves the view a little (up to half a metre).
- A crash when a controller feature read an empty game pointer is fixed.

## Smoother frames

- Frames are now paced by your headset's runtime (OpenXR), which keeps them on time more evenly.
- SteamVR on Linux: several fixes for its stutter. The runtime has a Vulkan queue of its own, the GPU starts on each
  frame earlier, and the game recovers from SteamVR's slow state without dropping to a lower frame rate.

## Known issues

- Streaming headsets (Virtual Desktop, Air Link): turn on Streaming optimal, try 90 Hz with Capped 45 first, and use a
  cable if you can.
- True size is new: in low doorways and ceilings the view can reach through the ceiling, so duck if you're tall.
- On Quest, frame synthesis can look smeary or jittery when the headset can't keep up. Set it to Off, or use 72 Hz.

## Special thanks

Special thanks to [legaiaflame](https://www.youtube.com/@legaiaflame) for all the bug-fix testing and the videos that
showed exactly what was wrong.

## License and credit

The VR mode is **GPL-3.0-or-later with a credit term**: if you share it, changed or not, keep the credit **"VR mode by
VR-WakerHD (https://github.com/VR-WakerHD/)"** in the source and where users see it. The original port's code stays
**MPL-2.0**. Built on [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp). The game itself belongs to
Nintendo; this is an unofficial fan project.
