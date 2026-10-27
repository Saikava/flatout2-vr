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
// __thiscall(): the post-processing RenderViews runs once its views are drawn (at 0x4CBB2C): the colour filter, the
// copy of the view into the screen texture, the glow and the speed blur (see kDevScreenTex, kDevPost)
constexpr uintptr_t kSlotPostProcess = kDeviceD3dVtbl + 0x150;
constexpr uintptr_t kPostProcess = 0x5AA390;

// Screen sprites: queued and drawn in batches as vertices in the back buffer's pixel space (x from 0
// to its width, y to its height), whatever the render target. __thiscall(const void*), ret 4:
// 0x5AB510 takes a rectangle {x0, y0, x1, y1, u0, v0, u1, v1, colour}, 0x5AB4C0 four ready vertices
// {x, y, z, colour, u, v} (the rectangle's: (x0, y0), (x1, y0), (x1, y1), (x0, y1), z 1). Their shader puts them at
// depth 0 whatever z: tested less-or-equal (the sun's glow, the lens flare) they come over everything.
constexpr uintptr_t kSlotSpriteRect = kDeviceD3dVtbl + 0xD8;
constexpr uintptr_t kSpriteRect = 0x5AB510;
constexpr uintptr_t kSlotSpriteQuad = kDeviceD3dVtbl + 0xD4;
constexpr uintptr_t kSpriteQuad = 0x5AB4C0;
// The queue: the number of sprites in it (int). It is drawn (0x5AB5D0) when the sprites' texture changes,
// when it is full (256, before the next is queued) and at the end, as one triangle list from index 0 with
// 4 vertices and 6 indices for each sprite in the queue's order, by an IDirect3DDevice9::DrawIndexedPrimitive
// that returns to kSpriteBatchDraw (once for each pass of the sprites' effect).
constexpr uintptr_t kSpriteQueueCount = 0x8DA44C;
constexpr uintptr_t kSpriteBatchDraw = 0x5AB6E7;
// The HUD's 2D particles (the nitro's flames by the gauges, ...): each a sprite quad queued from one of
// these return addresses (0x4C4xxx draws them all, a batch for each of their textures).
constexpr uintptr_t kHudParticleSprite[2] = { 0x4C4B14, 0x4C4D54 };
// Turned and scaled sprites (0x4B4D90: the needle, the crash bonus's icon, ...) are queued from here.
constexpr uintptr_t kHudTurnedSprite = 0x4B500A;
// The crash bonus by the rival's card: its icon, which grows and spins as it comes, drawn by the function
// from 0x4C0FC0 to 0x4C13E0 (it reads Settings.HUD CrashIconYOffset).
constexpr uintptr_t kHudCrashBonus = 0x4C0FC0;
constexpr uintptr_t kHudCrashBonusEnd = 0x4C13E0;
// Text in the big font (0x552B90: the music's title box at the top, the crash bonus's "SLAM" by the rival's
// card, which comes in large): each letter a screen sprite queued by 0x552E40, upright from the first of
// these return addresses, turned or scaled from the second.
constexpr uintptr_t kHudBigTextSprite[2] = { 0x552F16, 0x552FD4 };

// DeviceD3d fields: back buffer size (int). The post-processing in RenderViews draws its passes
// into the bound render target as pre-transformed quads of this size (and RenderViews ends with
// SetViewport(0, 0, w, h)), so an eye rendered into a target of another size must set it.
constexpr int kDevWidth = 0x8;
constexpr int kDevHeight = 0xC;

// Lens flare of one view: called by RenderViews per view after the scene, with the camera in
// eax and the flare object in edi. It projects the sun with the camera's frustum (kCamFrustum) into
// the viewport (kViewportX), lays its pieces along the line from there through the viewport's middle,
// sized by the viewport's width over the field of view (kCamFov), and draws them as screen sprites.
constexpr uintptr_t kLensFlareCall = 0x4CBBDA;
constexpr uintptr_t kLensFlare = 0x596980;
// The flare's sun: float[3], from the camera (the sun is drawn at the camera's position plus this).
// How much of it the flare shows comes from the sun's own test, a small render toward it (0x5962xx)
// that has the track and its objects but not the cars.
constexpr int kFlareSun = 0x15C;

