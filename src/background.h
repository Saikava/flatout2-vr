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
//   log TEXT                      a line in FO2VR.log
// Lines appended to the file are carried out once a frame.
struct _D3DPRESENT_PARAMETERS_;
struct HWND__;

// At startup. In background mode: mutes the game and hooks its window and DirectInput; otherwise
// undoes a mute left from an earlier background run. False if the exe differs.
bool InstallBackground();
bool BackgroundMode();

// Around IDirect3D9::CreateDevice: windowed instead of full screen, then the window goes to
// another monitor and the keyboard focus back to where it was.
void BackgroundBeforeDevice(_D3DPRESENT_PARAMETERS_* pp);
void BackgroundAfterDevice(HWND__* window);

// Once a frame: carries out new commands. Returns kPollShot / kPollTrace / kPollHud (the Hud key)
// when asked for.
constexpr int kPollShot = 1, kPollTrace = 2, kPollHud = 4;
int BackgroundPoll();

// The mod's own hotkeys: in background mode only while the game's window has the focus.
bool HotkeysAllowed();

// A screenshot at the next Present (background mode), like the "shot" command.
void RequestShot();
// One frame of render calls logged, like the "trace" command.
void RequestTrace();
