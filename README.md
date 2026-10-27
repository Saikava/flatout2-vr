# FlatOut 2 VR

**Play FlatOut 2 in VR.** A free mod for the PC game: the races in true stereo 3D with head tracking,
smooth at your headset's refresh rate, with the HUD on a panel in front of you. You drive with the game's
own controls (gamepad, wheel or keyboard) or with your VR controllers. Works with the GOG and the Steam
release.

[![Download](https://img.shields.io/github/v/release/Saikava/flatout2-vr?label=Download&style=for-the-badge&color=f0a30a)](https://github.com/Saikava/flatout2-vr/releases/latest) [![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=for-the-badge)](LICENSE)

## Quick start

1. Download `FlatOut2-VR-<version>.zip` from the [latest release](https://github.com/Saikava/flatout2-vr/releases/latest).
2. Extract it into the game folder, where `FlatOut2.exe` is.
3. Start SteamVR, then the game. On Steam, choose "Play FlatOut 2", not the Mod Manager.

Sit in the car as you want to sit and press **F8** to recenter the view. Playing through Virtual Desktop
without SteamVR? Change one line in the settings: see [Runtimes](#runtimes).

## Features

- **Stereo 3D and head tracking** in every race camera: lean out of the window, look back at the wrecks
  behind you. Each eye gets its own render of the scene, not a shifted copy.
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
- **VR controllers and gamepads**: drive with the VR controllers (thumbstick and triggers), and work the
  menus with them or with an Xbox controller (the game's menus take only the keyboard otherwise). The game's
  force feedback vibrates the VR controllers and Xbox controllers (it has no vibration for those
  otherwise). Game controllers and the game's sound work whether or not the game's window on the monitor has
  the focus.

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

`Runtime=` holds also when the game runs as administrator (for example started by a Steam that runs as
administrator): OpenXR's loader would then ignore it and start the system's active runtime, so the mod loads
the runtime itself.

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

### VR controllers

They drive as they are, beside the keyboard, with the keyboard chosen in the game's Options > Controller
options > Controller (as it is at first). Choose **VR controllers** there to change their controls in
Configure controls (the keyboard still drives beside them), and turn Force feedback On there for their
vibration in crashes. Until you change them:

| VR controller | In a race |
| --- | --- |
| Left thumbstick | Steer |
| Right trigger / left trigger | Accelerate / brake and reverse |
| A / B | Nitro / handbrake |
| X / Y | Change the camera / reset the car |
| Menu button | Pause |
| Right / left grip | Gear up / down (manual transmission) |
| Right thumbstick pressed | Look back |
| Both thumbsticks pressed | Recenter the view (as F8) |

In the menus (whichever controller is chosen) the thumbsticks move, A selects, B and the menu button go
back. Index and Windows Mixed Reality controllers work with what they have of these.

To drive with the keyboard, put the VR controllers down. A Quest then tracks your hands, and Virtual Desktop
makes controllers of them (a pinch pulls the trigger), which would take the throttle and the brake from the
keys. So the triggers and grips count from a thumbstick moved or a button pressed until the keyboard steers,
accelerates or brakes; with Virtual Desktop's own runtime the hands are left out altogether.

The menu button is on the left controller, under the thumb that steers. To pause with the right hand, set
`PauseButton=RightStick` (the right thumbstick pressed) or `RightGrip` in `scripts\FO2VR.ini`: that button
then pauses and continues the race, and does nothing else (with `RightStick`, move Look back to the left
thumbstick pressed in the game's Configure controls). The game's Configure controls has no Pause of its own.
A race also pauses when SteamVR's dashboard, the Quest's menu or Virtual Desktop's menu comes over the game
(through SteamVR the menu button may open the dashboard instead: it then pauses all the same).

Experimental, off by default (`GrabWheel=1`): steer by holding the cockpit's steering wheel. Reach for it
as you sit (recentered), squeeze a grip at its rim and turn it with your hands; 90 degrees is full lock, as
the cockpit's wheel turns (`WheelTurn`).

### Gamepads

The game's menus take only the keyboard; with the mod an Xbox controller works them too: the left
thumbstick or the D-pad moves, A or Start selects, B or Back goes back, and Start pauses a race. With Force
feedback On in the game's options, an Xbox controller vibrates in crashes (and with `EngineRumble` with the
engine's revs). In VR the game goes on, with its controllers and its sound, when its window on the monitor
loses the focus: switch to another program and the game's window goes under it. Other programs may not take
the focus from the game by themselves (SteamVR's windows did as it started); Alt+Tab or a click switch as
usual.

### Steering wheels

The cockpit's steering wheel turns as the game steers the car: 90 degrees at full lock, which your wheel
reaches only at its own (450 degrees on a 900-degree wheel), and slower and less at speed. Set `WheelRange=`
in `scripts\FO2VR.ini` to your wheel's range from lock to lock, in degrees, as set in its software (900 for
most), and the cockpit's wheel turns as yours, as far and as fast, up to its lock at 90 degrees either way,
where the driver's hands stop (the game has no animation past that).

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
| `Mirror` | 1 | The monitor shows the left eye (0: the game's own view, costs a third render); 2 for recording a video of what the headset shows: a steady view cut from the left eye, following your head smoothly with its horizon level with the car's, the HUD where it hangs in the headset (record the game's window, e.g. with OBS's game capture) |
| `MonitorFov`, `MonitorPitch`, `MonitorSmoothing` | 85, 10, 0.3 | `Mirror=2`: the view's width (degrees), how far it is aimed below where you look (degrees: the dashboard and your hands on the wheel in view) and how long it takes to follow your head (seconds; 0: with it). It is cut from the left eye's picture, which sees only some 40 degrees right of straight ahead: the eyes are rendered wider than the headset needs for it (the headset gets what it always does), some 20% more pixels at 85 degrees and 10 down, 30% at 90 |
| `PhysicsAtHeadsetRate` | 1 | One physics step per headset frame (0: the game's 10 ms steps) |
| `LodDistance` | 0 | Level-of-detail distances times this (0 = always full detail) |
| `GrassDistance` | 5.0 | How far grass is drawn, times this |
| `ViewDistance` | 3.0 | How far the world is drawn, times this |
| `FloatDepth` | 1 | The eyes keep depth as 32-bit floats, reversed (near 1, far 0), set in DXVK by the mod: what lies close together far away, such as a turn sign's arrow on its board, stays apart instead of showing through in crawling bands (0: 24-bit depth as the game keeps it) |
| `SpeedBlur` | 1 | The game's speed blur, this many times as strong (0.5 half as strong, 0 off, up to 2) |
| `SpeedZoom` | 1.0 | The game's widening of the view at speed: 1 = as on a monitor, 0 = off |
| `LargeAddressAware` | 1 | FlatOut2.exe may use 4 GB of memory, not 2 (see [Memory](#troubleshooting)): when the game starts, the mod sets the flag in the file (the original kept as `FlatOut2.exe.2GB.bak`) and starts the game anew with it (0: leave FlatOut2.exe as it is) |
| `MotionControllers` | 1 | The VR controllers as a game controller, "VR controllers" (see [VR controllers](#vr-controllers); 0: none) |
| `PauseButton` | (none) | A button of the VR controllers that pauses and continues a race besides the menu button: `RightStick`, `RightGrip`, `LeftStick`, `LeftGrip`, `X` or `Y` (the game then no longer sees it) |
| `Rumble` | 1.0 | How strongly the game's force feedback vibrates the VR controllers and Xbox controllers (up to 2; 0: not at all) |
| `EngineRumble` | 0 | The engine as a light, steady vibration while driving (VR controllers, an Xbox controller's light motor), stronger with the revs and the throttle, at most this strong (0.2-0.5: a hum under the crashes'; 0: none) |
| `GrabWheel` | 0 | Experimental: the VR controllers' grips take hold of the cockpit's steering wheel, which your hands then turn (instead of shifting gears) |
| `WheelTurn` | 90 | How far your hands turn the wheel for full lock, degrees (at 90 the cockpit's wheel turns with them; more: slower steering) |
| `WheelRange` | 0 | A steering wheel as the game controller: its range from lock to lock, degrees (as set in its software; 900 for most). The cockpit's steering wheel then turns as yours, up to its lock at 90 degrees either way (see [Steering wheels](#steering-wheels); 0: as the game steers the car, slower and less at speed) |

## Troubleshooting

The log, `scripts\FO2VR.log`, usually says what went wrong.

- **A window says "Waiting for SteamVR..." or "VR did not start".** The game waits for the VR runtime and
  goes on in VR by itself once it is ready; the window says what went wrong when it fails, with Try again,
  Play without VR and Quit. Start SteamVR (or connect the headset in Virtual Desktop) before the game, and
  check `Runtime=` in `scripts\FO2VR.ini`. If SteamVR hangs while it starts (its window never shows the
  headset), close it, end `vrserver.exe` in the Task Manager if it stays, and start SteamVR from Steam: when
  Virtual Desktop's Streamer runs as administrator, the SteamVR it starts may not come up. When another
  program's OpenXR API layer does not load into the game (ReShade's does not, set up for OpenXR in another game), the
  mod starts VR again without such layers; the log names them.
- **Memory: a track does not load** (the game stands still at the loading screen; `FlatOut2_d3d9.log` ends with
  "Memory allocation failed"). FlatOut2.exe is a 32-bit program and, as released, may use only 2 GB of memory,
  which it shares with the VR runtime and the graphics driver; larger tracks can run out of it in VR. The mod
  lets it use 4 GB by setting a flag in FlatOut2.exe (`LargeAddressAware=1`) and says so when it cannot, e.g. when
  the game's folder is not writable. To set the flag yourself, close the game and run this in PowerShell (with
  the path of your FlatOut2.exe; it keeps a copy as `FlatOut2.exe.bak`):
  ```
  $f = "C:\Program Files (x86)\Steam\steamapps\common\FlatOut 2\FlatOut2.exe"
  $b = [IO.File]::ReadAllBytes($f); $o = [BitConverter]::ToInt32($b, 0x3C) + 22
  if ($b[$o] -band 0x20) { "already 4 GB" } else { Copy-Item $f "$f.bak"; $b[$o] = $b[$o] -bor 0x20; [IO.File]::WriteAllBytes($f, $b); "done" }
  ```
- **The game closes with "The graphics driver was reset".** The graphics card stopped with an error and Windows
  reset its driver (the system's event log has it, as `nvlddmkm` on an NVIDIA card); the game cannot draw after
  that, so it closes rather than standing still in the headset. Start it again. If it keeps happening, open an
  issue with `scripts\FO2VR.log`.
- **The log says "the game does not run on DXVK".** `d3d9.dll` from the archive is missing from the game
  folder, or another mod replaced it.
- **The log says "unsupported FlatOut2.exe".** The mod needs version 1.2 of the game (the GOG or the
  Steam release).
- **The HUD looks stretched.** Choose a 16:9 resolution in the game's settings.
- **The VR controllers or the gamepad do nothing in a race.** Choose them in the game's Options > Controller
  options > Controller: the game drives with the one chosen there (the keyboard works besides).
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
