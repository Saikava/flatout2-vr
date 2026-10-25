#pragma once
struct IDirect3DDevice9;

// Hooks RenderViews and the engine's SetViewport / SetCamera / SetFrustum. False if the exe
// does not match FlatOut 2 v1.2.
bool InstallRenderHooks();

// The game created its Direct3D device: hook it and start VR.
void OnDeviceCreated(IDirect3DDevice9* dev);

// The player at this PC in a race (nullptr outside one).
unsigned char* LocalPlayer();

// The player's part is over while the race's world runs on: the game has switched the player's input off
// (kPlayerInputOff), as when a stunt's attempt has landed, whose round results then come over the running world with
// their menu (Continue stunt, ...).
bool PlayerRunOver();

// The player's car's steering wheel: its hub from where the recentred head sits in the cockpit (the driver's eyes
// moved by [VR] CameraOffset) and its node's axes (rows), in the car's axes (x right, y up, z forward; metres).
// False without a car.
bool SteeringWheel(float hub[3], float axes[9]);
