#pragma once
// [Test] Background=1: the game runs beside other work. It gets a window on another monitor
// instead of the full screen, plays no sound, keeps running without the keyboard focus (it is
// never told that it lost it) and takes its keys from scripts\fo2vr_input.txt instead of the
// keyboard (written into the game's own key state, and into DirectInput):
//   down KEY / up KEY / tap KEY   hold, release, or press briefly (KEY: ENTER, ESC, SPACE, UP,
//                                 DOWN, LEFT, RIGHT, F1..F12, A..Z, 0..9 or a DIK code like 0x1C)
//   shot                          screenshot (like F11)
//   head X Y Z YAW [PITCH]        the test head offset (metres), turn (degrees to the right) and tilt
//                                 (degrees up)
//   camera X Y Z                  [VR] CameraOffset (metres), written to the ini as the camera keys do
//   trace                         log one frame of render calls
//   eject                         throw the player's driver out of the car, as a heavy crash does
//   mirror L|R loose|off          knock the player's car's left or right mirror loose or off, as hits do
//   log TEXT                      a line in FO2VR.log
// Lines appended to the file are carried out once a frame.
struct _D3DPRESENT_PARAMETERS_;
struct HWND__;

// At startup. In background mode: mutes the game and hooks its window and DirectInput; otherwise
// undoes a mute left from an earlier background run. False if the exe differs.
bool InstallBackground();
bool BackgroundMode();
// As Direct3D starts: DirectInput's own interface hooked when the game's call to it is another's (see background.cpp).
void HookInputInterfaces();

// Around IDirect3D9::CreateDevice: windowed instead of full screen, then the window goes to
// another monitor and the keyboard focus back to where it was.
void BackgroundBeforeDevice(_D3DPRESENT_PARAMETERS_* pp);
void BackgroundAfterDevice(HWND__* window);
// VR, once its Direct3D device is made: the game is not told when its window loses the focus either (it reads no
// controls without it: SteamVR's windows took it as the game started SteamVR, and the race went on without the
// player), its keyboard and mouse seen idle meanwhile. Its window in full screen, kept on top of all, goes under the
// window in front instead, so that the player may use the PC beside it.
void KeepGameActive(HWND__* window);

// Once a frame: carries out new commands. Returns kPollShot / kPollTrace / kPollHud (the Hud key) /
// kPollEject when asked for.
constexpr int kPollShot = 1, kPollTrace = 2, kPollHud = 4, kPollEject = 8;
int BackgroundPoll();

// The mod's own hotkeys: in background mode only while the game's window has the focus.
bool HotkeysAllowed();

// The "mirror" command's knock, once: the side ('l' or 'r') and the part's state to give it (1 loose, 2 off).
bool TakeMirrorKnock(char* side, int* state);

// A screenshot at the next Present (background mode), like the "shot" command.
void RequestShot();
// One frame of render calls logged, like the "trace" command.
void RequestTrace();
