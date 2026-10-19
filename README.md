# FlatOut 2 VR

A VR mod for **FlatOut 2** (PC): the races in true stereo 3D with head tracking, at the headset's own
refresh rate, with the HUD on a panel in front of you.

- **Stereo 3D and head tracking** in every race camera: lean out of the window, look back at the wrecks
  behind you.
- **A cockpit**: the game's camera inside the car puts you in the driver's seat, with the driver's hands
  on the steering wheel, the dashboard, the roof and the pillars around you (the game itself shows no
  driver and no steering wheel there). Press C in a race until you are inside the car.
- **Smooth motion at 72 / 90 / 120 Hz**: the physics steps once per headset frame, so the world moves
  evenly at any refresh rate. The game's own steps are capped at 100 a second.
- **HUD on a floating panel**: position, laps, speedometer, map and the pause menu. Menus between
  races are shown on a large virtual screen.
- **Effects made right for VR**: the sun's glow and lens flare, particle streaks and sparks, the stunt
  cars' jet flames, the speed blur and speed zoom, the colour grading, and the stunt stadium's video
  screens. Each matches in both eyes. Fades to black cover the whole view.
- **Longer view distances** for the headset: full model detail and grass much further out.
  Everything is configurable.

## Requirements

- FlatOut 2 for PC, version 1.2: the GOG release or the Steam release (with ZOOM Platform's updates).
- Windows 10 or 11 and a graphics card with Vulkan support.
- A PC VR headset with one of these OpenXR runtimes:
  - **SteamVR**: any headset SteamVR runs, including Quest over Link or Air Link.
  - **Virtual Desktop's VDXR**: Quest over Virtual Desktop, no SteamVR needed.

  Meta's own OpenXR runtime (Quest Link without SteamVR) does not work with 32-bit games.

Tested on a Quest 3 (Virtual Desktop over USB at 120 Hz, and SteamVR over Link) with an NVIDIA GPU, with
the GOG and the Steam release.

## Installation

1. Download the latest release and extract it into the game folder, where `FlatOut2.exe` is. It
   contains:
   - `winmm.dll`: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), which loads the mod;
   - `d3d9.dll`: the 32-bit [DXVK](https://github.com/doitsujin/dxvk), which the mod needs to hand the
     game's picture to the headset;
   - `scripts\FO2VR.asi`, `scripts\FO2VR.ini`, `scripts\openxr_loader.dll`, `scripts\wineopenxr.dll`:
     the mod.
2. Choose the runtime in `scripts\FO2VR.ini`:
   - SteamVR (default): `Runtime=steamvr`. Start SteamVR before the game.
   - Virtual Desktop:
     `Runtime=C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json`.
     Connect the headset in Virtual Desktop before the game.
3. Start the game (on Steam: "Play FlatOut 2", not the Mod Manager). The races play in the headset; the
   menus appear on a virtual screen in front of you.

Use a 16:9 resolution in the game's settings (1920x1080 or higher): the HUD panel is drawn at the
game's resolution.

To uninstall, delete the files above.

## Controls

The game's controls are unchanged.

| Key | Action |
| --- | --- |
| F8  | Recenter the view (sit as you want to sit in the car and press it) |
| F9  | Hide the HUD while racing, or bring it back (it still shows for the countdown, the pause menu and the results) |
| Numpad 8 2 4 6, + - | Move the view against the car while held: up, down, left, right, forward, back (slowly at first), e.g. to sit a little lower in the cockpit. Numpad 5 puts it back. The view stays where you left it for the next game. Num Lock must be on |
| F11 | Screenshot of the monitor, both eyes and the HUD (into `scripts\shots\`) |

## Settings

`scripts\FO2VR.ini`, section `[VR]`:

| Setting | Default | |
| --- | --- | --- |
| `RenderScale` | 1.0 | Eye resolution relative to the headset's recommended size |
| `MSAA` | 4 | Antialiasing samples (0 = off) |
| `WorldScale` | 1.0 | Head movement scale |
| `Hud`, `HudDistance`, `HudWidth`, `HudHeight` | 1, 1.2, 1.3, 0.0 | The HUD panel: on/off (2 = not while racing, as with F9), distance, width, height (meters) |
| `Cockpit` | 1 | The camera inside the car is a cockpit (0: as the game shows it, from in front of the driver's head with no driver) |
| `CameraOffset` | 0 0 0 | The view moved against the car, meters (right, up, forward): in the cockpit from the driver's eyes; the numpad keys set it |
| `NearClip` | 0.05 | The cockpit's near clipping plane comes no nearer than this (meters); it is kept as far out as the roof and the sides allow |
| `ScreenDistance`, `ScreenWidth` | 2.0, 2.4 | The menus' virtual screen (meters) |
| `Mirror` | 1 | The monitor shows the left eye (0: the game's own view, costs a third render) |
| `PhysicsAtHeadsetRate` | 1 | One physics step per headset frame (0: the game's 10 ms steps) |
| `LodDistance` | 0 | Level-of-detail distances times this (0 = always full detail) |
| `GrassDistance` | 5.0 | How far grass is drawn, times this |
| `ViewDistance` | 3.0 | How far the world is drawn, times this |
| `SpeedBlur` | 1 | The game's speed blur (0 = off) |
| `SpeedZoom` | 1.0 | The game's widening of the view at speed: 1 = as on a monitor, 0 = off |

The log is `scripts\FO2VR.log`. If something does not work, it usually says why.

## Building

Visual Studio 2022 Build Tools (x86, C++), CMake and Ninja: run `build.cmd`. It builds `FO2VR.asi`
and `wineopenxr.dll` and copies them, with the OpenXR loader, into `..\scripts\`. The repository is
meant to be checked out inside the game folder.

`tools\package.ps1 -Version <n>` packs a release archive from the build, with the ASI loader and DXVK
taken from the game folder.

## License

MIT (see `LICENSE`). Third-party parts:

- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK-Source) headers and loader: Apache-2.0 / MIT
  (`third_party/openxr`).
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers): Apache-2.0 / MIT
  (`third_party/vulkan`).
- In the release only: [DXVK](https://github.com/doitsujin/dxvk) (zlib) and
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (MIT), with their licenses.

FlatOut 2 is a trademark of its owners. This mod is not affiliated with them and contains no game
files.
