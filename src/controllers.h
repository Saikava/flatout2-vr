#pragma once
// The game's game controllers (DirectInput): the VR controllers as a gamepad of the game's ("VR controllers"), the
// game controllers read in VR whether or not the game's window has the focus, and the game's force feedback as
// vibration: on Xbox controllers (XInput; DirectInput gives them no force feedback) and on the VR controllers.
#include <windows.h>

// At the start: the game's list of game controllers goes through the mod (see controllers.cpp).
bool InstallControllers();
// VR: the game's window, once its Direct3D device is made, is put in front if it is not (the game may have been
// started from the headset, with another window in front: the keyboard then went elsewhere).
void FocusGameWindow(HWND window);
// The window in front (the game's) is kept there: other programs may not take the focus by themselves until the
// player's next Alt or click elsewhere (LockSetForegroundWindow).
void LockGameFocus();
// Per frame: true once when both thumbsticks of the VR controllers are pressed together (recenter the view).
bool ControllersRecenter();
// Per frame: in the game's menus (which take only the keyboard) the VR controllers and Xbox controllers press its keys.
void ControllersMenuKeys();
// The player's own steering wheel's turn, for the cockpit's (render.cpp): -1 to 1 for the cockpit wheel's 90 degrees
// either way. The hands' turn while they hold the cockpit's wheel ([VR] GrabWheel), or a game controller's steering axis
// as its wheel turns ([VR] WheelRange). False when neither (the car's steering turns it, as the game has it).
bool ControllersWheelTurn(float& turn);
