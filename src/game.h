#pragma once
// FlatOut 2 v1.2, DRM-free retail / GOG build (FlatOut2.exe, 2,990,080 bytes, EP RVA 0x202638).
// Every address here was checked against that exe; Patch() also verifies the original bytes at
// runtime, so an unknown build is left untouched.
#include <cstdint>

namespace fo2 {

// ---- race frame -------------------------------------------------------------------------
// RenderRace (0x479200) computes the speed zoom, collects the view cameras and calls
// RenderViews(ctx, cams, fovScale) at 0x479311. RenderViews runs every scene pass; for each
// pass and view it calls DeviceD3d::SetViewport / SetCamera / SetFrustum and draws. HUD and
// race audio (0x4721C0) follow in RenderRace, so only RenderViews may be run more than once.
constexpr uintptr_t kRenderViewsCall = 0x479311;
constexpr uintptr_t kRenderViews = 0x4C9DC0;  // __stdcall(ctx, cams, fovScale), ret 0xC
constexpr int kCtxViewCount = 0x1BC;          // int
constexpr int kCtxLastView = 0x1C0;           // int, reset to -1 on entry
// Grass (the track's plant cards, drawn per view by 0x595000): whether it is on (int), how far
// along the view direction it is drawn and where it starts fading out (floats, 80 and 50 m).
// The far distance also ends the frustum the plant batches are culled with (0x58B290).
constexpr int kCtxPlantsOn = 0x0;
constexpr int kCtxPlantFar = 0x4;
constexpr int kCtxPlantFade = 0x8;

// ---- engine renderer (DeviceD3d) ----------------------------------------------------------
constexpr uintptr_t kDeviceD3d = 0x8DA718;      // DeviceD3d*
constexpr uintptr_t kDeviceD3dVtbl = 0x67D560;
// __thiscall(), returns bool: IDirect3DDevice9::Present, and the device reset when it reports
// D3DERR_DEVICENOTRESET (0x5A6210)
constexpr uintptr_t kSlotEnginePresent = kDeviceD3dVtbl + 0x20;
constexpr uintptr_t kEnginePresent = 0x5A6170;
// __thiscall(int x, int y, int w, int h, int view) -> IDirect3DDevice9::SetViewport, ret 0x14
constexpr uintptr_t kSlotSetViewport = kDeviceD3dVtbl + 0x30;
constexpr uintptr_t kSetViewport = 0x5A5710;
// __thiscall(const float* camMatrix): builds the view matrix (kViewMatrix), ret 4
constexpr uintptr_t kSlotSetCamera = kDeviceD3dVtbl + 0xAC;
constexpr uintptr_t kSetCamera = 0x5A6F50;
// __thiscall(const float* frustum): builds an off-center LH projection (kProjMatrix) and
// calls IDirect3DDevice9::SetTransform(D3DTS_PROJECTION), ret 4
constexpr uintptr_t kSlotSetFrustum = kDeviceD3dVtbl + 0xB0;
constexpr uintptr_t kSetFrustum = 0x5A7130;

// DeviceD3d fields: back buffer size (int). The post-processing in RenderViews draws its passes
// into the bound render target as pre-transformed quads of this size (and RenderViews ends with
// SetViewport(0, 0, w, h)), so an eye rendered into a target of another size must set it.
constexpr int kDevWidth = 0x8;
constexpr int kDevHeight = 0xC;

// Lens flare of one view: called by RenderViews per view after the scene, with the camera in
// eax and the flare object in edi; projects the sun with the camera's symmetric field of view.
constexpr uintptr_t kLensFlareCall = 0x4CBBDA;
constexpr uintptr_t kLensFlare = 0x596980;

// Screen-space crash sparks and debris ("HUD particles", flying at the camera): drawn after the race
// frame (RenderRace) by 0x4C4430, the particle object in eax, called at 0x45F78B.
constexpr uintptr_t kHudParticlesCall = 0x45F78B;
constexpr uintptr_t kHudParticles = 0x4C4430;

// RenderViews works out each camera's level-of-detail factor (kCamLodFactor) as atan(fov / 2)
// times the constant at kLodConstant; these are the two instructions that load it
// (fmul [kLodConstant]: the operand's address).
constexpr uintptr_t kLodConstant = 0x67E16C;
constexpr uintptr_t kLodConstantUse1 = 0x4C9EF7;
constexpr uintptr_t kLodConstantUse2 = 0x4CBDAB;
// RenderViews' object culling (0x4C9B80 calls 0x58AF10 per view) keeps an object only while its
// depth is under its size times the view's focal length in pixels, (screen height * 0.5) /
// tan(fov / 2); this is the operand of the fmul by 0.5 (the constant's address).
constexpr uintptr_t kVisibilityHalf = 0x67DB78;
constexpr uintptr_t kVisibilityHalfUse = 0x4C9D55;

// The trees' leaf pass (0x5548A0, per view and bucket of trees) builds the leaf cards of up to 64
// trees at a time into a dynamic vertex buffer: it locks the buffer (kLeafLock: the buffer in eax,
// the size in ecx, the chunk's tree count in ebx), writes four vertices per card (kLeafLoop up to
// kLeafDone), unlocks it and draws (from kLeafUnlock). The cards are turned by the camera's
// direction (kLeafBasis, float[3], set at the pass's start); its frame (ebp) holds the bucket's
// list of trees (kLeafFrameItems) and the chunk's first one (kLeafFrameFirst).
constexpr uintptr_t kLeafLock = 0x554C10;
constexpr uintptr_t kLeafLoop = 0x554C23;
constexpr uintptr_t kLeafDone = 0x554E5A;
constexpr uintptr_t kLeafLockFailed = 0x554F5E;
constexpr uintptr_t kLeafUnlock = 0x554E5D;
constexpr uintptr_t kLeafBasis = 0x8DD454;
constexpr int kLeafFrameItems = 0x48;
constexpr int kLeafFrameFirst = 0x60;

// The grass pass (0x595000, per view) writes the cards of the plant batches in view, batch after
// batch, into a dynamic vertex buffer of 2048 cards (96 bytes each), drawing it and locking it anew
// when the next batch might not fit. Its frame (esp) holds the batch index (kGrassFrameBatch), the
// list of batches (kGrassFrameList), the write pointer (kGrassFrameDst) and the cards still free
// (kGrassFrameFree).
constexpr uintptr_t kGrassLock = 0x595309;         // the lock (the buffer object in edi)
constexpr uintptr_t kGrassFlush = 0x5952B6;        // before a full buffer is drawn
constexpr uintptr_t kGrassFinalFlush = 0x595582;   // before the last one is
constexpr uintptr_t kGrassBatch = 0x59532C;        // a batch's cards start (edx: its card count)
constexpr uintptr_t kGrassBatchEnd = 0x595565;     // and end
constexpr int kGrassFrameFree = 0xC;
constexpr int kGrassFrameBatch = 0x38;
constexpr int kGrassFrameDst = 0x3C;
constexpr int kGrassFrameList = 0x110;
constexpr int kGrassCardBytes = 96;
constexpr int kGrassChunkBytes = 0x30000;

// The streak particles' trail builder (0x59F310, per particle and view): where it fills the
// segments of a trail that ended early.
constexpr uintptr_t kTrailFill = 0x59F848;

// The screen's shape (float width, height: 4 and 3, or 16 and 9), from which RenderViews takes the
// cameras' aspect ratio (DeviceD3d's getter 0x5AAD10, slot 9).
constexpr uintptr_t kScreenShape = 0x69BE18;

// Last viewport passed to SetViewport (x, y, w, h); the engine restores viewports from here.
constexpr uintptr_t kViewportX = 0x8DA2CC;

constexpr uintptr_t kD3DDevice = 0x8DA788;   // IDirect3DDevice9*
constexpr uintptr_t kDirect3D = 0x8DA784;    // IDirect3D9*
// Direct3DCreate9 import thunk (jmp [0x6523BC]) and the two calls to it.
constexpr uintptr_t kDirect3DCreate9Thunk = 0x5FFA80;
constexpr uintptr_t kCreateD3DCall1 = 0x5A5869;
constexpr uintptr_t kCreateD3DCall2 = 0x5A5A08;
constexpr uintptr_t kCamMatrixCopy = 0x8E5EC0;
constexpr uintptr_t kViewMatrix = 0x8E5F00;
constexpr uintptr_t kProjMatrix = 0x8E5F40;

// ---- render camera (cams[i] in RenderViews) -----------------------------------------------
constexpr int kCamMatrix = 0x80;   // float[16], rows: right, up, forward, position (y up, LH)
// The camera keeps more copies of its placement: the same matrix at +0x40, and the view matrix
// (its inverse) at +0x130 followed by the view matrices of the 7 frames before. The particle
// renderer (0x59F8B0) takes the camera's position from +0x70 and moves the particles into camera
// space itself (drawing them with an identity camera): a particle with the current view, the
// points of its motion streak (0x59F310) with the views of the frames before.
constexpr int kCamMatrixCopy2 = 0x40;
constexpr int kCamViews = 0x130;  // float[8][16]: now, 1 frame ago, ... 7 frames ago
constexpr int kCamViewCount = 8;
// float[7]: left, right, bottom, top (at the near plane), near, far, ortho flag.
// RenderViews rewrites it on entry from kCamFov / kCamAspect (symmetric).
constexpr int kCamFrustum = 0xF0;
constexpr int kCamNear = 0x10C;
constexpr int kCamFar = 0x110;
constexpr int kCamFov = 0x114;     // horizontal field of view, radians
constexpr int kCamAspect = 0x118;  // width / height
// Level-of-detail factor: distances from the camera are multiplied by it before they pick an
// object's model (0x54DEB0) or are held against the draw and fade distances (0x5538E0).
constexpr int kCamLodFactor = 0x11C;

// ---- game loop timing ------------------------------------------------------------------------
// On every pass the game loop (0x45F480) asks the frame gate (0x45F850, GameFlow in edi, called at
// 0x45F52A) whether to run a frame. The gate adds the frame timer's milliseconds to an
// accumulator; once a 10 ms simulation tick has gathered it advances the simulation by whole
// ticks (GameFlow +0x9AC) and returns 1, otherwise it returns 0 (after Sleep(1) when less than
// 5 ms gathered). The frame timer (0x54CB90, holder in esi) returns the average of the last 8
// passes in whole milliseconds and carries the remainder.
constexpr uintptr_t kFrameGateCall = 0x45F52A;
constexpr uintptr_t kFrameGate = 0x45F850;
constexpr uintptr_t kFrameTimerHolder = 0x6D6D48;  // pointer passed in esi to kReadFrameTimer
constexpr uintptr_t kReadFrameTimer = 0x54CB90;
constexpr uintptr_t kSimTotalMs = 0x8DA594;   // unsigned, sum of the timer's milliseconds
constexpr uintptr_t kLastTimerMs = 0x696DD8;  // the timer's last reading
constexpr uintptr_t kFrameCounter = 0x8DA598;
constexpr uintptr_t kSimAccumMs = 0x8DA5A0;   // milliseconds gathered but not simulated yet
constexpr int kGfResetTimer = 0x920;   // set: the next gate discards one timer reading
constexpr int kGfFixedFps = 0x944;     // set: every reading is 1000 / kGfFixedFpsRate ms
constexpr int kGfFixedFpsRate = 0x94C;
constexpr int kGfTimeShiftUp = 0x960;  // time scale: simulated ms = (real ms >> up) << down
constexpr int kGfTimeShiftDown = 0x964;
constexpr int kGfFrameDt = 0x9AC;      // simulated ms of this frame
constexpr int kGfFrameDtReal = 0x9A8;  // the same in real ms
// The race simulation (0x4725C0) steps a frame's time in pieces of at most 10 ms
// ("cmp eax, 10" / "mov eax, 10"), each with one world physics step pushed as "push 0.01f".
constexpr uintptr_t kRacePieceCmp = 0x4726BE;
constexpr uintptr_t kRacePieceMov = 0x4726C7;
constexpr uintptr_t kPushPhysicsStep = 0x472841;
// The race tick (0x472D30, once per frame, before the pieces) starts by setting the physics world's
// step: 10 (ms), 0.01f and 100.0f, each a "mov [ebp+disp32], imm32" (ebp = race simulation, the
// world at +0xA0; immediate 6 bytes in). The integrator reads the step in seconds; the millisecond
// step feeds the players' and AI's timers and each car's own step.
constexpr uintptr_t kTickSetStepMs = 0x472F3C;
constexpr uintptr_t kTickSetStep = 0x472F46;
constexpr uintptr_t kTickSetStepInv = 0x472F50;
constexpr int kWorldStep = 0x20724;    // float, seconds
constexpr int kWorldStepMs = 0x2072C;  // int
// Every car step sets the car's step (car +0x1E08, its inverse at +0x1E0C) from the world's step in
// whole milliseconds: "fild ms; fld 0.001; fmul" at 0x4297A7 (ebp = world). The wheels and the
// drivetrain integrate with it.
constexpr uintptr_t kCarStepLoad = 0x4297A7;

// ---- players and cars ------------------------------------------------------------------------
constexpr uintptr_t kLocalPlayer = 0x6B21F8;  // Player*
constexpr int kPlayerCar = 0x33C;             // Car*
constexpr int kCarMatrix = 0x1B0;             // float[16], rows right, up, forward, position
constexpr int kCarVelocity = 0x280;           // float[3], m/s

// ---- game state -----------------------------------------------------------------------------
constexpr uintptr_t kGameFlow = 0x8E8410;  // GameFlow*
constexpr int kGameState = 0x458;          // 1 = menu, 2 = race
constexpr int kRaceState = 0x91C;          // 8 = racing

}  // namespace fo2