// The markers over the rivals (in a derby): the HUD (0x4B88D0) calls 0x4C0200 at kMarkersCall, __stdcall(hud,
// the local player, the largest angle off the camera's direction), ret 0xC, the camera in esi. For each rival not
// wrecked, its car within kMarkerReach metres (float) of the camera and in front of it (kCamMatrixCopy2),
// it projects a point over the car with the camera's matrix and frustum (kCamMatrix, kCamFrustum) and draws a
// triangle there, tinted by the car's damage (green to yellow to red at 1/3 and 2/3), smaller the further the car,
// by 0x4B4BF0 at kMarkerDrawCall: __thiscall(x, y, ?, size, const DWORD* colour), ret 0x14, the rival in ebx.
constexpr uintptr_t kMarkersCall = 0x4BA943;
constexpr uintptr_t kMarkers = 0x4C0200;
constexpr uintptr_t kMarkerDrawCall = 0x4C0AE8;
constexpr uintptr_t kMarkerDraw = 0x4B4BF0;
constexpr uintptr_t kMarkerReach = 0x8DC6AC;

// The sun's glow: the sky pass (0x592470) calls 0x596540 with the camera in esi and the track's flare
// in ebx. It projects the sun with the camera's own frustum (kCamFrustum) and draws the glow there as
// a flat sprite, sized by the camera's field of view (kCamFov).
constexpr uintptr_t kSunGlowCall = 0x592509;
constexpr uintptr_t kSunGlow = 0x596540;

// Darkenings of the whole screen drawn with the HUD, each a screen sprite rectangle over the whole
// screen in its colour, alpha in the top byte (kSpriteRect, rect[8]); the return addresses of their
// sprite calls, and whether they go over the HUD:
// - the race's overlay fade (0x477D40, among the game loop's overlays run by 0x45FC90): to black and
//   back when the car is reset;
// - the game's screen fade (0x45FD00, run by 0x45FC90 while GameFlow +0x968 is set);
// - the dialogs' backdrop (0x496930): the race start dialog dims the picture behind it.
struct FadeSprite
{
    uintptr_t ret;
    bool overHud;
};
constexpr FadeSprite kFadeSprites[] = { { 0x477EAB, true }, { 0x45FE81, true }, { 0x4969E8, false } };

// The pause menu's question before a retry ("Are you sure you wish to restart...") is wrapped
// (0x54E8A0, called by 0x49BB80) to a width pushed as a constant, 540 (the float immediate of a push
// at kRetryTextWidth): the menus' 640x480 units, while the text is drawn scaled to the screen (its
// font and position by the screen's width / 640) and measured so. At 3840 pixels across it wraps into
// a narrow column that runs into the YES / NO below.
constexpr uintptr_t kRetryTextWidth = 0x49BE85;

// The HUD's own particles (object at 0x8E8470: impact_piece, impact_spark and nitro_flare sprites):
// pieces and sparks breaking off an opponent's health bar when it is hit (0x4C5210) and flames on the
// nitro gauge (0x4C4D80); flat, falling across the screen. Moved by 0x4C4200, drawn with the HUD by
// 0x4C4430 (called at 0x45F78B).

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
// For each view RenderViews hands the camera to the renderer (SetCamera with its matrix, SetFrustum with its
// frustum) and then picks the objects the view shows (0x4C9B80, __stdcall(ctx, camera, view), ret 0xC, called at
// kCullViewCall): the culling above, with the camera's own matrix, frustum and field of view.
constexpr uintptr_t kCullViewCall = 0x4CA1CA;
constexpr uintptr_t kCullView = 0x4C9B80;
// Before its views RenderViews renders each car's shadow into a texture of its own (a view from above, 8 cars by 5
// draws), the same for every view of a frame: 0x4C3CE0 (the car's shadow in eax) for each in its list, called at
// kCarShadowCall.
constexpr uintptr_t kCarShadowCall = 0x4C9FE3;
constexpr uintptr_t kCarShadow = 0x4C3CE0;

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
// cameras' aspect ratio (DeviceD3d's getter kScreenShapeGet, slot 9: fld [kScreenShape] at +0, fld
// [kScreenShape + 4] at +0x10). The Steam release's fo2_zoom.dll points the getter at the screen's
// resolution instead (0x6D68E8, two ints it reads as floats: their ratio is the monitor's).
constexpr uintptr_t kScreenShape = 0x69BE18;
constexpr uintptr_t kScreenShapeGet = 0x5AAD10;

