<p align="center">
  <img src="docs/img/hero.jpg" alt="The start of a race in FlatOut 2, seen in the headset" width="100%">
</p>

# FlatOut 2 VR

**Play FlatOut 2 in VR.** A free mod for the PC game: the races in true stereo 3D with head tracking,
smooth at your headset's refresh rate, with the HUD on a panel in front of you. You drive with the game's
own controls (gamepad, wheel or keyboard); no motion controllers needed. Works with the GOG and the Steam
release.

[![Download](https://img.shields.io/github/v/release/Saikava/flatout2-vr?label=Download&style=for-the-badge&color=f0a30a)](https://github.com/Saikava/flatout2-vr/releases/latest) [![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=for-the-badge)](LICENSE)

## Quick start

1. Download `FlatOut2-VR-<version>.zip` from the [latest release](https://github.com/Saikava/flatout2-vr/releases/latest).
2. Extract it into the game folder, where `FlatOut2.exe` is.
3. Start SteamVR, then the game. On Steam, choose "Play FlatOut 2", not the Mod Manager.

Sit in the car as you want to sit and press **F8** to recenter the view. Playing through Virtual Desktop
without SteamVR? Change one line in the settings: see [Runtimes](#runtimes).

<p align="center">
  <img src="docs/img/forest.jpg" alt="Racing through a forest" width="32%">
  <img src="docs/img/stunt.jpg" alt="A stunt car in mid-air in the stunt arena" width="32%">
  <img src="docs/img/stunt-players.jpg" alt="A stunt event with giant football players" width="32%">
</p>

## Features

- **Stereo 3D and head tracking** in every race camera: lean out of the window, look back at the wrecks
  behind you. Each eye gets its own render of the scene, not a shifted copy:

  <img src="docs/img/stereo.jpg" alt="The left and the right eye's views of the same moment" width="100%">

- **A cockpit**: the game's camera inside the car puts you in the driver's seat, with the driver's hands on
  the steering wheel, the dashboard, the roof, the pillars and the glass around you (the game itself shows
  none of it there). The side mirrors, convex as real ones are, show what is behind. The gauges, position,
  lap, map and the rival's name stand on the dashboard on one curved band, laid out the same in every car.
  Thrown out through the windscreen, you fly on in the driver's head. The first race with the mod starts in
  the cockpit; C switches the camera, and the game keeps your choice.
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
- A PC VR headset with SteamVR or Virtual Desktop (see [Runtimes](#runtimes)).

Tested on a Quest 3 (Virtual Desktop over USB at 120 Hz, and SteamVR over Link) with an NVIDIA GPU, with
the GOG and the Steam release.

## Runtimes

The mod talks to the headset through OpenXR. Choose the runtime in `scripts\FO2VR.ini`:

- **SteamVR** (default): `Runtime=steamvr`. Any headset SteamVR runs, including Quest over Link or Air
  Link. Start SteamVR before the game.
- **Virtual Desktop** (VDXR, no SteamVR needed):
  `Runtime=C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json`.
  Connect the headset in Virtual Desktop before the game.

Meta's own OpenXR runtime (Quest Link without SteamVR) does not work with 32-bit games.

## What's in the archive

- `winmm.dll`: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), which loads the mod;
- `d3d9.dll`: the 32-bit [DXVK](https://github.com/doitsujin/dxvk), which the mod needs to hand the
  game's picture to the headset;
- `scripts\FO2VR.asi`, `scripts\FO2VR.ini`, `scripts\openxr_loader.dll`, `scripts\wineopenxr.dll`:
  the mod.

To uninstall, delete these files.

Use a 16:9 resolution in the game's settings (1920x1080 or higher): the HUD panel is drawn at the
game's resolution.

## Controls

The game's controls are unchanged.

| Key | Action |
| --- | --- |
| F8  | Recenter the view (sit as you want to sit in the car and press it) |
| F9  | Hide the HUD while racing, or bring it back (it still shows for the countdown, the pause menu and the results); kept for the next game |
| Numpad 8 2 4 6, + - | Move the view against the car while held: up, down, left, right, forward, back (slowly at first), e.g. to sit a little lower in the cockpit. Numpad 5 puts it back. The view stays where you left it for the next game. Num Lock must be on |
| F11 | Screenshot of the monitor, both eyes and the HUD (into `scripts\shots\`) |

## Settings

`scripts\FO2VR.ini`, section `[VR]`:

| Setting | Default | |
| --- | --- | --- |
| `RenderScale` | 1.0 | Eye resolution relative to the headset's recommended size |
| `MSAA` | 4 | Antialiasing samples (0 = off) |
| `WorldScale` | 1.0 | Head movement scale |
| `Hud`, `HudDistance`, `HudWidth`, `HudHeight` | 1, 1.2, 1.3, 0.0 | The HUD panel: on/off (2 = not while racing; F9 switches it and writes it here), distance, width, height (meters) |
| `Cockpit` | 1 | The camera inside the car is a cockpit (0: as the game shows it, from in front of the driver's head with no driver) |
| `StartInCockpit` | 1 | The next start puts the game's race camera inside the car, once; the mod then sets it to 0 (the camera you choose with C the game keeps) |
| `CockpitHud` | 1 | In the cockpit the gauges (with the nitro's flames), position, lap, map and the rival's name (with the crash bonus, "SLAM" and the like) stand on the dashboard as cards on one band curved round the driver's eyes, laid out the same in every car: the gauges in the steering wheel's opening, the position left of the wheel, the lap and the rival's name right of it, the map below; the HUD panel keeps the messages, the countdown and the pause menu (0: all on the panel) |
| `CameraOffset` | 0 0 0 | The view moved against the car, meters (right, up, forward): in the cockpit from the driver's eyes; the numpad keys set it |
| `Mirrors` | 2 | In the cockpit the side mirrors show what is behind, as real mirrors do, set for the driver's eyes with the car's side just in sight, on their glass only (not on a frame painted round it), under the car's windows, bent with the mirror and until it is knocked off: 2 one picture for each eye, so what the mirror shows stands at its depth, as in a real mirror (each mirror in view costs two more small renders of the scene, some 0.6-1 ms); 1 one picture for both eyes, flat on the glass (half the cost); 0 the game's dark glass |
| `MirrorRadius` | 1.4 | The side mirrors' glass is convex, of this radius in metres, as real side mirrors are: they show more of what is behind, smaller (objects in them are closer than they appear), the near one some two and a half times as wide a view as flat glass, the far one five or so; larger is flatter, 0 flat glass |
| `NearClip` | 0.05 | The cockpit's near clipping plane comes no nearer than this (meters); it is kept as far out as the roof and the sides allow |
| `CockpitGlass` | 0.3 | How much the car's windows show in the cockpit, 0 to 1: their reflections, the sun's glints on them and their tint (0: no glass, 1: as strong as the game's own glass, which from inside darkens the view through the windscreen by half and turns the side windows into a bright haze). The sun and the dust beyond show through; broken glass looks as in the game |
| `EjectView` | 1 | When the driver is thrown out of the car (a heavy crash, the stunts' launch) the view stays in his eyes as he flies, from the cockpit: it follows his head without tumbling with it (level, turning with your head only) until the car is reset (2: also from the cameras outside the car, as in the stunts, whose cars have no camera inside; 0: the game's cut to its camera behind him) |
| `ScreenDistance`, `ScreenWidth` | 2.0, 2.4 | The menus' virtual screen (meters) |
| `Mirror` | 1 | The monitor shows the left eye (0: the game's own view, costs a third render) |
| `PhysicsAtHeadsetRate` | 1 | One physics step per headset frame (0: the game's 10 ms steps) |
| `LodDistance` | 0 | Level-of-detail distances times this (0 = always full detail) |
| `GrassDistance` | 5.0 | How far grass is drawn, times this |
| `ViewDistance` | 3.0 | How far the world is drawn, times this |
| `SpeedBlur` | 1 | The game's speed blur (0 = off) |
| `SpeedZoom` | 1.0 | The game's widening of the view at speed: 1 = as on a monitor, 0 = off |

## Troubleshooting

The log, `scripts\FO2VR.log`, usually says what went wrong.

- **The game starts on the monitor only.** The mod found no headset: start SteamVR (or connect the
  headset in Virtual Desktop) before the game, and check `Runtime=` in `scripts\FO2VR.ini`.
- **The log says "the game does not run on DXVK".** `d3d9.dll` from the archive is missing from the game
  folder, or another mod replaced it.
- **The log says "unsupported FlatOut2.exe".** The mod needs version 1.2 of the game (the GOG or the
  Steam release).
- **The HUD looks stretched.** Choose a 16:9 resolution in the game's settings.
- **It stutters in the headset.** Each frame has to be ready in the headset's time (8.3 ms at 120 Hz); the log's
  `frame avg/max ms` lines show how long the eyes take. In `scripts\FO2VR.ini`, `[VR]`, these cost the most:
  `Mirrors` (1 halves the mirrors' cost, 0 drops it), `MSAA` (2 or 0), `RenderScale` (0.8 or 0.9),
  `GrassDistance` and `ViewDistance` (lower), and `Mirror=0` (the monitor's own view, a third render: keep it 1).
  A lower refresh rate in the headset (90 or 72 Hz) gives each frame more time.

Something else? [Open an issue](https://github.com/Saikava/flatout2-vr/issues) and attach
`scripts\FO2VR.log`.

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
