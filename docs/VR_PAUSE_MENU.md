# VR pause menu (the GamePad panel)

WWHD draws its pause/collection, item, map/sea chart and bag menus, and the Wind Waker's song list, on the Wii U
GamePad's screen. In VR that screen is shown as a panel in the headset (an OpenXR quad layer, or a cylinder layer
when curved), drawn from the GamePad picture the game already renders
(`runtime/src/gfx/vulkan/vr_vulkan.cpp`).

## When it shows

`vr.gamepadScreen`: `menus` (default) while one of those menus is open — the port reads the same game state it uses
for its automatic picture-in-picture (`gfx/gamepad_ui_state.cpp`); `always`; `off`. Press the menu button (Plus)
to open the pause menu; the game's own buttons and sticks move through it, as on the GamePad.

## Where it shows

| `vr.menuPlacement` | Behaviour |
|---|---|
| `gaze` (default) | placed straight ahead of where you look horizontally when it opens, then fixed in the room (you can look around it) |
| `head` | follows the head (stays in front of you) |
| `front` | ahead of the tracking space's forward direction, at your head's position when it opens |

Distance (`vr.menuDistance`, 1.125 m), height relative to eye level (`vr.menuHeight`, -0.08 m), size
(`vr.menuScale` x 0.8 m) and curvature (`vr.menuCurvature`) are their own settings, in the launcher's Camera tab
and the game's settings overlay. They apply the next time the panel opens. The gameplay camera is never moved for
the menu: the world keeps rendering behind the panel.

The default distance is the first version's: 0.75 m base x 1.5 (the requested 50 % farther), as its own setting.

## Curved panels

With `vr.menuCurvature` above 0 and a runtime offering `XR_KHR_composition_layer_cylinder` (the log says
`curved menus ... available`), the panel is a cylinder segment of radius `menuDistance / curvature` passing through
the panel's centre; 1 wraps it around the head at the menu distance. Without the extension it stays flat.

## Limits

- The panel is not touchable with a controller yet (menus are navigated with sticks and buttons).
- The TV's own menus (title, file select) are drawn in the world view, smaller and farther (`vr.menuZoomOut`).