// The renderer's screen textures (up to 4 render targets of 512x512, each a wrapper with its
// IDirect3DTexture9 at kTexD3D). Its post-processing (DeviceD3d slot 0x150, 0x5AA390, called by
// RenderViews per view at 0x4CBB2C) copies the view just rendered into the first one and blurs from
// it (bloom). The stunt stadium's video screens ("screenmaterial") show that first one: the view of
// the last post-processed render.
constexpr int kDevScreenTex = 0x478;
// A texture wrapper: its size (int) and its IDirect3DTexture9.
constexpr int kTexWidth = 0x38;
constexpr int kTexHeight = 0x3C;
constexpr int kTexD3D = 0x50;

// The post-processing object (DeviceD3d field). Its colour filter (0x5A9C10, run by the
// post-processing when the track's look has ColorFilter on) copies the rendered view into the first
// of two render textures the size of the back buffer (wrappers at kPostFilterTex), filters it through
// the second and draws the result back over the view.
constexpr int kDevPost = 0x488;
constexpr int kPostFilterTex = 0x24;  // two texture wrappers

// The speed blur: the post-processing smears the bloom in passes (0x5A8C60) zooming out from a point
// of the screen (Data.Effect.PostProcess.RadialBlurZoomCenter, float x, y; 0.5, 0.5 by default, the
// tracks' looks set it), with a strength of RadialBlurStrength times each view's speed factor
// (kRadialBlurSpeed[view], 0..1: it grows with the car's speed and more with nitro, smoothed; 0x470100
// keeps it on the player at +0x6C0 and 0x478FC0 hands it to the renderer every frame).
constexpr uintptr_t kRadialBlurCenter = 0x8E83C4;
constexpr uintptr_t kRadialBlurPasses = 0x8E83B8;  // int
constexpr uintptr_t kRadialBlurStrength = 0x8E83CC;
constexpr uintptr_t kRadialBlurSpeed = 0x8DA468;  // float[4]
constexpr uintptr_t kColorFilterOn = 0x8E83D0;    // Data.Effect.PostProcess.ColorFilter (int)
// Set once the post-processing's first run (0x5A7740) has filled in its settings' defaults and handed
// them to the data (byte).
constexpr uintptr_t kPostSettingsLoaded = 0x8E8606;

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
// The shaders get the world, view and projection matrices multiplied (c0: world, kViewMatrix, kProjMatrix), made
// anew by the next draw (0x5A6D20) once this is set (SetCamera and SetFrustum set it).
constexpr uintptr_t kViewProjChanged = 0x8E5DF4;  // int

// ---- render camera (cams[i] in RenderViews) -----------------------------------------------
constexpr int kCamMatrix = 0x80;   // float[16], rows: right, up, forward, position (y up, LH)
// The camera keeps more copies of its placement: the same matrix at +0x40, and the view matrix
// (its inverse) at +0x130 followed by the view matrices of the 7 steps before (the race shifts them
// on with every physics step, 0x551C00, and fills all eight with the present one when the camera
// cuts, 0x551B50). The particle renderer (0x59F8B0) takes the camera's position from +0x70 and moves
// the particles into camera space itself (drawing them with an identity camera): a particle with the
// current view, the points of its motion streak (0x59F310, where the particle was 0.04 s, 0.08 s, ...
// before) with the views of the steps before. A streak so shows the particle's motion against the
// camera's.
constexpr int kCamMatrixCopy2 = 0x40;
constexpr int kCamViews = 0x130;  // float[8][16]: now, 1 step ago, ... 7 steps ago
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

