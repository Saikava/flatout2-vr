#pragma once
// OpenXR output. The game's Direct3D 9 runs on DXVK (d3d9.dll from DXVK in the game folder); the
// OpenXR session uses DXVK's own Vulkan device and queue, and eye images rendered by the game into
// D3D9 textures are copied into OpenXR swapchains on that queue (DXVK interop).
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace xr {

// Eye pose relative to the recenter origin, in game axes (x right, y up, z forward, meters).
struct EyePose
{
    float right[3], up[3], forward[3], position[3];
};

// Tangents of the eye's field of view: left and down are negative.
struct EyeFov
{
    float left, right, up, down;
};

// Before the game creates Direct3D: OpenXR instance and headset. False when there is no headset;
// the game then runs without VR.
bool InitInstance();
bool Available();  // InitInstance succeeded
// The game's device was created: start the session (needs DXVK).
void OnDevice(IDirect3DDevice9* dev);
void PollEvents();
bool Running();
// The session has the input focus: not while the runtime's own menu is over the game (SteamVR's dashboard, the Quest's
// or Virtual Desktop's menu).
bool Focused();
// [Test] without a headset: the focus taken away and given back by hand (the background command "focus").
void SetTestFocus(bool focused);

// Eye render size (recommended size times RenderScale).
void EyeSize(unsigned* w, unsigned* h);
// Eye textures larger than that ([VR] Mirror=2 renders them wider): where the headset's picture begins in each, in pixels.
void SetEyeOffsets(unsigned leftX, unsigned leftY, unsigned rightX, unsigned rightY);

// Per frame: WaitFrame waits for the headset's next frame and opens it (nothing to do when one is
// open). The game loop calls it before the game advances, so the game follows the headset's
// timing. BeginFrame opens a frame if none is open and locates the eyes; it returns true when eye
// images should be rendered. An open frame must be finished with SubmitEyes, SubmitScreen or
// EndFrameEmpty.
bool WaitFrame();
bool BeginFrame();
bool FrameOpen();
bool ShouldRender();  // the open frame will be shown (the session is visible)
// The open frame: when it will be shown and the headset's frame period, in seconds.
double DisplayTime();
double DisplayPeriod();
EyePose Pose(int eye);
EyeFov Fov(int eye);
// The eyes, and optionally the game's HUD (premultiplied alpha) on a panel in front of the recenter
// origin.
void SubmitEyes(IDirect3DTexture9* left, IDirect3DTexture9* right, IDirect3DTexture9* hud = nullptr, unsigned hudW = 0, unsigned hudH = 0);
void SubmitScreen(IDirect3DTexture9* screen, unsigned w, unsigned h);
// The flat screen's last picture once more (nothing when it has none). With the HUD panel's size, its swapchain is
// made meanwhile if there is none of that size (the race's first frames: see render.cpp's HoldLoadingScreen).
void SubmitLastScreen(unsigned hudW = 0, unsigned hudH = 0);
void EndFrameEmpty();
// The HUD panel's swapchain made ahead (in the menus) for a panel of w x h, if there is none of that size.
void PrepareHud(unsigned w, unsigned h);

// The current head position and direction become "straight ahead" on the next frame.
void Recenter();

// A Touch controller (OpenXR actions, read with the eyes in BeginFrame; only while the session has the
// focus): where it points (its aim pose, relative to the recenter origin like the eyes) and its buttons.
struct Controller
{
    bool tracked = false;
    EyePose aim = {};               // forward: the pointing ray
    EyePose hold = {};              // where the hand holds it (the grip pose; the aim pose when there is none)
    float trigger = 0, grip = 0;    // the index trigger, the grip (0 to 1)
    bool button = false;            // B (right) or Y (left)
    bool lower = false;             // A (right) or X (left)
    bool menu = false;              // the menu button (left)
    bool thumb = false;             // the thumbstick pressed
    float stick[2] = {};            // the thumbstick: x right, y up
};
Controller Hand(int hand);  // 0 left, 1 right
// [Test] without a headset: a controller given by hand (the background command "hand").
void SetTestHand(int hand, const Controller& c);
// A controller vibrates this strongly (0 to 1; 0 stops it) until told otherwise. Called every frame while it changes.
void Haptics(int hand, float amplitude);
float HapticsAmplitude(int hand);  // what Haptics was last told (also without a headset, for the log)

// Where the time of handing images to the runtime goes (seconds, summed since the last call).
struct SubmitTimes
{
    double flush = 0, acquire = 0, wait = 0, fence = 0, submit = 0, endFrame = 0;
    int count = 0;
};
SubmitTimes TakeSubmitTimes();

}  // namespace xr
