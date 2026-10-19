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

// Screen sprites: queued and drawn in batches as vertices in the back buffer's pixel space (x from 0
// to its width, y to its height), whatever the render target. __thiscall(const void*), ret 4:
// 0x5AB510 takes a rectangle {x0, y0, x1, y1, u0, v0, u1, v1, colour}, 0x5AB4C0 four ready vertices.
constexpr uintptr_t kSlotSpriteRect = kDeviceD3dVtbl + 0xD8;
constexpr uintptr_t kSpriteRect = 0x5AB510;
constexpr uintptr_t kSlotSpriteQuad = kDeviceD3dVtbl + 0xD4;
constexpr uintptr_t kSpriteQuad = 0x5AB4C0;

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
constexpr uintptr_t kSampleAnimCall = 0x57CA2F;
constexpr uintptr_t kSampleAnim = 0x57ADA0;
// An animation: its bone count (+0), start and end time (+4, +8), time per frame (+0x14).
constexpr int kAnimBones = 0x0;
constexpr int kAnimStart = 0x4;
constexpr int kAnimEnd = 0x8;
constexpr int kAnimFrameTime = 0x14;
constexpr int kCarSteeringWheel = 0x32F0;  // scene node*
constexpr int kCarWindows = 0x45BC;        // begin, end of an array of pointers

// ---- game state -----------------------------------------------------------------------------
constexpr uintptr_t kGameFlow = 0x8E8410;  // GameFlow*
constexpr int kGameState = 0x458;          // 1 = menu, 2 = race
constexpr int kRaceState = 0x91C;          // 8 = racing

}  // namespace fo2