// ---- particles ---------------------------------------------------------------------------------
// Every view keeps its particles in lists: a main one of 8192 and two of 512, each with its count
// and capacity (int) in the view's entry (kParticleViews, kParticleViewCount entries of 0x44 bytes;
// also the view's position, +0x30). An effect adds a particle to the lists of the views near enough
// with 0x58EE80, 0x58EED0 or 0x58EF30 (cdecl: particle, a flag byte per view), which drop it when a
// list is full.
constexpr uintptr_t kParticleViews = 0x8DE050;
constexpr int kParticleViewSize = 0x44;
constexpr uintptr_t kParticleViewCount = 0x8DDF10;  // unsigned
constexpr uintptr_t kAddParticle = 0x58EE80;        // the main list, count at +0x10
constexpr uintptr_t kAddParticleB = 0x58EED0;       // count at +0x28
constexpr uintptr_t kAddParticleC = 0x58EF30;       // count at +0x1C
// Effects that make particles in bulk, and the calls that add them (call site, add function, its
// list's count in a view's entry, how often the effect runs):
// - the track's emitters (fountains of sparks and fire; the stunt stadium is full of them): every
//   physics step of the race (0x4725C0) runs them through 0x591E50, a particle per emitter and step
//   (0x59A940);
// - dust and dirt off the ground: the race tick (0x472D30) calls 0x43E480 for every car once per frame
//   (at 0x47372E), whose wheels' contacts kick it up (0x5856B0, 0x582960); and every step hands the
//   world's contacts of the step to 0x5878B0 (also through 0x591E50).
enum class Every
{
    Step,
    Frame
};
struct ParticleAddCall
{
    uintptr_t site, target;
    int countOffset;
    Every every;
};
constexpr ParticleAddCall kBulkParticleAdds[] = {
    { 0x59BB47, kAddParticle, 0x10, Every::Step },   { 0x5861E7, kAddParticleB, 0x28, Every::Frame },
    { 0x586B0F, kAddParticleC, 0x1C, Every::Frame }, { 0x587246, kAddParticle, 0x10, Every::Frame },
    { 0x587895, kAddParticleB, 0x28, Every::Frame }, { 0x5837B4, kAddParticle, 0x10, Every::Frame },
    { 0x58800F, kAddParticleB, 0x28, Every::Step },  { 0x588263, kAddParticle, 0x10, Every::Step },
    { 0x58889D, kAddParticle, 0x10, Every::Step },
};

