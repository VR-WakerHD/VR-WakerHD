# Wind Waker HD VR on Meta Quest 3 and 3S (standalone)

**VR-WakerHD** runs *The Legend of Zelda: The Wind Waker HD* in VR standalone on Meta Quest 3 and Quest 3S, with no PC.
This page covers installing it on the Quest, the recommended settings, and what to do if the frame rate is low.
Quest can also play the PC version over Link or Virtual Desktop; see [COMPATIBILITY.md](COMPATIBILITY.md).

## Install

1. Put the headset in **developer mode**. That takes a Meta developer account, then the setting in the Meta Horizon
   app.
2. Install `VR-WakerHD-Quest3-<version>.apk` from the
   [latest release](https://github.com/VR-WakerHD/VR-WakerHD/releases/latest) with SideQuest or `adb install`.
3. Open it from *Unknown sources* and choose your game files. The app offers to extract a disc image. Setup compiles
   the game on the headset, which takes a while the first time.

Details: [INSTALLATION.md](INSTALLATION.md).

## Recommended settings

The Quest's processor is the limit, so the defaults are tuned for it:

| Setting | Default on Quest | If it is not smooth |
|---|---|---|
| Refresh rate and frame rate | 90 Hz, Capped 30 fps | 72 Hz with 36 fps and frame synthesis on |
| Headset resolution | 0.70 | Lower it (for example 0.65) |
| Field of view | Full | Lower it: this helps the frame rate most |
| Upscaling | Sharpening (CAS) with edge smoothing (FXAA) | — |
| Foveated rendering | On | — |
| Frame synthesis | Auto | Turn it **Off** if the picture looks smeary or jittery |
| Shader compile | Off | **Fast** can reduce stutter on first visits; objects may be missing for a frame or two |

Change them in the VR launcher, or in the headset: click the **left stick**, then use the grips to switch tabs.

## Low frame rate or stutter

- **The first visit to an area stutters** while its shaders are made. It eases as you play. *Shader compile: Fast*
  (Graphics tab) trades the stutter for objects briefly missing.
- **Low frame rate throughout:** lower the field of view and the headset resolution, and try 72 Hz with 36 fps.
- **Hands or held items look out of place** at low frame rates. Steady hands draws the hands at the headset's rate
  while the world updates more slowly. Raising the frame rate is the fix.

## Bug reports

Attach your log: `/sdcard/Android/data/org.wwhdrecomp.wwhd/files/captures/wwhd.log`. From a Windows PC over USB it is
at `This PC\Quest 3\Internal shared storage\Android\data\org.wwhdrecomp.wwhd\files\captures\wwhd.log`. Use the
[bug report form](https://github.com/VR-WakerHD/VR-WakerHD/issues/new/choose).

Technical details of the Quest build: [VR_ANDROID.md](VR_ANDROID.md), [VR_FOVEATION.md](VR_FOVEATION.md),
[VR_MULTIVIEW.md](VR_MULTIVIEW.md).

See also: [COMPATIBILITY.md](COMPATIBILITY.md) · [FAQ.md](FAQ.md) · [back to the README](../README.md)