// ---- players and cars ------------------------------------------------------------------------
constexpr uintptr_t kLocalPlayer = 0x6B21F8;  // Player*
constexpr int kPlayerCar = 0x33C;             // Car*
// kLocalPlayer is the first of the race's local players (entries of 0x1C bytes), listed as the players are
// made (0x46B670: those of kPlayerType 1) except in the stunt events (kStuntEvent), where the list stays empty.
// There the player at this PC is found in the race's own list of players (GameFlow +kGfRace).
constexpr uintptr_t kStuntEvent = 0x8DA516;   // bool
constexpr int kGfRace = 0x9B8;
constexpr int kRacePlayers = 0x14;            // Player** begin, end
constexpr int kPlayerType = 0x36C;            // 1: the player at this PC
constexpr int kCarName = 0x40;                // char[], the car's name ("Venom")
constexpr int kCarFolder = 0x78;              // const char*, its data folder ("data/Cars/Car_18/")
constexpr int kCarMatrix = 0x1B0;             // float[16], rows right, up, forward, position
constexpr int kCarVelocity = 0x280;           // float[3], m/s
// Each frame the race renderer (0x4C9B80, per view) shows every car as the view needs it: at 0x4C9D23 it
// calls 0x43DAD0 (car in ecx, mode in eax). Mode 1 shows the whole car with its driver and steering
// wheel; the car the view follows gets its camera's choice instead (the camera's vfunc 8): 0 for the
// camera inside the car, which leaves the driver (kCarDriver), the steering wheel (kCarSteeringWheel)
// and the windows (the car's materials of shader 6, listed from kCarWindows: flag 0x10000 at +8) out,
// or 2 with no car at all. The driver and the steering wheel are scene nodes, left out by flag 2 at
// +0x10. The car keeps the mode at kCarViewMode.
constexpr uintptr_t kCarViewCall = 0x4C9D23;
constexpr uintptr_t kSetCarView = 0x43DAD0;
constexpr int kCarViewMode = 0x291C;
// The driver (the male or the female model) is placed at the car's DriverLoc (the seat, below the hips),
// with the car's axes. The camera inside the car is placed from there too (its PositionType 4 in the
// car's camera.ini), in front of the driver's head and above it, differently in every car.
constexpr int kCarDriver = 0x2AC0;         // scene node*
// The driver's place in the car's axes (kCarMatrix): DriverLoc (at +0x1CA4) less the centre of mass
// (+0x1C60). The camera inside the car is placed from it (0x4D0070).
constexpr int kCarDriverLoc = 0x1C78;      // float[3]
// The cabin's box (CollisionTopMin and Max in the car's body.ini, float[3] each, read at 0x433E5E), in
// the model's axes: less the centre of mass (kCarCentreOfMass) they are in kCarMatrix's.
constexpr int kCarCabinMin = 0x190;
constexpr int kCarCabinMax = 0x19C;
constexpr int kCarCentreOfMass = 0x1C60;   // float[3]
// The driver's ragdoll (+0x2A90) keeps its 11 bones' skinning matrices from +0x50 (float[16] each, model
// to world, in the order of data/drivers/ragdoll/male_bones.ini or female_bones.ini: the lower torso
// first, the head 7th), posed in every frame from the driving animation while the driver sits in the
// car, before the frame is drawn. At +0 it points to its segments (the head first), each a box placed
// in the model's axes by a matrix (float[16], the box's middle at +0x30). Every car step
// poses it from the steering (0x57C970, called at 0x4295E4): the animation (ragdoll +0x858) is sampled
// at (steering + 1) / 2 of its length by 0x57ADA0 (at 0x57CA2F), __stdcall(animation, node, float t,
// float* matrices), edx = 0. That takes the time floor(t * (length - 1)) and gives every bone its pose
// of the frame there, without blending the next: the driving animation has 100 frames over the
// steering's whole travel, and the hands move in steps.
constexpr int kCarRagdoll = 0x2A90;
constexpr int kRagdollMatrices = 0x50;
constexpr int kRagdollHeadBone = 6;
constexpr int kRagdollSegments = 0x0;    // the head's segment first
constexpr int kSegmentBoxMiddle = 0x30;  // float[3], the model's axes
// When the driver is thrown out (a heavy crash, or launched in the stunts) the ragdoll is let loose
// (kRagdollOut set) until the car is reset with him back in it. While it is out the segments are bodies of
// the world's physics, and each segment's matrix (0x170 bytes apart) is its box's place in the world: the
// player's camera follows the head's (0x4D37A0: the ragdoll camera, data/drivers/ragdoll/camera.ini, 4 m
// behind and 2 m above), the car step no longer poses the driver from the driving animation (0x4294D9) and
// the renderer adds his shadow under the third segment (0x4CB495).
constexpr int kRagdollOut = 0x20;        // int, the car's +0x2AB0
constexpr int kSegmentStride = 0x170;
// The crashes throw him out with __stdcall(car, the car's int at kCarCrashArg), ret 8 (called at 0x43859F and
// 0x438C3B; with the game's Ragdoll setting on): the ragdoll put where the driver sits, given the car's speed and
// let loose (0x57C2A0). The stunts' launch is 0x425900.
constexpr uintptr_t kThrowDriverOut = 0x424EF0;
constexpr int kCarCrashArg = 0x2A84;
constexpr uintptr_t kSampleAnimCall = 0x57CA2F;
constexpr uintptr_t kSampleAnim = 0x57ADA0;
// The car's steering, -1 (left) to 1: the player's (see kSteeringCubic) times the speed's factor.
constexpr int kCarSteering = 0x1E04;  // float
// Every car step poses its parts with __stdcall(car, float dt) (kPoseCarParts, called at kPoseCarPartsCall), the
// steering wheel among them (kCarSteeringWheel): turned by the steering (and a shake on rough ground), clamped to -1..1,
// times 90 degrees. Then it poses the driver from the steering with __stdcall(ragdoll, float* place, float steering)
// (kPoseDriver at kPoseDriverCall; see kSampleAnim). Each reads the steering once.
constexpr uintptr_t kPoseCarPartsCall = 0x42948E;
constexpr uintptr_t kPoseCarParts = 0x4231D0;
constexpr uintptr_t kPoseDriverCall = 0x4295E4;
constexpr uintptr_t kPoseDriver = 0x57C970;
// An animation: its bone count (+0), start and end time (+4, +8), time per frame (+0x14).
constexpr int kAnimBones = 0x0;
constexpr int kAnimStart = 0x4;
constexpr int kAnimEnd = 0x8;
constexpr int kAnimFrameTime = 0x14;
constexpr int kCarSteeringWheel = 0x32F0;  // scene node*
// The engine: its revs (per minute), its idle and its limit (the engine's sound is given the revs and the limit,
// 0x42431F), and the throttle as the engine has it (0 to 1, the pedal through the automatic gearbox's shifts).
constexpr int kCarEngineRevs = 0x5D8;      // float
constexpr int kCarEngineIdle = 0x390;      // float
constexpr int kCarEngineLimit = 0x3AC;     // float
constexpr int kCarEngineThrottle = 0x5B8;  // float
// A scene node's world matrix (float[16], rows right, up, forward, position); its local one is at +0x40.
constexpr int kNodeWorld = 0x80;
// Its flags: kNodeHidden leaves it out (SetCarView sets it on the driver for the camera inside the car).
constexpr int kNodeFlags = 0x10;
constexpr unsigned kNodeHidden = 2;
// The car's body: the scene node of the "body" mesh of its body.bgm, the outside and the inside of the
// cabin (its material "interior": the dashboard, the seats, the doors' insides), in the model's axes. A
// mesh node keeps its models (one a level of detail, the nearest first) from +0xF0, 8 bytes apart; a
// model the begin and end of its batches' pointers at +0x30. A batch (drawn by 0x5AD450) has its vertices'
// FVF at +0xC (position, normal, colour, then the texture coordinates), their size at +0x10, the primitive type
// at +0x14 (4 list, 5 strip), the words vertex count +0x18,
// index count +0x1C (0: not indexed), base vertex +0x1E, first vertex +0x20 and first index +0x22, its
// vertex buffer at +0x24 and index buffer at +0x28 (objects with the D3D9 buffer at +0x24 and +0x1C) and
// its vertices on the CPU at +0x2C (the body's, which the crashes bend; from the first vertex). A vertex
// has its position first.
constexpr int kCarBody = 0x1980;           // scene node*
constexpr int kNodeModels = 0xF0;
constexpr int kModelBatches = 0x30;
constexpr int kBatchFvf = 0xC;
constexpr int kBatchStride = 0x10;
constexpr int kBatchPrimType = 0x14;
constexpr int kBatchVertexCount = 0x18;
constexpr int kBatchIndexCount = 0x1C;
constexpr int kBatchBaseVertex = 0x1E;
constexpr int kBatchFirstVertex = 0x20;
constexpr int kBatchFirstIndex = 0x22;
constexpr int kBatchVertexBuffer = 0x24;
constexpr int kBatchIndexBuffer = 0x28;
constexpr int kVertexBufferD3D = 0x24;
constexpr int kIndexBufferD3D = 0x1C;
constexpr int kBatchVertices = 0x2C;
constexpr int kCarWindows = 0x45BC;        // begin, end of an array of pointers
// The car's glass: the batches listed from kCarWindows (their materials have shader 6, "car window", which draws
// with pro_car_window.sha) come last of the scene, blended (ONE, SRCALPHA) with the depth written and the faces
// turned away culled (each window of the Boxer, Car_3, is two layers, one facing out and one in). The shader
// reflects the track's environment cube map by a Fresnel term f = c33.x + c33.y * (1 - |N.V|)^5 (the technique
// sets 0.25 and 0.5) and adds the specular colour (c22, 0.8) times the map's alpha (the sun); what lies behind is
// kept times (1 - f) * c32.w, the car's WindowBrightness (kCarWindowConstant, float[4]: 0.5 0.5 0.5 0.75 in most
// cars; the batch's +4 points to it). Its vertex shader takes the world matrix's rows from c4 and the eye's place
// from c8 for the reflection and f, the projection from c0 alone. The maps have the ground black: from inside the
// car the windscreen reflects downwards and only darkens the picture, while the side windows, seen at a glancing
// angle, reflect the bright horizon.
// The dynamic shader class's batch draw (0x5B10C0) sets a batch's constants, then draws its geometry (kBatchDraw,
// the batch in esi) at kDynamicBatchDrawCall.
constexpr uintptr_t kBatchDraw = 0x5AD450;
constexpr uintptr_t kDynamicBatchDrawCall = 0x5B115C;
constexpr int kCarWindowConstant = 0x4620;
// The car's four windows (front, rear, left, right), kWindowSlotSize bytes apart: the material (window_front or
// windows, window_rear, ...), its texture, windows_damaged.tga and whether the window is broken (byte). A window
// that breaks (0x425898 for the front) gets windows_damaged.tga and the shader "car window damaged"
// (pro_default_dynamic.sha: cracked glass with holes); a repair (0x427590) gives it its glass back.
constexpr int kCarWindowSlots = 0x45C8;
constexpr int kWindowSlotSize = 0x14;
constexpr int kWindowBroken = 0xC;
// The car's scene tree (the meshes of its body.bgm): its root node, and each node's first child and next
// sibling. Mesh nodes (vtable kMeshNodeVtbl) have their name at kNodeName (an MSVC std::string: the text in place
// below 16 characters, else a pointer to it; its capacity at +0x14) and kNodeModelCount models from kNodeModels.
// The parts that come off in crashes are named part_<n>_<what>; the side mirrors part_<n>_mirror_l and _r
// ("part_11_mirror_l"), placed in the tree under the root or their door, and in the car's parts ini under their
// door (Parent = "door_l"): they move with it, swing when they are knocked loose and fly off when knocked away.
// A mirror has no glass of its own: its housing is one batch in the car's body material, and the glass is that
// mesh's largest flat face looking back, textured as dark glass (in the Chili the glass looks 17 degrees down, in the
// Venom straight back: set for no one's eyes).
constexpr int kCarSceneRoot = 0x338;
constexpr int kNodeFirstChild = 0xC4;
constexpr int kNodeNextSibling = 0xC8;
constexpr uintptr_t kMeshNodeVtbl = 0x67AE30;
constexpr int kNodeName = 0xD0;
constexpr int kNodeModelCount = 0x108;
// The parts that come off (bumpers, doors, the hood, mirrors, ...) are objects of their own, listed from kCarParts
// (begin and end of their pointers; made from the car's parts ini at 0x43305B), each with its scene node at kPartNode
// and its state at kPartState. Every hit adds to the part's momentum: past its UnlockMomentum it is knocked loose
// (state 1: a door or the hood swings on its hinge, 0x4289A0; a mirror or a bumper is given the car's speed,
// 0x436600), past its ReleaseMomentum it is knocked off (state 2: it flies with a body of its own, which its node
// follows from then on: 0x4366F0, 0x428C6E).
constexpr int kCarParts = 0x1BEC;
constexpr int kPartState = 0x10;
constexpr int kPartNode = 0x2B0;
constexpr int kPartKnockedOff = 2;
// The part's virtual calls (thiscall, no arguments) that knock it loose and off: the slots of 0x436600 and 0x4366F0.
constexpr int kPartKnockLoose = 10;
constexpr int kPartKnockOff = 11;

// ---- the game's settings ----------------------------------------------------------------------
// "Settings.Game.Camera" (Savegame\options.cfg, read at the game's start, written back when it is left): the race
// camera's index in the car's list, which the race takes for the player at its start (0x478CA7; the last one in a car
// with fewer) and the C key's choice sets (0x4796D9). Index 4 is the camera inside the car.
constexpr uintptr_t kSettingCamera = 0x69C2A0;  // int
constexpr int kCameraInside = 4;

// ---- input ------------------------------------------------------------------------------------
// The game's input (0x55B240) makes its DirectInput (0x561270, kept at kDirectInput) and lists its game controllers
// (kControllerEnum, called at kControllerEnumCall with the input in esi): see controllers.cpp.
constexpr uintptr_t kControllerEnumCall = 0x55B253;
constexpr uintptr_t kControllerEnum = 0x5612C0;
constexpr uintptr_t kDirectInput = 0x8DA74C;  // IDirectInput8A*
// The input's controllers (the keyboard and the chosen game controller): their number and list (kInputControllers
// at the input, kInputControllerList on). A controller waiting for a key or button to be assigned to one of its
// controls (Configure controls) has the control's index at kControllerRemap (-1 otherwise).
constexpr uintptr_t kInput = 0x8E844C;  // the input's object*
constexpr int kInputControllers = 0x4;
constexpr int kInputControllerList = 0x1C;
constexpr int kControllerRemap = 0x784;
// A controller's product GUID and its settings, in percent (Options > Controller options): the sensitivity bends its
// steering ((1 - k) v + k v^3 with k = (100 - sensitivity) / 100, 0x561780, a table of 1024 read at 0x55D920), the
// dead zone, and the saturation, which first stretches each half axis's value v about its middle ((v - 0.5) /
// (saturation / 100) + 0.5, clamped to 0..1).
constexpr int kControllerGuid = 0x8;
constexpr int kControllerSensitivity = 0x134;
constexpr int kControllerSaturation = 0x13C;
// A controller's controls (Configure controls; Savegame\am*.dat keeps them): 20 entries of kActionSize bytes from
// kControllerActions, each a key (type 0: its virtual key at +4), an axis (type 1: the index of its value in
// DIJOYSTATE2 at +4, from which value to which in shorts at +0xC and +0xE) or a button (type 2). The steering is the
// right half axis's value less the left one's (kActionSteerRight, kActionSteerLeft), each read by kActionValue (the
// entry in eax, the controller in edx; an axis's (value - from) / (to - from), 0..1, on the FPU stack), then stretched
// by the saturation and bent by the sensitivity (0x55D920).
constexpr int kControllerActions = 0x644;
constexpr int kActionSize = 16;
constexpr int kActionType = 8;
constexpr int kActionSteerLeft = 12;
constexpr int kActionSteerRight = 13;
constexpr uintptr_t kActionValue = 0x55EA50;
// A controller's read of its player's controls (0 steer left, 1 steer right, 2 throttle, 3 brake, ...: the player's
// input asks for those four, 0x46D22F): each control's entry (a byte at kControllerControls on, 0xFF none), read by the
// controller's virtual kControllerReadEntry (thiscall, the entry: nonzero while held).
constexpr int kControllerControls = 0x78C;
constexpr int kControllerReadEntry = 0x2C;  // vtable offset
constexpr int kDrivingControls = 4;
// The player's input (0x46D22F, every frame): none while kPlayerInputOff is set (the steering put back to the middle);
// else the steering from its controls, an axis's (kPlayerSteerKeys clear) or the keys'.
constexpr int kPlayerInputOff = 0x378;
constexpr int kPlayerSteerKeys = 0x668;
// The player's controller (one of the input's list: kInputControllerList), which the race gives the player from the
// options' choice (kChosenController: 0 the keyboard) as it starts (0x45F9F0). A game controller's button reads take
// the keyboard's key for the same control as well (0x55D760): the keyboard's keys drive beside a chosen game controller.
// Its steering, throttle and brake each take the axis over the keys when the axis is off its rest by more than the float
// at kTinyValue (1e-6, a constant the game shares elsewhere): the operands of those three comparisons (fcomp) are at
// kAxisOverKeysOperands. A trigger at rest comes out at 0.0018 there (the VR controllers' as the game reads it), so the
// keyboard's throttle and brake never counted beside a gamepad.
constexpr int kPlayerController = 0x32C;
constexpr uintptr_t kChosenController = 0x8D7BE4;  // int
constexpr uintptr_t kTinyValue = 0x67DCFC;
constexpr uintptr_t kAxisOverKeysOperands[] = { 0x46D2BA, 0x46D33B, 0x46D392 };
// The player's steering (0x46F6C4) bends the controller's again, (1 - c) y + c y^3 with c here (0.5, from the game's
// data), moves toward it at a limited rate (centre to full lock in half a second), and the car's steering is that times
// a factor that falls with the speed (0x46FA50: 1 up to 20, 0.8 at 40, 0.5 at 100, 0.25 at 250). The steering wheel in
// the cockpit turns 90 degrees at full lock.
constexpr uintptr_t kSteeringCubic = 0x8DB9E0;  // float
// The game's keyboard: its WH_KEYBOARD hook (0x55AAD0) keeps a state per virtual key (0x80 down, bit 0 toggled on
// every press), counts presses per key and the keys held. The menus read these.
constexpr uintptr_t kKeyState = 0x8D7E60;    // BYTE[256]
constexpr uintptr_t kKeyPresses = 0x8D7D60;  // BYTE[256]
constexpr uintptr_t kKeysHeld = 0x8DA740;    // int

// ---- sound -------------------------------------------------------------------------------------
// The game's import of GetProcAddress, through which its sound (FMOD 3.75, built in) gets DirectSoundCreate8.
constexpr uintptr_t kIatGetProcAddress = 0x6521C8;

// ---- game state -----------------------------------------------------------------------------
constexpr uintptr_t kGameFlow = 0x8E8410;  // GameFlow*
constexpr int kGameState = 0x458;          // 1 = menu, 2 = race
constexpr int kGfEventType = 0x4AC;        // 3: a derby (0: a race, a single event); the HUD lays itself out by it
constexpr int kRaceState = 0x91C;          // 8 = racing

}  // namespace fo2
