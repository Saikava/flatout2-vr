// Probe build: hooks the race render path, logs what the engine does with cameras, frustums,
// viewports and render targets, and offers test modes (head sway, side-by-side stereo on the
// monitor, eyes rendered into offscreen targets, double render) to verify that the engine can
// be driven per eye.
#include "render.h"

#include "background.h"
#include "controllers.h"
#include "dxvk_interop.h"
#include "eyeshare.h"
#include "game.h"
#include "largeaddress.h"
#include "mirrorglass.h"
#include "pacing.h"
#include "profile.h"
#include "record.h"
#include "screenshot.h"
#include "util.h"
#include "xr.h"

#include <windows.h>
#include <d3d9.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The streak particles' trail builder (0x59F310) ends a trail that is shorter than its six
// segments by filling the rest with copies of its last vertex pair: one copy from the stack, then
// the vertex buffer copied onto itself. That reads the locked vertex buffer back, from memory that
// DXVK maps for writing and the processor reads very slowly: with a few hundred sparks in view it
// took 2-4 ms per eye. The copies are made from the stack instead.
static bool PatchTrailFill()
{
    static const uint8_t original[41] = {
        0xB8, 0x06, 0x00, 0x00, 0x00,  // mov eax, 6
        0x2B, 0xC5,                    // sub eax, ebp
        0x8D, 0x04, 0x40,              // lea eax, [eax+eax*2]
        0x8B, 0xFB,                    // mov edi, ebx
        0xC1, 0xE0, 0x04,              // shl eax, 4: bytes left (0x30 per segment)
        0xB9, 0x0C, 0x00, 0x00, 0x00,  // mov ecx, 12
        0x8D, 0x74, 0x24, 0x78,        // lea esi, [esp+0x78]: the vertex pair
        0xF3, 0xA5,                    // rep movsd
        0x8D, 0x48, 0xD0,              // lea ecx, [eax-0x30]
        0xC1, 0xE9, 0x02,              // shr ecx, 2
        0x8B, 0xF3,                    // mov esi, ebx
        0x8D, 0x7B, 0x30,              // lea edi, [ebx+0x30]
        0xF3, 0xA5,                    // rep movsd: the buffer onto itself
        0x03, 0xD8,                    // add ebx, eax
    };
    static const uint8_t fill[41] = {
        0xB8, 0x06, 0x00, 0x00, 0x00,  // mov eax, 6
        0x2B, 0xC5,                    // sub eax, ebp: segments left
        0x8B, 0xFB,                    // mov edi, ebx
        0x8D, 0x74, 0x24, 0x78,        // next: lea esi, [esp+0x78]
        0xB9, 0x0C, 0x00, 0x00, 0x00,  // mov ecx, 12
        0xF3, 0xA5,                    // rep movsd
        0x48,                          // dec eax
        0x75, 0xF2,                    // jnz next
        0x8B, 0xDF,                    // mov ebx, edi
        0xEB, 0x0E,                    // jmp over the rest
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    };
    return PatchCode(fo2::kTrailFill, original, fill, sizeof(fill));
}

// Levels of detail: the engine measures how far objects are from the camera times a factor
// (kCamLodFactor) to pick the model it draws (simpler ones farther away; a car's wheels go first),
// how far it draws small objects and where it fades them out. Separately it drops objects whose
// size on screen gets too small (kVisibilityHalf). Made for a monitor, these distances are short
// for the headset: [VR] LodDistance multiplies them, by dividing the constant the factor is made
// with and multiplying the screen-size one. 0 removes the limits: every object at its most
// detailed model, never faded or dropped, out to the far clipping plane (the track's fog).
static float g_lodConstant, g_visibilityHalf;

static bool PatchLodDistance(float scale)
{
    if (scale == 1.0f) return true;
    bool unlimited = scale <= 0.0f;
    if (unlimited) scale = 10000.0f;
    g_lodConstant = *(const float*)fo2::kLodConstant / scale;
    g_visibilityHalf = *(const float*)fo2::kVisibilityHalf * scale;
    const uint32_t original = (uint32_t)fo2::kLodConstant, ours = (uint32_t)(uintptr_t)&g_lodConstant;
    const uint32_t originalHalf = (uint32_t)fo2::kVisibilityHalf, oursHalf = (uint32_t)(uintptr_t)&g_visibilityHalf;
    bool ok = PatchCode(fo2::kLodConstantUse1, &original, &ours, 4);
    ok &= PatchCode(fo2::kLodConstantUse2, &original, &ours, 4);
    ok &= PatchCode(fo2::kVisibilityHalfUse, &originalHalf, &oursHalf, 4);
    if (ok && unlimited)
        Log("level of detail: full detail at every distance");
    else if (ok)
        Log("level of detail: distances x%.2f", scale);
    return ok;
}

namespace {

using RenderViewsFn = void(__stdcall*)(void* ctx, BYTE** cams, float* fovScale);
using SetViewportFn = void(__thiscall*)(void* self, int x, int y, int w, int h, int view);
using SetCameraFn = void(__thiscall*)(void* self, const float* m);
using SetFrustumFn = void(__thiscall*)(void* self, const float* fr);

const auto oRenderViews = (RenderViewsFn)fo2::kRenderViews;
const auto oSetViewport = (SetViewportFn)fo2::kSetViewport;
const auto oSetCamera = (SetCameraFn)fo2::kSetCamera;
const auto oSetFrustum = (SetFrustumFn)fo2::kSetFrustum;

// ---- IDirect3DDevice9 hooks (vtable) -------------------------------------------------------
namespace vt {
constexpr int Reset = 16, StretchRect = 34, SetRenderTarget = 37, SetDepthStencilSurface = 39,
              BeginScene = 41, EndScene = 42, Clear = 43, SetRenderState = 57, DrawPrimitive = 81, DrawIndexedPrimitive = 82,
              DrawPrimitiveUP = 83, DrawIndexedPrimitiveUP = 84;
}
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using StretchRectFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                          D3DTEXTUREFILTERTYPE);
using SetRenderTargetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthStencilSurfaceFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ClearFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetRenderStateFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using DrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                                     const void*, UINT);

ResetFn oReset;
StretchRectFn oStretchRect;
SetRenderTargetFn oSetRenderTarget;
SetDepthStencilSurfaceFn oSetDepthStencilSurface;
SceneFn oBeginScene, oEndScene;
ClearFn oClear;
SetRenderStateFn oSetRenderState;
DrawPrimitiveFn oDrawPrimitive;
DrawIndexedPrimitiveFn oDrawIndexedPrimitive;
DrawPrimitiveUPFn oDrawPrimitiveUP;
DrawIndexedPrimitiveUPFn oDrawIndexedPrimitiveUP;
IDirect3DDevice9* g_hookedDevice;

// ---- state ----------------------------------------------------------------------------------
UINT g_screenW, g_screenH;  // back buffer size

// VR: eye targets (multisampled render target + depth, resolved into a texture that is handed to
// OpenXR) and the screen texture for menus. All D3DPOOL_DEFAULT: released before Reset.
struct VrTargets
{
    IDirect3DSurface9* rt[2] = {};
    IDirect3DSurface9* ds[2] = {};
    IDirect3DTexture9* tex[2] = {};
    UINT w = 0, h = 0;
    UINT eyeW = 0, eyeH = 0;  // the headset's picture in them (w, h, unless they have room beside it: SteadyRoom)
    UINT room[4] = {};        // the left eye's extra pixels left, right, above and below it (the right eye's left for right)
    bool failed = false;
    IDirect3DTexture9* screen = nullptr;
    IDirect3DSurface9* screenResolve = nullptr;  // back buffer format, single sample
    UINT screenW = 0, screenH = 0;
} g_vrt;
bool g_screenFailLogged;

// While an eye is rendered: replaces the game's (widened) main frustums with the eye's, and its
// full-screen viewport with the eye target's.
struct VrEye
{
    bool active = false;
    int index = 0;         // 0 left, 1 right
    bool cockpit = false;  // seen from the driver's seat (CockpitEye)
    bool mirror = false;   // a side mirror's view (RenderMirrors), not an eye
    UINT w = 0, h = 0;     // the target's size
    float coverTan = 0;  // tan(fov/2) the camera was given so the engine's culling covers the eye
    xr::EyeFov fov = {};
    // The game camera's own view on the monitor: horizontal field of view (radians) and the tangents
    // of its half angles.
    float monitorFov = 0, monitorTanH = 0, monitorTanV = 0;
    // The monitor the lens flare is drawn on (see LensFlare): tangents of its half angles.
    float flareTanH = 0, flareTanV = 0;
} g_vrEye;
float g_sunShare = 1.0f;  // the cockpit: the sun's share seen past the car (MeasureSun)
float g_cockpitNear;      // and its last near clipping plane (CockpitNear)

// [VR] FloatDepth: the eyes keep depth reversed, 1 at the near clipping plane and 0 at the far one, in 32-bit floats
// (DXVK's d3d9.useD32forD24, set before Direct3D starts, see d3dcreate.cpp). The engine's 24-bit depth comes in steps
// of even size in 1 / distance, which crowd far out: with the cockpit's near plane some 0.13 m away they are 0.4 mm
// apart at 30 m and 2 mm at 70 m, and what lies that close there (a turn sign's arrow on its board) shows through
// what it covers in bands that crawl as the head moves. Floats, reversed, grow finer as the depth falls towards 0,
// as the steps of 1 / distance do: far out they part what lies a hair's breadth apart. While the eyes render
// (ReverseEyeDepth) the projections' depth is turned around (ReverseProjection), depth is cleared to 0 instead of 1
// (ClearHook) and the depth tests are turned around (SetRenderStateHook); the mod's own drawings take their depth
// from EyeDepth.
bool g_depthReversed;

DWORD ReversedDepthTest(DWORD test)
{
    switch (test)
    {
    case D3DCMP_LESS: return D3DCMP_GREATER;
    case D3DCMP_LESSEQUAL: return D3DCMP_GREATEREQUAL;
    case D3DCMP_GREATER: return D3DCMP_LESS;
    case D3DCMP_GREATEREQUAL: return D3DCMP_LESSEQUAL;
    default: return test;
    }
}

// The engine's projection (and the device's) with its depth turned around: z -> w - z. Done twice it is as it was.
void ReverseProjection(IDirect3DDevice9* dev)
{
    float* p = (float*)fo2::kProjMatrix;
    for (int r = 0; r < 4; r++) p[4 * r + 2] = p[4 * r + 3] - p[4 * r + 2];
    *(int*)fo2::kViewProjChanged = 1;
    dev->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)p);
}

void ReverseEyeDepth(IDirect3DDevice9* dev, bool on)
{
    if (!g_cfg.vrFloatDepth || g_depthReversed == on) return;
    g_depthReversed = on;
    // The depth test the device has now, turned around (the engine's state cache keeps it as it was asked for).
    DWORD test = D3DCMP_LESSEQUAL;
    if (SUCCEEDED(dev->GetRenderState(D3DRS_ZFUNC, &test)))
        oSetRenderState(dev, D3DRS_ZFUNC, ReversedDepthTest(test));
    else
    {
        static bool logged;
        if (!logged) Log("FloatDepth: the device's depth test unknown (GetRenderState failed)");
        logged = true;
    }
    ReverseProjection(dev);
}

// The depth an eye keeps for a point z ahead, between the near plane n and the far plane f.
float EyeDepth(float n, float f, float z)
{
    return g_depthReversed ? n * (f - z) / (z * (f - n)) : f / (f - n) * (1.0f - n / z);
}
// VR races: the game draws its HUD (everything after RenderViews until Present: HUD, pause menu,
// the HUD's own particles) into this target instead of the back buffer, over transparent black, so
// that it can go to the headset on its own panel and be laid over the monitor's picture. The colours
// end up premultiplied by alpha; the alpha channel is blended on its own ("over" for normal blending,
// left alone for additive blending, see SetRenderStateHook). The cockpit's instruments, when they are
// shown in the car, are drawn into targets of their own instead (see DrawHudBatch, HudCardOf).
struct HudTargets
{
    IDirect3DTexture9* tex = nullptr;    // back buffer size
    IDirect3DSurface9* rt = nullptr;     // its level 0
    IDirect3DSurface9* ds = nullptr;     // single-sample depth of the same size
    IDirect3DTexture9* cards[2] = {};    // the instruments, the same size: the rival's card in the second
    IDirect3DSurface9* cardRT[2] = {};   // (on the screen it overlaps the map)
    IDirect3DTexture9* panel = nullptr;  // smaller copy for the headset
    UINT w = 0, h = 0, panelW = 0, panelH = 0;
    bool failed = false;
    bool active = false;    // the game's drawing goes here now
    bool captured = false;  // this frame's HUD is in panel
    IDirect3DSurface9* gameRT = nullptr;  // the game's targets, put back at Present
    IDirect3DSurface9* gameDS = nullptr;
    DWORD gameColorWrite = 0xF;  // the game's D3DRS_COLORWRITEENABLE (the capture adds alpha)
} g_hud;
void EndHudCapture(IDirect3DDevice9* dev);
void ReleaseMirrorTargets();
void KnockMirror(char side, int state);
bool RouteHudCards();
bool HudCardsRouted();
int HudSpriteOwner(const void* stack, uintptr_t from, float x0, float y0, float x1, float y1);
void FadeTarget(IDirect3DDevice9* dev, IDirect3DSurface9* target, DWORD colour, bool hud);
bool g_eyesPending;  // this frame's eyes are rendered; they go to the headset at Present (with the HUD)
bool g_vrFrameDone;  // the VR frame of this game frame was already submitted
bool g_recenterKeyWasDown;

// A per-frame time, summed and its worst over a log interval.
struct Timing
{
    double sum = 0, max = 0;
    int n = 0;
    void Add(double t)
    {
        sum += t;
        max = std::max(max, t);
        n++;
    }
    double AvgMs() const { return n ? 1000.0 * sum / n : 0.0; }
    double MaxMs() const { return 1000.0 * max; }
};

struct VrStats
{
    int eyeFrames = 0, screenFrames = 0;
    double wait = 0, eyes = 0, submit = 0;
    // Where a race frame's time goes: the game's own work from the gate to RenderViews, our eyes,
    // the game's HUD drawing, handing the frame to the headset, the monitor's Present; and the
    // GPU's time for the eyes and for the whole frame (timestamps).
    Timing game, eyesT, hud, submitT, present, gpuEyes, gpuFrame;
} g_vrStats;
double g_renderViewsEnd;  // when this frame's RenderViews hook returned (0: none)
double g_frameGame, g_frameRenderViews, g_frameHud;  // this frame's phases (seconds), for the profiler

// GPU time of a VR frame: timestamps before the eyes, after them and before the frame goes to the
// headset, read a few frames later so that nothing waits for them. While the GPU is the bottleneck
// the spans are its work; otherwise they include waits for the CPU's commands.
struct GpuTimer
{
    static constexpr int kSlots = 4;
    struct Slot
    {
        IDirect3DQuery9* disjoint = nullptr;
        IDirect3DQuery9* freq = nullptr;
        IDirect3DQuery9* t[3] = {};
        bool pending = false;
    } slots[kSlots];
    int cur = 0;
    bool open = false, failed = false;

    void Release()
    {
        for (Slot& s : slots)
        {
            IDirect3DQuery9* all[5] = { s.disjoint, s.freq, s.t[0], s.t[1], s.t[2] };
            for (IDirect3DQuery9* q : all)
                if (q) q->Release();
            s = Slot();
        }
        open = false;
    }
    bool Create(IDirect3DDevice9* dev)
    {
        bool ok = true;
        for (Slot& s : slots)
        {
            ok &= SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint));
            ok &= SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq));
            for (auto& q : s.t) ok &= SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q));
        }
        if (!ok) Release();
        return ok;
    }
    void Collect(Slot& s)
    {
        s.pending = false;
        BOOL disjoint = TRUE;
        UINT64 f = 0, t[3] = {};
        if (s.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || s.freq->GetData(&f, sizeof(f), 0) != S_OK) return;
        for (int i = 0; i < 3; i++)
            if (s.t[i]->GetData(&t[i], sizeof(t[i]), 0) != S_OK) return;
        if (disjoint || f == 0) return;
        g_vrStats.gpuEyes.Add((double)(t[1] - t[0]) / (double)f);
        g_vrStats.gpuFrame.Add((double)(t[2] - t[0]) / (double)f);
    }
    void Begin(IDirect3DDevice9* dev)
    {
        if (failed) return;
        if (!slots[0].disjoint && !Create(dev))
        {
            failed = true;
            Log("GPU timer: no timestamp queries");
            return;
        }
        Slot& s = slots[cur];
        if (s.pending) Collect(s);
        s.disjoint->Issue(D3DISSUE_BEGIN);
        s.t[0]->Issue(D3DISSUE_END);
        open = true;
    }
    void EyesDone()
    {
        if (open) slots[cur].t[1]->Issue(D3DISSUE_END);
    }
    void End()
    {
        if (!open) return;
        Slot& s = slots[cur];
        s.t[2]->Issue(D3DISSUE_END);
        s.disjoint->Issue(D3DISSUE_END);
        s.freq->Issue(D3DISSUE_END);
        s.pending = true;
        open = false;
        cur = (cur + 1) % kSlots;
    }
} g_gpu;

struct Stats
{
    int frames = 0;
    int renderViews = 0;
    int setCamera = 0;
    int setFrustum = 0;
    int setViewport = 0;
    double rvTime = 0, rvTimeMax = 0;
    void Reset() { *this = Stats(); }
} g_stats;

bool g_inRenderViews;
double g_lastLog, g_lastShot;
int g_shotIndex;
bool g_shotKeyWasDown;
bool g_loggedTargets;
bool g_saveEyesThisFrame;
bool g_eyeShotPending;  // the eyes rendered this frame are saved at Present

// One-frame trace of render target traffic (the background mode's "trace" command, ReplayTrace).
enum class Trace { Off, Armed, Running, Done } g_trace = Trace::Off;
int g_traceLines;

// Unique viewport / frustum calls seen inside one RenderViews, logged once.
struct VpCall { int x, y, w, h, view; };
VpCall g_vpSeen[16];
int g_vpSeenCount;
const float* g_frSeen[16];
float g_frSeenVal[16][7];
int g_frSeenCount;
bool g_seenLogged;

bool InRace()
{
    BYTE* gf = *(BYTE**)fo2::kGameFlow;
    return gf && *(int*)(gf + fo2::kGameState) == 2;
}

int RaceState()
{
    BYTE* gf = *(BYTE**)fo2::kGameFlow;
    return gf ? *(int*)(gf + fo2::kRaceState) : -1;
}

// ---- logging helpers ------------------------------------------------------------------------

std::string Describe(IDirect3DSurface9* s)
{
    if (!s) return "null";
    D3DSURFACE_DESC d;
    s->GetDesc(&d);
    char buf[160];
    IDirect3DSurface9* bb = nullptr;
    bool isBB = g_hookedDevice && SUCCEEDED(g_hookedDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb == s;
    if (bb) bb->Release();
    const char* tag = isBB ? " BACKBUFFER" : s == g_vrt.rt[0] || s == g_vrt.rt[1] ? " EYE" : "";
    snprintf(buf, sizeof(buf), "%p %ux%u fmt %d msaa %d usage %X%s", (void*)s, d.Width, d.Height, (int)d.Format,
             (int)d.MultiSampleType, (unsigned)d.Usage, tag);
    return buf;
}

// What the game does in an eye besides drawing the scene into it: screen sprites (placed in the
// back buffer's 2D space, like the sun's glow), render target switches and copies between surfaces
// (render to texture, like the bloom copy the stadium screens show). Such things may come out
// different in each eye. Each new place in the game's code is logged once (at most 256), with the
// three nearest game return addresses.
// True for a place not logged yet; ret gets the game code that led here.
bool NewEyeCall(const char* what, const void* stack, unsigned ret[3])
{
    if (!g_vrEye.active) return false;
    prof::GameCallers(stack, ret);
    static unsigned seen[256];
    static int count;
    unsigned key = ret[0] * 31u + ret[1] * 7u + (unsigned)(uintptr_t)what;
    for (int i = 0; i < count; i++)
        if (seen[i] == key) return false;
    if (count == 256) return false;
    seen[count++] = key;
    return true;
}

void LogEyeCall(const char* what, const unsigned ret[3], const std::string& detail = std::string())
{
    Log("eyes: %s from %08X < %08X < %08X%s%s", what, ret[0], ret[1], ret[2], detail.empty() ? "" : ": ", detail.c_str());
}

using SpriteFn = void(__thiscall*)(void* self, const void* data);
const auto oSpriteRect = (SpriteFn)fo2::kSpriteRect;
const auto oSpriteQuad = (SpriteFn)fo2::kSpriteQuad;

// The game darkens the whole screen with the HUD (when the car is reset, behind a dialog; see game.h):
// in VR that would land on the HUD panel alone, a dark sheet floating in front of the player. Their
// colours are taken instead (alpha in the top byte; 0 for none this frame) and laid over everything
// at Present (FadeTarget): both eyes, the monitor's picture and, for those that go over the HUD, the
// HUD itself. g_fade[0]: over the HUD; g_fade[1]: over the picture alone.
DWORD g_fade[2];

// a over b, colours with alpha in the top byte.
DWORD FadeOver(DWORD a, DWORD b)
{
    float aa = (a >> 24) / 255.0f, ba = (b >> 24) / 255.0f, oa = aa + ba * (1.0f - aa);
    if (oa <= 0.0f) return 0;
    DWORD out = (DWORD)lroundf(oa * 255.0f) << 24;
    for (int shift = 0; shift < 24; shift += 8)
    {
        float c = (((a >> shift) & 0xFF) * aa + ((b >> shift) & 0xFF) * ba * (1.0f - aa)) / oa;
        out |= (DWORD)lroundf(c) << shift;
    }
    return out;
}

// While on, screen sprites are moved from one picture of the back buffer's pixel space into another
// (see LensFlare): x' = x * scale[0] + offset[0], y' = y * scale[1] + offset[1]; and their colours
// (alpha too) are dimmed by `fade`.
struct SpriteMap
{
    bool on = false;
    float scale[2] = { 1, 1 }, offset[2] = {};
    float fade = 1;
} g_spriteMap;

DWORD FadeColour(DWORD c, float f)
{
    if (f >= 1.0f) return c;
    DWORD out = 0;
    for (int shift = 0; shift < 32; shift += 8) out |= (DWORD)lroundf(((c >> shift) & 0xFF) * f) << shift;
    return out;
}

// A box in the back buffer's pixels.
struct ScreenBox
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int count = 0;
    void Add(float x, float y) { x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y); }
};

// The screen sprites in the game's queue while the HUD is captured (see game.h, kSpriteQueueCount), in
// its order: each one's box and, when the game code that queues it tells, the instrument it belongs to
// (HudSpriteOwner; kNoOwner: where the batch's other sprites with none lie, see DrawHudBatch).
constexpr int kNoOwner = -1, kOwnerPanel = -2;
struct QueuedSprite
{
    float x0, y0, x1, y1;  // x0 > x1: not known (queued before the capture)
    int owner;
};
struct SpriteQueue
{
    std::vector<QueuedSprite> sprites;
    bool drawn = false;  // the batch is drawn (once for each pass): the next sprite starts the next one

    void Add(const void* stack, uintptr_t from, float x0, float y0, float x1, float y1)
    {
        // Called after the game queued it (a full queue is drawn first): the queue holds it and those before.
        size_t queued = (size_t)std::max(1, *(const int*)fo2::kSpriteQueueCount);
        if (drawn || sprites.size() + 1 != queued) sprites.assign(queued - 1, QueuedSprite{ 1, 0, 0, 0, kNoOwner });
        drawn = false;
        sprites.push_back({ x0, y0, x1, y1, HudCardsRouted() ? HudSpriteOwner(stack, from, x0, y0, x1, y1) : kNoOwner });
    }
} g_hudQueue;

void __fastcall SpriteRectHook(void* self, void*, const void* rect)
{
    unsigned ret[3];
    if (NewEyeCall("screen sprite", _AddressOfReturnAddress(), ret))
    {
        const float* r = (const float*)rect;
        char buf[96];
        snprintf(buf, sizeof(buf), "%.0f %.0f %.0f %.0f", r[0], r[1], r[2], r[3]);
        LogEyeCall("screen sprite", ret, buf);
    }
    if (g_eyesPending)
    {
        uintptr_t from = (uintptr_t)_ReturnAddress();
        for (const auto& f : fo2::kFadeSprites)
            if (from == f.ret)
            {
                DWORD& fade = g_fade[f.overHud ? 0 : 1];
                fade = FadeOver(((const DWORD*)rect)[8], fade);
                if (g_hud.active) return;
            }
    }
    if (g_spriteMap.on)
    {
        float r[9];  // x0, y0, x1, y1, u0, v0, u1, v1, colour
        memcpy(r, rect, sizeof(r));
        for (int i = 0; i < 4; i++) r[i] = r[i] * g_spriteMap.scale[i & 1] + g_spriteMap.offset[i & 1];
        DWORD& colour = *(DWORD*)&r[8];
        colour = FadeColour(colour, g_spriteMap.fade);
        oSpriteRect(self, r);
    }
    else
        oSpriteRect(self, rect);
    if (g_hud.active)
    {
        const float* r = (const float*)rect;
        g_hudQueue.Add(_AddressOfReturnAddress(), (uintptr_t)_ReturnAddress(), std::min(r[0], r[2]), std::min(r[1], r[3]),
                       std::max(r[0], r[2]), std::max(r[1], r[3]));
    }
}

void __fastcall SpriteQuadHook(void* self, void*, const void* vertices)
{
    unsigned ret[3];
    if (NewEyeCall("screen sprite quad", _AddressOfReturnAddress(), ret)) LogEyeCall("screen sprite quad", ret);
    if (g_spriteMap.on)
    {
        float v[4][6];  // x, y, z, colour, u, v
        memcpy(v, vertices, sizeof(v));
        for (auto& p : v)
        {
            for (int i = 0; i < 2; i++) p[i] = p[i] * g_spriteMap.scale[i] + g_spriteMap.offset[i];
            DWORD& colour = *(DWORD*)&p[3];
            colour = FadeColour(colour, g_spriteMap.fade);
        }
        oSpriteQuad(self, v);
    }
    else
        oSpriteQuad(self, vertices);
    if (g_hud.active)
    {
        const float* v = (const float*)vertices;  // x, y, z, colour, u, v
        ScreenBox b;
        for (int i = 0; i < 4; i++) b.Add(v[6 * i], v[6 * i + 1]);
        g_hudQueue.Add(_AddressOfReturnAddress(), (uintptr_t)_ReturnAddress(), b.x0, b.y0, b.x1, b.y1);
    }
}

void TraceLine(const char* fmt, ...)
{
    if (g_trace != Trace::Running || g_traceLines >= 2000) return;
    g_traceLines++;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("  trace%s %s", g_inRenderViews ? " [RV]" : "     ", buf);
}

void LogRenderTargets(IDirect3DDevice9* dev)
{
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    Log("at RenderViews entry: render target %s", Describe(rt).c_str());
    Log("at RenderViews entry: depth stencil %s", Describe(ds).c_str());
    ID3D9VkInteropDevice* dxvk = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&dxvk)))
    {
        Log("device runs on DXVK (Vulkan)");
        dxvk->Release();
    }
    else
        Log("device is regular Direct3D 9");
    if (rt) rt->Release();
    if (ds) ds->Release();
}

void LogCamera(BYTE* ctx, BYTE** cams, const float* fovScale)
{
    int views = *(int*)(ctx + fo2::kCtxViewCount);
    BYTE* cam = views > 0 ? cams[0] : nullptr;
    Log("race state %d, views %d, camera %p, fovScale %.3f", RaceState(), views, (void*)cam, fovScale ? fovScale[0] : -1.0f);
    Log("  grass %s, drawn to %.0f m, fading out from %.0f m", *(int*)(ctx + fo2::kCtxPlantsOn) ? "on" : "off",
        *(float*)(ctx + fo2::kCtxPlantFar), *(float*)(ctx + fo2::kCtxPlantFade));
    if (!cam) return;
    Log("  camera near %.3f far %.1f hfov %.2f deg aspect %.4f", *(float*)(cam + fo2::kCamNear), *(float*)(cam + fo2::kCamFar),
        *(float*)(cam + fo2::kCamFov) * 57.29578f, *(float*)(cam + fo2::kCamAspect));
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    if (car)
    {
        // The camera's place in the car's axes.
        const float* c = (const float*)(car + fo2::kCarMatrix);
        const float* m = (const float*)(cam + fo2::kCamMatrix);
        float d[3] = { m[12] - c[12], m[13] - c[13], m[14] - c[14] }, p[3];
        for (int i = 0; i < 3; i++) p[i] = d[0] * c[4 * i] + d[1] * c[4 * i + 1] + d[2] * c[4 * i + 2];
        Log("  camera in the player's car at %.3f %.3f %.3f (right, up, forward), car view mode %d", p[0], p[1], p[2],
            *(const int*)(car + fo2::kCarViewMode));
        if (g_vrEye.cockpit) Log("  cockpit: the sun's share past the car %.2f, near clipping plane %.3f m", g_sunShare, g_cockpitNear);
    }
    const float* center = (const float*)fo2::kRadialBlurCenter;
    Log("  post: colour filter %d, speed blur %d passes from %.2f %.2f, strength %.2f x speed %.2f (in the eyes x %.2f, SpeedBlur)",
        *(const int*)fo2::kColorFilterOn, *(const int*)fo2::kRadialBlurPasses, center[0], center[1], *(const float*)fo2::kRadialBlurStrength,
        *(const float*)fo2::kRadialBlurSpeed, g_cfg.vrSpeedBlur);
}

// ---- engine hooks ---------------------------------------------------------------------------

void __fastcall SetViewportHook(void* self, void*, int x, int y, int w, int h, int view)
{
    g_stats.setViewport++;
    if (g_inRenderViews && !g_seenLogged && g_vpSeenCount < 16)
    {
        bool known = false;
        for (int i = 0; i < g_vpSeenCount; i++)
        {
            const VpCall& v = g_vpSeen[i];
            known |= v.x == x && v.y == y && v.w == w && v.h == h && v.view == view;
        }
        if (!known) g_vpSeen[g_vpSeenCount++] = { x, y, w, h, view };
    }
    TraceLine("engine SetViewport %d %d %d %d view %d", x, y, w, h, view);
    // Only the full-screen viewport belongs to the player view; smaller ones are helper passes.
    bool fullScreen = x == 0 && y == 0 && (UINT)w == g_screenW && (UINT)h == g_screenH;
    if (g_vrEye.active && fullScreen)
    {
        x = y = 0;
        w = (int)g_vrEye.w;
        h = (int)g_vrEye.h;
    }
    oSetViewport(self, x, y, w, h, view);
}

void __fastcall SetCameraHook(void* self, void*, const float* m)
{
    g_stats.setCamera++;
    TraceLine("engine SetCamera %p%s from %p right %.2f %.2f %.2f fwd %.2f %.2f %.2f pos %.2f %.2f %.2f", (const void*)m,
              g_vrEye.active ? " [eye]" : "", _ReturnAddress(), m[0], m[1], m[2], m[8], m[9], m[10], m[12], m[13], m[14]);
    oSetCamera(self, m);
}


void __fastcall SetFrustumHook(void* self, void*, const float* fr)
{
    g_stats.setFrustum++;
    if (g_inRenderViews && !g_seenLogged && g_frSeenCount < 16)
    {
        bool known = false;
        for (int i = 0; i < g_frSeenCount; i++) known |= g_frSeen[i] == fr;
        if (!known)
        {
            g_frSeen[g_frSeenCount] = fr;
            memcpy(g_frSeenVal[g_frSeenCount++], fr, 7 * sizeof(float));
        }
    }
    TraceLine("engine SetFrustum %p l %.4f r %.4f b %.4f t %.4f n %.3f f %.1f", (const void*)fr, fr[0], fr[1], fr[2], fr[3], fr[4], fr[5]);
    if (g_vrEye.active && *(const int*)(fr + 6) == 0 && fr[4] > 0.0f)
    {
        // The camera's frustum and the sky's are built from the camera's field of view; helper
        // passes (car shadows, sun visibility) have their own.
        float n = fr[4];
        bool symmetric = fabsf(fr[0] + fr[1]) <= 1e-4f * fabsf(fr[1]) && fabsf(fr[2] + fr[3]) <= 1e-4f * fabsf(fr[3]);
        if (symmetric && fabsf(fr[1] / n - g_vrEye.coverTan) <= 1e-3f * g_vrEye.coverTan)
        {
            float f[7];
            memcpy(f, fr, sizeof(f));
            f[0] = n * g_vrEye.fov.left;
            f[1] = n * g_vrEye.fov.right;
            f[2] = n * g_vrEye.fov.down;
            f[3] = n * g_vrEye.fov.up;
            oSetFrustum(self, f);
            if (g_depthReversed) ReverseProjection(*(IDirect3DDevice9**)fo2::kD3DDevice);
            return;
        }
    }
    oSetFrustum(self, fr);
    if (g_depthReversed) ReverseProjection(*(IDirect3DDevice9**)fo2::kD3DDevice);
}

// The sun's glow (the moon's on night tracks) is placed with the camera's frustum, which in an eye is
// the widened symmetric one: the glow would sit left of the sun in the left eye, right of it in the
// right eye and lower in both, two glows that do not fuse and shimmer. While an eye renders, the glow
// gets the eye's frustum and horizontal field of view (its size follows that). It is a screen sprite,
// drawn in the back buffer's pixel space (see game.h), so it is also given the back buffer's size as
// the viewport and screen size; the screen shape stays the eye's, which keeps the sprite round.
static uintptr_t g_sunGlowFn = fo2::kSunGlow;

// (camera, flare)
static __declspec(naked) void __stdcall CallSunGlow(BYTE*, void*)
{
    __asm {
        push esi
        push ebx
        mov esi, [esp + 12]
        mov ebx, [esp + 16]
        call dword ptr [g_sunGlowFn]
        pop ebx
        pop esi
        ret 8
    }
}

static void __stdcall SunGlow(BYTE* cam, void* flare)
{
    if (!g_vrEye.active)
    {
        CallSunGlow(cam, flare);
        return;
    }
    float* fr = (float*)(cam + fo2::kCamFrustum);
    float* fov = (float*)(cam + fo2::kCamFov);
    float saved[4] = { fr[0], fr[1], fr[2], fr[3] }, savedFov = *fov;
    float n = fr[4];
    fr[0] = n * g_vrEye.fov.left;
    fr[1] = n * g_vrEye.fov.right;
    fr[2] = n * g_vrEye.fov.down;
    fr[3] = n * g_vrEye.fov.up;
    *fov = atanf(g_vrEye.fov.right) - atanf(g_vrEye.fov.left);
    int* vp = (int*)fo2::kViewportX;
    int* size = (int*)(*(BYTE**)fo2::kDeviceD3d + fo2::kDevWidth);
    int savedVp[4] = { vp[0], vp[1], vp[2], vp[3] }, savedSize[2] = { size[0], size[1] };
    vp[0] = vp[1] = 0;
    vp[2] = size[0] = (int)g_screenW;
    vp[3] = size[1] = (int)g_screenH;
    CallSunGlow(cam, flare);
    memcpy(fr, saved, sizeof(saved));
    *fov = savedFov;
    memcpy(vp, savedVp, sizeof(savedVp));
    size[0] = savedSize[0];
    size[1] = savedSize[1];
}

static __declspec(naked) void SunGlowHook()
{
    __asm {
        push ebx
        push esi
        call SunGlow
        ret
    }
}

// The lens flare (RenderViews calls it per view after the scene, the camera in eax, the flare in edi)
// lays its ghosts along the line from the sun through the middle of the picture. The middle of an
// eye's picture looks some degrees to the side (the eyes' fields of view are not symmetric), a
// different direction in each eye: the ghosts would land in different places and not fuse. So it is
// drawn as on a monitor whose middle is straight ahead of the head, and its sprites are moved into the
// eye by direction (g_spriteMap): both eyes show the same flare, as far away as the sun. That monitor
// is the back buffer's shape and tall enough to cover the eyes (g_vrEye.flareTanH/V, the tangents of
// its half angles); the field of view the flare is given makes its pieces the angular size they have
// on the game's own monitor.
static uintptr_t g_lensFlareFn = fo2::kLensFlare;

// (camera, flare)
static __declspec(naked) void __stdcall CallLensFlare(BYTE*, void*)
{
    __asm {
        push edi
        mov eax, [esp + 8]
        mov edi, [esp + 12]
        call dword ptr [g_lensFlareFn]
        pop edi
        ret 8
    }
}

// In the cockpit the flare would shine through the roof: the sun's own test leaves the cars out. While
// the left eye draws its flare, two occlusion queries count the samples of a small square at the sun,
// at the far end of the depth range: those the eye's picture lets through, and all of them. The flare
// is dimmed by their share (both eyes alike, a frame or two late, as the queries come back).
struct SunQuery
{
    IDirect3DQuery9* q[2] = {};  // through the picture, all
    bool issued = false;
};
SunQuery g_sunQueries[3];
bool g_sunLogged;

void ReleaseSunQueries()
{
    for (SunQuery& s : g_sunQueries)
    {
        for (IDirect3DQuery9*& q : s.q)
        {
            if (q) q->Release();
            q = nullptr;
        }
        s.issued = false;
    }
}

void DrawSunSquare(IDirect3DDevice9* dev, float x, float y, float half, D3DCMPFUNC test)
{
    struct V { float x, y, z, rhw; };
    const float z = g_depthReversed ? 0.00001f : 0.99999f;
    const V quad[4] = { { x - half, y - half, z, 1 }, { x + half, y - half, z, 1 }, { x - half, y + half, z, 1 }, { x + half, y + half, z, 1 } };
    dev->SetRenderState(D3DRS_ZFUNC, test);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
}

void MeasureSun(BYTE* cam, const void* flare)
{
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)fo2::kD3DDevice;
    for (SunQuery& s : g_sunQueries)
    {
        DWORD n[2];
        if (!s.issued || s.q[0]->GetData(&n[0], sizeof(DWORD), 0) != S_OK || s.q[1]->GetData(&n[1], sizeof(DWORD), 0) != S_OK)
            continue;
        s.issued = false;
        if (n[1] > 0) g_sunShare = std::min(1.0f, (float)n[0] / (float)n[1]);
        if (!g_sunLogged)
        {
            g_sunLogged = true;
            Log("cockpit: sun query %u of %u samples through", (unsigned)n[0], (unsigned)n[1]);
        }
    }
    SunQuery* free = nullptr;
    for (SunQuery& s : g_sunQueries)
        if (!s.issued) free = &s;
    if (!free) return;
    if (!free->q[0] && (FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &free->q[0])) || FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &free->q[1]))))
    {
        ReleaseSunQueries();
        return;
    }
    // The sun in the eye's picture.
    const float* m = (const float*)(cam + fo2::kCamMatrix);
    const float* s = (const float*)((const BYTE*)flare + fo2::kFlareSun);
    float x = s[0] * m[0] + s[1] * m[1] + s[2] * m[2];
    float y = s[0] * m[4] + s[1] * m[5] + s[2] * m[6];
    float z = s[0] * m[8] + s[1] * m[9] + s[2] * m[10];
    if (z <= 0.0f) return;
    const xr::EyeFov& f = g_vrEye.fov;
    float W = (float)g_vrt.w, H = (float)g_vrt.h;
    float px = (x / z - f.left) / (f.right - f.left) * W;
    float py = (f.up - y / z) / (f.up - f.down) * H;
    float half = 0.01f / (f.right - f.left) * W;  // some 0.6 degrees, a little more than the sun
    if (px < -half || px > W + half || py < -half || py > H + half) return;

    // The eye's own target and depth (the flare may come after the post-processing, which leaves the
    // depth unbound), put back afterwards; the state block keeps the rest.
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    dev->SetRenderTarget(0, g_vrt.rt[0]);
    dev->SetDepthStencilSurface(g_vrt.ds[0]);
    D3DVIEWPORT9 vp = { 0, 0, g_vrt.w, g_vrt.h, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    free->q[0]->Issue(D3DISSUE_BEGIN);
    DrawSunSquare(dev, px, py, half, D3DCMP_LESSEQUAL);
    free->q[0]->Issue(D3DISSUE_END);
    free->q[1]->Issue(D3DISSUE_BEGIN);
    DrawSunSquare(dev, px, py, half, D3DCMP_ALWAYS);
    free->q[1]->Issue(D3DISSUE_END);
    free->issued = true;
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    saved->Apply();
    saved->Release();
}

static void __stdcall LensFlare(BYTE* cam, void* flare)
{
    if (!g_vrEye.active)
    {
        CallLensFlare(cam, flare);
        return;
    }
    if (g_vrEye.mirror) return;  // none in the side mirrors
    if (g_vrEye.cockpit && g_vrEye.index == 0) MeasureSun(cam, flare);
    g_spriteMap.fade = g_vrEye.cockpit ? g_sunShare : 1.0f;
    float tanH = g_vrEye.flareTanH, tanV = g_vrEye.flareTanV;
    float* fr = (float*)(cam + fo2::kCamFrustum);
    float* fov = (float*)(cam + fo2::kCamFov);
    float* shape = (float*)fo2::kScreenShape;
    int* vp = (int*)fo2::kViewportX;
    int* size = (int*)(*(BYTE**)fo2::kDeviceD3d + fo2::kDevWidth);
    float savedFr[4] = { fr[0], fr[1], fr[2], fr[3] }, savedFov = *fov, savedShape[2] = { shape[0], shape[1] };
    int savedVp[4] = { vp[0], vp[1], vp[2], vp[3] }, savedSize[2] = { size[0], size[1] };
    float n = fr[4], W = (float)g_screenW, H = (float)g_screenH;
    fr[0] = -n * tanH;
    fr[1] = n * tanH;
    fr[2] = -n * tanV;
    fr[3] = n * tanV;
    *fov = g_vrEye.monitorFov * tanH / g_vrEye.monitorTanH;
    shape[0] = W;
    shape[1] = H;
    vp[0] = vp[1] = 0;
    vp[2] = size[0] = (int)g_screenW;
    vp[3] = size[1] = (int)g_screenH;
    // A point of that monitor at x (tangent (2x / W - 1) * tanH) is at the same tangent in the eye's
    // picture, which spans fov.left to fov.right over the back buffer's width; y alike, downwards.
    const xr::EyeFov& f = g_vrEye.fov;
    g_spriteMap.scale[0] = 2.0f * tanH / (f.right - f.left);
    g_spriteMap.offset[0] = W * (-tanH - f.left) / (f.right - f.left);
    g_spriteMap.scale[1] = 2.0f * tanV / (f.up - f.down);
    g_spriteMap.offset[1] = H * (f.up - tanV) / (f.up - f.down);
    g_spriteMap.on = true;
    CallLensFlare(cam, flare);
    g_spriteMap.on = false;
    memcpy(fr, savedFr, sizeof(savedFr));
    *fov = savedFov;
    shape[0] = savedShape[0];
    shape[1] = savedShape[1];
    memcpy(vp, savedVp, sizeof(savedVp));
    size[0] = savedSize[0];
    size[1] = savedSize[1];
}

static __declspec(naked) void LensFlareHook()
{
    __asm {
        push edi
        push eax
        call LensFlare
        ret
    }
}

// The pause menu's retry question wraps at the menus' 540 units, scaled to the screen like the text
// (see game.h). width: the back buffer's.
static void SetRetryTextWidth(UINT width)
{
    static float current = 540.0f;
    float wrap = 540.0f * (float)width / 640.0f;
    if (wrap != current && PatchCode(fo2::kRetryTextWidth, &current, &wrap, 4)) current = wrap;
}

// ---- device hooks ---------------------------------------------------------------------------

void ReleaseHudTargets()
{
    IUnknown* all[10] = { g_hud.tex, g_hud.rt, g_hud.ds, g_hud.cards[0], g_hud.cardRT[0], g_hud.cards[1], g_hud.cardRT[1], g_hud.panel,
                          g_hud.gameRT, g_hud.gameDS };
    for (IUnknown* r : all)
        if (r) r->Release();
    g_hud = HudTargets();
}

// The HUD panel's picture for a back buffer of w x h: the panel covers about 60 degrees of view, and 1920 pixels across
// are sharper than the headset.
void HudPanelSize(UINT w, UINT h, UINT* pw, UINT* ph)
{
    *pw = std::min(w, 1920u);
    *ph = (UINT)((unsigned long long)h * *pw / w);
}

bool EnsureHudTargets(IDirect3DDevice9* dev)
{
    if (g_hud.tex && g_hud.w == g_screenW && g_hud.h == g_screenH) return true;
    if (g_hud.failed || g_screenW == 0) return false;
    ReleaseHudTargets();
    UINT w = g_screenW, h = g_screenH, pw, ph;
    HudPanelSize(w, h, &pw, &ph);
    bool ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hud.tex, nullptr)) &&
              SUCCEEDED(g_hud.tex->GetSurfaceLevel(0, &g_hud.rt)) &&
              SUCCEEDED(dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_hud.ds, nullptr)) &&
              SUCCEEDED(dev->CreateTexture(pw, ph, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hud.panel, nullptr));
    for (int i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hud.cards[i], nullptr)) &&
             SUCCEEDED(g_hud.cards[i]->GetSurfaceLevel(0, &g_hud.cardRT[i]));
    Log("VR HUD targets %ux%u, panel %ux%u: %s", w, h, pw, ph, ok ? "created" : "FAILED");
    if (ok) PrepareScreenshots(dev, pw, ph, D3DFMT_A8R8G8B8, 1);
    if (!ok)
    {
        ReleaseHudTargets();
        g_hud.failed = true;
        return false;
    }
    g_hud.w = w;
    g_hud.h = h;
    g_hud.panelW = pw;
    g_hud.panelH = ph;
    return true;
}

// Alpha of the HUD target: "over" (a + d * (1 - a)) for normal blending, unchanged for additive
// blending, whose colour only adds light.
void SetHudAlphaBlend(IDirect3DDevice9* dev, DWORD destBlend)
{
    bool additive = destBlend == D3DBLEND_ONE;
    oSetRenderState(dev, D3DRS_SRCBLENDALPHA, additive ? D3DBLEND_ZERO : D3DBLEND_ONE);
    oSetRenderState(dev, D3DRS_DESTBLENDALPHA, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
}

void BeginHudCapture(IDirect3DDevice9* dev)
{
    if (g_hud.active || !EnsureHudTargets(dev)) return;
    dev->GetRenderTarget(0, &g_hud.gameRT);
    dev->GetDepthStencilSurface(&g_hud.gameDS);
    DWORD destBlend = D3DBLEND_ZERO, colorWrite = 0xF;
    dev->GetRenderState(D3DRS_DESTBLEND, &destBlend);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    g_hud.gameColorWrite = colorWrite;
    if (RouteHudCards())
        for (IDirect3DSurface9* card : g_hud.cardRT)
        {
            oSetRenderTarget(dev, 0, card);
            oClear(dev, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        }
    oSetRenderTarget(dev, 0, g_hud.rt);
    oSetDepthStencilSurface(dev, g_hud.ds);
    oClear(dev, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    oSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    oSetRenderState(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    SetHudAlphaBlend(dev, destBlend);
    oSetRenderState(dev, D3DRS_COLORWRITEENABLE, colorWrite | D3DCOLORWRITEENABLE_ALPHA);
    g_hudQueue = SpriteQueue();
    g_hud.active = true;
}

HRESULT __stdcall SetRenderStateHook(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value)
{
    if (g_hud.active)
    {
        switch (state)
        {
        case D3DRS_COLORWRITEENABLE:
            g_hud.gameColorWrite = value;
            value |= D3DCOLORWRITEENABLE_ALPHA;
            break;
        case D3DRS_DESTBLEND:
            SetHudAlphaBlend(dev, value);
            break;
        case D3DRS_SEPARATEALPHABLENDENABLE:
        case D3DRS_SRCBLENDALPHA:
        case D3DRS_DESTBLENDALPHA:
        case D3DRS_BLENDOPALPHA:
            return D3D_OK;  // the capture's own while it runs
        default:
            break;
        }
    }
    if (state == D3DRS_ZFUNC && g_depthReversed) value = ReversedDepthTest(value);
    return oSetRenderState(dev, state, value);
}

// The stadium screens show the renderer's first screen texture, which its post-processing fills with
// a reduced copy of the view it has just rendered (see game.h). Rendered per eye, the left eye's
// screens would show the right eye's view of the frame before and the right eye's the left eye's of
// this frame: two pictures framed differently that do not fuse. The texture is saved as the left eye
// finds it and put back before the right eye renders, so both eyes show the same picture.
IDirect3DTexture9* g_screenTexCopy;

void CopyGameScreenTexture(IDirect3DDevice9* dev, bool save)
{
    BYTE* engine = *(BYTE**)fo2::kDeviceD3d;
    BYTE* wrapper = engine ? *(BYTE**)(engine + fo2::kDevScreenTex) : nullptr;
    IDirect3DTexture9* game = wrapper ? *(IDirect3DTexture9**)(wrapper + fo2::kTexD3D) : nullptr;
    D3DSURFACE_DESC d;
    if (!game || FAILED(game->GetLevelDesc(0, &d))) return;
    if (g_screenTexCopy)
    {
        D3DSURFACE_DESC c;
        g_screenTexCopy->GetLevelDesc(0, &c);
        if (c.Width != d.Width || c.Height != d.Height || c.Format != d.Format)
        {
            g_screenTexCopy->Release();
            g_screenTexCopy = nullptr;
        }
    }
    if (!g_screenTexCopy &&
        (!save || FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_screenTexCopy, nullptr))))
        return;
    IDirect3DSurface9 *gameSurface = nullptr, *copySurface = nullptr;
    if (SUCCEEDED(game->GetSurfaceLevel(0, &gameSurface)) && SUCCEEDED(g_screenTexCopy->GetSurfaceLevel(0, &copySurface)))
    {
        if (save)
            oStretchRect(dev, gameSurface, nullptr, copySurface, nullptr, D3DTEXF_NONE);
        else
            oStretchRect(dev, copySurface, nullptr, gameSurface, nullptr, D3DTEXF_NONE);
    }
    if (gameSurface) gameSurface->Release();
    if (copySurface) copySurface->Release();
}

// The colour filter works on two render textures the size of the back buffer (see game.h): an eye's
// picture (3072x3264) went through 3840x2160 and back and lost a third of its rows. While the eyes
// render, the filter's texture wrappers hold textures of the eye's size instead.
struct FilterTextures
{
    IDirect3DTexture9* tex[2] = {};
    BYTE* wrapper[2] = {};
    int savedSize[2][2] = {};
    IDirect3DTexture9* savedTex[2] = {};
    bool swapped = false;
} g_filter;

void UseEyeFilterTextures(IDirect3DDevice9* dev)
{
    BYTE* engine = *(BYTE**)fo2::kDeviceD3d;
    BYTE* post = engine ? *(BYTE**)(engine + fo2::kDevPost) : nullptr;
    if (!post) return;
    BYTE** wrappers = (BYTE**)(post + fo2::kPostFilterTex);
    for (int i = 0; i < 2; i++)
    {
        IDirect3DTexture9* game = wrappers[i] ? *(IDirect3DTexture9**)(wrappers[i] + fo2::kTexD3D) : nullptr;
        D3DSURFACE_DESC d;
        if (!game || FAILED(game->GetLevelDesc(0, &d))) return;
        if (!g_filter.tex[i] &&
            FAILED(dev->CreateTexture(g_vrt.w, g_vrt.h, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_filter.tex[i], nullptr)))
            return;
    }
    for (int i = 0; i < 2; i++)
    {
        BYTE* w = g_filter.wrapper[i] = wrappers[i];
        g_filter.savedSize[i][0] = *(int*)(w + fo2::kTexWidth);
        g_filter.savedSize[i][1] = *(int*)(w + fo2::kTexHeight);
        g_filter.savedTex[i] = *(IDirect3DTexture9**)(w + fo2::kTexD3D);
        *(int*)(w + fo2::kTexWidth) = (int)g_vrt.w;
        *(int*)(w + fo2::kTexHeight) = (int)g_vrt.h;
        *(IDirect3DTexture9**)(w + fo2::kTexD3D) = g_filter.tex[i];
    }
    g_filter.swapped = true;
}

void RestoreFilterTextures()
{
    if (!g_filter.swapped) return;
    for (int i = 0; i < 2; i++)
    {
        BYTE* w = g_filter.wrapper[i];
        *(int*)(w + fo2::kTexWidth) = g_filter.savedSize[i][0];
        *(int*)(w + fo2::kTexHeight) = g_filter.savedSize[i][1];
        *(IDirect3DTexture9**)(w + fo2::kTexD3D) = g_filter.savedTex[i];
    }
    g_filter.swapped = false;
}

void ReleaseVrTargets()
{
    ReleaseSunQueries();
    ReleaseMirrorTargets();
    if (g_screenTexCopy) g_screenTexCopy->Release();
    g_screenTexCopy = nullptr;
    for (IDirect3DTexture9*& t : g_filter.tex)
    {
        if (t) t->Release();
        t = nullptr;
    }
    for (int i = 0; i < 2; i++)
    {
        if (g_vrt.rt[i]) g_vrt.rt[i]->Release();
        if (g_vrt.ds[i]) g_vrt.ds[i]->Release();
        if (g_vrt.tex[i]) g_vrt.tex[i]->Release();
    }
    if (g_vrt.screen) g_vrt.screen->Release();
    if (g_vrt.screenResolve) g_vrt.screenResolve->Release();
    g_vrt = VrTargets();
    ReleaseHudTargets();
    g_gpu.Release();
}

// [VR] Mirror=2: the eyes are rendered wider than the headset's picture (which is all it gets, see xr::SetEyeOffsets), so
// that the monitor's steady view (SteadyMonitorView), aimed down by MonitorPitch, fits in the left eye's picture with
// kSteadyRoom degrees to turn each way: the left eye sees some 40 degrees right of straight ahead (54 left, 44 up, 55 down),
// and a view turned down spreads at its bottom corners. The extra pixels beside the left eye's picture (f, w x h): left,
// right, above and below (the right eye has the same, left for right, so both are of one size).
constexpr float kSteadyRoom = 5.0f;

void SteadyRoom(const xr::EyeFov& f, UINT w, UINT h, UINT room[4])
{
    room[0] = room[1] = room[2] = room[3] = 0;
    if (g_cfg.vrMirror != 2) return;
    float tx = tanf(0.5f * std::clamp(g_cfg.vrMonitorFov, 20.0f, 150.0f) * 0.017453293f);
    float ty = tx * (g_screenW ? (float)g_screenH / (float)g_screenW : 0.5625f);
    float lo[2] = { f.left, f.down }, hi[2] = { f.right, f.up };
    for (int i = 0; i < 9; i++)
    {
        // The view turned down by b, then aside by a.
        float a = (float)(i % 3 - 1) * kSteadyRoom * 0.017453293f, b = (g_cfg.vrMonitorPitch + (float)(i / 3 - 1) * kSteadyRoom) * 0.017453293f;
        for (int c = 0; c < 4; c++)
        {
            float x = c & 1 ? tx : -tx, y = c & 2 ? ty : -ty;
            float y1 = y * cosf(b) - sinf(b), z1 = y * sinf(b) + cosf(b);
            float x2 = x * cosf(a) + z1 * sinf(a), z2 = std::max(-x * sinf(a) + z1 * cosf(a), 0.2f);
            lo[0] = std::min(lo[0], x2 / z2), hi[0] = std::max(hi[0], x2 / z2);
            lo[1] = std::min(lo[1], y1 / z2), hi[1] = std::max(hi[1], y1 / z2);
        }
    }
    float px = (float)w / (f.right - f.left), py = (float)h / (f.up - f.down);
    room[0] = std::min((UINT)ceilf((f.left - lo[0]) * px), w / 2);
    room[1] = std::min((UINT)ceilf((hi[0] - f.right) * px), w / 2);
    room[2] = std::min((UINT)ceilf((hi[1] - f.up) * py), h / 2);
    room[3] = std::min((UINT)ceilf((f.down - lo[1]) * py), h / 2);
}

bool EnsureVrEyeTargets(IDirect3DDevice9* dev)
{
    if (g_vrt.tex[0]) return true;
    if (g_vrt.failed) return false;
    unsigned eyeW, eyeH;
    xr::EyeSize(&eyeW, &eyeH);
    UINT room[4];
    SteadyRoom(xr::Fov(0), eyeW, eyeH, room);
    unsigned w = eyeW + room[0] + room[1], h = eyeH + room[2] + room[3];
    if (w != eyeW || h != eyeH)
        Log("VR eyes: the headset's %ux%u with room for the monitor's steady view: %u %u %u %u pixels left, right, up, down", eyeW, eyeH,
            room[0], room[1], room[2], room[3]);
    int samples = g_cfg.vrMsaa;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        D3DMULTISAMPLE_TYPE ms = samples >= 2 ? (D3DMULTISAMPLE_TYPE)samples : D3DMULTISAMPLE_NONE;
        bool ok = true;
        for (int i = 0; i < 2; i++)
        {
            ok &= SUCCEEDED(dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, ms, 0, FALSE, &g_vrt.rt[i], nullptr));
            ok &= SUCCEEDED(dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, ms, 0, TRUE, &g_vrt.ds[i], nullptr));
            ok &= SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_vrt.tex[i], nullptr));
        }
        Log("VR eye targets %ux%u, msaa %d: %s", w, h, (int)ms, ok ? "created" : "FAILED");
        if (ok)
        {
            g_vrt.w = w;
            g_vrt.h = h;
            g_vrt.eyeW = eyeW;
            g_vrt.eyeH = eyeH;
            memcpy(g_vrt.room, room, sizeof(room));
            xr::SetEyeOffsets(room[0], room[2], room[1], room[2]);
            // Screenshots: both eyes and the monitor.
            PrepareScreenshots(dev, w, h, D3DFMT_A8R8G8B8, 2);
            IDirect3DSurface9* bb = nullptr;
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
            {
                D3DSURFACE_DESC d;
                bb->GetDesc(&d);
                if (d.MultiSampleType == D3DMULTISAMPLE_NONE) PrepareScreenshots(dev, d.Width, d.Height, d.Format, 1);
                bb->Release();
            }
            return true;
        }
        IDirect3DTexture9* screen = g_vrt.screen;
        g_vrt.screen = nullptr;
        ReleaseVrTargets();
        g_vrt.screen = screen;
        samples = 0;  // retry without multisampling
    }
    g_vrt.failed = true;
    return false;
}

bool EnsureVrScreenTexture(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bb)
{
    if (g_vrt.screen && g_vrt.screenW == bb.Width && g_vrt.screenH == bb.Height) return true;
    if (g_vrt.screen) g_vrt.screen->Release();
    if (g_vrt.screenResolve) g_vrt.screenResolve->Release();
    g_vrt.screen = nullptr;
    g_vrt.screenResolve = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_vrt.screen, nullptr));
    if (ok && bb.MultiSampleType != D3DMULTISAMPLE_NONE)
        ok = SUCCEEDED(dev->CreateRenderTarget(bb.Width, bb.Height, bb.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_vrt.screenResolve, nullptr));
    Log("VR screen texture %ux%u: %s", bb.Width, bb.Height, ok ? "created" : "FAILED");
    if (!ok) return false;
    g_vrt.screenW = bb.Width;
    g_vrt.screenH = bb.Height;
    return true;
}

// Shows the game's picture (menus, loading screens) on a flat screen in the headset.
void SubmitVrScreen(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* s = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
    {
        D3DSURFACE_DESC d;
        bb->GetDesc(&d);
        if (EnsureVrScreenTexture(dev, d) && SUCCEEDED(g_vrt.screen->GetSurfaceLevel(0, &s)))
        {
            // Resolve the multisampled back buffer first, then convert X8R8G8B8 -> A8R8G8B8.
            IDirect3DSurface9* src = bb;
            ok = true;
            if (g_vrt.screenResolve)
            {
                ok = SUCCEEDED(dev->StretchRect(bb, nullptr, g_vrt.screenResolve, nullptr, D3DTEXF_NONE));
                src = g_vrt.screenResolve;
            }
            ok = ok && SUCCEEDED(dev->StretchRect(src, nullptr, s, nullptr, D3DTEXF_NONE));
            s->Release();
            if (!ok && !g_screenFailLogged)
            {
                g_screenFailLogged = true;
                Log("VR screen: copying the back buffer failed (format %d, msaa %d)", (int)d.Format, (int)d.MultiSampleType);
            }
        }
        bb->Release();
    }
    if (ok)
    {
        xr::SubmitScreen(g_vrt.screen, g_vrt.screenW, g_vrt.screenH);
        g_vrStats.screenFrames++;
        // The races' HUD panel's swapchain, made here in the menus rather than as a race starts (see xr.cpp's QueueLock).
        UINT pw, ph;
        HudPanelSize(g_vrt.screenW, g_vrt.screenH, &pw, &ph);
        if (g_cfg.vrHud) xr::PrepareHud(pw, ph);
    }
    else
        xr::EndFrameEmpty();
}

// A race's first frames, as it comes in after the loading screen, draw much for the first time: DXVK makes shaders
// and uploads textures as they are first used. The first ones take long (some 0.05 s on a fast PC, near a second
// on others), and the race stood still in the headset in between. The headset keeps showing the loading screen
// instead, steady in its place, while they render (and the HUD's swapchain is made): the eyes are shown from the
// third frame in a row that comes within one and a half headset frames of the last, or after two seconds.
struct
{
    bool screenShown;  // the headset's last frame was the flat screen's
    bool holding;      // the race's first frames: the screen's last picture stays
    int frames, inTime;
    double since;
    int gaps[8];       // the first frames' times from the last (ms), for the log
} g_hold;

// At the race's frame's Present, the time since the last Present given: true while the screen's picture stays.
bool HoldLoadingScreen(double now, double gap)
{
    if (g_hold.screenShown)
    {
        g_hold = {};
        g_hold.holding = true;
        g_hold.since = now;
    }
    if (!g_hold.holding) return false;
    if (g_hold.frames < 8) g_hold.gaps[g_hold.frames] = (int)(1000.0 * gap + 0.5);
    g_hold.frames++;
    double period = xr::DisplayPeriod();
    g_hold.inTime = period > 0 && gap <= 1.5 * period ? g_hold.inTime + 1 : 0;
    if (g_hold.inTime < 3 && now - g_hold.since < 2.0) return true;
    g_hold.holding = false;
    std::string gaps;
    for (int i = 0; i < g_hold.frames && i < 8; i++) gaps += " " + std::to_string(g_hold.gaps[i]);
    Log("eyes: the race shown in the headset from its frame %d, %.2f s after the first (the loading screen stayed meanwhile; the "
        "frames' times:%s%s ms)", g_hold.frames, now - g_hold.since, gaps.c_str(), g_hold.frames > 8 ? " ..." : "");
    return false;
}

HRESULT __stdcall ResetHook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    ReleaseVrTargets();
    g_screenW = g_screenH = 0;  // re-read from the new back buffer
    if (pp && xr::Available()) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    BackgroundBeforeDevice(pp);
    HRESULT hr = oReset(dev, pp);
    Log("device Reset %ux%u: %08X", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, (unsigned)hr);
    return hr;
}

HRESULT __stdcall StretchRectHook(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                                  const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    if (g_trace == Trace::Running) TraceLine("StretchRect %s -> %s", Describe(src).c_str(), Describe(dst).c_str());
    unsigned ret[3];
    if (NewEyeCall("StretchRect", _AddressOfReturnAddress(), ret)) LogEyeCall("StretchRect", ret, Describe(src) + " -> " + Describe(dst));
    return oStretchRect(dev, src, sr, dst, dr, f);
}

HRESULT __stdcall SetRenderTargetHook(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
    if (g_trace == Trace::Running) TraceLine("SetRenderTarget %u %s", (unsigned)index, Describe(s).c_str());
    unsigned ret[3];
    if (NewEyeCall("SetRenderTarget", _AddressOfReturnAddress(), ret)) LogEyeCall("SetRenderTarget", ret, Describe(s));
    if (g_hud.active && index == 0 && s && s == g_hud.gameRT) s = g_hud.rt;
    return oSetRenderTarget(dev, index, s);
}

HRESULT __stdcall SetDepthStencilSurfaceHook(IDirect3DDevice9* dev, IDirect3DSurface9* s)
{
    if (g_trace == Trace::Running) TraceLine("SetDepthStencilSurface %s", Describe(s).c_str());
    if (g_hud.active && s && s == g_hud.gameDS) s = g_hud.ds;
    return oSetDepthStencilSurface(dev, s);
}

HRESULT __stdcall BeginSceneHook(IDirect3DDevice9* dev)
{
    TraceLine("BeginScene");
    return oBeginScene(dev);
}

HRESULT __stdcall EndSceneHook(IDirect3DDevice9* dev)
{
    TraceLine("EndScene");
    return oEndScene(dev);
}

HRESULT __stdcall ClearHook(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    if (g_trace == Trace::Running)
    {
        D3DVIEWPORT9 vp;
        dev->GetViewport(&vp);
        TraceLine("Clear rects %u flags %X color %08X z %.2f (viewport %u %u %u %u)", (unsigned)count, (unsigned)flags, (unsigned)color, z,
                  (unsigned)vp.X, (unsigned)vp.Y, (unsigned)vp.Width, (unsigned)vp.Height);
    }
    if (g_depthReversed && (flags & D3DCLEAR_ZBUFFER)) z = 1.0f - z;
    return oClear(dev, count, rects, flags, color, z, stencil);
}

bool KeyPressed(int vk, bool& wasDown)
{
    bool down = HotkeysAllowed() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

// [VR] CameraOffset moves the eyes against the car, to set the view from the car just so (say the
// bonnet camera a little lower and further back for a video). In a race the camera keys move it while
// they are held: slowly at first, for fine steps, and faster the longer they are held. When they are
// let go the offset is written to the ini, so the next game starts with it.
double g_cameraLast, g_cameraHeld;
bool g_cameraMoved, g_cameraResetWasDown;

void CameraKeys(double now)
{
    double dt = std::min(now - g_cameraLast, 0.1);
    g_cameraLast = now;
    float* o = g_cfg.vrCameraOffset;
    bool race = InRace();  // outside races the offset is not seen
    if (KeyPressed(g_cfg.keyCameraReset, g_cameraResetWasDown) && race)
    {
        o[0] = o[1] = o[2] = 0.0f;
        g_cameraMoved = true;
    }
    // Right/left, up/down, forward/back: the axes of the offset, each key pair one way and the other.
    const int keys[6] = { g_cfg.keyCameraRight, g_cfg.keyCameraLeft, g_cfg.keyCameraUp,
                          g_cfg.keyCameraDown, g_cfg.keyCameraForward, g_cfg.keyCameraBack };
    float dir[3] = {};
    bool held = false;
    for (int i = 0; i < 6; i++)
        if (race && keys[i] && HotkeysAllowed() && (GetAsyncKeyState(keys[i]) & 0x8000))
        {
            dir[i / 2] += i % 2 ? -1.0f : 1.0f;
            held = true;
        }
    if (held)
    {
        // 3 cm a second, and 6 cm a second faster with every second held, up to 25 cm a second.
        g_cameraHeld += dt;
        float step = (float)(dt * std::min(0.03 + 0.06 * g_cameraHeld, 0.25));
        for (int i = 0; i < 3; i++) o[i] += dir[i] * step;
        g_cameraMoved = true;
        return;
    }
    g_cameraHeld = 0;
    if (g_cameraMoved)
    {
        g_cameraMoved = false;
        SaveCameraOffset();
        Log("camera keys: CameraOffset=%.3f %.3f %.3f (right, up, forward)", o[0], o[1], o[2]);
    }
}

// [VR] Cockpit: the camera inside the car leaves the player's driver, steering wheel and windows out
// (see game.h). In the headset the car is shown whole, as from the cameras outside.
static uintptr_t g_setCarViewFn = fo2::kSetCarView;
bool g_cockpitView;  // the player's car is seen from inside (its camera chose mode 0)

// The cockpit's eyes: at the driver's face, the front of its head a little below the middle (metres from
// the middle of the head's box, in the model's axes). There the head is out of sight however one leans:
// forwards it stays behind, backwards one is inside it, whose faces are seen from behind and left out.
// The driver's own eyes are some 3 cm behind and 7 cm higher, which in the headset seems too high. The
// game's camera inside the car is some 40 cm off: in front of the face, above it and towards the middle.
constexpr float kEyeFromHead[3] = { 0.0f, -0.03f, 0.115f };
// Where kEyeFromHead puts the male driver's eyes, from the driver's place (in the car's axes): for a
// head that is not posed (yet).
constexpr float kDriverEye[3] = { 0.0f, 0.60f, -0.01f };

// The eyes from the driver's place in the car's axes, worked out once a car from its driver's head as the
// driving animation poses it (the male and the female drivers are made differently), and then kept: the
// head nods with the animation, the eyes stay put.
BYTE* g_eyeCar;
float g_eyeFromSeat[3];

bool HeadEyeFromSeat(BYTE* car, float* out)
{
    BYTE* rag = car + fo2::kCarRagdoll;
    const BYTE* head = *(BYTE**)(rag + fo2::kRagdollSegments);
    if (!head) return false;
    const float* m = (const float*)(rag + fo2::kRagdollMatrices) + 16 * fo2::kRagdollHeadBone;
    const float* c = (const float*)(car + fo2::kCarMatrix);
    const float* seat = (const float*)(car + fo2::kCarDriverLoc);
    const float* box = (const float*)(head + fo2::kSegmentBoxMiddle);
    float p[3], w[3];
    for (int i = 0; i < 3; i++) p[i] = box[i] + kEyeFromHead[i];
    for (int i = 0; i < 3; i++) w[i] = p[0] * m[i] + p[1] * m[4 + i] + p[2] * m[8 + i] + m[12 + i] - c[12 + i];
    for (int i = 0; i < 3; i++) out[i] = w[0] * c[4 * i] + w[1] * c[4 * i + 1] + w[2] * c[4 * i + 2] - seat[i];
    return fabsf(out[0]) < 0.3f && out[1] > 0.3f && out[1] < 1.0f && fabsf(out[2]) < 0.4f;  // else not posed yet
}

void DriverEyeFromSeat(BYTE* car, float* out)
{
    if (car != g_eyeCar && HeadEyeFromSeat(car, g_eyeFromSeat))
    {
        g_eyeCar = car;
        Log("cockpit: the eyes %.3f %.3f %.3f from the driver's place", g_eyeFromSeat[0], g_eyeFromSeat[1], g_eyeFromSeat[2]);
    }
    memcpy(out, car == g_eyeCar ? g_eyeFromSeat : kDriverEye, sizeof(g_eyeFromSeat));
}

// The driver's eyes in the world, as the cockpit has them.
void CarEye(BYTE* car, float* eye)
{
    const float* m = (const float*)(car + fo2::kCarMatrix);
    const float* seat = (const float*)(car + fo2::kCarDriverLoc);
    float eyeFromSeat[3];
    DriverEyeFromSeat(car, eyeFromSeat);
    float p[3] = { seat[0] + eyeFromSeat[0], seat[1] + eyeFromSeat[1], seat[2] + eyeFromSeat[2] };
    for (int i = 0; i < 3; i++) eye[i] = m[12 + i] + p[0] * m[i] + p[1] * m[4 + i] + p[2] * m[8 + i];
}

// (SteeringWheel)
bool WheelPose(float hub[3], float axes[9])
{
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    BYTE* wheel = car ? *(BYTE**)(car + fo2::kCarSteeringWheel) : nullptr;
    if (!wheel) return false;
    const float* c = (const float*)(car + fo2::kCarMatrix);
    const float* w = (const float*)(wheel + fo2::kNodeWorld);
    const float* seat = (const float*)(car + fo2::kCarDriverLoc);
    float eye[3];
    DriverEyeFromSeat(car, eye);
    for (int i = 0; i < 3; i++)
    {
        float d = (w[12] - c[12]) * c[4 * i] + (w[13] - c[13]) * c[4 * i + 1] + (w[14] - c[14]) * c[4 * i + 2];
        hub[i] = d - seat[i] - eye[i] - g_cfg.vrCameraOffset[i];
        for (int j = 0; j < 3; j++) axes[3 * i + j] = w[4 * i] * c[4 * j] + w[4 * i + 1] * c[4 * j + 1] + w[4 * i + 2] * c[4 * j + 2];
    }
    return true;
}

// When the eyes look from inside the player's car, the camera's placement moves to the driver's eyes,
// fixed in the car (the driver's own node flies off with it when it is thrown out: see FlightEye). Which
// camera the view has comes out while the view renders (CarViewMode): this is the last frame's, so a cut from
// the camera inside to one outside is told by the distance.
bool CockpitEye(float* anchor)
{
    if (!g_cockpitView || !g_cfg.vrCockpit) return false;
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    if (!car) return false;
    float eye[3], d2 = 0;
    CarEye(car, eye);
    for (int i = 0; i < 3; i++) d2 += (eye[i] - anchor[12 + i]) * (eye[i] - anchor[12 + i]);
    if (d2 > 1.0f) return false;
    memcpy(anchor + 12, eye, sizeof(eye));
    return true;
}

// The player at this PC (see game.h: in the stunts kLocalPlayer stays empty).
BYTE* LocalPlayer()
{
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* gf = *(BYTE**)fo2::kGameFlow;
    BYTE* race = gf && !player ? *(BYTE**)(gf + fo2::kGfRace) : nullptr;
    if (!race) return player;
    BYTE** begin = *(BYTE***)(race + fo2::kRacePlayers);
    BYTE** end = *(BYTE***)(race + fo2::kRacePlayers + 4);
    for (BYTE** p = begin; p && p < end && p - begin < 16; p++)
        if (*p && *(int*)(*p + fo2::kPlayerType) == 1) return *p;
    return nullptr;
}

// A turn (the rows right, up, forward of a matrix) as a quaternion (x, y, z, w), and back.
void RowsToQuat(const float* m, float* q)
{
    // a(i, j): the matrix with the rows as its columns.
    auto a = [m](int i, int j) { return m[4 * j + i]; };
    float tr = a(0, 0) + a(1, 1) + a(2, 2);
    if (tr > 0.0f)
    {
        float s = 2.0f * sqrtf(tr + 1.0f);
        q[0] = (a(2, 1) - a(1, 2)) / s, q[1] = (a(0, 2) - a(2, 0)) / s, q[2] = (a(1, 0) - a(0, 1)) / s, q[3] = 0.25f * s;
    }
    else if (a(0, 0) > a(1, 1) && a(0, 0) > a(2, 2))
    {
        float s = 2.0f * sqrtf(std::max(1.0f + a(0, 0) - a(1, 1) - a(2, 2), 1e-6f));
        q[0] = 0.25f * s, q[1] = (a(0, 1) + a(1, 0)) / s, q[2] = (a(0, 2) + a(2, 0)) / s, q[3] = (a(2, 1) - a(1, 2)) / s;
    }
    else if (a(1, 1) > a(2, 2))
    {
        float s = 2.0f * sqrtf(std::max(1.0f + a(1, 1) - a(0, 0) - a(2, 2), 1e-6f));
        q[0] = (a(0, 1) + a(1, 0)) / s, q[1] = 0.25f * s, q[2] = (a(1, 2) + a(2, 1)) / s, q[3] = (a(0, 2) - a(2, 0)) / s;
    }
    else
    {
        float s = 2.0f * sqrtf(std::max(1.0f + a(2, 2) - a(0, 0) - a(1, 1), 1e-6f));
        q[0] = (a(0, 2) + a(2, 0)) / s, q[1] = (a(1, 2) + a(2, 1)) / s, q[2] = 0.25f * s, q[3] = (a(1, 0) - a(0, 1)) / s;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= l > 1e-6f ? l : 1.0f;
}

void QuatToRows(const float* q, float* m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    const float a[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                            { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                            { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[4 * j + i] = a[i][j];
}

// From the turn a to b by t (0..1), at an even pace.
void Slerp(const float* a, const float* b, float t, float* out)
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sb = d < 0.0f ? -1.0f : 1.0f;
    d = fabsf(d);
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f)
    {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1.0f - t) * th) / s, wb = sinf(t * th) / s;
    }
    float l = 0;
    for (int i = 0; i < 4; i++) out[i] = wa * a[i] + wb * sb * b[i], l += out[i] * out[i];
    l = sqrtf(l);
    for (int i = 0; i < 4; i++) out[i] /= l > 1e-6f ? l : 1.0f;
}

float SmoothStep(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// [VR] EjectView: when the driver is thrown out of the car (a heavy crash, or launched in the stunts; see
// game.h, kRagdollOut) the game cuts to its camera behind him. When the eyes were his (the cockpit) they stay
// in his head instead as he flies: in the middle of its box as the physics moves it (not at the face, which
// often lands on the ground: the middle stays 8 cm or more clear of what the box lies on), but not turned with
// it as it tumbles: the view turns with the user's head only, against the car's heading at the moment he left
// it with the horizon level (the world comes level over the first second). From where they were the moment
// before (the driver's eyes in the car) the eyes glide into the head over the first 0.3 s, so there is no cut.
// His own body is left out of the eyes (as for the camera inside the car), the near clipping plane comes in to
// 6 cm (the ground, the car he goes through), the speed zoom and blur (the car's) stay out, and the cockpit's
// cards with the car (the HUD is all on its panel, as with the cameras outside). When the car is reset with
// him back in it the eyes go back to the cockpit (the game cuts its own camera there), coming up from black over
// 0.4 s. EjectView=2 does the same from the cameras outside the car (the stunts' cars have no camera inside):
// the eyes glide in from the game's camera over 0.8 s.
constexpr float kFlightNear = 0.06f;         // metres
constexpr float kFlightGlide = 0.3f;         // seconds, from the cockpit
constexpr float kFlightGlideOutside = 0.8f;  // from a camera outside the car
constexpr float kFlightLevel = 1.0f;         // seconds until the view is level
constexpr float kFlightBackFade = 0.4f;      // seconds from black, back in the car
// The game puts the ragdoll by the windscreen and its first physics step pushes it on, clear of the car (some
// 0.4 m more than its speed): for so many frames the eyes stay where they were (in the cockpit, with the car) and
// only then glide into the head.
constexpr int kFlightSettleFrames = 2;

struct Flight
{
    BYTE* car = nullptr;  // the player's car, its driver out and the eyes in his head (null: none)
    double start = 0;     // when the eyes start gliding into his head (after kFlightSettleFrames)
    bool cockpit = false;  // the eyes were in the cockpit (else the game's camera)
    bool gliding = false;  // the ragdoll has settled, the eyes glide (or are) in his head
    float fromCamera[3] = {}, fromMove[3] = {};  // the game's camera the frame before, and its last step
    float glideTime = 0;
    float glide[3] = {};                // where the eyes were less where his are, at the start (it fades out)
    float dvel[3] = {};                 // and how fast they went less how fast his go (m/s; it fades out too)
    float from[4] = {}, level[4] = {};  // the view's turn at the start and level (quaternions)
    float launch[3] = {};               // his head at the start
    float head[3] = {};                 // his head in the last frame
    double lastNow = 0;                 // and when that was
    // For the log: the frames, the eyes' last place and step, their first steps (and the one before), the
    // largest change of step and the head's largest step (the physics').
    int frames = 0;
    float last[3] = {}, lastStep = 0, maxJerk = 0, maxJerkAt = 0, firstSteps[6] = {}, stepBefore = 0;
    float maxHeadStep = 0, maxHeadStepAt = 0;
    bool driverHidden = false;  // the driver's node was left out before the eyes left him out
    // The frame before (FlightSeen): the eyes' anchor and step, whether it was the cockpit.
    float prevAnchor[16] = {}, prevStep = 0, prevMove[3] = {};
    bool prevCockpit = false;
    double prevTime = 0;
    double back = 0;  // when the eyes went back into the car
} g_flight;

// The thrown driver's head in the world: the middle of its box (its segment's matrix, which the physics moves).
bool HeadEye(BYTE* car, float* eye)
{
    const float* m = *(const float**)(car + fo2::kCarRagdoll + fo2::kRagdollSegments);
    if (!m) return false;
    const float* c = (const float*)(car + fo2::kCarMatrix);
    float d2 = 0;
    for (int i = 0; i < 3; i++)
    {
        eye[i] = m[12 + i];
        d2 += (eye[i] - c[12 + i]) * (eye[i] - c[12 + i]);
    }
    return d2 < 500.0f * 500.0f;  // (false for NaN too)
}

void EndFlight(const char* why)
{
    Flight& f = g_flight;
    float d = 0;
    for (int i = 0; i < 3; i++) d += (f.head[i] - f.launch[i]) * (f.head[i] - f.launch[i]);
    Log("eject: %s after %.2f s, %d frames, %.1f m from where he left the car; the eyes' steps %.3f before, then %.3f %.3f %.3f %.3f "
        "%.3f %.3f m; their step changed at most %.3f m (at %.2f s); his head moved at most %.3f m in a frame (at %.2f s)",
        why, NowSeconds() - f.start, f.frames, sqrtf(d), f.stepBefore, f.firstSteps[0], f.firstSteps[1], f.firstSteps[2], f.firstSteps[3],
        f.firstSteps[4], f.firstSteps[5], f.maxJerk, f.maxJerkAt, f.maxHeadStep, f.maxHeadStepAt);
    f.car = nullptr;
    f.back = NowSeconds();
}

// Once a frame, before the eyes: when the player's driver is out, the eyes' anchor in his head (true).
bool FlightEye(float* anchor)
{
    Flight& f = g_flight;
    BYTE* player = LocalPlayer();
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    bool out = car && *(const int*)(car + fo2::kCarRagdoll + fo2::kRagdollOut) != 0;
    if (f.car && car != f.car) EndFlight("the eyes leave the thrown driver (another car)");
    if (f.car && !out) EndFlight("back in the car");
    // Back in the car: the eyes come up from black (laid over them at Present, see g_fade).
    float back = (float)(NowSeconds() - f.back) / kFlightBackFade;
    if (back < 1.0f) g_fade[1] = FadeOver(g_fade[1], (DWORD)lroundf(255.0f * (1.0f - SmoothStep(back))) << 24);
    if (!car || !g_cfg.vrEjectView || !out) return false;
    double now = NowSeconds();
    float head[3];
    bool placed = HeadEye(car, head);
    const float* c = (const float*)(car + fo2::kCarMatrix);
    if (!f.car)
    {
        // Thrown out just now: the eyes were in the cockpit the frame before (or, EjectView=2, with any camera).
        bool cockpit = f.prevCockpit && now - f.prevTime < 0.25;
        if (!placed || now - f.prevTime >= 0.25 || (!cockpit && g_cfg.vrEjectView < 2)) return false;
        Flight next;
        memcpy(next.prevAnchor, f.prevAnchor, sizeof(next.prevAnchor));
        next.car = car;
        next.start = now;
        next.cockpit = cockpit;
        memcpy(next.fromCamera, f.prevAnchor + 12, sizeof(next.fromCamera));
        memcpy(next.fromMove, f.prevMove, sizeof(next.fromMove));
        next.glideTime = cockpit ? kFlightGlide : kFlightGlideOutside;
        memcpy(next.launch, head, sizeof(head));
        memcpy(next.head, head, sizeof(head));
        memcpy(next.last, f.prevAnchor + 12, sizeof(next.last));
        next.lastStep = next.stepBefore = f.prevStep;
        // The view's turn the frame before, and the same heading level (the car's, if it looked straight up
        // or down).
        RowsToQuat(f.prevAnchor, next.from);
        float fx = f.prevAnchor[8], fz = f.prevAnchor[10];
        if (fx * fx + fz * fz < 0.04f) fx = c[8], fz = c[10];
        float l = sqrtf(fx * fx + fz * fz);
        if (l < 1e-3f) fx = 0.0f, fz = l = 1.0f;
        fx /= l, fz /= l;
        float level[16] = { fz, 0, -fx, 0, 0, 1, 0, 0, fx, 0, fz, 0, 0, 0, 0, 1 };
        RowsToQuat(level, next.level);
        f = next;
    }
    if (!f.gliding && f.frames >= kFlightSettleFrames && placed)
    {
        // Settled: from where the eyes were (in the cockpit, or the game's camera going on as it went).
        float from[3];
        if (f.cockpit)
            CarEye(car, from);
        else
            for (int i = 0; i < 3; i++) from[i] = f.fromCamera[i] + (float)(f.frames + 1) * f.fromMove[i];
        f.gliding = true;
        f.start = now;
        float dt = (float)std::clamp(now - f.lastNow, 1.0 / 150.0, 1.0 / 30.0);
        for (int i = 0; i < 3; i++)
        {
            f.glide[i] = from[i] - head[i];
            f.dvel[i] = ((from[i] - f.last[i]) - (head[i] - f.head[i])) / dt;
        }
        memcpy(f.head, head, sizeof(head));
        const float* v = (const float*)(car + fo2::kCarVelocity);
        float gl = sqrtf(f.glide[0] * f.glide[0] + f.glide[1] * f.glide[1] + f.glide[2] * f.glide[2]);
        Log("eject: the driver is thrown out at %.0f km/h, the eyes follow his head (from %s, %.2f m away; the view %.0f deg off level)",
            3.6f * sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), f.cockpit ? "the cockpit" : "the game's camera", gl,
            2.0f * acosf(std::min(1.0f, fabsf(f.from[0] * f.level[0] + f.from[1] * f.level[1] + f.from[2] * f.level[2] +
                                              f.from[3] * f.level[3]))) * 57.29578f);
    }
    float t = (float)(now - f.start);
    if (!f.gliding)
    {
        // Still where they were, turned as the frame before.
        QuatToRows(f.from, anchor);
        if (f.cockpit)
            CarEye(car, anchor + 12);
        else
            for (int i = 0; i < 3; i++) anchor[12 + i] = f.fromCamera[i] + (float)(f.frames + 1) * f.fromMove[i];
        if (placed) memcpy(f.head, head, sizeof(head));
    }
    else if (placed && t > 0.0f)
    {
        float hs = 0;
        for (int i = 0; i < 3; i++) hs += (head[i] - f.head[i]) * (head[i] - f.head[i]);
        if (sqrtf(hs) > f.maxHeadStep) f.maxHeadStep = sqrtf(hs), f.maxHeadStepAt = t;
        memcpy(f.head, head, sizeof(head));  // (else where it was)
    }
    if (f.gliding)
    {
        float q[4];
        Slerp(f.from, f.level, SmoothStep(t / kFlightLevel), q);
        QuatToRows(q, anchor);
        // The glide's offset and the difference of speed fade out together, the eyes' speed with no step (the
        // offset as a smoothstep, the speed as the Hermite curve that starts with it and ends at rest).
        float u = std::min(t / f.glideTime, 1.0f), g = 1.0f - SmoothStep(u), h = f.glideTime * u * (1.0f - u) * (1.0f - u);
        for (int i = 0; i < 3; i++) anchor[12 + i] = f.head[i] + g * f.glide[i] + h * f.dvel[i];
    }
    anchor[3] = anchor[7] = anchor[11] = 0.0f;
    anchor[15] = 1.0f;
    // How evenly the eyes move (the log): their step from the last frame and its change.
    float step = 0;
    for (int i = 0; i < 3; i++) step += (anchor[12 + i] - f.last[i]) * (anchor[12 + i] - f.last[i]);
    step = sqrtf(step);
    if (f.frames < 6) f.firstSteps[f.frames] = step;
    if (fabsf(step - f.lastStep) > f.maxJerk) f.maxJerk = fabsf(step - f.lastStep), f.maxJerkAt = t;
    f.lastStep = step;
    memcpy(f.last, anchor + 12, sizeof(f.last));
    f.lastNow = now;
    f.frames++;
    BYTE* driver = *(BYTE**)(car + fo2::kCarDriver);
    f.driverHidden = driver && (*(const unsigned*)(driver + fo2::kNodeFlags) & fo2::kNodeHidden);
    return true;
}

// After the eyes' anchor is set: for the next frame's FlightEye.
void FlightSeen(const float* anchor, bool cockpit)
{
    Flight& f = g_flight;
    for (int i = 0; i < 3; i++) f.prevMove[i] = anchor[12 + i] - f.prevAnchor[12 + i];
    f.prevStep = sqrtf(f.prevMove[0] * f.prevMove[0] + f.prevMove[1] * f.prevMove[1] + f.prevMove[2] * f.prevMove[2]);
    memcpy(f.prevAnchor, anchor, sizeof(f.prevAnchor));
    f.prevCockpit = cockpit;
    f.prevTime = NowSeconds();
}

// The thrown driver's body is left out of the eyes: SetCarView (which sets the driver's node's flags when the
// car's view changes) has run for the view (CarViewDone); after the eyes he is put back as he was.
void FlightDriverBack()
{
    BYTE* driver = g_flight.car ? *(BYTE**)(g_flight.car + fo2::kCarDriver) : nullptr;
    if (driver && !g_flight.driverHidden) *(unsigned*)(driver + fo2::kNodeFlags) &= ~fo2::kNodeHidden;
}

// For tests (the background mode's "eject"): the player's driver thrown out of the car, as a heavy crash does.
void ThrowDriverOut()
{
    BYTE* player = LocalPlayer();
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    if (!car || !InRace() || *(const int*)(car + fo2::kCarRagdoll + fo2::kRagdollOut))
    {
        Log("eject: no car with its driver in it");
        return;
    }
    using ThrowFn = void(__stdcall*)(BYTE* car, int arg);
    ((ThrowFn)fo2::kThrowDriverOut)(car, *(const int*)(car + fo2::kCarCrashArg));
    Log("eject: the driver thrown out for the test: %s", *(const int*)(car + fo2::kCarRagdoll + fo2::kRagdollOut) ? "out" : "still in");
}

// The drivers' hands follow the steering through the driving animation's frames, one step every 0.02 of
// the steering's travel (see game.h, kSampleAnim): turning slowly the hands stop and jump. The two frames
// either side are sampled and the bones' matrices blended between them by how far the steering is
// past the first.
static uintptr_t g_sampleAnimFn = fo2::kSampleAnim;

// (animation, node, t, matrices, mode in edx)
static __declspec(naked) void __stdcall CallSampleAnim(void*, void*, float, float*, int)
{
    __asm {
        mov edx, [esp + 20]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        call dword ptr [g_sampleAnimFn]
        ret 20
    }
}

// a's rows moved towards b's by w, each scaled back to the length it has between the two.
void BlendMatrix(float* a, const float* b, float w)
{
    for (int r = 0; r < 4; r++)
    {
        float* x = a + 4 * r;
        const float* y = b + 4 * r;
        float la = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]), lb = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        for (int i = 0; i < 4; i++) x[i] += (y[i] - x[i]) * w;
        if (r == 3) continue;
        float l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (l > 1e-6f)
            for (int i = 0; i < 3; i++) x[i] *= (la + (lb - la) * w) / l;
    }
}

static void __stdcall SampleAnimSmooth(BYTE* anim, void* node, float t, float* out, int mode)
{
    int bones = *(int*)(anim + fo2::kAnimBones);
    int span = *(int*)(anim + fo2::kAnimEnd) - *(int*)(anim + fo2::kAnimStart);
    int step = *(int*)(anim + fo2::kAnimFrameTime);
    if (mode != 0 || bones <= 0 || bones > 32 || span <= 1 || step <= 0)
    {
        CallSampleAnim(anim, node, t, out, mode);
        return;
    }
    // The frame the game would take and the next one, each sampled in the middle of its time.
    float f = std::clamp(t, 0.0f, 1.0f) * (float)(span - 1) / (float)step;
    int last = (span - 1) / step;
    int f0 = std::min((int)f, last), f1 = std::min(f0 + 1, last);
    float w = f - (float)f0;
    auto at = [&](int k) { return std::min(1.0f, ((float)k * step + 0.5f * step) / (float)(span - 1)); };
    if (f1 == f0 || w < 0.01f)
    {
        CallSampleAnim(anim, node, at(f0), out, mode);
        return;
    }
    float next[32 * 16];
    CallSampleAnim(anim, node, at(f1), next, mode);
    CallSampleAnim(anim, node, at(f0), out, mode);  // last, as the game leaves it
    for (int i = 0; i < bones; i++) BlendMatrix(out + 16 * i, next + 16 * i, w);
}

// At kSampleAnimCall, instead of kSampleAnim.
static __declspec(naked) void SampleAnimHook()
{
    __asm {
        push edx
        push dword ptr [esp + 20]
        push dword ptr [esp + 20]
        push dword ptr [esp + 20]
        push dword ptr [esp + 20]
        call SampleAnimSmooth
        ret 16
    }
}

// [VR] WheelRange (and GrabWheel): the player's car's steering wheel, and the driver's hands on it, turn as the player's
// own wheel (or the hands holding the cockpit's: ControllersWheelTurn), not as the car's steering. That follows the
// controller's bent by the game's curves, at a limited rate and less at speed (game.h, kSteeringCubic), and its full
// lock turns the cockpit's wheel 90 degrees: with a wheel of 900 degrees the cockpit's turned a fifth as far, less on the
// move. The car's steering is set to the player's own wheel's while its parts and its driver are posed (kPoseCarParts,
// kPoseDriver), and put back; past 90 degrees the cockpit's wheel stays at its lock (the driving animation's end).
bool g_ownWheel;  // both calls hooked

bool OwnWheelTurn(const BYTE* car, float& turn)
{
    BYTE* player = LocalPlayer();
    if (!g_ownWheel || !player || car != *(BYTE**)(player + fo2::kPlayerCar) || !InRace()) return false;
    if (*(const int*)(player + fo2::kPlayerInputOff) || *(const int*)(player + fo2::kPlayerSteerKeys)) return false;
    if (*(const int*)(car + fo2::kCarRagdoll + fo2::kRagdollOut)) return false;
    return ControllersWheelTurn(turn);
}

// At kPoseCarPartsCall, instead of kPoseCarParts.
static void __stdcall PoseCarPartsHook(BYTE* car, float dt)
{
    float* steering = (float*)(car + fo2::kCarSteering);
    float game = *steering, turn;
    bool own = OwnWheelTurn(car, turn);
    if (own) *steering = turn;
    ((void(__stdcall*)(BYTE*, float))fo2::kPoseCarParts)(car, dt);
    *steering = game;
    if (own && g_cfg.testControllersLog)
    {
        static double logged;
        static float last;
        if (NowSeconds() - logged > 0.1 && std::abs(turn - last) > 0.001f)
        {
            logged = NowSeconds(), last = turn;
            Log("cockpit: the steering wheel at %.1f degrees as the player's own (the car's steering at %.1f)", turn * 90.0f, game * 90.0f);
        }
    }
}

// At kPoseDriverCall, instead of kPoseDriver.
static void __stdcall PoseDriverHook(BYTE* ragdoll, float* place, float steering)
{
    float turn;
    if (OwnWheelTurn(ragdoll - fo2::kCarRagdoll, turn)) steering = turn;
    ((void(__stdcall*)(BYTE*, float*, float))fo2::kPoseDriver)(ragdoll, place, steering);
}

static int __stdcall CarViewMode(int mode, BYTE* car)
{
    bool flying = g_vrEye.active && car == g_flight.car;  // the eyes in the thrown driver's head: the car whole
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    if (!player || car != *(BYTE**)(player + fo2::kPlayerCar)) return flying ? 1 : mode;
    g_cockpitView = mode == 0;
    bool cockpit = g_cockpitView && g_cfg.vrCockpit && xr::Running();
    return cockpit || flying ? 1 : mode;
}

// After SetCarView: in the eyes the thrown driver's own body is left out (see FlightEye).
static void __stdcall CarViewDone(BYTE* car)
{
    BYTE* driver = g_vrEye.active && car == g_flight.car ? *(BYTE**)(car + fo2::kCarDriver) : nullptr;
    if (driver) *(unsigned*)(driver + fo2::kNodeFlags) |= fo2::kNodeHidden;
}

// At kCarViewCall, instead of kSetCarView (car in ecx, mode in eax).
static __declspec(naked) void CarViewHook()
{
    __asm {
        push ecx  // (for CarViewDone)
        push ecx
        push eax
        call CarViewMode
        mov ecx, [esp]
        call dword ptr [g_setCarViewFn]
        call CarViewDone
        ret
    }
}

// [VR] CockpitGlass: the car's windows seen from the driver's seat. The game's camera inside the car leaves them
// out; the cockpit shows the car whole (CarViewMode), and from inside the game's glass (see game.h) looks wrong both
// ways: the windscreen reflects the environment map's black ground, no glass to be seen, only the picture darkened
// by half (and its depth hides the sun's flare and the dust beyond it), while the side windows, at their glancing
// angle, are a bright haze. In the cockpit's eyes the player's whole glass is drawn lighter: its reflection and tint
// times CockpitGlass (1: as strong as the game's), the sun's glints on it times its square (from inside they glare
// over the road ahead), the reflection turned upwards (mirrored in the level plane through the eye: the sky, as on
// the glass seen from outside, instead of the ground), and without its depth, so that what lies beyond shows through.
// Every window, whole or broken, is drawn as its layer facing out, which glass seen from outside always has (the one
// facing in is up to the model); broken glass otherwise as the game draws it.
static uintptr_t g_batchDrawFn = fo2::kBatchDraw;

// (batch in esi)
static __declspec(naked) void __stdcall CallBatchDraw(BYTE*)
{
    __asm {
        push esi
        mov esi, [esp + 8]
        call dword ptr [g_batchDrawFn]
        pop esi
        ret 4
    }
}

// The player's car's glass: whether a batch is some of it, and whether that is broken.
enum class Glass { None, Whole, Broken };

Glass GlassOf(BYTE* batch)
{
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    if (!car) return Glass::None;
    BYTE** first = *(BYTE***)(car + fo2::kCarWindows);
    BYTE** last = *(BYTE***)(car + fo2::kCarWindows + 4);
    if (!first || std::find(first, last, batch) == last) return Glass::None;
    BYTE* material = *(BYTE**)batch;
    for (int i = 0; i < 4; i++)
    {
        const BYTE* window = car + fo2::kCarWindowSlots + i * fo2::kWindowSlotSize;
        if (*(BYTE* const*)window == material) return window[fo2::kWindowBroken] ? Glass::Broken : Glass::Whole;
    }
    return Glass::Whole;  // glass that does not break
}

// The side mirrors' housings' textures, while they are looked for (see MakeGlassMask).
bool g_mirrorSkinWanted;
void NoteMirrorSkin(BYTE* batch);
void MirrorsIntoEye();

static void __stdcall DynamicBatchDraw(BYTE* batch)
{
    if (g_mirrorSkinWanted && g_vrEye.active) NoteMirrorSkin(batch);
    // The mirrors look along the car's side, its windows edge on: drawn, a dark streak (real glass is hardly seen so).
    if (g_vrEye.active && g_vrEye.mirror && GlassOf(batch) != Glass::None) return;
    Glass glass = g_vrEye.active && g_vrEye.cockpit ? GlassOf(batch) : Glass::None;
    if (glass == Glass::None)
    {
        CallBatchDraw(batch);
        return;
    }
    MirrorsIntoEye();  // the side mirrors seen through the windows (see RenderMirrors)
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)fo2::kD3DDevice;
    DWORD cull = D3DCULL_CCW;
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    dev->SetRenderState(D3DRS_CULLMODE, cull == D3DCULL_CW ? D3DCULL_CCW : D3DCULL_CW);
    // Broken glass (shards round the frame, cracks) is blended over what is behind it but writes its depth all over the
    // window, the clear part too: the particles drawn after the cars (the smoke and fire from the bonnet, the nitro's
    // flames, the dust) and the rivals' markers were left out wherever they were seen through it. It writes no depth,
    // as the whole glass.
    if (glass == Glass::Broken)
    {
        DWORD depthWrite = TRUE;
        dev->GetRenderState(D3DRS_ZWRITEENABLE, &depthWrite);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        CallBatchDraw(batch);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, depthWrite);
        dev->SetRenderState(D3DRS_CULLMODE, cull);
        return;
    }
    float g = g_cfg.vrCockpitGlass;
    if (g <= 0.0f)
    {
        dev->SetRenderState(D3DRS_CULLMODE, cull);
        return;
    }
    // The vertex shader's constants (pro_car_window.sha): the world matrix's rows from c4, the eye at c8, the specular
    // colour at c22, the tint (w) at c32, the Fresnel term's base and slope at c33.
    float world[3][4], eye[4], specular[4], tint[4], fresnel[4];
    dev->GetVertexShaderConstantF(4, &world[0][0], 3);
    dev->GetVertexShaderConstantF(8, eye, 1);
    dev->GetVertexShaderConstantF(22, specular, 1);
    dev->GetVertexShaderConstantF(32, tint, 1);
    dev->GetVertexShaderConstantF(33, fresnel, 1);
    float s[4] = { specular[0] * g * g, specular[1] * g * g, specular[2] * g * g, specular[3] };
    float t[4] = { tint[0], tint[1], tint[2], 1.0f - (1.0f - tint[3]) * g };
    float f[4] = { fresnel[0] * g, fresnel[1] * g, fresnel[2], fresnel[3] };
    // y -> 2 eye.y - y: the eye stays, the reflection's vertical turns.
    float up[4] = { -world[1][0], -world[1][1], -world[1][2], 2.0f * eye[1] - world[1][3] };
    dev->SetVertexShaderConstantF(22, s, 1);
    dev->SetVertexShaderConstantF(32, t, 1);
    dev->SetVertexShaderConstantF(33, f, 1);
    dev->SetVertexShaderConstantF(5, up, 1);
    DWORD depthWrite = TRUE;
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &depthWrite);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    CallBatchDraw(batch);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, depthWrite);
    dev->SetRenderState(D3DRS_CULLMODE, cull);
    dev->SetVertexShaderConstantF(5, world[1], 1);
    dev->SetVertexShaderConstantF(22, specular, 1);
    dev->SetVertexShaderConstantF(32, tint, 1);
    dev->SetVertexShaderConstantF(33, fresnel, 1);
}

// At kDynamicBatchDrawCall, instead of kBatchDraw (the batch in esi).
static __declspec(naked) void DynamicBatchDrawHook()
{
    __asm {
        push esi
        call DynamicBatchDraw
        ret
    }
}

// [VR] Hud=2: the HUD panel is left out while the race runs, and comes back for its start (the
// countdown), the pause menu and the results. The race state stays "racing" in the pause menu; that
// the game is paused shows in the physics instead: a running race steps the world in every frame, a
// paused one not at all. The Hud key switches between 1 and 2.
constexpr int kRaceStateRacing = 8;
bool g_hudKeyWasDown;
int g_lastPhysicsSteps, g_framesWithoutStep;
bool g_hudLeftOut;

void ToggleHud()
{
    if (g_cfg.vrHud == 0)
    {
        Log("Hud key: Hud=0, no HUD panel to show");
        return;
    }
    g_cfg.vrHud = g_cfg.vrHud == 2 ? 1 : 2;
    SaveIniValue("VR", "Hud", g_cfg.vrHud == 2 ? "2" : "1");  // (kept for the next game)
    Log("Hud key: Hud=%d (%s)", g_cfg.vrHud, g_cfg.vrHud == 2 ? "not while racing" : "always");
}

// Once a frame: whether the HUD panel goes to the headset with this frame's eyes.
bool HudPanelShown()
{
    int steps = PhysicsSteps();
    g_framesWithoutStep = steps != g_lastPhysicsSteps ? 0 : g_framesWithoutStep + 1;
    g_lastPhysicsSteps = steps;
    bool leftOut = g_cfg.vrHud == 2 && InRace() && RaceState() == kRaceStateRacing && g_framesWithoutStep < 2;
    if (leftOut != g_hudLeftOut)
    {
        g_hudLeftOut = leftOut;
        Log("HUD panel %s", leftOut ? "left out (racing)" : "shown");
    }
    return !leftOut;
}

std::wstring ShotPath(const char* tag)
{
    std::wstring dir = ModuleDir() + L"shots\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[96];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(name, 96, L"fo2vr_%02d%02d%02d_%03d_%hs.png", t.wHour, t.wMinute, t.wSecond, g_shotIndex++, tag);
    return dir + name;
}

// DeviceD3d::Present (engine, slot 8). It calls IDirect3DDevice9::Present and, when the device
// reports D3DERR_DEVICENOTRESET, resets it. Hooked in the engine rather than on the device:
// Direct3D 9 implementations may rewrite the device's vtable (D3D9On12 did), undoing vtable hooks.
using EnginePresentFn = int(__thiscall*)(void* self);
const auto oEnginePresent = (EnginePresentFn)fo2::kEnginePresent;
void* g_loggedVtbl;

// Whether the game's window has the focus, logged when that changes, with the program in front then (SteamVR's windows
// take it as the game starts SteamVR; in VR the game is not told: see KeepGameActive).
void NoteFocus(IDirect3DDevice9* dev)
{
    static int last = -1;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
    HWND front = GetForegroundWindow();
    int state = (front == cp.hFocusWindow ? 1 : 0) | (IsIconic(cp.hFocusWindow) ? 2 : 0);
    if (state == last) return;
    last = state;
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    wchar_t exe[MAX_PATH] = L"nothing";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        DWORD n = MAX_PATH;
        if (!QueryFullProcessImageNameW(p, 0, exe, &n)) wcscpy(exe, L"?");
        CloseHandle(p);
    }
    const wchar_t* name = wcsrchr(exe, L'\\');
    Log("window: the game's window %s%s (in front: %ls)", state & 1 ? "has the focus" : "lost the focus", state & 2 ? ", minimized" : "",
        name ? name + 1 : exe);
}

int __fastcall EnginePresentHook(void* self, void*)
{
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)fo2::kD3DDevice;
    if (!dev) return oEnginePresent(self);
    if (*(void**)dev != g_loggedVtbl)
    {
        g_loggedVtbl = *(void**)dev;
        Log("device %p vtable now %p", (void*)dev, g_loggedVtbl);
    }
    rec::MenuTick();
    if (xr::Available()) NoteFocus(dev);
    if (g_renderViewsEnd > 0)
    {
        g_frameHud = NowSeconds() - g_renderViewsEnd;
        g_vrStats.hud.Add(g_frameHud);
        g_renderViewsEnd = 0;
    }
    if (g_hud.active) EndHudCapture(dev);
    g_gpu.End();
    if (dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
    {
        // Our D3DPOOL_DEFAULT targets must go before the engine resets the device.
        if (g_eyesPending)
        {
            g_eyesPending = false;
            xr::EndFrameEmpty();
        }
        ReleaseVrTargets();
        g_screenW = g_screenH = 0;
        Log("device needs a reset: VR targets released");
    }

    double now = NowSeconds();
    static double lastPresent;
    double sincePresent = now - lastPresent;
    lastPresent = now;
    g_stats.frames++;

    if (g_trace == Trace::Running)
    {
        TraceLine("Present");
        prof::TraceDraws(false);
        g_trace = Trace::Done;
        Log("trace end");
    }

    ScreenshotTick();
    bool shot = KeyPressed(g_cfg.keyScreenshot, g_shotKeyWasDown);
    int bg = BackgroundPoll();
    shot |= (bg & kPollShot) != 0;
    if ((bg & kPollTrace) && g_trace != Trace::Running) g_trace = Trace::Armed;
    if (KeyPressed(g_cfg.keyHud, g_hudKeyWasDown) || (bg & kPollHud)) ToggleHud();
    if (bg & kPollEject) ThrowDriverOut();
    char knockSide;
    int knockState;
    if (TakeMirrorKnock(&knockSide, &knockState)) KnockMirror(knockSide, knockState);
    if (g_cfg.autoShotSeconds > 0 && InRace() && now - g_lastShot >= g_cfg.autoShotSeconds) shot = true;
    if (shot)
    {
        g_lastShot = now;
        SaveBackBufferPng(dev, ShotPath("normal").c_str());
        if (xr::Running()) g_saveEyesThisFrame = true;
        IDirect3DSurface9* panel = nullptr;
        if (g_hud.captured && SUCCEEDED(g_hud.panel->GetSurfaceLevel(0, &panel)))
        {
            SaveSurfacePng(dev, panel, ShotPath("vr_hud").c_str(), true);
            panel->Release();
            // and the cockpit's instruments, drawn apart
            for (int i = 0; i < 2 && HudCardsRouted(); i++) SaveSurfacePng(dev, g_hud.cardRT[i], ShotPath(i ? "vr_cards2" : "vr_cards").c_str(), true);
        }
    }

    DWORD world = FadeOver(g_fade[0], g_fade[1]);
    if ((g_eyesPending || g_eyeShotPending) && (world >> 24))
    {
        for (IDirect3DTexture9* eye : g_vrt.tex)
        {
            IDirect3DSurface9* s = nullptr;
            if (eye && SUCCEEDED(eye->GetSurfaceLevel(0, &s)))
            {
                FadeTarget(dev, s, world, false);
                s->Release();
            }
        }
    }
    g_fade[0] = g_fade[1] = 0;
    if (g_eyeShotPending)
    {
        g_eyeShotPending = false;
        // The resolved eye textures: what the headset gets.
        for (int eye = 0; eye < 2; eye++)
        {
            IDirect3DSurface9* s = nullptr;
            if (g_vrt.tex[eye] && SUCCEEDED(g_vrt.tex[eye]->GetSurfaceLevel(0, &s)))
            {
                SaveSurfacePng(dev, s, ShotPath(eye == 0 ? "vr_left" : "vr_right").c_str());
                s->Release();
            }
        }
    }
    bool hudShown = HudPanelShown();
    if (g_eyesPending)
    {
        g_eyesPending = false;
        double s0 = NowSeconds();
        bool hud = g_cfg.vrHud && g_hud.captured && hudShown;
        if (HoldLoadingScreen(now, sincePresent))
            xr::SubmitLastScreen(g_cfg.vrHud ? g_hud.panelW : 0, g_cfg.vrHud ? g_hud.panelH : 0);
        else
            xr::SubmitEyes(g_vrt.tex[0], g_vrt.tex[1], hud ? g_hud.panel : nullptr, g_hud.panelW, g_hud.panelH);
        g_vrStats.submit += NowSeconds() - s0;
        g_vrStats.submitT.Add(NowSeconds() - s0);
        g_vrStats.eyeFrames++;
    }
    g_hud.captured = false;
    if (InRace() && g_frameRenderViews > 0) prof::EndFrame(g_frameGame, g_frameRenderViews, g_frameHud);
    g_frameGame = g_frameRenderViews = g_frameHud = 0;
    prof::SetPhase(prof::Game);

    // VR: whatever the race path did not submit this frame is shown on the flat screen.
    xr::PollEvents();
    if (KeyPressed(g_cfg.keyRecenter, g_recenterKeyWasDown) | ControllersRecenter()) xr::Recenter();  // F8, both thumbsticks pressed
    ControllersMenuKeys();
    CameraKeys(now);
    if (xr::Running() && !g_vrFrameDone)
    {
        double w0 = NowSeconds();
        bool render = xr::BeginFrame();
        g_vrStats.wait += NowSeconds() - w0;
        if (render)
        {
            SubmitVrScreen(dev);
            g_hold.screenShown = true;
        }
        else
            xr::EndFrameEmpty();
    }
    g_vrFrameDone = false;

    if (now - g_lastLog >= g_cfg.logIntervalSeconds && g_stats.frames > 0)
    {
        NoteAddressSpace();
        double span = now - g_lastLog;
        if (g_lastLog > 0 && InRace())
        {
            int n = g_stats.renderViews ? g_stats.renderViews : 1;
            Log("%.1f fps | RenderViews %d calls, avg %.2f ms, max %.2f ms | per frame: SetCamera %.1f SetFrustum %.1f SetViewport %.1f",
                g_stats.frames / span, g_stats.renderViews, 1000.0 * g_stats.rvTime / n, 1000.0 * g_stats.rvTimeMax,
                (double)g_stats.setCamera / g_stats.frames, (double)g_stats.setFrustum / g_stats.frames,
                (double)g_stats.setViewport / g_stats.frames);
        }
        if (g_lastLog > 0 && xr::Running())
        {
            int f = g_stats.frames;
            int e = g_vrStats.eyeFrames ? g_vrStats.eyeFrames : 1;
            share::Stats ss = share::TakeStats();
            Log("VR %.1f fps: %d eye frames, %d screen frames | wait %.2f ms/frame, eyes %.2f ms, submit %.2f ms | right eye: "
                "leaf chunks %d copied, %d built, grass batches %d copied, %d built",
                f / span, g_vrStats.eyeFrames, g_vrStats.screenFrames, 1000.0 * g_vrStats.wait / f, 1000.0 * g_vrStats.eyes / e,
                1000.0 * g_vrStats.submit / e, ss.reused, ss.rebuilt, ss.grassCopied, ss.grassBuilt);
            const VrStats& v = g_vrStats;
            if (v.game.n > 0)
                Log("   frame avg/max ms: game %.2f/%.2f, eyes %.2f/%.2f, HUD %.2f/%.2f, to headset %.2f/%.2f, monitor Present %.2f/%.2f | "
                    "GPU: eyes %.2f/%.2f, frame %.2f/%.2f",
                    v.game.AvgMs(), v.game.MaxMs(), v.eyesT.AvgMs(), v.eyesT.MaxMs(), v.hud.AvgMs(), v.hud.MaxMs(), v.submitT.AvgMs(),
                    v.submitT.MaxMs(), v.present.AvgMs(), v.present.MaxMs(), v.gpuEyes.AvgMs(), v.gpuEyes.MaxMs(), v.gpuFrame.AvgMs(),
                    v.gpuFrame.MaxMs());
            xr::SubmitTimes st = xr::TakeSubmitTimes();
            if (st.count > 0)
            {
                double k = 1000.0 / st.count;
                Log("   submit: flush %.2f, acquire %.2f, wait image %.2f, fence %.2f, copy+release %.2f, xrEndFrame %.2f ms", st.flush * k,
                    st.acquire * k, st.wait * k, st.fence * k, st.submit * k, st.endFrame * k);
            }
        }
        g_stats.Reset();
        g_vrStats = VrStats();
        g_lastLog = now;
    }

    double p0 = NowSeconds();
    int r = oEnginePresent(self);
    if (InRace()) g_vrStats.present.Add(NowSeconds() - p0);
    if (g_trace == Trace::Armed)
    {
        g_trace = Trace::Running;
        g_traceLines = 0;
        prof::TraceDraws(true);
        Log("trace start: one frame of render target traffic");
    }
    return r;
}

template <class T>
void HookSlot(void** vtbl, int index, void* hook, T& orig)
{
    if (vtbl[index] == hook) return;
    orig = (T)vtbl[index];
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

HRESULT __stdcall DrawPrimitiveHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT first, UINT n);
HRESULT __stdcall DrawIndexedPrimitiveHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT least, UINT count, UINT start, UINT n);
HRESULT __stdcall DrawPrimitiveUPHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride);
HRESULT __stdcall DrawIndexedPrimitiveUPHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT least, UINT count, UINT n, const void* i,
                                             D3DFORMAT f, const void* v, UINT stride);

void HookDevice(IDirect3DDevice9* dev)
{
    if (!dev || dev == g_hookedDevice) return;
    void** vtbl = *(void***)dev;
    HookSlot(vtbl, vt::Reset, (void*)&ResetHook, oReset);
    HookSlot(vtbl, vt::StretchRect, (void*)&StretchRectHook, oStretchRect);
    HookSlot(vtbl, vt::SetRenderTarget, (void*)&SetRenderTargetHook, oSetRenderTarget);
    HookSlot(vtbl, vt::SetDepthStencilSurface, (void*)&SetDepthStencilSurfaceHook, oSetDepthStencilSurface);
    HookSlot(vtbl, vt::BeginScene, (void*)&BeginSceneHook, oBeginScene);
    HookSlot(vtbl, vt::EndScene, (void*)&EndSceneHook, oEndScene);
    HookSlot(vtbl, vt::Clear, (void*)&ClearHook, oClear);
    HookSlot(vtbl, vt::SetRenderState, (void*)&SetRenderStateHook, oSetRenderState);
    HookSlot(vtbl, vt::DrawPrimitive, (void*)&DrawPrimitiveHook, oDrawPrimitive);
    HookSlot(vtbl, vt::DrawIndexedPrimitive, (void*)&DrawIndexedPrimitiveHook, oDrawIndexedPrimitive);
    HookSlot(vtbl, vt::DrawPrimitiveUP, (void*)&DrawPrimitiveUPHook, oDrawPrimitiveUP);
    HookSlot(vtbl, vt::DrawIndexedPrimitiveUP, (void*)&DrawIndexedPrimitiveUPHook, oDrawIndexedPrimitiveUP);
    g_hookedDevice = dev;
    Log("IDirect3DDevice9 %p hooked (vtable %p)", (void*)dev, (void*)vtbl);
    prof::Hook(dev, g_cfg.testDeviceTiming || g_cfg.testReplayTrace > 0);
}

// The engine restores "the last viewport" before its full-screen passes. Before an eye render it
// must say full screen, so SetViewportHook maps those passes onto the eye's rectangle.
void SetFullViewportGlobals()
{
    int* vp = (int*)fo2::kViewportX;
    vp[0] = 0;
    vp[1] = 0;
    vp[2] = (int)g_screenW;
    vp[3] = (int)g_screenH;
}

// The inverse of a camera matrix (rows right, up, forward, position; rotation rows orthonormal):
// the view matrix.
void InvertCamera(const float* c, float* v)
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++) v[4 * i + j] = c[4 * j + i];
        v[4 * i + 3] = 0.0f;
    }
    for (int j = 0; j < 3; j++) v[12 + j] = -(c[12] * c[4 * j] + c[13] * c[4 * j + 1] + c[14] * c[4 * j + 2]);
    v[15] = 1.0f;
}

// a * b for the game's matrices, which move row vectors: a's transform, then b's.
void MulMatrix(const float* a, const float* b, float* out)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}

// The speed blur zooms the bloom out from a point of the picture: on a monitor the middle (the tracks'
// looks may move it), where the game camera looks. In an eye it zooms from where that direction is in
// the eye's picture: the same direction in both eyes, so the streaks fuse, far away like the horizon.
// Looking aside, the point moves to the side or off the picture, as the direction of travel does.
// center: the game's (0..1 across and down the monitor's picture); out: the eye's.
void EyeBlurCenter(const float center[2], const xr::EyePose& p, const xr::EyeFov& f, float out[2])
{
    // The direction in the game camera's axes (right, up, forward), then in the eye's, whose axes are
    // given in the camera's (see BuildEyeMatrix).
    float d[3] = { (2.0f * center[0] - 1.0f) * g_vrEye.monitorTanH, (1.0f - 2.0f * center[1]) * g_vrEye.monitorTanV, 1.0f };
    float x = d[0] * p.right[0] + d[1] * p.right[1] + d[2] * p.right[2];
    float y = d[0] * p.up[0] + d[1] * p.up[1] + d[2] * p.up[2];
    float z = d[0] * p.forward[0] + d[1] * p.forward[1] + d[2] * p.forward[2];
    // Looking back, the zoom's lines meet at the opposite point, the direction one comes from.
    if (fabsf(z) < 0.01f) z = z < 0.0f ? -0.01f : 0.01f;
    out[0] = std::clamp((x / z - f.left) / (f.right - f.left), -100.0f, 100.0f);
    out[1] = std::clamp((f.up - y / z) / (f.up - f.down), -100.0f, 100.0f);
}

// The eyes moved by [VR] CameraOffset, and for tests the head by [Test] HeadOffset, tilted up and
// turned about the vertical axis (the background mode's "head" command).
void ApplyHeadOffsets(xr::EyePose& p)
{
    if (g_cfg.testHeadPitchDeg != 0.0f)
    {
        float a = g_cfg.testHeadPitchDeg * 0.017453293f, c = cosf(a), s = sinf(a);
        for (float* v : { p.right, p.up, p.forward, p.position })
        {
            float y = v[1], z = v[2];
            v[1] = y * c + z * s;
            v[2] = -y * s + z * c;
        }
    }
    if (g_cfg.testHeadYawDeg != 0.0f)
    {
        float a = g_cfg.testHeadYawDeg * 0.017453293f, c = cosf(a), s = sinf(a);
        for (float* v : { p.right, p.up, p.forward, p.position })
        {
            float x = v[0], z = v[2];
            v[0] = x * c + z * s;
            v[2] = -x * s + z * c;
        }
    }
    for (int i = 0; i < 3; i++) p.position[i] += g_cfg.testHeadOffset[i] + g_cfg.vrCameraOffset[i];
}

// Runtimes give both eyes the head's orientation, up to rounding: made exactly the same, so the
// right eye can take over what the left eye built from its direction (see eyeshare.h).
void SameDirection(xr::EyePose* pose)
{
    float d = 0;
    for (int i = 0; i < 3; i++)
        d = std::max({ d, fabsf(pose[0].right[i] - pose[1].right[i]), fabsf(pose[0].up[i] - pose[1].up[i]),
                       fabsf(pose[0].forward[i] - pose[1].forward[i]) });
    if (d > 1e-4f) return;
    memcpy(pose[1].right, pose[0].right, sizeof(pose[1].right));
    memcpy(pose[1].up, pose[0].up, sizeof(pose[1].up));
    memcpy(pose[1].forward, pose[0].forward, sizeof(pose[1].forward));
}

// Eye camera = game camera (anchor) * head pose relative to the recenter origin.
void BuildEyeMatrix(const float* anchor, const xr::EyePose& p, float* e)
{
    const float* aR = anchor;
    const float* aU = anchor + 4;
    const float* aF = anchor + 8;
    const float* aP = anchor + 12;
    float s = g_cfg.vrWorldScale;
    const float* hp = p.position;
    for (int i = 0; i < 3; i++)
    {
        e[0 + i] = aR[i] * p.right[0] + aU[i] * p.right[1] + aF[i] * p.right[2];
        e[4 + i] = aR[i] * p.up[0] + aU[i] * p.up[1] + aF[i] * p.up[2];
        e[8 + i] = aR[i] * p.forward[0] + aU[i] * p.forward[1] + aF[i] * p.forward[2];
        e[12 + i] = aP[i] + s * (aR[i] * hp[0] + aU[i] * hp[1] + aF[i] * hp[2]);
    }
    e[3] = e[7] = e[11] = 0.0f;
    e[15] = 1.0f;
}

// The cockpit's near clipping plane, as far out as the cabin lets it be. The depth's 24 bits are spread
// out from the near plane: a few centimetres away, what lies a millimetre or two apart some tens of
// metres off (a sign's face on its board) comes out at the same depth, in bands. Nearest to the eyes in
// the cockpit are the roof over the head and the sides beside it (the cabin's box, less some 3 cm of roof
// and doors), and a plane at h from the eye, its normal N towards it, comes no nearer along the view than
// h over the largest v . N of the corner directions v = (x, y, 1) of an eye's frustum (in its axes).
float CockpitNear(const float* anchor, const xr::EyePose* pose, const xr::EyeFov* fov, float maxNear)
{
    float nearest = maxNear;
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    if (car)
    {
        const float* m = (const float*)(car + fo2::kCarMatrix);
        const float* lo = (const float*)(car + fo2::kCarCabinMin);
        const float* hi = (const float*)(car + fo2::kCarCabinMax);
        const float* com = (const float*)(car + fo2::kCarCentreOfMass);
        const float wall = 0.03f;
        for (int eye = 0; eye < 2; eye++)
        {
            float e[16], c[4][3];  // the eye's right, up, forward and place in the car's axes
            BuildEyeMatrix(anchor, pose[eye], e);
            for (int r = 0; r < 4; r++)
                for (int i = 0; i < 3; i++)
                {
                    const float* v = e + 4 * r;
                    float d[3] = { v[0], v[1], v[2] };
                    if (r == 3)
                        for (int k = 0; k < 3; k++) d[k] -= m[12 + k];
                    c[r][i] = d[0] * m[4 * i] + d[1] * m[4 * i + 1] + d[2] * m[4 * i + 2];
                }
            // The roof (normal up), the left and the right side.
            const struct { int axis; float sign, h; } planes[] = {
                { 1, 1.0f, hi[1] - com[1] - wall - c[3][1] },
                { 0, -1.0f, c[3][0] - (lo[0] - com[0] + wall) },
                { 0, 1.0f, hi[0] - com[0] - wall - c[3][0] },
            };
            for (const auto& p : planes)
            {
                if (p.h <= 0.0f) continue;  // the head out of the box
                float most = 0.0f;
                for (float x : { fov[eye].left, fov[eye].right })
                    for (float y : { fov[eye].down, fov[eye].up })
                        most = std::max(most, p.sign * (x * c[0][p.axis] + y * c[1][p.axis] + c[2][p.axis]));
                if (most > 0.0f) nearest = std::min(nearest, p.h / most);
            }
        }
    }
    g_cockpitNear = std::clamp(0.85f * nearest, g_cfg.vrNearClip, maxNear);
    return g_cockpitNear;
}

// The cockpit's instruments ([VR] CockpitHud): the HUD's gauges, position, lap, map and the rival's name
// and damage stand in the car as cards on one band curved round the driver's eyes (a sphere), drawn into
// the eyes' pictures with the depth, so the hands and the steering wheel go in front of them. They are laid
// out the same in every car (as made by hand in the Venom: the gauges in the wheel's opening above its hub,
// the position left of the wheel, the lap and the rival's card right of it, the map below the rival's),
// turned to the wheel's hub as the driver's eyes see it (LayCockpitCards). The HUD draws them into targets of
// their own instead of its own (DrawHudBatch; the eyes show the frame before's), so each card shows all of
// its instrument, its effects too (the nitro's flames by the gauges, the crash bonus by the rival's card), and
// nothing else; the HUD panel keeps the rest (the messages, the countdown, the pause menu).
struct CockpitCard
{
    float du0, dv0, du1, dv1;  // where the HUD draws it, fractions of the (16:9) screen: a draw that lies
                               // inside goes to the card's target (CardOfBox)
    float u0, v0, u1, v1;      // the part of that target the card shows, all the drawing does
    float left, right, top, bottom;  // its place on the band: degrees right of and above the wheel's hub as
                                     // the driver's eyes see it
    float Aspect() const { return (u1 - u0) * 16.0f / ((v1 - v0) * 9.0f); }
};
enum { kCardGauges, kCardPosition, kCardLap, kCardMap, kCardRival, kCardCount };
constexpr CockpitCard kCockpitCards[kCardCount] = {
    // gauges (and the nitro's label above, the nitro's bar at their left and its flames)
    { 0.65f, 0.58f, 1.10f, 1.10f, 0.710f, 0.645f, 1.000f, 1.000f, -5.34f, 3.84f, 12.30f, 5.98f },
    // position
    { -0.10f, -0.10f, 0.30f, 0.16f, 0.000f, 0.000f, 0.230f, 0.150f, -31.54f, -17.42f, 12.88f, 7.68f },
    // lap
    { 0.70f, -0.10f, 1.10f, 0.16f, 0.770f, 0.000f, 1.000f, 0.150f, 17.24f, 31.35f, 12.88f, 7.68f },
    // map (a tilted disc)
    { -0.10f, 0.50f, 0.23f, 0.86f, 0.010f, 0.560f, 0.220f, 0.840f, 25.16f, 36.47f, 7.29f, -1.21f },
    // the rival's name and damage, with the crash bonus above it and its stars to the right (which reach
    // into the map's place and past the damage's bar)
    { -0.20f, 0.785f, 0.50f, 1.10f, 0.000f, 0.740f, 0.620f, 0.970f, 24.59f, 62.18f, 15.50f, 7.53f },
};
// A derby's HUD is laid out otherwise (kGfEventType): the speed as a number over two long bars, the nitro's and the
// car's damage (the nitro's flames at its left end); the cars remaining and the contact timer where the position and
// the lap are, the timer's words longer than the lap's, reaching to where the rival's portrait is on the band; the
// arena's map a mesh of its own on a larger canvas, the arena where the track puts it on it. Its cards: the gauges a
// strip in the wheel's opening, the rival's card further right, the map's the canvas as large as it fits below that.
constexpr CockpitCard kDerbyCards[kCardCount] = {
    // gauges: the speed and the bars
    { 0.60f, 0.70f, 1.20f, 1.10f, 0.640f, 0.800f, 1.000f, 0.975f, -9.75f, 8.25f, 11.50f, 6.58f },
    // the cars remaining
    { -0.10f, -0.10f, 0.30f, 0.16f, 0.000f, 0.000f, 0.240f, 0.150f, -31.54f, -16.81f, 12.88f, 7.68f },
    // the contact timer
    { 0.70f, -0.10f, 1.10f, 0.16f, 0.760f, 0.000f, 1.000f, 0.150f, 16.62f, 31.35f, 12.88f, 7.68f },
    // the arena's map
    { -0.10f, 0.38f, 0.40f, 0.93f, -0.060f, 0.385f, 0.345f, 0.925f, 25.16f, 39.16f, 7.29f, -3.21f },
    // the rival's name and damage (and the crash bonus)
    { -0.20f, 0.785f, 0.50f, 1.10f, 0.000f, 0.740f, 0.620f, 0.970f, 31.59f, 69.18f, 15.50f, 7.53f },
};
constexpr float kCardsBand = 0.732f;      // the band's radius (metres), where the dashboard lets it be
constexpr float kCardDepthPull = 0.015f;  // metres nearer in depth than they are, not to sink into the dashboard

// The cards are drawn: in the cockpit, in a race, with the HUD on.
bool CockpitCardsOn()
{
    return g_cfg.vrCockpitHud && g_cfg.vrCockpit && g_cfg.vrHud != 0 && g_cockpitView && InRace() && g_hud.tex;
}

// A mesh node's triangles (see game.h, kCarBody: its nearest model) in the car's axes, 9 floats each, and
// the batch each comes from, appended. The indices are read from the batches' index buffers, the vertices
// from the CPU's copy or else the vertex buffer.
void ReadMesh(BYTE* car, BYTE* node, std::vector<float>& tris, std::vector<int>& batchOf)
{
    BYTE* model = node ? *(BYTE**)(node + fo2::kNodeModels) : nullptr;
    if (!model) return;
    // The model's axes and place in the car's.
    const float* c = (const float*)(car + fo2::kCarMatrix);
    const float* w = (const float*)(node + fo2::kNodeWorld);
    float m[12];
    for (int r = 0; r < 4; r++)
    {
        float d[3] = { w[4 * r], w[4 * r + 1], w[4 * r + 2] };
        if (r == 3)
            for (int k = 0; k < 3; k++) d[k] -= c[12 + k];
        for (int i = 0; i < 3; i++) m[3 * r + i] = d[0] * c[4 * i] + d[1] * c[4 * i + 1] + d[2] * c[4 * i + 2];
    }
    BYTE** begin = *(BYTE***)(model + fo2::kModelBatches);
    BYTE** end = *(BYTE***)(model + fo2::kModelBatches + 4);
    for (BYTE** it = begin; it && it < end && it - begin < 64; it++)
    {
        BYTE* b = *it;
        if (!b) continue;
        DWORD prim = *(DWORD*)(b + fo2::kBatchPrimType);
        int stride = *(int*)(b + fo2::kBatchStride);
        int count = *(WORD*)(b + fo2::kBatchVertexCount), nidx = *(WORD*)(b + fo2::kBatchIndexCount);
        int base = *(WORD*)(b + fo2::kBatchBaseVertex), first = *(WORD*)(b + fo2::kBatchFirstVertex);
        int start = *(WORD*)(b + fo2::kBatchFirstIndex);
        BYTE* ibo = *(BYTE**)(b + fo2::kBatchIndexBuffer);
        BYTE* vbo = *(BYTE**)(b + fo2::kBatchVertexBuffer);
        auto* ib = ibo ? *(IDirect3DIndexBuffer9**)(ibo + fo2::kIndexBufferD3D) : nullptr;
        auto* vb = vbo ? *(IDirect3DVertexBuffer9**)(vbo + fo2::kVertexBufferD3D) : nullptr;
        const BYTE* verts = *(const BYTE**)(b + fo2::kBatchVertices);
        if (!ib || nidx < 3 || count <= 0 || stride < 12 || (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP)) continue;
        D3DINDEXBUFFER_DESC idesc;
        if (FAILED(ib->GetDesc(&idesc)) || idesc.Format != D3DFMT_INDEX16 || (start + nidx) * 2u > idesc.Size) continue;
        void* vdata = nullptr;
        if (!verts)
        {
            D3DVERTEXBUFFER_DESC vdesc;
            if (!vb || FAILED(vb->GetDesc(&vdesc)) || (unsigned)(base + first + count) * stride > vdesc.Size ||
                FAILED(vb->Lock((base + first) * stride, count * stride, &vdata, D3DLOCK_READONLY)) || !vdata)
                continue;
            verts = (const BYTE*)vdata;
        }
        void* idata = nullptr;
        if (SUCCEEDED(ib->Lock(start * 2, nidx * 2, &idata, D3DLOCK_READONLY)) && idata)
        {
            const WORD* idx = (const WORD*)idata;
            auto at = [&](int i, float* out) {
                int k = idx[i] - first;
                if (k < 0 || k >= count) return false;
                const float* p = (const float*)(verts + k * stride);
                for (int j = 0; j < 3; j++) out[j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
                return true;
            };
            int n = prim == D3DPT_TRIANGLELIST ? nidx / 3 : nidx - 2;
            for (int t = 0; t < n; t++)
            {
                int i0 = prim == D3DPT_TRIANGLELIST ? 3 * t : t;
                if (idx[i0] == idx[i0 + 1] || idx[i0 + 1] == idx[i0 + 2] || idx[i0] == idx[i0 + 2]) continue;  // a strip's joints
                float v[9];
                if (!at(i0, v) || !at(i0 + 1, v + 3) || !at(i0 + 2, v + 6)) continue;
                tris.insert(tris.end(), v, v + 9);
                batchOf.push_back((int)(it - begin));
            }
            ib->Unlock();
        }
        if (vdata) vb->Unlock();
    }
}

// The cards' layout in the player's car (LayCockpitCards): on the band, a sphere round the driver's eyes.
struct CockpitLayout
{
    BYTE* car = nullptr;
    bool derby = false;           // laid out for a derby's HUD (kDerbyCards)
    bool on[kCardCount] = {};     // the card is in the car
    float corner[kCardCount][4][3];  // top left, top right, bottom left, bottom right, in the car's axes
    float centre[3];              // the band's middle, in the car's axes
    float radius = 0;             // its radius (metres)
    float view[kCardCount][4];    // each card on it: its left and right (azimuth), top and bottom (elevation), radians
};
CockpitLayout g_dash;

bool DerbyEvent()
{
    const BYTE* gf = *(const BYTE* const*)fo2::kGameFlow;
    return gf && *(const int*)(gf + fo2::kGfEventType) == 3;
}

// The cards as the layout has them (a race's or a derby's).
const CockpitCard* Cards() { return g_dash.derby ? kDerbyCards : kCockpitCards; }

// Cards placed by hand with the Touch controllers ([Test] MoveCards, MoveCockpitCards): each one's middle
// in the car's axes and its height (metres), kept for the car's model in scripts\FO2VR_cards.ini (a section
// for each model, named by its folder, "Car_18"; a key for each card: x y z height; and the band's radius).
// They stand in for the layout's places, seen from the band's middle the same.
struct CardPlace
{
    bool set = false;
    float mid[3] = {}, height = 0;
};
CardPlace g_places[kCardCount];
float g_bandRadius;  // the car model's band (metres), 0: the layout's (LayCockpitCards)
const char* const kCardNames[kCardCount] = { "gauges", "position", "lap", "map", "rival" };

std::wstring CardsFile() { return ModuleDir() + L"FO2VR_cards.ini"; }

// The car's model: the last part of its data folder ("Car_18").
std::wstring CarModel(BYTE* car)
{
    const char* folder = *(const char**)(car + fo2::kCarFolder);
    std::string s = folder ? folder : "";
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    return std::wstring(s.begin(), s.end());
}

void LoadCardPlaces(BYTE* car)
{
    std::wstring model = CarModel(car), file = CardsFile();
    int n = 0;
    for (int k = 0; k < kCardCount; k++)
    {
        CardPlace& p = g_places[k];
        p = CardPlace();
        wchar_t buf[128] = L"", name[16];
        swprintf(name, 16, L"%hs", kCardNames[k]);
        GetPrivateProfileStringW(model.c_str(), name, L"", buf, 128, file.c_str());
        p.set = swscanf(buf, L"%f %f %f %f", &p.mid[0], &p.mid[1], &p.mid[2], &p.height) == 4 && p.height > 0.005f;
        n += p.set;
    }
    wchar_t buf[32] = L"";
    GetPrivateProfileStringW(model.c_str(), L"radius", L"", buf, 32, file.c_str());
    g_bandRadius = std::clamp((float)_wtof(buf), 0.0f, 1.5f);
    if (n) Log("cockpit: %d cards placed by hand in %ls (%ls)", n, model.c_str(), file.c_str());
}

void SaveCardPlace(BYTE* car, int k)
{
    const CardPlace& p = g_places[k];
    std::wstring model = CarModel(car), file = CardsFile();
    wchar_t buf[128], name[16], carName[40];
    swprintf(name, 16, L"%hs", kCardNames[k]);
    swprintf(buf, 128, L"%.4f %.4f %.4f %.4f", p.mid[0], p.mid[1], p.mid[2], p.height);
    swprintf(carName, 40, L"%.32hs", (const char*)(car + fo2::kCarName));
    WritePrivateProfileStringW(model.c_str(), L"car", carName, file.c_str());
    WritePrivateProfileStringW(model.c_str(), name, p.set ? buf : nullptr, file.c_str());
    if (g_dash.radius > 0)
    {
        // The band stays as it is now (pushed out or pulled in with a card).
        g_bandRadius = g_dash.radius;
        swprintf(buf, 128, L"%.4f", g_bandRadius);
        WritePrivateProfileStringW(model.c_str(), L"radius", buf, file.c_str());
    }
    if (p.set)
        Log("cockpit: %s placed by hand in %ls at %.3f %.3f %.3f, %.3f m high (the band %.3f m round the driver's eyes)", kCardNames[k],
            model.c_str(), p.mid[0], p.mid[1], p.mid[2], p.height, g_dash.radius);
    else
        Log("cockpit: %s back to the layout in %ls", kCardNames[k], model.c_str());
}

// The band (see CockpitLayout): a point on it from its middle at (azimuth, elevation), r metres out; and a
// point's azimuth, elevation and distance. The car's axes: x right, y up, z forward.
void BandPoint(float az, float el, float r, float* out)
{
    const float dir[3] = { cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az) };
    for (int i = 0; i < 3; i++) out[i] = g_dash.centre[i] + r * dir[i];
}

void BandAngles(const float* p, float& az, float& el, float& dist)
{
    float d[3] = { p[0] - g_dash.centre[0], p[1] - g_dash.centre[1], p[2] - g_dash.centre[2] };
    float flat = sqrtf(d[0] * d[0] + d[2] * d[2]);
    az = atan2f(d[0], d[2]), el = atan2f(d[1], flat), dist = sqrtf(flat * flat + d[1] * d[1]);
}

// A card on the band: its corners from its place there (view).
void BandCorners(int k)
{
    const float* v = g_dash.view[k];
    BandPoint(v[0], v[2], g_dash.radius, g_dash.corner[k][0]);
    BandPoint(v[1], v[2], g_dash.radius, g_dash.corner[k][1]);
    BandPoint(v[0], v[3], g_dash.radius, g_dash.corner[k][2]);
    BandPoint(v[1], v[3], g_dash.radius, g_dash.corner[k][3]);
}

// A card onto the band from its middle and height (metres where it is): in the same direction and as
// large as seen from the band's middle.
void BendCard(int k, const float* mid, float height)
{
    float az, el, d;
    BandAngles(mid, az, el, d);
    d = std::max(d, 0.05f);
    float ah = 2.0f * atanf(0.5f * height / d), aw = 2.0f * atanf(0.5f * height * Cards()[k].Aspect() / d);
    float* v = g_dash.view[k];
    v[0] = az - 0.5f * aw, v[1] = az + 0.5f * aw, v[2] = el + 0.5f * ah, v[3] = el - 0.5f * ah;
    BandCorners(k);
}

// The band moved out or in (radius metres): the cards stay as seen from its middle; those placed by hand
// keep their places on it.
void SetBandRadius(float radius)
{
    g_dash.radius = radius;
    for (int k = 0; k < kCardCount; k++)
    {
        if (!g_dash.on[k]) continue;
        BandCorners(k);
        if (!g_places[k].set) continue;
        const float* v = g_dash.view[k];
        BandPoint(0.5f * (v[0] + v[1]), 0.5f * (v[2] + v[3]), radius, g_places[k].mid);
        g_places[k].height = 2.0f * radius * tanf(0.5f * (v[2] - v[3]));
    }
}

// A card placed by hand: on the band.
void PlaceCard(int k)
{
    const CardPlace& p = g_places[k];
    BendCard(k, p.mid, p.height);
    g_dash.on[k] = true;
}

// A ray against a triangle (9 floats): the distance along d (a unit vector) from o, or -1.
float RayTriangle(const float* o, const float* d, const float* t)
{
    float e1[3] = { t[3] - t[0], t[4] - t[1], t[5] - t[2] }, e2[3] = { t[6] - t[0], t[7] - t[1], t[8] - t[2] };
    float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (fabsf(det) < 1e-12f) return -1.0f;
    float inv = 1.0f / det;
    float s[3] = { o[0] - t[0], o[1] - t[1], o[2] - t[2] };
    float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
    float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
}

// The car's body and the steering wheel's hub as its first layout read them, before the start (the body
// whole, the wheel straight).
struct CabinMeshes
{
    BYTE* car = nullptr;
    std::vector<float> body;
    float hub[3] = {};
} g_cabin;

// The cards laid out in a car (see CockpitCard), once it has its driver's eyes: the band's middle there (as
// the headset has them when centred, moved by [VR] CameraOffset; kept in the car, not following the head),
// the layout turned to the wheel's hub as seen from it, the band as far out as in the Venom unless the car's
// body (its dashboard, a door) is nearer at a card: then that near, so that no card sinks into it (where the
// body is nearer than 0.45 m, a pillar, the card may be hidden behind it rather than the band brought in so
// far). The cards placed by hand ([Test] MoveCards) stand in for theirs, on the band as far out as it was then.
void LayCockpitCards(BYTE* car, BYTE* wheel)
{
    double t0 = NowSeconds();
    g_dash = CockpitLayout();
    g_dash.car = car;
    g_dash.derby = DerbyEvent();
    const float* c = (const float*)(car + fo2::kCarMatrix);
    if (g_cabin.car != car)
    {
        g_cabin = CabinMeshes();
        g_cabin.car = car;
        std::vector<int> batches;
        ReadMesh(car, *(BYTE**)(car + fo2::kCarBody), g_cabin.body, batches);
        const float* w = (const float*)(wheel + fo2::kNodeWorld) + 12;
        for (int i = 0; i < 3; i++) g_cabin.hub[i] = (w[0] - c[12]) * c[4 * i] + (w[1] - c[13]) * c[4 * i + 1] + (w[2] - c[14]) * c[4 * i + 2];
    }
    const float* seat = (const float*)(car + fo2::kCarDriverLoc);
    float eyes[3];
    DriverEyeFromSeat(car, eyes);
    for (int i = 0; i < 3; i++) g_dash.centre[i] = seat[i] + eyes[i] + g_cfg.vrCameraOffset[i];
    float hubAz, hubEl, hubDist;
    BandAngles(g_cabin.hub, hubAz, hubEl, hubDist);
    constexpr float kDeg = 3.14159265f / 180.0f;
    const std::vector<float>& body = g_cabin.body;
    float radius = g_bandRadius > 0 ? g_bandRadius : kCardsBand;
    int nearest = -1;
    for (int k = 0; k < kCardCount; k++)
    {
        const CockpitCard& card = Cards()[k];
        float* v = g_dash.view[k];
        v[0] = hubAz + card.left * kDeg, v[1] = hubAz + card.right * kDeg, v[2] = hubEl + card.top * kDeg, v[3] = hubEl + card.bottom * kDeg;
        g_dash.on[k] = true;
        if (g_bandRadius > 0) continue;
        // The body across the card: 3 by 3 rays from the band's middle.
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
            {
                float az = v[0] + (v[1] - v[0]) * a / 2, el = v[2] + (v[3] - v[2]) * b / 2;
                const float d[3] = { cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az) };
                for (size_t t = 0; t + 9 <= body.size(); t += 9)
                {
                    float h = RayTriangle(g_dash.centre, d, &body[t]);
                    if (h > 0.45f && h < radius + 0.02f) radius = h - 0.02f, nearest = k;
                }
            }
    }
    g_dash.radius = radius;
    int placed = 0;
    for (int k = 0; k < kCardCount; k++)
    {
        BandCorners(k);
        if (g_places[k].set) PlaceCard(k), placed++;
    }
    Log("cockpit: %ls (%.32s): the %s cards turned to the wheel's hub (%+.1f %+.1f deg from the driver's eyes at %.3f %.3f %.3f) on a band "
        "%.3f m round them%s%s (%d placed by hand, %.1f ms)", CarModel(car).c_str(), (const char*)(car + fo2::kCarName),
        g_dash.derby ? "derby's" : "race's", hubAz / kDeg, hubEl / kDeg,
        g_dash.centre[0], g_dash.centre[1], g_dash.centre[2], radius, nearest >= 0 ? ", as near as the body at the " : "",
        nearest >= 0 ? kCardNames[nearest] : "", placed, 1000.0 * (NowSeconds() - t0));
}

// The instruments the HUD draws into their own targets this frame (those shown in the car): set as its
// capture begins (BeginHudCapture). The eyes, rendered before it, show the frame before's.
bool g_hudRoute[kCardCount];

bool RouteHudCards()
{
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    bool shown = g_hud.cards[1] && CockpitCardsOn() && player && g_dash.car == *(BYTE**)(player + fo2::kPlayerCar), any = false;
    for (int k = 0; k < kCardCount; k++) any |= g_hudRoute[k] = shown && g_dash.on[k];
    return any;
}

bool HudCardsRouted()
{
    for (bool r : g_hudRoute)
        if (r) return true;
    return false;
}

UINT VertexCount(D3DPRIMITIVETYPE t, UINT n)
{
    return t == D3DPT_TRIANGLELIST ? 3 * n : t == D3DPT_LINELIST ? 2 * n : t == D3DPT_POINTLIST ? n : n + 2;
}

// The instrument whose place a box in the back buffer's pixels lies in (see CockpitCard), or -1. The
// rival's before the map's, whose place holds the rival's crash bonus too.
int CardOfBox(const ScreenBox& b)
{
    float x0 = b.x0 / g_hud.w, x1 = b.x1 / g_hud.w, y0 = b.y0 / g_hud.h, y1 = b.y1 / g_hud.h;
    for (int k : { kCardRival, kCardGauges, kCardPosition, kCardLap, kCardMap })
    {
        const CockpitCard& c = Cards()[k];
        if (x0 >= c.du0 && x1 <= c.du1 && y0 >= c.dv0 && y1 <= c.dv1) return k;
    }
    return -1;
}

// A draw that stays on the HUD panel though its middle lies in the place of an instrument shown in the car:
// logged (once for each place in the game's code that leads to it, at most 20), to find what else belongs
// with the instrument.
void LogStrayHudDraw(const void* stack, const ScreenBox& b, int sprites)
{
    static unsigned seen[20];
    static int count;
    if (count >= 20 || b.x0 > b.x1) return;
    float u = 0.5f * (b.x0 + b.x1) / g_hud.w, v = 0.5f * (b.y0 + b.y1) / g_hud.h;
    for (int k : { kCardRival, kCardGauges, kCardPosition, kCardLap, kCardMap })
    {
        const CockpitCard& c = Cards()[k];
        if (!g_hudRoute[k] || u < c.du0 || u > c.du1 || v < c.dv0 || v > c.dv1) continue;
        unsigned ret[3];
        prof::GameCallers(stack, ret);
        unsigned key = ret[0] * 31u + ret[1] * 7u + ret[2];
        for (int i = 0; i < count; i++)
            if (seen[i] == key) return;
        seen[count++] = key;
        Log("cockpit: a HUD draw over the %s's place stays on the panel: %.3f %.3f .. %.3f %.3f (%d sprites), from %08X < %08X < %08X",
            kCardNames[k], b.x0 / g_hud.w, b.y0 / g_hud.h, b.x1 / g_hud.w, b.y1 / g_hud.h, sprites, ret[0], ret[1], ret[2]);
        return;
    }
}

// A sprite's instrument by the game code that queues it (see SpriteQueue), else kNoOwner. The crash
// bonus goes to the rival's card: its icon and its word ("SLAM") grow out of their place as they come.
// The 2D particles go to the instrument they are by, the gauges' (the nitro's flames) or the rival's, else
// to the panel: they are drawn in batches by texture, whatever they come from.
int HudSpriteOwner(const void* stack, uintptr_t from, float x0, float y0, float x1, float y1)
{
    float u = 0.5f * (x0 + x1) / g_hud.w, v = 0.5f * (y0 + y1) / g_hud.h;
    if (from == fo2::kHudParticleSprite[0] || from == fo2::kHudParticleSprite[1])
        return v < 0.45f ? kOwnerPanel : u >= 0.6f ? kCardGauges : v >= 0.6f ? kCardRival : kOwnerPanel;
    // The big font's letters by the rival's card while racing: the crash bonus's word ("SLAM", "POWER HIT",
    // as it comes in large) and its stars, to the right of the rival's name (the pause menu's are there too).
    if ((from == fo2::kHudBigTextSprite[0] || from == fo2::kHudBigTextSprite[1]) && u < 0.65f && v > 0.7f && g_framesWithoutStep < 2)
        return kCardRival;
    if (from == fo2::kHudTurnedSprite)
    {
        unsigned ret[3];  // the turned sprite's own caller
        prof::GameCallers(stack, ret);
        if (ret[0] == fo2::kHudTurnedSprite && ret[1] >= fo2::kHudCrashBonus && ret[1] < fo2::kHudCrashBonusEnd) return kCardRival;
    }
    return kNoOwner;
}

IDirect3DSurface9* CurrentTarget(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* rt = nullptr;
    dev->GetRenderTarget(0, &rt);
    if (rt) rt->Release();
    return rt;
}

// A draw of the HUD's other than its sprites' (vertices from v, `stride` apart; null: from stream 0), the
// vertices from `first`: the instrument whose place it lies in, when that one is shown in the car; else -1.
// These (the map's disc, a model) have their positions in the screen's pixels first in the vertex: their
// box is read once for each place in the game's code that draws them (reading the vertices back costs a
// wait for the GPU).
int HudCardOf(IDirect3DDevice9* dev, const void* v, UINT stride, UINT first, UINT count)
{
    if (!g_hud.active || count == 0 || !HudCardsRouted() || CurrentTarget(dev) != g_hud.rt) return -1;
    DWORD fvf = 0;
    dev->GetFVF(&fvf);
    if (!(fvf & D3DFVF_POSITION_MASK)) return -1;  // (a declaration of its own: not the HUD's)
    unsigned ret[3];
    prof::GameCallers(_AddressOfReturnAddress(), ret);
    unsigned key = ret[0] ^ (ret[1] * 3u) ^ (ret[2] * 7u) ^ (count * 13u);
    static struct { unsigned key; ScreenBox box; } known[16];
    static int knownCount;
    int n = 0;
    while (n < knownCount && known[n].key != key) n++;
    ScreenBox box;
    if (n < knownCount)
        box = known[n].box;
    else
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        void* locked = nullptr;
        if (!v)
        {
            UINT offset = 0;
            D3DVERTEXBUFFER_DESC desc;
            if (FAILED(dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb) return -1;
            if (stride < 8 || FAILED(vb->GetDesc(&desc)) || offset + (first + count) * stride > desc.Size ||
                FAILED(vb->Lock(offset + first * stride, count * stride, &locked, D3DLOCK_READONLY)) || !locked)
            {
                vb->Release();
                return -1;
            }
            v = locked;
        }
        else
            v = (const BYTE*)v + first * stride;
        for (UINT i = 0; i < count; i++)
        {
            const float* p = (const float*)((const BYTE*)v + i * stride);
            box.Add(p[0], p[1]);
        }
        if (locked) vb->Unlock();
        if (vb) vb->Release();
        if (knownCount < 16) known[knownCount++] = { key, box };
    }
    int k = CardOfBox(box);
    if (k >= 0 && g_hudRoute[k]) return k;
    LogStrayHudDraw(_AddressOfReturnAddress(), box, 0);
    return -1;
}

// A draw into an instrument's target (the same size as the HUD's), and back.
template <class F>
HRESULT DrawToCard(IDirect3DDevice9* dev, int k, F draw)
{
    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    oSetRenderTarget(dev, 0, g_hud.cardRT[k == kCardRival]);
    dev->SetViewport(&vp);
    HRESULT hr = draw();
    oSetRenderTarget(dev, 0, g_hud.rt);
    dev->SetViewport(&vp);
    return hr;
}

// A batch of the HUD's screen sprites (see SpriteQueue), each where it belongs: the sprites with an
// owner (HudSpriteOwner) where that says, all the others together where their box lies (an element's
// pieces stay together); drawn in runs of sprites that go to the same place, in the queue's order.
HRESULT DrawHudBatch(IDirect3DDevice9* dev, const void* stack, D3DPRIMITIVETYPE t, INT base, UINT least, UINT count, UINT start, UINT n)
{
    const std::vector<QueuedSprite>& q = g_hudQueue.sprites;
    g_hudQueue.drawn = true;
    UINT m = (UINT)q.size();
    if (!g_hud.active || !HudCardsRouted() || !m || t != D3DPT_TRIANGLELIST || least || start || count != 4 * m || n != 2 * m ||
        CurrentTarget(dev) != g_hud.rt)
        return oDrawIndexedPrimitive(dev, t, base, least, count, start, n);
    ScreenBox rest;
    for (const QueuedSprite& s : q)
        if (s.owner == kNoOwner && s.x0 <= s.x1) rest.Add(s.x0, s.y0), rest.Add(s.x1, s.y1), rest.count++;
    int restCard = CardOfBox(rest);
    if (restCard >= 0 && !g_hudRoute[restCard]) restCard = -1;
    if (restCard < 0 && rest.count) LogStrayHudDraw(stack, rest, rest.count);
    auto place = [&](UINT i) {
        int o = q[i].owner;
        return o == kNoOwner ? restCard : o >= 0 && g_hudRoute[o] ? o : -1;
    };
    HRESULT hr = D3D_OK;
    for (UINT a = 0, b; a < m; a = b)
    {
        int k = place(a);
        for (b = a + 1; b < m && place(b) == k;) b++;
        auto draw = [&] { return oDrawIndexedPrimitive(dev, t, base, 4 * a, 4 * (b - a), 6 * a, 2 * (b - a)); };
        hr = k >= 0 ? DrawToCard(dev, k, draw) : draw();
    }
    return hr;
}

HRESULT __stdcall DrawPrimitiveHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT first, UINT n)
{
    int k = HudCardOf(dev, nullptr, 0, first, VertexCount(t, n));
    if (k < 0) return oDrawPrimitive(dev, t, first, n);
    return DrawToCard(dev, k, [&] { return oDrawPrimitive(dev, t, first, n); });
}

HRESULT __stdcall DrawIndexedPrimitiveHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT least, UINT count, UINT start, UINT n)
{
    if ((uintptr_t)_ReturnAddress() == fo2::kSpriteBatchDraw)
    {
        if (!g_depthReversed) return DrawHudBatch(dev, _AddressOfReturnAddress(), t, base, least, count, start, n);
        // The screen sprites' shader puts them at depth 0, whatever their z, and the engine tests them as asked (the sun's
        // glow and the lens flare less-or-equal: over everything). Reversed, 0 is the far end: they keep the game's test.
        DWORD test = D3DCMP_ALWAYS;
        dev->GetRenderState(D3DRS_ZFUNC, &test);
        oSetRenderState(dev, D3DRS_ZFUNC, ReversedDepthTest(test));
        HRESULT hr = DrawHudBatch(dev, _AddressOfReturnAddress(), t, base, least, count, start, n);
        oSetRenderState(dev, D3DRS_ZFUNC, test);
        return hr;
    }
    int k = base >= 0 ? HudCardOf(dev, nullptr, 0, base + least, count) : -1;
    if (k < 0) return oDrawIndexedPrimitive(dev, t, base, least, count, start, n);
    return DrawToCard(dev, k, [&] { return oDrawIndexedPrimitive(dev, t, base, least, count, start, n); });
}

HRESULT __stdcall DrawPrimitiveUPHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride)
{
    int k = stride >= 8 ? HudCardOf(dev, v, stride, 0, VertexCount(t, n)) : -1;
    if (k < 0) return oDrawPrimitiveUP(dev, t, n, v, stride);
    return DrawToCard(dev, k, [&] { return oDrawPrimitiveUP(dev, t, n, v, stride); });
}

HRESULT __stdcall DrawIndexedPrimitiveUPHook(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT least, UINT count, UINT n, const void* i,
                                             D3DFORMAT f, const void* v, UINT stride)
{
    int k = stride >= 8 ? HudCardOf(dev, v, stride, least, count) : -1;
    if (k < 0) return oDrawIndexedPrimitiveUP(dev, t, least, count, n, i, f, v, stride);
    return DrawToCard(dev, k, [&] { return oDrawIndexedPrimitiveUP(dev, t, least, count, n, i, f, v, stride); });
}

// Placing the cards by hand ([Test] MoveCards, the Touch controllers; for working out a layout): each
// controller in use (moved or touched in the last 6 s) points a ray, drawn into the eyes up to the card it
// points at (outlined) or 1 m out. Holding the trigger (or the grip) on a card carries it over the band where
// the ray meets it; the thumbstick up and down pushes the band out and pulls it in with all its cards, left
// and right makes the card smaller and larger. Let go, it stays there for the car's model (g_places). B or Y
// on a card gives it back to the layout. Once a frame, with the eyes' poses (the game camera's placement as
// they have it).
struct HandRay
{
    bool shown = false, held = false;
    float from[3] = {}, to[3] = {};  // the car's axes
    int card = -1;                   // the card it points at or carries
} g_rays[2];

// Where a ray (from o along the unit d, the car's axes) meets the band from inside (its far side; from
// outside its near one): the distance along it, or -1.
float RayBand(const float* o, const float* d)
{
    float oc[3] = { o[0] - g_dash.centre[0], o[1] - g_dash.centre[1], o[2] - g_dash.centre[2] };
    float b = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
    float c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - g_dash.radius * g_dash.radius;
    float disc = b * b - c;
    if (disc < 0) return -1.0f;
    float t0 = -b - sqrtf(disc), t1 = -b + sqrtf(disc);
    return t0 > 0.05f ? t0 : t1 > 0.05f ? t1 : -1.0f;
}

void MoveCockpitCards(const float* anchor)
{
    static struct Hold
    {
        int card = -1;
        float turn[2] = {};                  // its middle's azimuth and elevation from the point held
        bool down = false, button = false;
        float rest[3] = {};                  // where the controller was when last moved
        double until = 0;                    // in use until then
    } hold[2];
    static double last;
    double now = NowSeconds();
    float dt = (float)std::clamp(now - last, 0.0, 0.1);
    last = now;
    for (HandRay& r : g_rays) r = HandRay();
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    BYTE* wheel = car ? *(BYTE**)(car + fo2::kCarSteeringWheel) : nullptr;
    bool on = g_cfg.testMoveCards && wheel && g_dash.car == car && CockpitCardsOn() && !g_hudLeftOut && g_dash.radius > 0;
    const float* c = car ? (const float*)(car + fo2::kCarMatrix) : nullptr;
    for (int h = 0; h < 2; h++)
    {
        Hold& g = hold[h];
        xr::Controller in = xr::Hand(h);
        if (!on || !in.tracked)
        {
            if (on && g.card >= 0) SaveCardPlace(car, g.card);
            g.card = -1;
            g.down = g.button = false;
            continue;
        }
        // The ray in the world (as the eyes are placed), then in the car's axes.
        xr::EyePose aim = in.aim;
        for (int i = 0; i < 3; i++) aim.position[i] += g_cfg.testHeadOffset[i] + g_cfg.vrCameraOffset[i];
        float m[16], o[3], d[3];
        BuildEyeMatrix(anchor, aim, m);
        for (int i = 0; i < 3; i++)
        {
            o[i] = (m[12] - c[12]) * c[4 * i] + (m[13] - c[13]) * c[4 * i + 1] + (m[14] - c[14]) * c[4 * i + 2];
            d[i] = m[8] * c[4 * i] + m[9] * c[4 * i + 1] + m[10] * c[4 * i + 2];
        }
        float ld = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& x : d) x /= ld > 1e-6f ? ld : 1.0f;
        float stick[2];
        for (int i = 0; i < 2; i++) stick[i] = fabsf(in.stick[i]) < 0.15f ? 0.0f : in.stick[i];
        float moved = 0;
        for (int i = 0; i < 3; i++) moved += (o[i] - g.rest[i]) * (o[i] - g.rest[i]);
        if (in.trigger > 0.05f || in.grip > 0.05f || in.button || stick[0] || stick[1] || moved > 0.03f * 0.03f || g.card >= 0)
        {
            g.until = now + 6.0;
            memcpy(g.rest, o, sizeof(o));
        }
        // The card it points at: where the ray meets the band.
        int card = -1;
        float best = 3.0f, at[2] = {};  // (its azimuth and elevation there)
        {
            float t = RayBand(o, d), p[3], dist;
            for (int i = 0; i < 3; i++) p[i] = o[i] + t * d[i];
            if (t > 0 && t < best) BandAngles(p, at[0], at[1], dist);
            for (int k = 0; k < kCardCount && t > 0 && t < best && card < 0; k++)
            {
                const float* v = g_dash.view[k];
                if (g_dash.on[k] && g_hudRoute[k] && at[0] >= v[0] && at[0] <= v[1] && at[1] <= v[2] && at[1] >= v[3]) card = k, best = t;
            }
        }
        bool down = std::max(in.trigger, in.grip) > (g.down ? 0.3f : 0.6f);
        if (down && !g.down && card >= 0 && hold[1 - h].card != card)
        {
            // Taken: placed by hand from now on, where it is.
            CardPlace& pl = g_places[card];
            const float* v = g_dash.view[card];
            if (!pl.set)
            {
                BandPoint(0.5f * (v[0] + v[1]), 0.5f * (v[2] + v[3]), g_dash.radius, pl.mid);
                pl.height = 2.0f * g_dash.radius * tanf(0.5f * (v[2] - v[3]));
                pl.set = true;
            }
            g.card = card;
            g.turn[0] = 0.5f * (v[0] + v[1]) - at[0], g.turn[1] = 0.5f * (v[2] + v[3]) - at[1];
        }
        if (!down && g.card >= 0)
        {
            SaveCardPlace(car, g.card);
            g.card = -1;
        }
        g.down = down;
        if (g.card >= 0)
        {
            // Over the band where the ray meets it (as far out as the stick puts it).
            if (stick[1]) SetBandRadius(std::clamp(g_dash.radius * expf(0.5f * stick[1] * dt), 0.3f, 1.5f));
            CardPlace& pl = g_places[g.card];
            pl.height = std::clamp(pl.height * expf(1.0f * stick[0] * dt), 0.02f, 0.4f);
            float t = RayBand(o, d), p[3], az, el, dist;
            for (int i = 0; i < 3; i++) p[i] = o[i] + t * d[i];
            if (t > 0)
            {
                BandAngles(p, az, el, dist);
                BandPoint(az + g.turn[0], el + g.turn[1], g_dash.radius, pl.mid);
                best = t;
            }
            PlaceCard(g.card);
            card = g.card;
        }
        if (in.button && !g.button && card >= 0 && g.card < 0 && g_places[card].set)
        {
            g_places[card].set = false;
            SaveCardPlace(car, card);
            LayCockpitCards(car, wheel);
        }
        g.button = in.button;
        HandRay& r = g_rays[h];
        r.shown = now < g.until;
        r.held = g.card >= 0;
        r.card = card;
        for (int i = 0; i < 3; i++) r.from[i] = o[i] + 0.03f * d[i], r.to[i] = o[i] + (card >= 0 ? best : 1.0f) * d[i];
    }
}

// How full the particle lists are (see game.h kParticleViews): the most a list held in a log interval of frames and in
// how many frames it was full, for the first view (the race's camera; the eyes share it), for the log.
struct ParticleLists
{
    int most[3] = {}, full[3] = {}, frames = 0;
    double lastLog = 0;
} g_particleLists;

void NoteParticleLists()
{
    ParticleLists& p = g_particleLists;
    if (*(const unsigned*)fo2::kParticleViewCount == 0) return;
    const int offsets[3] = { 0x10, 0x28, 0x1C };  // count, capacity: the main list, the two smaller ones
    for (int i = 0; i < 3; i++)
    {
        const int* list = (const int*)(fo2::kParticleViews + offsets[i]);
        p.most[i] = std::max(p.most[i], list[0]);
        p.full[i] += list[0] >= list[1];
    }
    p.frames++;
    double now = NowSeconds();
    if (now - p.lastLog < g_cfg.logIntervalSeconds) return;
    if (p.lastLog > 0)
    {
        const int* l = (const int*)fo2::kParticleViews;
        Log("particles: the lists held at most %d of %d, %d of %d, %d of %d; full in %d, %d, %d of %d frames", p.most[0], l[0x14 / 4], p.most[1],
            l[0x2C / 4], p.most[2], l[0x20 / 4], p.full[0], p.full[1], p.full[2], p.frames);
    }
    p = ParticleLists();
    p.lastLog = now;
}

// The markers over the rivals ([VR]; see game.h kMarkers): the game draws them on its HUD where the cars are on the
// monitor's picture, so in the headset they stood on the HUD panel, and some in the cockpit's cards. They are drawn
// over the cars in the eyes instead. The game picks the rivals and their colours as it draws its HUD: those within its
// camera's field of view (95 degrees) of where the camera looks. It marks them twice, the camera turned as the left
// eye looks and then turned round (MarkersBegin, MarkersTurn), which takes in every rival within reach whichever way
// the head turns; the triangles are noted, not drawn (MarkerDrawn), and the eyes draw the last frame's over the cars
// where they are now, as large as the game draws them on the monitor (some 4 degrees across near by, shrinking to
// nothing at kMarkerReach).
float Dot3(const float* a, const float* b);

struct RivalMarker
{
    BYTE* car;
    DWORD colour;
};
struct MarkerState
{
    std::vector<RivalMarker> next, shown;  // being noted by this frame's HUD, and the last frame's
    double shownAt = 0;                    // when they were noted
    float eye[16] = {};                    // the left eye's matrix in the world (rows right, up, forward, place)
    bool eyeValid = false;
    float saved[2][16] = {};               // the camera's placement while the HUD marks the cars (kCamMatrixCopy2, kCamMatrix)
    BYTE* cam = nullptr;
} g_markers;
constexpr float kMarkerAbove = 1.4f;  // metres over the car's middle
constexpr float kMarkerWide = 4.0f;   // degrees across, near by
constexpr float kMarkerTall = 0.4f;   // its height, of its width

static uintptr_t g_markersFn = fo2::kMarkers, g_markerDrawFn = fo2::kMarkerDraw;
static BYTE* g_markersCam;  // (the camera, for the second pass's esi)

static void __stdcall MarkersBegin(BYTE* cam)
{
    MarkerState& m = g_markers;
    m.next.clear();
    m.cam = nullptr;
    if (!g_hud.active || !m.eyeValid || !cam) return;
    m.cam = g_markersCam = cam;
    float* placement[2] = { (float*)(cam + fo2::kCamMatrixCopy2), (float*)(cam + fo2::kCamMatrix) };
    for (int i = 0; i < 2; i++)
    {
        memcpy(m.saved[i], placement[i], sizeof(m.saved[i]));
        memcpy(placement[i], m.eye, sizeof(m.eye));
    }
}

// The camera turned round (about its up), to mark the rivals behind the eyes too: 1 when it was.
static int __stdcall MarkersTurn()
{
    MarkerState& m = g_markers;
    if (!m.cam) return 0;
    float turned[16];
    memcpy(turned, m.eye, sizeof(turned));
    for (int i = 0; i < 3; i++) turned[i] = -m.eye[i], turned[8 + i] = -m.eye[8 + i];
    memcpy(m.cam + fo2::kCamMatrixCopy2, turned, sizeof(turned));
    memcpy(m.cam + fo2::kCamMatrix, turned, sizeof(turned));
    return 1;
}

static void __stdcall MarkersEnd()
{
    MarkerState& m = g_markers;
    if (!m.cam) return;
    memcpy(m.cam + fo2::kCamMatrixCopy2, m.saved[0], sizeof(m.saved[0]));
    memcpy(m.cam + fo2::kCamMatrix, m.saved[1], sizeof(m.saved[1]));
    m.cam = nullptr;
    m.shown.swap(m.next);
    m.shownAt = NowSeconds();
}

// At kMarkersCall, instead of kMarkers: the same three arguments, the camera in esi, every register as the game left it;
// in the headset called again with the camera turned round.
static __declspec(naked) void MarkersHook()
{
    __asm {
        pushad
        push esi
        call MarkersBegin
        popad
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        call dword ptr [g_markersFn]
        pushad
        call MarkersTurn
        mov [esp + 28], eax
        popad
        test eax, eax
        jz marked
        push esi
        mov esi, dword ptr [g_markersCam]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        call dword ptr [g_markersFn]
        pop esi
    marked:
        pushad
        call MarkersEnd
        popad
        ret 12
    }
}

// A rival's triangle (its player in ebx): noted while the camera marks the eyes' cars, and left out of the HUD.
static int __stdcall MarkerDrawn(BYTE* player, const DWORD* colour)
{
    if (!g_markers.cam || !player || !colour) return 1;
    BYTE* car = *(BYTE**)(player + fo2::kPlayerCar);
    bool noted = !car;
    for (const RivalMarker& r : g_markers.next) noted |= r.car == car;  // (marked by both passes)
    if (!noted) g_markers.next.push_back({ car, *colour });
    return 0;
}

// At kMarkerDrawCall, instead of kMarkerDraw (__thiscall, five arguments, the colour's address the fifth).
static __declspec(naked) void MarkerDrawHook()
{
    __asm {
        push ecx
        push dword ptr [esp + 24]
        push ebx
        call MarkerDrawn
        pop ecx
        test eax, eax
        jz leftOut
        jmp dword ptr [g_markerDrawFn]
    leftOut:
        ret 20
    }
}

// The Steam release's fo2_zoom.dll hooks the triangle's call (kMarkerDrawCall) for its widescreen HUD: a jump into code
// of its own that calls kMarkerDraw as the game would, every register as it was. The triangle is then taken at
// kMarkerDraw's start (it draws others of the HUD's too, which MarkerDrawn lets through): its first two instructions
// (sub esp, 8; movzx eax, byte [ecx+1Ch]) run in MarkerDrawRest before the rest of it.
static uintptr_t g_markerDrawResume = fo2::kMarkerDraw + 7;

static __declspec(naked) void MarkerDrawRest()
{
    __asm {
        sub esp, 8
        movzx eax, byte ptr [ecx + 0x1C]
        jmp dword ptr [g_markerDrawResume]
    }
}

static bool HookMarkerDraw()
{
    const BYTE* site = (const BYTE*)fo2::kMarkerDrawCall;
    if (site[0] == 0xE8 && fo2::kMarkerDrawCall + 5 + *(const int32_t*)(site + 1) == fo2::kMarkerDraw)
        return PatchCall(fo2::kMarkerDrawCall, fo2::kMarkerDraw, (const void*)&MarkerDrawHook);
    static const BYTE start[7] = { 0x83, 0xEC, 0x08, 0x0F, 0xB6, 0x41, 0x1C };
    BYTE jump[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    int32_t rel = (int32_t)((uintptr_t)&MarkerDrawHook - (fo2::kMarkerDraw + 5));
    memcpy(jump + 1, &rel, 4);
    if (!PatchCode(fo2::kMarkerDraw, start, jump, sizeof(jump))) return false;
    g_markerDrawFn = (uintptr_t)&MarkerDrawRest;
    Log("markers: the triangle's call is another's (fo2_zoom.dll's) - the triangles are taken where they are drawn");
    return true;
}

// In an eye: the last frame's markers over their cars, over everything as the game draws them on the monitor (with the
// eye's depth, the cockpit's pillars, doors and roof hid most of them).
void DrawMarkers(IDirect3DDevice9* dev, BYTE* cam)
{
    MarkerState& m = g_markers;
    if (m.shown.empty() || NowSeconds() - m.shownAt > 0.1 || g_hudLeftOut) return;  // (Hud=2 leaves them out with the HUD)
    const float* e = (const float*)(cam + fo2::kCamMatrix);  // the eye's
    float n = *(const float*)(cam + fo2::kCamNear);
    float reach = std::max(*(const float*)fo2::kMarkerReach, 1.0f);
    const xr::EyeFov& fov = g_vrEye.fov;
    float W = (float)g_vrt.w, H = (float)g_vrt.h;
    struct V { float x, y, z, rhw; DWORD colour; };
    std::vector<V> tris;
    for (const RivalMarker& r : m.shown)
    {
        const float* c = (const float*)(r.car + fo2::kCarMatrix) + 12;
        float d[3] = { c[0] - e[12], c[1] + kMarkerAbove - e[13], c[2] - e[14] };
        float x = Dot3(d, e), y = Dot3(d, e + 4), z = Dot3(d, e + 8);
        if (z < n * 1.01f) continue;
        float wide = 2.0f * tanf(0.5f * kMarkerWide * 3.14159265f / 180.0f) * (1.0f - sqrtf(Dot3(d, d)) / reach);
        if (wide <= 0.0f) continue;
        // In the eye's picture: its middle, half its width and height (pixels), pointing down.
        float px = (x / z - fov.left) / (fov.right - fov.left) * W - 0.5f, py = (fov.up - y / z) / (fov.up - fov.down) * H - 0.5f;
        float hw = 0.5f * wide / (fov.right - fov.left) * W, hh = 0.5f * wide * kMarkerTall / (fov.up - fov.down) * H;
        float depth = 0.0f, rhw = 1.0f / z;
        // A dark rim (the triangle a little larger behind), then the triangle in its colour.
        DWORD rim = (r.colour & 0xFF000000) | ((r.colour >> 2) & 0x3F3F3F);
        float grow = std::max(1.5f, 0.12f * hw);
        tris.push_back({ px - hw - grow, py - hh - 0.5f * grow, depth, rhw, rim });
        tris.push_back({ px + hw + grow, py - hh - 0.5f * grow, depth, rhw, rim });
        tris.push_back({ px, py + hh + grow, depth, rhw, rim });
        tris.push_back({ px - hw, py - hh, depth, rhw, r.colour });
        tris.push_back({ px + hw, py - hh, depth, rhw, r.colour });
        tris.push_back({ px, py + hh, depth, rhw, r.colour });
    }
    if (tris.empty()) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(tris.size() / 3), tris.data(), sizeof(V));
    saved->Apply();
    saved->Release();
}

void DrawCockpitCards(IDirect3DDevice9* dev, BYTE* cam, int eye)
{
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    BYTE* wheel = car ? *(BYTE**)(car + fo2::kCarSteeringWheel) : nullptr;
    if (!wheel || !CockpitCardsOn() || g_hudLeftOut) return;  // (Hud=2 leaves them out with the panel)
    const float* c = (const float*)(car + fo2::kCarMatrix);  // the car's axes
    const float* e = (const float*)(cam + fo2::kCamMatrix);  // the eye's
    // Laid out once the car has its driver's eyes (from the driver's posed head), and for the event's HUD.
    if ((g_dash.car != car || g_dash.derby != DerbyEvent()) && car == g_eyeCar)
    {
        LoadCardPlaces(car);
        LayCockpitCards(car, wheel);
    }
    if (g_dash.car != car) return;
    float n = *(const float*)(cam + fo2::kCamNear), f = *(const float*)(cam + fo2::kCamFar);
    const xr::EyeFov& fov = g_vrEye.fov;
    float W = (float)g_vrt.w, H = (float)g_vrt.h;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    struct V { float x, y, z, rhw; DWORD colour; float u, v; };
    std::vector<V> verts;
    std::vector<WORD> tris[2];  // the cards of each target (g_hud.cards)
    // A point in the world into the eye's picture; false in front of the near plane.
    auto project = [&](const float* p, float u, float v, V& out) {
        float d[3] = { p[0] - e[12], p[1] - e[13], p[2] - e[14] };
        float x = dot(d, e), y = dot(d, e + 4), z = dot(d, e + 8);
        if (z < n) return false;
        float zd = std::max(z - kCardDepthPull, n * 1.001f);
        out = { (x / z - fov.left) / (fov.right - fov.left) * W - 0.5f, (fov.up - y / z) / (fov.up - fov.down) * H - 0.5f,
                EyeDepth(n, f, zd), 1.0f / z, 0xFFFFFFFF, u, v };
        return true;
    };
    auto toWorld = [&](const float* q, float* out) {
        for (int i = 0; i < 3; i++) out[i] = c[12 + i] + q[0] * c[i] + q[1] * c[4 + i] + q[2] * c[8 + i];
    };
    // A card on the band: its top and bottom at each of some columns along it, left to right (one every 2
    // degrees: bent as the band is); `cols` pieces.
    constexpr int kMaxColumns = 32;
    auto bandColumns = [&](int k, float (*p)[3], int& cols) {
        const float* v = g_dash.view[k];
        cols = std::clamp((int)ceilf((v[1] - v[0]) / (2.0f * 3.14159265f / 180.0f)), 1, kMaxColumns);
        for (int j = 0; j <= cols; j++)
        {
            float az = v[0] + (v[1] - v[0]) * j / cols, q[3];
            BandPoint(az, v[2], g_dash.radius, q);
            toWorld(q, p[2 * j]);
            BandPoint(az, v[3], g_dash.radius, q);
            toWorld(q, p[2 * j + 1]);
        }
    };
    for (int k = 0; k < kCardCount; k++)
    {
        const CockpitCard& card = Cards()[k];
        if (!g_dash.on[k] || !g_hudRoute[k]) continue;
        float p[2 * (kMaxColumns + 1)][3];  // top and bottom of each column
        int cols;
        bandColumns(k, p, cols);
        WORD first = (WORD)verts.size();
        bool in = true;
        for (int j = 0; j <= cols; j++)
        {
            float u = card.u0 + (card.u1 - card.u0) * j / cols;
            V top = {}, bottom = {};
            in &= project(p[2 * j], u, card.v0, top) && project(p[2 * j + 1], u, card.v1, bottom);
            verts.push_back(top), verts.push_back(bottom);
        }
        std::vector<WORD>& to = tris[k == kCardRival];
        for (int j = 0; j < cols && in; j++)
        {
            WORD a = (WORD)(first + 2 * j);
            to.insert(to.end(), { a, (WORD)(a + 2), (WORD)(a + 1), (WORD)(a + 2), (WORD)(a + 3), (WORD)(a + 1) });
        }
    }
    // The controllers' rays and the outline of the card each points at (MoveCockpitCards), over all.
    std::vector<V> lines;
    auto line = [&](const float* a, const float* b, DWORD colour, float width, bool world = false) {
        float pa[3], pb[3], za = 0, zb = 0, nearZ = n * 1.01f;
        if (world)
            memcpy(pa, a, sizeof(pa)), memcpy(pb, b, sizeof(pb));
        else
            toWorld(a, pa), toWorld(b, pb);
        for (int i = 0; i < 3; i++) za += (pa[i] - e[12 + i]) * e[8 + i], zb += (pb[i] - e[12 + i]) * e[8 + i];
        if (za < nearZ && zb < nearZ) return;
        float* behind = za < nearZ ? pa : zb < nearZ ? pb : nullptr;  // brought up to the near plane
        float t = behind == pa ? (nearZ - za) / (zb - za) : (nearZ - zb) / (za - zb);
        if (behind)
            for (int i = 0; i < 3; i++) behind[i] += ((behind == pa ? pb : pa)[i] - behind[i]) * t;
        V va, vb;
        if (!project(pa, 0, 0, va) || !project(pb, 0, 0, vb)) return;
        float dx = vb.x - va.x, dy = vb.y - va.y, l = sqrtf(dx * dx + dy * dy);
        if (l < 0.5f) return;
        float ox = -dy / l * 0.5f * width, oy = dx / l * 0.5f * width;
        V q[4] = { va, va, vb, vb };
        q[0].x += ox, q[0].y += oy, q[1].x -= ox, q[1].y -= oy, q[2].x += ox, q[2].y += oy, q[3].x -= ox, q[3].y -= oy;
        for (V& x : q) x.colour = colour;
        lines.insert(lines.end(), { q[0], q[1], q[2], q[1], q[3], q[2] });
    };
    for (const HandRay& r : g_rays)
    {
        if (!r.shown) continue;
        line(r.from, r.to, 0xB0B0B0B0, 3.0f);  // white (premultiplied)
        if (r.card < 0) continue;
        const DWORD blue = r.held ? 0xF0F0C040 : 0xE05AB4E0;  // light blue, or amber while carried
        float p[2 * (kMaxColumns + 1)][3];  // along the band
        int cols;
        bandColumns(r.card, p, cols);
        for (int j = 0; j < cols; j++) line(p[2 * j], p[2 * j + 2], blue, 4.0f, true), line(p[2 * j + 1], p[2 * j + 3], blue, 4.0f, true);
        line(p[0], p[1], blue, 4.0f, true), line(p[2 * cols], p[2 * cols + 1], blue, 4.0f, true);
    }
    if (tris[0].empty() && tris[1].empty() && lines.empty()) return;

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    dev->SetRenderTarget(0, g_vrt.rt[eye]);
    dev->SetDepthStencilSurface(g_vrt.ds[eye]);
    D3DVIEWPORT9 vp = { 0, 0, g_vrt.w, g_vrt.h, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    for (int i = 0; i < 2; i++)
    {
        if (tris[i].empty()) continue;
        dev->SetTexture(0, g_hud.cards[i]);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(), (UINT)(tris[i].size() / 3), tris[i].data(), D3DFMT_INDEX16,
                                    verts.data(), sizeof(V));
    }
    if (!lines.empty())
    {
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(lines.size() / 3), lines.data(), sizeof(V));
    }

    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    saved->Apply();
    saved->Release();
}

// The engine's back buffer size (DeviceD3d fields), which its post-processing uses for the
// target of its final passes.
int* EngineScreenSize() { return (int*)(*(BYTE**)fo2::kDeviceD3d + fo2::kDevWidth); }

// The eyes, the mirrors and the lens flare give the game's screen shape (kScreenShape) their own aspect ratio while they
// render: the engine culls with a frustum built from it. The Steam release's fo2_zoom.dll points its getter
// (kScreenShapeGet) at the monitor's resolution: the eyes were culled as 16:9, cutting off what lies above and below
// a 16:9 picture in the headset's taller one. While they render, the getter reads the screen shape again
// (OwnScreenShape), and what fo2_zoom.dll set afterwards. The getter is looked at as the eyes first render, when
// fo2_zoom.dll has long done its changes.
struct ScreenShapeGetter
{
    bool checked = false;
    uint32_t* operand[2] = {};  // the getter's two addresses, when another's (null: the game's own)
    uint32_t theirs[2] = {};
} g_shapeGetter;

void CheckScreenShapeGetter()
{
    BYTE* get = (BYTE*)fo2::kScreenShapeGet;
    if (get[0] != 0xD9 || get[1] != 0x05 || get[0x10] != 0xD9 || get[0x11] != 0x05)
    {
        Log("screen shape: its getter differs - the eyes are culled by the monitor's aspect ratio");
        return;
    }
    uint32_t* operand[2] = { (uint32_t*)(get + 2), (uint32_t*)(get + 0x12) };
    if (*operand[0] == fo2::kScreenShape && *operand[1] == fo2::kScreenShape + 4) return;
    DWORD old;
    if (!VirtualProtect(get, 0x16, PAGE_EXECUTE_READWRITE, &old)) return;
    for (int i = 0; i < 2; i++)
    {
        g_shapeGetter.operand[i] = operand[i];
        g_shapeGetter.theirs[i] = *operand[i];
    }
    Log("screen shape: the getter reads %08X (fo2_zoom.dll's) - the eyes and mirrors set it back while they render",
        (unsigned)g_shapeGetter.theirs[0]);
}

void OwnScreenShape(bool own)
{
    ScreenShapeGetter& g = g_shapeGetter;
    if (!g.checked)
    {
        g.checked = true;
        CheckScreenShapeGetter();
    }
    if (!g.operand[0]) return;
    *g.operand[0] = own ? fo2::kScreenShape : g.theirs[0];
    *g.operand[1] = own ? fo2::kScreenShape + 4 : g.theirs[1];
    FlushInstructionCache(GetCurrentProcess(), (const void*)fo2::kScreenShapeGet, 0x16);
}

// The bound render target's size.
bool BoundTargetSize(IDirect3DDevice9* dev, UINT* w, UINT* h)
{
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt))) return false;
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    rt->Release();
    *w = d.Width;
    *h = d.Height;
    return true;
}

// A vertex in the render target's pixels; rhw 1 over its depth, for a picture laid on it in perspective.
struct ScreenVertex
{
    float x, y, z, rhw, u, v;
};

// Draws a list of triangles with a texture into the bound render target, opaque or as premultiplied
// alpha over what is there; outside the texture black (border) or its edge (clamp). Every device
// state is put back, so the engine's state cache stays valid.
void DrawScreenTriangles(IDirect3DDevice9* dev, IDirect3DTexture9* tex, const ScreenVertex* verts, UINT triangles, bool premultiplied,
                         bool border = false)
{
    UINT w, h;
    if (triangles == 0 || !BoundTargetSize(dev, &w, &h)) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;

    D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, border ? D3DTADDRESS_BORDER : D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, border ? D3DTADDRESS_BORDER : D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_BORDERCOLOR, 0xFF000000);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, premultiplied);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, verts, sizeof(ScreenVertex));
    saved->Apply();
    saved->Release();
}

// Draws a texture (or the part u0..u1, v0..v1 of it) over the whole bound render target, opaque
// or as premultiplied alpha over what is there.
void DrawScreenQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float u0, float v0, float u1, float v1, bool premultiplied)
{
    UINT w, h;
    if (!BoundTargetSize(dev, &w, &h)) return;
    float W = (float)w - 0.5f, H = (float)h - 0.5f;
    const ScreenVertex quad[6] = { { -0.5f, -0.5f, 0, 1, u0, v0 }, { W, -0.5f, 0, 1, u1, v0 }, { -0.5f, H, 0, 1, u0, v1 },
                                   { W, -0.5f, 0, 1, u1, v0 }, { W, H, 0, 1, u1, v1 },       { -0.5f, H, 0, 1, u0, v1 } };
    DrawScreenTriangles(dev, tex, quad, 2, premultiplied);
}

// Lays the screen fade's colour (alpha in the top byte) over a render target (the bound one when null).
// Over a picture it blends as the game's fade does; over the HUD (premultiplied colours over
// transparency, laid over the eyes later) it fades the HUD's own colours and adds the fade's colour
// only where the HUD covers, so that HUD and eyes together come out as the game's fade over both.
// The target's alpha is kept.
void FadeTarget(IDirect3DDevice9* dev, IDirect3DSurface9* target, DWORD colour, bool hud)
{
    IDirect3DSurface9* old = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &old))) return;
    IDirect3DSurface9* rt = target ? target : old;
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)))
    {
        old->Release();
        return;
    }
    if (target) dev->SetRenderTarget(0, target);

    DWORD a = colour >> 24;
    if (hud)  // premultiplied by its alpha, scaled by the HUD's own (DESTALPHA)
        colour = (a << 24) | ((((colour >> 16) & 0xFF) * a / 255) << 16) | ((((colour >> 8) & 0xFF) * a / 255) << 8) | ((colour & 0xFF) * a / 255);
    struct V { float x, y, z, rhw; DWORD c; };
    float W = (float)d.Width - 0.5f, H = (float)d.Height - 0.5f;
    const V quad[4] = { { -0.5f, -0.5f, 0, 1, colour }, { W, -0.5f, 0, 1, colour }, { -0.5f, H, 0, 1, colour }, { W, H, 0, 1, colour } };

    D3DVIEWPORT9 vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, hud ? D3DBLEND_DESTALPHA : D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    saved->Apply();
    saved->Release();
    if (target) dev->SetRenderTarget(0, old);
    old->Release();
}

// Shows the left eye on the monitor instead of rendering the scene a third time: a 16:9 crop
// around the eye's straight-ahead direction, drawn into the bound render target (the HUD goes
// over it afterwards).
void MirrorLeftEye(IDirect3DDevice9* dev, const xr::EyeFov& fov)
{
    // Crop in texture coordinates: 80% of the eye's width, centred on where it looks straight ahead.
    float cx = -fov.left / (fov.right - fov.left);
    float cy = fov.up / (fov.up - fov.down);
    float w = 0.8f;
    float h = w * (float)g_vrt.w / (float)g_vrt.h * (float)g_screenH / (float)std::max(g_screenW, 1u);
    float u0 = std::clamp(cx - 0.5f * w, 0.0f, 1.0f - w), v0 = std::clamp(cy - 0.5f * h, 0.0f, std::max(0.0f, 1.0f - h));
    DrawScreenQuad(dev, g_vrt.tex[0], u0, v0, u0 + w, v0 + h, false);
}

// ---- the monitor's steady view ([VR] Mirror=2) -------------------------------------------------------------------------
// What the headset shows, for a video: the left eye's picture seen from the eye by a camera that follows the head's turning
// smoothly (in two exponential steps of [VR] MonitorSmoothing/2 each: the head's small shakes are gone, its turns come
// out eased), its horizon level with the car's, aimed [VR] MonitorPitch degrees down, [VR] MonitorFov degrees across at
// the monitor's shape. It costs no render of its own: the eye's picture is wider and is cut from, turned (a grid over the
// monitor, each point's direction looked up in the eye); the eyes are rendered wider than the headset's picture for it
// (SteadyRoom: the left eye sees only some 40 degrees right of straight ahead). Near the edge of the eye's picture the
// camera is held back from it, so it eases into following the head as close as it must. The HUD's panel is drawn where it
// hangs in the headset (EndHudCapture); the cockpit's instruments are in the eye's picture already.

// An orientation: the unit quaternion (x, y, z, w) of the turn taking the game's axes (x right, y up, z forward) to a
// pose's, relative to the recenter origin.
struct Quat
{
    float x, y, z, w;
};

Quat AxesToQuat(const float* r, const float* u, const float* f)
{
    // m[i][j]: row i of the matrix whose columns are r, u and f.
    float m00 = r[0], m01 = u[0], m02 = f[0], m10 = r[1], m11 = u[1], m12 = f[1], m20 = r[2], m21 = u[2], m22 = f[2];
    float t = m00 + m11 + m22;
    if (t > 0.0f)
    {
        float s = 2.0f * sqrtf(1.0f + t);
        return { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s };
    }
    if (m00 > m11 && m00 > m22)
    {
        float s = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
        return { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
    }
    if (m11 > m22)
    {
        float s = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
        return { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
    }
    float s = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
    return { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
}

void QuatToAxes(const Quat& q, float* r, float* u, float* f)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    r[0] = 1.0f - 2.0f * (y * y + z * z), r[1] = 2.0f * (x * y + z * w), r[2] = 2.0f * (x * z - y * w);
    u[0] = 2.0f * (x * y - z * w), u[1] = 1.0f - 2.0f * (x * x + z * z), u[2] = 2.0f * (y * z + x * w);
    f[0] = 2.0f * (x * z + y * w), f[1] = 2.0f * (y * z - x * w), f[2] = 1.0f - 2.0f * (x * x + y * y);
}

// a after b.
Quat QuatMul(const Quat& a, const Quat& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

// The turn from a to b: its axis (unit) and angle (0 to pi).
float TurnBetween(const Quat& a, const Quat& b, float axis[3])
{
    Quat d = QuatMul(b, { -a.x, -a.y, -a.z, a.w });
    if (d.w < 0.0f) d = { -d.x, -d.y, -d.z, -d.w };
    float s = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (s < 1e-7f)
    {
        axis[0] = 1.0f, axis[1] = axis[2] = 0.0f;
        return 0.0f;
    }
    axis[0] = d.x / s, axis[1] = d.y / s, axis[2] = d.z / s;
    return 2.0f * atan2f(s, d.w);
}

// a turned about the axis by the angle.
Quat Turned(const Quat& a, const float axis[3], float angle)
{
    float s = sinf(0.5f * angle);
    Quat q = QuatMul({ axis[0] * s, axis[1] * s, axis[2] * s, cosf(0.5f * angle) }, a);
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return { q.x / n, q.y / n, q.z / n, q.w / n };
}

// The share t of the way from a to b.
Quat Slerp(const Quat& a, const Quat& b, float t)
{
    float axis[3], angle = TurnBetween(a, b, axis);
    return Turned(a, axis, t * angle);
}

// Looking along f (unit) with the horizon level: right across the car's floor. False looking straight up or down.
bool LevelAxes(const float* f, float* r, float* u)
{
    float n = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (n < 0.05f) return false;
    r[0] = f[2] / n, r[1] = 0.0f, r[2] = -f[0] / n;
    u[0] = f[1] * r[2] - f[2] * r[1], u[1] = f[2] * r[0] - f[0] * r[2], u[2] = f[0] * r[1] - f[1] * r[0];
    return true;
}

struct SteadyView
{
    xr::EyePose eye = {};  // the left eye's pose and field of view, as its picture was rendered (RenderVrEyes)
    xr::EyeFov eyeFov = {};
    bool valid = false;    // the camera's orientations follow the head (in a race)
    double time = 0;       // when they were last moved
    Quat q[2] = {};        // the two smoothing steps' orientations; the camera's is the second
    float tanX = 0, tanY = 0;  // half the view's width and height, as tangents
    float right[3] = {}, up[3] = {}, forward[3] = {};  // this frame's camera
    bool shown = false;    // the monitor shows it this frame (the HUD's panel goes on it at Present)
} g_steady;

// Whether the camera turned so sees inside the eye's picture only (its corners do: the picture's edges are straight lines in
// the eye's too).
bool SteadyFits(const Quat& q)
{
    const xr::EyePose& e = g_steady.eye;
    const xr::EyeFov& v = g_steady.eyeFov;
    float r[3], u[3], f[3];
    QuatToAxes(q, r, u, f);
    for (int c = 0; c < 4; c++)
    {
        float x = c & 1 ? g_steady.tanX : -g_steady.tanX, y = c & 2 ? g_steady.tanY : -g_steady.tanY;
        float d[3] = { x * r[0] + y * u[0] + f[0], x * r[1] + y * u[1] + f[1], x * r[2] + y * u[2] + f[2] };
        float ex = Dot3(d, e.right), ey = Dot3(d, e.up), ez = Dot3(d, e.forward);
        if (ez < 0.01f || ex < v.left * ez || ex > v.right * ez || ey < v.down * ez || ey > v.up * ez) return false;
    }
    return true;
}

// q held back from the eye's picture's edge: turned from `home` (which fits) by at most half of what fits that way as it
// is, and beyond that by less and less, never reaching the edge.
Quat SteadyContain(const Quat& home, const Quat& q)
{
    float axis[3], angle = TurnBetween(home, q, axis);
    if (angle < 1e-5f) return q;
    float lo = 0.0f, hi = 1.5f;  // radians
    for (int i = 0; i < 16; i++)
    {
        float mid = 0.5f * (lo + hi);
        if (SteadyFits(Turned(home, axis, mid)))
            lo = mid;
        else
            hi = mid;
    }
    float most = lo, ease = 0.5f * most;
    if (most <= 0.0f) return home;
    if (angle <= ease) return q;
    return Turned(home, axis, ease + (most - ease) * tanhf((angle - ease) / (most - ease)));
}

// The monitor's picture in a race (in RenderViews, after the eyes): the camera follows the head a frame further and the
// view is cut from the eye's picture.
void SteadyMonitorView(IDirect3DDevice9* dev)
{
    SteadyView& s = g_steady;
    UINT w, h;
    if (!BoundTargetSize(dev, &w, &h)) return;
    s.tanX = tanf(0.5f * std::clamp(g_cfg.vrMonitorFov, 20.0f, 150.0f) * 0.017453293f);
    s.tanY = s.tanX * (float)h / (float)w;

    // Where it would look: where the eye does, level, and down by [VR] MonitorPitch (the dashboard and the hands on the wheel
    // in view); if the eye's picture has no room for that (a view too wide, the head tilted far), as near it towards the
    // picture's middle as there is.
    const xr::EyePose& e = s.eye;
    const xr::EyeFov& v = s.eyeFov;
    float r[3], u[3], f[3], down = g_cfg.vrMonitorPitch * 0.017453293f;
    if (!LevelAxes(e.forward, r, u))
    {
        memcpy(r, e.right, sizeof(r));
        memcpy(u, e.up, sizeof(u));
    }
    for (int k = 0; k < 3; k++)
    {
        f[k] = e.forward[k] * cosf(down) - u[k] * sinf(down);
        u[k] = u[k] * cosf(down) + e.forward[k] * sinf(down);
    }
    Quat home = AxesToQuat(r, u, f);
    if (!SteadyFits(home))
    {
        float cx = 0.5f * (v.left + v.right), cy = 0.5f * (v.up + v.down);
        for (int i = 0; i < 3; i++) f[i] = cx * e.right[i] + cy * e.up[i] + e.forward[i];
        float n = sqrtf(Dot3(f, f));
        for (float& c : f) c /= n;
        Quat middle = LevelAxes(f, r, u) ? AxesToQuat(r, u, f) : home;
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 12; i++)
        {
            float mid = 0.5f * (lo + hi);
            if (SteadyFits(Slerp(home, middle, mid)))
                hi = mid;
            else
                lo = mid;
        }
        home = Slerp(home, middle, hi);
    }

    double now = NowSeconds(), dt = std::clamp(now - s.time, 0.0, 0.1);  // (a hitch of the game's: as a slow frame)
    s.time = now;
    if (!s.valid || g_cfg.vrMonitorSmoothing <= 0.0f)
        s.q[0] = s.q[1] = home;
    else
    {
        float a = 1.0f - expf(-(float)dt / (0.5f * g_cfg.vrMonitorSmoothing));
        s.q[0] = Slerp(s.q[0], home, a);
        s.q[1] = Slerp(s.q[1], s.q[0], a);
    }
    s.valid = true;
    s.q[0] = SteadyContain(home, s.q[0]);
    s.q[1] = SteadyContain(home, s.q[1]);
    QuatToAxes(s.q[1], s.right, s.up, s.forward);

    // A grid over the monitor; rhw is the depth along the eye's axis, so the picture is laid on it in the eye's perspective.
    constexpr int kGridX = 32, kGridY = 18;
    static ScreenVertex grid[kGridY + 1][kGridX + 1];
    static ScreenVertex tris[kGridX * kGridY * 6];
    for (int j = 0; j <= kGridY; j++)
        for (int i = 0; i <= kGridX; i++)
        {
            float x = (2.0f * i / kGridX - 1.0f) * s.tanX, y = (1.0f - 2.0f * j / kGridY) * s.tanY, d[3];
            for (int k = 0; k < 3; k++) d[k] = x * s.right[k] + y * s.up[k] + s.forward[k];
            float ex = Dot3(d, e.right), ey = Dot3(d, e.up), ez = std::max(Dot3(d, e.forward), 1e-3f);
            grid[j][i] = { (float)i * w / kGridX - 0.5f, (float)j * h / kGridY - 0.5f, 0.0f, ez,
                           (ex / ez - v.left) / (v.right - v.left), (v.up - ey / ez) / (v.up - v.down) };
        }
    int n = 0;
    for (int j = 0; j < kGridY; j++)
        for (int i = 0; i < kGridX; i++)
            for (int c : { 0, 1, 2, 1, 3, 2 }) tris[n++] = grid[j + (c >> 1)][i + (c & 1)];
    DrawScreenTriangles(dev, g_vrt.tex[0], tris, (UINT)n / 3, false, true);
    s.shown = true;
}

// The HUD's panel on the steady view, where it hangs in the headset: a grid over the panel, each point where the camera
// sees it (parts behind the camera left out).
void DrawSteadyPanel(IDirect3DDevice9* dev)
{
    const SteadyView& s = g_steady;
    UINT w, h;
    if (!BoundTargetSize(dev, &w, &h) || g_hud.w == 0) return;
    // The panel in front of the recenter origin (xr::PanelPose), seen from the eye where the head is: not moved by [VR]
    // CameraOffset or [Test] HeadOffset as the eyes are (the headset puts it against the head itself).
    float eye[3];
    for (int k = 0; k < 3; k++) eye[k] = s.eye.position[k] - g_cfg.vrCameraOffset[k] - g_cfg.testHeadOffset[k];
    float pw = g_cfg.vrHudWidth, ph = pw * (float)g_hud.h / (float)g_hud.w;
    constexpr int kGridX = 16, kGridY = 9;
    static ScreenVertex grid[kGridY + 1][kGridX + 1];
    static bool front[kGridY + 1][kGridX + 1];
    static ScreenVertex tris[kGridX * kGridY * 6];
    for (int j = 0; j <= kGridY; j++)
        for (int i = 0; i <= kGridX; i++)
        {
            float u = (float)i / kGridX, v = (float)j / kGridY;
            float p[3] = { (u - 0.5f) * pw - eye[0], g_cfg.vrHudHeight + (0.5f - v) * ph - eye[1], g_cfg.vrHudDistance - eye[2] };
            float x = Dot3(p, s.right), y = Dot3(p, s.up), z = Dot3(p, s.forward);
            front[j][i] = z > 0.05f;
            z = std::max(z, 0.05f);
            grid[j][i] = { (0.5f + 0.5f * x / z / s.tanX) * w - 0.5f, (0.5f - 0.5f * y / z / s.tanY) * h - 0.5f, 0.0f, 1.0f / z, u, v };
        }
    int n = 0;
    for (int j = 0; j < kGridY; j++)
        for (int i = 0; i < kGridX; i++)
        {
            if (!front[j][i] || !front[j][i + 1] || !front[j + 1][i] || !front[j + 1][i + 1]) continue;
            for (int c : { 0, 1, 2, 1, 3, 2 }) tris[n++] = grid[j + (c >> 1)][i + (c & 1)];
        }
    DrawScreenTriangles(dev, g_hud.tex, tris, (UINT)n / 3, true);
}

// Present: the game's drawing goes back to its own targets; the HUD is laid over the monitor's
// picture and copied to the headset's panel texture.
void EndHudCapture(IDirect3DDevice9* dev)
{
    g_hud.active = false;
    oSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    oSetRenderState(dev, D3DRS_COLORWRITEENABLE, g_hud.gameColorWrite);
    oSetRenderTarget(dev, 0, g_hud.gameRT);
    oSetDepthStencilSurface(dev, g_hud.gameDS);
    if (g_hud.gameRT) g_hud.gameRT->Release();
    if (g_hud.gameDS) g_hud.gameDS->Release();
    g_hud.gameRT = g_hud.gameDS = nullptr;
    DWORD world = FadeOver(g_fade[0], g_fade[1]);
    if (world >> 24) FadeTarget(dev, nullptr, world, false);
    if (g_fade[0] >> 24) FadeTarget(dev, g_hud.rt, g_fade[0], true);
    if (g_steady.shown)
    {
        // The steady view (Mirror=2): the panel where the headset shows it (if it does: Hud=2 leaves it out while racing).
        g_steady.shown = false;
        if (!g_hudLeftOut) DrawSteadyPanel(dev);
    }
    else
    {
        DrawScreenQuad(dev, g_hud.tex, 0.0f, 0.0f, 1.0f, 1.0f, true);
        // The monitor shows the cockpit's instruments too (in the headset they are in the car, not on the panel).
        if (HudCardsRouted())
            for (IDirect3DTexture9* card : g_hud.cards) DrawScreenQuad(dev, card, 0.0f, 0.0f, 1.0f, 1.0f, true);
    }
    IDirect3DSurface9* panel = nullptr;
    if (SUCCEEDED(g_hud.panel->GetSurfaceLevel(0, &panel)))
    {
        g_hud.captured = SUCCEEDED(oStretchRect(dev, g_hud.rt, nullptr, panel, nullptr, D3DTEXF_LINEAR));
        panel->Release();
    }
}

// ---- side mirrors ([VR] Mirrors) ---------------------------------------------------------------------
// The game has no mirrors: a car's side mirror is a housing whose flat back shows a texture of dark glass (see
// game.h, kCarSceneRoot). In the cockpit each mirror in view shows what is behind: the scene is rendered once more
// (RenderViews, as for an eye, before the eyes) from the eyes reflected in the mirror's plane, through the glass as
// through a window (the camera looks along the mirror's normal, its frustum off-centre through the glass, its near
// plane on the mirror, so that nothing behind the mirror shows), into a small target of its own. That picture is laid
// over the glass in each eye with the eye's depth (the hands and the pillars go in front of it), as the eye draws, under
// the car's windows; seen from the eyes it comes out turned left for right, as in a mirror. Mirrors=1 renders one picture from the eyes' middle for both eyes
// (it lies on the glass, flat); Mirrors=2 one from each eye for that eye (what the mirror shows stands at its depth
// behind the glass, as in a real one, for twice the cost). Moving the head moves what the mirror shows. The glass is
// convex as real side mirrors are ([VR] MirrorRadius): the picture takes in a wider view about the glass's middle,
// laid on the same glass, so what it shows is smaller, and with a picture for each eye it stands nearer behind the
// glass (the far away within about half the radius), as in a convex mirror. A mirror's
// render is kept small: the objects are picked for the glass alone (CullViewHook), the cars' shadows are the eyes' of
// the frame before (CarShadowHook), the world and the grass end where the game ends them, and there is no lens flare
// or post-processing (the eyes' takes the mirrors in).
//
// The models' glass is not set for the driver's eyes: the Chili's looks straight back and 17 degrees down, and
// shows the road beside the car. The mirror is set as a driver sets it, turned on its housing (once a car, for the
// eyes at rest): the car's side in sight at its inner edge, over kMirrorCarShare of its width, and its middle
// kMirrorAimDown below level. It stays so in the housing, and moves with it when the door swings open or the
// mirror is bent or knocked loose; only a mirror knocked off (its part's state, see game.h kCarParts) shows nothing.
//
// The face found for the glass is often the housing's whole back with its frame painted on it around the glass (the
// Sunray's), and the glass may be painted on past it, across the faces next to it (the Venom's housing's back is bent
// in three): where the glass is comes from the texture (MakeGlassMask), and the picture shows there alone.
// A mirror's picture has as many pixels as its glass takes up in the eye (kMirrorSharp times as many each way, in steps
// of kMirrorStep), up to kMirrorMax a side: its targets are that big, and a picture takes up their top left corner.
constexpr UINT kMirrorMax = 1024, kMirrorStep = 32, kMirrorLeast = 64;
constexpr float kMirrorSharp = 1.25f;
constexpr float kMirrorAimDown = 1.0f * 3.14159265f / 180.0f;
constexpr float kMirrorCarShare = 0.15f;
constexpr float kMirrorShade = 0.85f;       // a mirror gives back some 85% of the light
constexpr float kMirrorDepthPull = 0.003f;  // metres nearer in depth than the glass, not to fight with it
constexpr float kGlassBend = 0.707f;        // the cosine of the most a face next to the glass's may turn from it, its glass painted on
constexpr float kGlassReach = 0.6f;         // and how far off its texture may be, of the glass's face's in the texture

struct SideMirror
{
    BYTE* node = nullptr;
    BYTE* part = nullptr;  // its part (see game.h, kCarParts; null: not found)
    int state = 0;         // the part's state as last seen
    std::string name;
    BYTE* batch = nullptr;        // the batch of the housing's mesh that holds the glass
    std::vector<int> vertices;    // the glass's vertices: indices into the batch's (from its first vertex)
    std::vector<float> model;     // their places in the model's axes as found (before the crashes bend them)
    std::vector<float> skinUv;    // their texture coordinates (2 floats each)
    std::vector<WORD> triangles;  // the glass's triangles: indices into `vertices`
    int faceTriangles = 0;        // of them (3 a triangle) the face found for it, first; the rest the faces next to it
    float aim[3] = {};            // the mirror's normal as set (towards the eyes), in the model's axes
    float rest[3] = {};           // the glass's middle in the car's axes as found
    bool away = false;            // knocked off (or, its part not found, far from its place)
    const char* why = "";         // why it was not shown the last time (for the log)
    int whyLogged = 0;
    // Where the glass is on the face (MakeGlassMask): the housing's texture as the eyes draw it (until the mask is
    // made from it), and the mask (in its alpha) over the face's texture coordinates maskBox (u0, v0, u1, v1); no
    // mask: the whole face.
    IDirect3DTexture9* skin = nullptr;
    bool maskDone = false;
    IDirect3DTexture9* mask = nullptr;
    float maskBox[4] = {};
    // A picture for each eye (Mirrors=2; else the first for both).
    IDirect3DTexture9* tex[2] = {};
    IDirect3DSurface9* rt[2] = {};
    // This frame: whether each picture was rendered, and its size; the glass's vertices in the world (3 floats each),
    // and where each lies in the pictures' targets (u, v).
    bool shown[2] = {};
    UINT size[2][2] = {};
    std::vector<float> world, uv[2];
};

struct MirrorState
{
    BYTE* car = nullptr;  // the car the mirrors were looked for in
    std::vector<SideMirror> list;
    IDirect3DSurface9* ds = nullptr;    // their depth (they render one after the other)
    IDirect3DTexture9* filter[2] = {};  // the colour filter's textures of their size (see FilterTextures)
    bool failed = false;
    int pictures = 0;    // the targets made: 1 or 2 pictures a mirror
    Timing time;         // the frames' mirror work (CPU)
    int renders = 0;
    double pixels = 0;   // (and theirs, for the log)
    double lastLog = 0;
} g_mirrors;

void ReleaseMirrorTargets()
{
    for (SideMirror& m : g_mirrors.list)
        for (int i = 0; i < 2; i++)
        {
            if (m.rt[i]) m.rt[i]->Release();
            if (m.tex[i]) m.tex[i]->Release();
            m.rt[i] = nullptr;
            m.tex[i] = nullptr;
            m.shown[i] = false;
        }
    if (g_mirrors.ds) g_mirrors.ds->Release();
    g_mirrors.ds = nullptr;
    for (IDirect3DTexture9*& t : g_mirrors.filter)
    {
        if (t) t->Release();
        t = nullptr;
    }
    g_mirrors.pictures = 0;
}

// The car's mirrors forgotten (for another car's): their targets, textures and masks let go.
void ForgetMirrors()
{
    ReleaseMirrorTargets();
    for (SideMirror& m : g_mirrors.list)
    {
        if (m.skin) m.skin->Release();
        if (m.mask) m.mask->Release();
    }
    g_mirrors.list.clear();
    g_mirrorSkinWanted = false;
}

// The mirrors' targets: a texture for each picture, with mipmaps that make themselves (D3DUSAGE_AUTOGENMIPMAP): a
// mirror's picture is shown some 50 to 200 of the eye's pixels across, and without them it would shimmer.
bool EnsureMirrorTargets(IDirect3DDevice9* dev, int pictures)
{
    if (g_mirrors.failed) return false;
    if (g_mirrors.pictures != pictures) ReleaseMirrorTargets();
    g_mirrors.pictures = pictures;
    bool ok = true, made = false, mips = true;
    if (!g_mirrors.ds)
        ok = SUCCEEDED(dev->CreateDepthStencilSurface(kMirrorMax, kMirrorMax, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_mirrors.ds, nullptr));
    for (SideMirror& m : g_mirrors.list)
        for (int i = 0; i < pictures; i++)
        {
            if (!ok || m.tex[i]) continue;
            made = true;
            if (FAILED(dev->CreateTexture(kMirrorMax, kMirrorMax, 0, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                          &m.tex[i], nullptr)))
            {
                mips = false;
                ok = SUCCEEDED(dev->CreateTexture(kMirrorMax, kMirrorMax, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m.tex[i], nullptr));
            }
            ok = ok && SUCCEEDED(m.tex[i]->GetSurfaceLevel(0, &m.rt[i]));
        }
    if (!ok)
    {
        Log("mirrors: render targets %ux%u FAILED", kMirrorMax, kMirrorMax);
        ReleaseMirrorTargets();
        g_mirrors.failed = true;
        return false;
    }
    if (made)
    {
        PrepareScreenshots(dev, kMirrorMax, kMirrorMax, D3DFMT_A8R8G8B8, pictures * (int)g_mirrors.list.size());
        Log("mirrors: render targets %ux%u%s, %s", kMirrorMax, kMirrorMax, mips ? ", with mipmaps" : ", without mipmaps",
            pictures == 2 ? "a picture for each eye" : "one picture for both eyes");
    }
    return true;
}

// A mesh node's name (see game.h, kNodeName).
std::string NodeName(BYTE* node)
{
    const BYTE* s = node + fo2::kNodeName;
    const char* text = *(const unsigned*)(s + 0x14) >= 16 ? *(const char* const*)s : (const char*)s;
    return text ? std::string(text, strnlen(text, 64)) : std::string();
}

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Cross3(const float* a, const float* b, float* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool Normalize3(float* v)
{
    float l = sqrtf(Dot3(v, v));
    if (l < 1e-9f) return false;
    for (int i = 0; i < 3; i++) v[i] /= l;
    return true;
}

// The glass of a mirror's housing (its nearest model): the largest flat face that looks backwards and towards the
// driver's eyes (eyes: their place in the car's axes), with the faces next to it that its texture goes on across, and
// the mirror set for them as that face looks (body: the car's body's triangles in its axes, see ReadMesh). The
// housing's faces are told apart by their direction (within 5 degrees) and their plane (within 4 mm); a face looks the
// way its triangles' winding says (clockwise in front, as Direct3D draws them). out[8], for the log, of that face: the
// glass's width and height (metres), the way it looks (degrees out from straight
// back and up), how far the mirror was turned, how wide an angle it shows, how far out its middle looks and how far
// out the car's side reaches (degrees).
bool FindGlass(BYTE* car, SideMirror& m, const float* eyes, const std::vector<float>& body, float* out)
{
    BYTE* model = *(BYTE**)(m.node + fo2::kNodeModels);
    if (!model) return false;
    // The model's axes in the car's (as ReadMesh).
    const float* c = (const float*)(car + fo2::kCarMatrix);
    const float* w = (const float*)(m.node + fo2::kNodeWorld);
    float toCar[12];
    for (int r = 0; r < 4; r++)
    {
        float d[3] = { w[4 * r], w[4 * r + 1], w[4 * r + 2] };
        if (r == 3)
            for (int k = 0; k < 3; k++) d[k] -= c[12 + k];
        for (int i = 0; i < 3; i++) toCar[3 * r + i] = d[0] * c[4 * i] + d[1] * c[4 * i + 1] + d[2] * c[4 * i + 2];
    }
    auto inCar = [&](const float* p, float* q, bool direction) {
        for (int j = 0; j < 3; j++) q[j] = p[0] * toCar[j] + p[1] * toCar[3 + j] + p[2] * toCar[6 + j] + (direction ? 0.0f : toCar[9 + j]);
    };
    struct Face
    {
        float n[3], offset, area = 0;
        std::vector<int> tris;  // 3 vertex indices each
    };
    float bestArea = 0.0015f;  // at least some 5 by 3 cm
    BYTE** begin = *(BYTE***)(model + fo2::kModelBatches);
    BYTE** end = *(BYTE***)(model + fo2::kModelBatches + 4);
    for (BYTE** it = begin; it && it < end && it - begin < 16; it++)
    {
        BYTE* b = *it;
        if (!b) continue;
        DWORD prim = *(DWORD*)(b + fo2::kBatchPrimType);
        int stride = *(int*)(b + fo2::kBatchStride);
        int count = *(WORD*)(b + fo2::kBatchVertexCount), nidx = *(WORD*)(b + fo2::kBatchIndexCount);
        int base = *(WORD*)(b + fo2::kBatchBaseVertex), first = *(WORD*)(b + fo2::kBatchFirstVertex);
        int start = *(WORD*)(b + fo2::kBatchFirstIndex);
        BYTE* ibo = *(BYTE**)(b + fo2::kBatchIndexBuffer);
        BYTE* vbo = *(BYTE**)(b + fo2::kBatchVertexBuffer);
        auto* ib = ibo ? *(IDirect3DIndexBuffer9**)(ibo + fo2::kIndexBufferD3D) : nullptr;
        auto* vb = vbo ? *(IDirect3DVertexBuffer9**)(vbo + fo2::kVertexBufferD3D) : nullptr;
        const BYTE* cpu = *(const BYTE**)(b + fo2::kBatchVertices);
        if (!ib || nidx < 3 || count <= 0 || stride < 12 || (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP)) continue;
        D3DINDEXBUFFER_DESC idesc;
        if (FAILED(ib->GetDesc(&idesc)) || idesc.Format != D3DFMT_INDEX16 || (start + nidx) * 2u > idesc.Size) continue;
        // The vertices' places, the model's axes.
        std::vector<float> pos(3 * count);
        const BYTE* verts = cpu;
        void* vdata = nullptr;
        if (!verts)
        {
            D3DVERTEXBUFFER_DESC vdesc;
            if (!vb || FAILED(vb->GetDesc(&vdesc)) || (unsigned)(base + first + count) * stride > vdesc.Size ||
                FAILED(vb->Lock((base + first) * stride, count * stride, &vdata, D3DLOCK_READONLY)) || !vdata)
                continue;
            verts = (const BYTE*)vdata;
        }
        for (int k = 0; k < count; k++) memcpy(&pos[3 * k], verts + k * stride, 12);
        // And their texture coordinates (see game.h, kBatchFvf).
        DWORD fvf = *(DWORD*)(b + fo2::kBatchFvf);
        int uvAt = 12 + (fvf & D3DFVF_NORMAL ? 12 : 0) + (fvf & D3DFVF_DIFFUSE ? 4 : 0) + (fvf & D3DFVF_SPECULAR ? 4 : 0);
        std::vector<float> tex(2 * count);
        if ((fvf & D3DFVF_TEXCOUNT_MASK) && uvAt + 8 <= stride)
            for (int k = 0; k < count; k++) memcpy(&tex[2 * k], verts + k * stride + uvAt, 8);
        if (vdata) vb->Unlock();
        // The triangles, clockwise in front.
        std::vector<int> tris;
        void* idata = nullptr;
        if (FAILED(ib->Lock(start * 2, nidx * 2, &idata, D3DLOCK_READONLY)) || !idata) continue;
        const WORD* idx = (const WORD*)idata;
        int triCount = prim == D3DPT_TRIANGLELIST ? nidx / 3 : nidx - 2;
        for (int t = 0; t < triCount; t++)
        {
            int i0 = prim == D3DPT_TRIANGLELIST ? 3 * t : t;
            int v[3] = { idx[i0] - first, idx[i0 + 1] - first, idx[i0 + 2] - first };
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;  // a strip's joints
            if (prim == D3DPT_TRIANGLESTRIP && (t & 1)) std::swap(v[1], v[2]);
            if (std::min({ v[0], v[1], v[2] }) < 0 || std::max({ v[0], v[1], v[2] }) >= count) continue;
            tris.insert(tris.end(), v, v + 3);
        }
        ib->Unlock();
        // The flat faces, the largest triangles first.
        struct Tri
        {
            int at;
            float n[3], area;
        };
        std::vector<Tri> order;
        for (size_t t = 0; t < tris.size(); t += 3)
        {
            const float* p0 = &pos[3 * tris[t]];
            const float* p1 = &pos[3 * tris[t + 1]];
            const float* p2 = &pos[3 * tris[t + 2]];
            float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
            Tri x = { (int)t };
            Cross3(e1, e2, x.n);
            x.area = 0.5f * sqrtf(Dot3(x.n, x.n));
            if (x.area > 1e-8f && Normalize3(x.n)) order.push_back(x);
        }
        std::sort(order.begin(), order.end(), [](const Tri& a, const Tri& b) { return a.area > b.area; });
        std::vector<Face> faces;
        for (const Tri& x : order)
        {
            float offset = Dot3(x.n, &pos[3 * tris[x.at]]);
            Face* in = nullptr;
            for (Face& f : faces)
                if (Dot3(f.n, x.n) > 0.9962f && fabsf(offset - f.offset) < 0.004f) in = &f;
            if (!in)
            {
                faces.emplace_back();
                in = &faces.back();
                memcpy(in->n, x.n, sizeof(x.n));
                in->offset = offset;
            }
            in->area += x.area;
            in->tris.insert(in->tris.end(), &tris[x.at], &tris[x.at] + 3);
        }
        // Triangles (vertex indices, 3 a triangle) backwards (within some 70 degrees) and towards the eyes, in the car's
        // axes (normal: theirs in the model's): their normal, middle and the way to it from the eyes.
        auto towardsEyes = [&](const float* normal, const int* v, size_t count, float* n, float* mid, float* toGlass) {
            float p[3];
            inCar(normal, n, true);
            Normalize3(n);
            for (int k = 0; k < 3; k++) mid[k] = 0.0f;
            for (size_t i = 0; i < count; i++)
            {
                inCar(&pos[3 * v[i]], p, false);
                for (int k = 0; k < 3; k++) mid[k] += p[k] / count;
            }
            for (int k = 0; k < 3; k++) toGlass[k] = mid[k] - eyes[k];
            return n[2] <= -0.35f && Dot3(n, toGlass) < 0.0f;
        };
        // Whether a triangle has an edge of the triangles: the same two vertices, in place and texture coordinates (the
        // texture goes on across it).
        auto same = [&](int a, int b) {
            for (int k = 0; k < 3; k++)
                if (fabsf(pos[3 * a + k] - pos[3 * b + k]) > 1e-4f) return false;
            return fabsf(tex[2 * a] - tex[2 * b]) <= 1e-4f && fabsf(tex[2 * a + 1] - tex[2 * b + 1]) <= 1e-4f;
        };
        auto touches = [&](const int* t, const std::vector<int>& to) {
            for (int e = 0; e < 3; e++)
                for (size_t u = 0; u < to.size(); u += 3)
                    for (int g = 0; g < 3; g++)
                    {
                        int a0 = t[e], a1 = t[(e + 1) % 3], b0 = to[u + g], b1 = to[u + (g + 1) % 3];
                        if ((same(a0, b0) && same(a1, b1)) || (same(a0, b1) && same(a1, b0))) return true;
                    }
            return false;
        };
        for (const Face& f : faces)
        {
            if (f.area <= bestArea) continue;
            float n[3], mid[3], p[3], toGlass[3];
            if (!towardsEyes(f.n, f.tris.data(), f.tris.size(), n, mid, toGlass)) continue;
            bestArea = f.area;
            m.batch = b;
            m.vertices.clear();
            m.triangles.clear();
            m.model.clear();
            m.skinUv.clear();
            // The glass painted on past the face: the triangles joined to it (the texture going on across their edges)
            // that look within kGlassBend of its way, backwards towards the eyes, their texture next to the face's
            // (within kGlassReach of its size: the Venom's housing's back is bent in three, its glass painted across
            // them all); where on them the glass is MakeGlassMask finds.
            float uvLo[2] = { 1e9f, 1e9f }, uvHi[2] = { -1e9f, -1e9f };
            for (int v : f.tris)
                for (int k = 0; k < 2; k++) uvLo[k] = std::min(uvLo[k], tex[2 * v + k]), uvHi[k] = std::max(uvHi[k], tex[2 * v + k]);
            auto nearFace = [&](const int* t) {
                for (int j = 0; j < 3; j++)
                    for (int k = 0; k < 2; k++)
                    {
                        float reach = kGlassReach * (uvHi[k] - uvLo[k]), uv = tex[2 * t[j] + k];
                        if (uv < uvLo[k] - reach || uv > uvHi[k] + reach) return false;
                    }
                return true;
            };
            std::vector<int> glassTris = f.tris;
            std::vector<BYTE> taken(tris.size() / 3);
            for (size_t t = 0; t < tris.size(); t += 3)
                for (size_t u = 0; u < f.tris.size() && !taken[t / 3]; u += 3)
                    taken[t / 3] = tris[t] == f.tris[u] && tris[t + 1] == f.tris[u + 1] && tris[t + 2] == f.tris[u + 2];
            for (bool grew = true; grew;)
            {
                grew = false;
                for (size_t t = 0; t < tris.size(); t += 3)
                {
                    if (taken[t / 3]) continue;
                    const float *p0 = &pos[3 * tris[t]], *p1 = &pos[3 * tris[t + 1]], *p2 = &pos[3 * tris[t + 2]];
                    float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
                    float tn[3], cn[3], cmid[3], cto[3];
                    Cross3(e1, e2, tn);
                    if (!Normalize3(tn) || Dot3(tn, f.n) < kGlassBend || !nearFace(&tris[t]) || !towardsEyes(tn, &tris[t], 3, cn, cmid, cto) ||
                        !touches(&tris[t], glassTris))
                        continue;
                    taken[t / 3] = 1;
                    glassTris.insert(glassTris.end(), &tris[t], &tris[t] + 3);
                    grew = true;
                }
            }
            m.faceTriangles = (int)f.tris.size();
            for (int v : glassTris)
            {
                size_t k = std::find(m.vertices.begin(), m.vertices.end(), v) - m.vertices.begin();
                if (k == m.vertices.size())
                {
                    m.vertices.push_back(v);
                    m.model.insert(m.model.end(), &pos[3 * v], &pos[3 * v] + 3);
                    m.skinUv.insert(m.skinUv.end(), &tex[2 * v], &tex[2 * v] + 2);
                }
                m.triangles.push_back((WORD)k);
            }
            memcpy(m.rest, mid, sizeof(mid));
            // Its width and height: across the car's up and along it, in the glass's plane.
            float up[3] = { 0.0f, 1.0f, 0.0f }, side[3];
            for (int k = 0; k < 3; k++) up[k] -= n[1] * n[k];
            Normalize3(up);
            Cross3(up, n, side);
            float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
            for (int v : f.tris)
            {
                inCar(&pos[3 * v], p, false);
                float a = Dot3(p, side), h = Dot3(p, up);
                lo[0] = std::min(lo[0], a), hi[0] = std::max(hi[0], a), lo[1] = std::min(lo[1], h), hi[1] = std::max(hi[1], h);
            }
            out[0] = hi[0] - lo[0];
            out[1] = hi[1] - lo[1];
            out[2] = (mid[0] < 0.0f ? -1.0f : 1.0f) * atan2f(n[0], -n[2]) * 57.29578f;
            out[3] = asinf(std::clamp(n[1], -1.0f, 1.0f)) * 57.29578f;
            // The mirror set (see above): its normal halfway between the way back to the eyes and the way it shows. It
            // shows as wide and high an angle as the glass takes up in sight, and convex ([VR] MirrorRadius) as much
            // again as its normal turns across it, twice (halfW, halfH: halves of it).
            Normalize3(toGlass);
            float sign = mid[0] < 0.0f ? -1.0f : 1.0f, down = kMirrorAimDown, inner = 1e9f, az[2] = { 1e9f, -1e9f }, el[2] = { 1e9f, -1e9f };
            for (int v : f.tris)
            {
                inCar(&pos[3 * v], p, false);
                inner = std::min(inner, sign * p[0]);
                float d[3] = { p[0] - eyes[0], p[1] - eyes[1], p[2] - eyes[2] }, a = atan2f(d[0], d[2]), e = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
                az[0] = std::min(az[0], a), az[1] = std::max(az[1], a), el[0] = std::min(el[0], e), el[1] = std::max(el[1], e);
            }
            float curve = g_cfg.vrMirrorRadius > 0.0f ? 1.0f / g_cfg.vrMirrorRadius : 0.0f;
            float halfW = 0.5f * (az[1] - az[0]) + out[0] * curve, halfH = 0.5f * (el[1] - el[0]) + out[1] * curve, flank = 0.0f;
            // How far out the car's side reaches behind the glass, as seen from its inner edge within the mirror's
            // height: the mirror shows that much of it there.
            for (size_t k = 0; k + 2 < body.size(); k += 3)
            {
                float back = mid[2] - body[k + 2];
                if (back > 0.3f && fabsf(atan2f(body[k + 1] - mid[1], back) + down) < halfH + 0.05f)
                    flank = std::max(flank, atan2f(sign * body[k] - inner, back));
            }
            float outward = std::clamp(flank + (1.0f - 2.0f * kMirrorCarShare) * halfW, 0.5f * halfW, 0.5f);
            float shows[3] = { sign * sinf(outward) * cosf(down), -sinf(down), -cosf(outward) * cosf(down) };
            float aim[3] = { shows[0] - toGlass[0], shows[1] - toGlass[1], shows[2] - toGlass[2] };
            Normalize3(aim);
            for (int r = 0; r < 3; r++)  // into the model's axes (toCar's rows)
                m.aim[r] = Dot3(aim, toCar + 3 * r) / std::max(Dot3(toCar + 3 * r, toCar + 3 * r), 1e-6f);
            out[4] = acosf(std::clamp(Dot3(aim, n), -1.0f, 1.0f)) * 57.29578f;
            out[5] = 2.0f * halfW * 57.29578f;
            out[6] = outward * 57.29578f;
            out[7] = flank * 57.29578f;
        }
    }
    return m.batch != nullptr;
}

// The player's car's side mirrors (once a car, when it has its driver's eyes), logged.
void FindMirrors(BYTE* car)
{
    ForgetMirrors();
    g_mirrors.car = car;
    g_mirrors.failed = false;
    // The eyes at rest (the headset centred), in the car's axes.
    const float* seat = (const float*)(car + fo2::kCarDriverLoc);
    float eyes[3];
    DriverEyeFromSeat(car, eyes);
    for (int i = 0; i < 3; i++) eyes[i] += seat[i] + g_cfg.vrCameraOffset[i];
    std::vector<float> body;  // (the cockpit's cards read it for their layout, see LayCockpitCards)
    std::vector<int> batches;
    if (g_cabin.car == car)
        body = g_cabin.body;
    else
        ReadMesh(car, *(BYTE**)(car + fo2::kCarBody), body, batches);
    BYTE* root = *(BYTE**)(car + fo2::kCarSceneRoot);
    std::vector<BYTE*> next;  // the tree, depth first: the lists of siblings still to go
    if (root && *(BYTE**)(root + fo2::kNodeFirstChild)) next.push_back(*(BYTE**)(root + fo2::kNodeFirstChild));
    int nodes = 0, found = 0;
    std::string log;
    while (!next.empty() && nodes < 1024)
    {
        BYTE* node = next.back();
        next.pop_back();
        for (; node && nodes < 1024; node = *(BYTE**)(node + fo2::kNodeNextSibling), nodes++)
        {
            if (BYTE* child = *(BYTE**)(node + fo2::kNodeFirstChild)) next.push_back(child);
            if (*(uintptr_t*)node != fo2::kMeshNodeVtbl || *(int*)(node + fo2::kNodeModelCount) <= 0) continue;
            std::string name = NodeName(node);
            if (name.size() < 8 || (name.compare(name.size() - 8, 8, "mirror_l") && name.compare(name.size() - 8, 8, "mirror_r"))) continue;
            found++;
            SideMirror m;
            m.node = node;
            m.name = name;
            BYTE** parts = *(BYTE***)(car + fo2::kCarParts);
            BYTE** partsEnd = *(BYTE***)(car + fo2::kCarParts + 4);
            for (BYTE** it = parts; it && it < partsEnd && it - parts < 256 && !m.part; it++)
                if (*it && *(BYTE**)(*it + fo2::kPartNode) == node) m.part = *it;
            if (m.part) m.state = *(const int*)(m.part + fo2::kPartState);
            float about[8] = {};
            char buf[400];
            if (FindGlass(car, m, eyes, body, about))
            {
                snprintf(buf, sizeof(buf),
                         "; %s: glass %.3f x %.3f m (%d triangles, %d more next to it) at %.2f %.2f %.2f looking %+.0f deg out from straight back "
                         "and %+.0f up, set turned %.0f deg: %.1f deg across, its middle %.1f deg out (the car's side to %.1f)%s",
                         name.c_str(), about[0], about[1], m.faceTriangles / 3, ((int)m.triangles.size() - m.faceTriangles) / 3, m.rest[0], m.rest[1],
                         m.rest[2], about[2], about[3], about[4], about[5], about[6], about[7], m.part ? "" : " (its part not found)");
                g_mirrors.list.push_back(std::move(m));
            }
            else
                snprintf(buf, sizeof(buf), "; %s: no glass found", name.c_str());
            log += buf;
        }
    }
    Log("mirrors: %ls (%.32s): %d of %d mirrors with their glass%s", CarModel(car).c_str(), (const char*)(car + fo2::kCarName),
        (int)g_mirrors.list.size(), found, log.c_str());
}

// While the housings' textures are looked for (MakeGlassMask), the one an eye draws a housing with (DynamicBatchDraw,
// for each dynamic batch the eyes draw: its textures are set).
void NoteMirrorSkin(BYTE* batch)
{
    if (g_vrEye.mirror) return;
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)fo2::kD3DDevice;
    for (SideMirror& m : g_mirrors.list)
    {
        if (m.batch != batch || m.skin || m.maskDone) continue;
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(dev->GetTexture(0, &t)) || !t) continue;
        if (t->GetType() == D3DRTYPE_TEXTURE)
            m.skin = (IDirect3DTexture9*)t;
        else
            t->Release();
    }
}

// Where on the face the glass is, from the housing's texture as the eyes draw it: the face's part of it is drawn into a
// small target and read back. In every car seen the glass is painted with a rim round it, a line that reflects nothing
// (the texture's alpha is how much the skin reflects) and darker or lighter than the glass and the frame; the glass
// inside may be painted whole (a reflection, sky and ground) or broken (cracks running out from a point), the frame
// one colour, rusty or chromed. So the glass's outline is a closed path round the middle of the face's texture, one
// radius a direction (kRays of them, kRadii radii each from kRayNear to kRayFar of the way to its edge), through the
// strongest steps across the rays in the colour and the alpha (the cracks run along the rays), kept smooth (a radius
// step between neighbouring directions costs kRayCost, and at most kRaySlope of them) and made convex. Where the edge in
// a direction is of the glass's colour and alpha (within kGlassLike of the middle's) with no rim on the way (alpha
// below kRimAlpha of the middle's), the glass may run off there, which counts as a strong step (kRunOff): a face with
// no frame painted on it is glass to its edges (the Venom's middle face alone is cut by its top and bottom). The glass
// taking up between kGlassLeast and kGlassMost of the face is kept as a mask; else the whole face is glass (and the
// faces next to it not). Once a mirror; it stalls the GPU for the read back.
constexpr int kRays = 128, kRadii = 80, kRaySlope = 4, kGlassLike = 40;
constexpr float kRimAlpha = 0.4f;                 // a rim reflects less than this of the glass's middle
constexpr float kRayNear = 0.3f, kRayFar = 1.15f;
constexpr float kRayCost = 0.15f, kRunOff = 2.0f;  // of the 90th percentile of the steps
constexpr float kNoStep = 0.35f;                  // past the face's edge where it is not like the glass: of the median step
constexpr float kGlassLeast = 0.35f, kGlassMost = 0.9f;

void MakeGlassMask(IDirect3DDevice9* dev, SideMirror& m)
{
    m.maskDone = true;
    IDirect3DTexture9* skin = m.skin;
    m.skin = nullptr;
    if (!skin) return;
    size_t nv = m.skinUv.size() / 2;
    float box[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (size_t i = 0; i < nv; i++)
    {
        box[0] = std::min(box[0], m.skinUv[2 * i]), box[1] = std::min(box[1], m.skinUv[2 * i + 1]);
        box[2] = std::max(box[2], m.skinUv[2 * i]), box[3] = std::max(box[3], m.skinUv[2 * i + 1]);
    }
    D3DSURFACE_DESC td = {};
    skin->GetLevelDesc(0, &td);
    float texels[2] = { (box[2] - box[0]) * td.Width, (box[3] - box[1]) * td.Height };
    if (nv < 3 || texels[0] < 6.0f || texels[1] < 6.0f)
    {
        skin->Release();
        Log("mirrors: %s: its glass takes up %.0f x %.0f texels: all the face", m.name.c_str(), texels[0], texels[1]);
        return;
    }
    // Two samples a texel, at most 256 a side.
    int W = std::clamp((int)lroundf(2.0f * texels[0]), 16, 256), H = std::clamp((int)lroundf(2.0f * texels[1]), 16, 256);
    IDirect3DSurface9 *rt = nullptr, *sys = nullptr, *oldRT = nullptr, *oldDS = nullptr;
    IDirect3DStateBlock9* saved = nullptr;
    std::vector<unsigned> px;
    if (SUCCEEDED(dev->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &saved)))
    {
        dev->GetRenderTarget(0, &oldRT);
        dev->GetDepthStencilSurface(&oldDS);
        dev->SetRenderTarget(0, rt);
        dev->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp = { 0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetTexture(0, skin);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        struct V { float x, y, z, rhw, u, v; };
        const V quad[4] = { { -0.5f, -0.5f, 0, 1, box[0], box[1] }, { W - 0.5f, -0.5f, 0, 1, box[2], box[1] },
                            { -0.5f, H - 0.5f, 0, 1, box[0], box[3] }, { W - 0.5f, H - 0.5f, 0, 1, box[2], box[3] } };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        saved->Apply();
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(dev->GetRenderTargetData(rt, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        {
            px.resize((size_t)W * H);
            for (int y = 0; y < H; y++) memcpy(&px[(size_t)y * W], (const BYTE*)lr.pBits + (size_t)y * lr.Pitch, 4 * W);
            sys->UnlockRect();
        }
    }
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    if (saved) saved->Release();
    if (sys) sys->Release();
    if (rt) rt->Release();
    skin->Release();
    if (px.empty())
    {
        Log("mirrors: %s: its texture could not be read: all the face is glass", m.name.c_str());
        return;
    }
    // The face in the samples (its triangles): 1 the face found for the glass, 2 the faces next to it.
    std::vector<BYTE> inside((size_t)W * H);
    float sx = W / (box[2] - box[0]), sy = H / (box[3] - box[1]);
    for (size_t t = 0; t + 2 < m.triangles.size(); t += 3)
    {
        float p[3][2];
        for (int j = 0; j < 3; j++)
        {
            const float* uv = &m.skinUv[2 * m.triangles[t + j]];
            p[j][0] = (uv[0] - box[0]) * sx, p[j][1] = (uv[1] - box[1]) * sy;
        }
        int x0 = std::max(0, (int)floorf(std::min({ p[0][0], p[1][0], p[2][0] }))), x1 = std::min(W - 1, (int)ceilf(std::max({ p[0][0], p[1][0], p[2][0] })));
        int y0 = std::max(0, (int)floorf(std::min({ p[0][1], p[1][1], p[2][1] }))), y1 = std::min(H - 1, (int)ceilf(std::max({ p[0][1], p[1][1], p[2][1] })));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
            {
                float q[2] = { x + 0.5f, y + 0.5f }, s[3];
                for (int j = 0; j < 3; j++)
                {
                    const float *a0 = p[j], *a1 = p[(j + 1) % 3];
                    s[j] = (a1[0] - a0[0]) * (q[1] - a0[1]) - (a1[1] - a0[1]) * (q[0] - a0[0]);
                }
                if (((s[0] >= 0 && s[1] >= 0 && s[2] >= 0) || (s[0] <= 0 && s[1] <= 0 && s[2] <= 0)) && !inside[(size_t)y * W + x])
                    inside[(size_t)y * W + x] = (int)t < m.faceTriangles ? 1 : 2;
            }
    }
    auto channel = [&](size_t i, int c) { return (int)((px[i] >> (8 * c)) & 0xFF); };
    int face = 0;
    for (BYTE in : inside) face += in != 0;
    // 0: not the face, 1: the glass, 2: the frame; the outline's directions that run off the face. Null, or why there
    // was no outline to find.
    std::vector<BYTE> cls(inside);
    int glass = 0, ranOff = 0;
    auto outline = [&]() -> const char* {
        // The middle of the face found for the glass (the faces next to it may reach far off in the texture), and the
        // glass's colour and alpha there (each channel's middle one, the face's middle 30%).
        int lo[2] = { W, H }, hi[2] = { 0, 0 };
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (inside[(size_t)y * W + x] == 1)
                    lo[0] = std::min(lo[0], x), lo[1] = std::min(lo[1], y), hi[0] = std::max(hi[0], x + 1), hi[1] = std::max(hi[1], y + 1);
        if (lo[0] >= hi[0]) lo[0] = lo[1] = 0, hi[0] = W, hi[1] = H;
        std::vector<int> mid[4];
        float cx = 0.5f * (lo[0] + hi[0]), cy = 0.5f * (lo[1] + hi[1]), glassColour[4] = {};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (fabsf(x + 0.5f - cx) <= 0.15f * (hi[0] - lo[0]) && fabsf(y + 0.5f - cy) <= 0.15f * (hi[1] - lo[1]))
                    for (int c = 0; c < 4; c++) mid[c].push_back(channel((size_t)y * W + x, c));
        if (mid[0].empty()) return "its face is too small in its texture";
        for (int c = 0; c < 4; c++)
        {
            std::nth_element(mid[c].begin(), mid[c].begin() + mid[c].size() / 2, mid[c].end());
            glassColour[c] = (float)mid[c][mid[c].size() / 2];
        }
        // The samples between their middles (x, y in samples, a sample's middle at +0.5).
        auto at = [&](float x, float y, float* out) {
            x = std::clamp(x - 0.5f, 0.0f, W - 1.001f), y = std::clamp(y - 0.5f, 0.0f, H - 1.001f);
            int x0 = (int)x, y0 = (int)y;
            float fx = x - x0, fy = y - y0;
            size_t i = (size_t)y0 * W + x0;
            for (int c = 0; c < 4; c++)
                out[c] = (channel(i, c) * (1 - fx) + channel(i + 1, c) * fx) * (1 - fy) + (channel(i + W, c) * (1 - fx) + channel(i + W + 1, c) * fx) * fy;
        };
        // Each direction's way to the samples' edge (the texture round the face is the housing's too), whether the glass
        // may run off there, and the steps across the ray at its radii (the largest of a channel's over a sample either
        // side).
        std::vector<float> reach(kRays), dirs(2 * kRays), steps((size_t)kRays * kRadii), inner;
        std::vector<BYTE> past((size_t)kRays * kRadii), glassLike(kRays);
        auto radius = [](int k) { return kRayNear + (kRayFar - kRayNear) * (k + 0.5f) / kRadii; };
        for (int i = 0; i < kRays; i++)
        {
            float a = (i + 0.5f) * 6.2831853f / kRays, dx = cosf(a), dy = sinf(a);
            float t = std::min(dx > 1e-6f ? (W - cx) / dx : dx < -1e-6f ? cx / -dx : 1e9f, dy > 1e-6f ? (H - cy) / dy : dy < -1e-6f ? cy / -dy : 1e9f);
            dirs[2 * i] = dx, dirs[2 * i + 1] = dy, reach[i] = t;
            // At the edge of the glass's colour and alpha, with no rim on the way (a line that reflects nothing).
            float c[4], d = 0.0f;
            at(cx + dx * (t - 1.0f), cy + dy * (t - 1.0f), c);
            for (int ch = 0; ch < 4; ch++) d = std::max(d, fabsf(c[ch] - glassColour[ch]));
            glassLike[i] = d <= kGlassLike;
            for (float r = kRayNear * t; r < t - 1.0f && glassLike[i]; r += 0.5f)
            {
                at(cx + dx * r, cy + dy * r, c);
                glassLike[i] = c[3] >= kRimAlpha * glassColour[3];
            }
            for (int k = 0; k < kRadii; k++)
            {
                size_t j = (size_t)i * kRadii + k;
                float r = t * radius(k);
                if (radius(k) >= 1.0f - 0.5f / t)
                {
                    past[j] = 1;
                    continue;
                }
                float a0[4], a1[4], s = 0.0f;
                at(cx + dx * (r - 1.0f), cy + dy * (r - 1.0f), a0);
                at(cx + dx * (r + 1.0f), cy + dy * (r + 1.0f), a1);
                for (int ch = 0; ch < 4; ch++) s = std::max(s, fabsf(a1[ch] - a0[ch]));
                steps[j] = s;
                inner.push_back(s);
            }
        }
        if (inner.size() < (size_t)kRays) return "its face is too small in its texture";
        std::nth_element(inner.begin(), inner.begin() + inner.size() / 2, inner.end());
        float median = inner[inner.size() / 2];
        std::nth_element(inner.begin(), inner.begin() + inner.size() * 9 / 10, inner.end());
        float high = inner[inner.size() * 9 / 10], cost = kRayCost * high;
        for (int i = 0; i < kRays; i++)
            for (int k = 0; k < kRadii; k++)
                if (past[(size_t)i * kRadii + k]) steps[(size_t)i * kRadii + k] = glassLike[i] ? kRunOff * high : kNoStep * median;
        // The path of the most steps (less its cost), closed: from each of some radii in the first direction round to
        // within kRaySlope of it.
        std::vector<float> score(kRadii), next(kRadii);
        std::vector<signed char> back((size_t)kRays * kRadii);
        std::vector<int> path(kRays), best;
        float bestScore = -1e30f;
        for (int start = 0; start < kRadii; start += 4)
        {
            std::fill(score.begin(), score.end(), -1e30f);
            score[start] = steps[start];
            for (int i = 1; i < kRays; i++)
            {
                for (int k = 0; k < kRadii; k++)
                {
                    float v = -1e30f;
                    int from = k;
                    for (int d = -kRaySlope; d <= kRaySlope; d++)
                    {
                        int p = k - d;
                        if (p < 0 || p >= kRadii || score[p] - cost * abs(d) <= v) continue;
                        v = score[p] - cost * abs(d);
                        from = p;
                    }
                    next[k] = v + steps[(size_t)i * kRadii + k];
                    back[(size_t)i * kRadii + k] = (signed char)(from - k);
                }
                std::swap(score, next);
            }
            for (int k = std::max(0, start - kRaySlope); k <= std::min(kRadii - 1, start + kRaySlope); k++)
            {
                if (score[k] <= bestScore) continue;
                bestScore = score[k];
                path[kRays - 1] = k;
                for (int i = kRays - 1; i > 0; i--) path[i - 1] = path[i] + back[(size_t)i * kRadii + path[i]];
                best = path;
            }
        }
        // Its convex hull (counter-clockwise in the samples' x right, y down: clockwise on screen); the glass is the face
        // within it.
        std::vector<std::pair<float, float>> points, hull;
        for (int i = 0; i < kRays && !best.empty(); i++)
        {
            float r = reach[i] * radius(best[i]);
            ranOff += past[(size_t)i * kRadii + best[i]];
            points.push_back({ cx + dirs[2 * i] * r, cy + dirs[2 * i + 1] * r });
        }
        std::sort(points.begin(), points.end());
        auto cross = [](std::pair<float, float> o, std::pair<float, float> a, std::pair<float, float> b) {
            return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
        };
        for (int pass = 0; pass < 2 && points.size() >= 3; pass++)
        {
            size_t start = hull.size();
            for (size_t k = 0; k < points.size(); k++)
            {
                auto p = pass ? points[points.size() - 1 - k] : points[k];
                while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0) hull.pop_back();
                hull.push_back(p);
            }
            hull.pop_back();
        }
        for (size_t i = 0; i < cls.size(); i++)
        {
            if (!cls[i]) continue;
            float x = (float)(i % W) + 0.5f, y = (float)(i / W) + 0.5f;
            bool in = hull.size() >= 3;
            for (size_t k = 0; k < hull.size() && in; k++)
            {
                auto a = hull[k], b = hull[(k + 1) % hull.size()];
                in = (b.first - a.first) * (y - a.second) - (b.second - a.second) * (x - a.first) >= 0.0f;
            }
            cls[i] = in ? 1 : 2;
            glass += in;
        }
        return nullptr;
    };
    // The glass as checked by eye (mirrorglass.h), else as found.
    const MirrorGlass* hand = nullptr;
    std::wstring model = g_mirrors.car ? CarModel(g_mirrors.car) : L"";
    for (const MirrorGlass& g : kMirrorGlass)
        if (*g.car && model == std::wstring(g.car, g.car + strlen(g.car)) && m.name == g.mirror) hand = &g;
    const char* failed = nullptr;
    if (hand)
        for (size_t i = 0; i < cls.size(); i++)
        {
            if (!cls[i]) continue;
            float u = box[0] + ((float)(i % W) + 0.5f) / sx, v = box[1] + ((float)(i / W) + 0.5f) / sy;
            // Inside the convex outline: on the same side of all its edges.
            int sides = 0;
            for (int k = 0; k < hand->points; k++)
            {
                const float *a = &hand->uv[2 * k], *b = &hand->uv[2 * ((k + 1) % hand->points)];
                float c = (b[0] - a[0]) * (v - a[1]) - (b[1] - a[1]) * (u - a[0]);
                sides |= c > 0.0f ? 1 : c < 0.0f ? 2 : 0;
            }
            bool in = hand->points >= 3 && sides != 3;
            cls[i] = in ? 1 : 2;
            glass += in;
        }
    else
        failed = outline();
    float share = face ? (float)glass / face : 0.0f;
    bool rim = hand ? glass > 0 : !failed && share >= kGlassLeast && share <= kGlassMost;
    if (!rim)
    {
        // No rim found: the face is glass to its edges, and the faces next to it are not.
        const char* rest = m.faceTriangles < (int)m.triangles.size() ? ", the faces next to it not" : "";
        if (hand)
            Log("mirrors: %s: %s: all its face is glass%s", m.name.c_str(), hand->points ? "the glass checked by eye is not on its faces"
                : "no glass painted (checked by eye)", rest);
        else if (failed)
            Log("mirrors: %s: %s: all its face is glass%s", m.name.c_str(), failed, rest);
        else
            Log("mirrors: %s: no rim round the glass in its texture (%dx%d samples over %.4f %.4f .. %.4f %.4f: the outline found takes in %.0f%% "
                "of the face, running off it in %d of %d directions): all its face is glass%s", m.name.c_str(), W, H, box[0], box[1], box[2], box[3],
                100.0f * share, ranOff, kRays, rest);
        for (size_t i = 0; i < cls.size(); i++) cls[i] = inside[i] == 1 ? 1 : inside[i] ? 2 : 0;
    }
    if (g_cfg.testBackground)
    {
        // For checking it: the face's part of the texture, and the same with the frame darkened.
        std::vector<unsigned> look(px);
        for (size_t i = 0; i < look.size(); i++)
            if (cls[i] != 1) look[i] = 0xFF000000 | ((look[i] >> 2) & 0x3F3F3F);
        std::string tag = "mirror_skin_" + m.name;
        SavePixelsPng(ShotPath(tag.c_str()).c_str(), px.data(), W, H);
        SavePixelsPng(ShotPath((tag + "_glass").c_str()).c_str(), look.data(), W, H);
        // And where the faces lie on it: the face found for the glass green, the faces next to it blue.
        for (size_t i = 0; i < look.size(); i++)
            look[i] = inside[i] == 1 ? 0xFF000000 | ((px[i] >> 1) & 0x7F7F7F) | 0x008000 : inside[i] == 2 ? 0xFF000000 | ((px[i] >> 1) & 0x7F7F7F) | 0x000080
                                                                                                   : 0xFF000000 | ((px[i] >> 2) & 0x3F3F3F);
        SavePixelsPng(ShotPath((tag + "_faces").c_str()).c_str(), look.data(), W, H);
    }
    if (!rim && m.faceTriangles >= (int)m.triangles.size()) return;
    // The mask, its edge softened over a sample.
    if (FAILED(dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m.mask, nullptr)))
    {
        m.mask = nullptr;
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(m.mask->LockRect(0, &lr, nullptr, 0)))
    {
        m.mask->Release();
        m.mask = nullptr;
        return;
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            int sum = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int qx = std::clamp(x + dx, 0, W - 1), qy = std::clamp(y + dy, 0, H - 1);
                    sum += cls[(size_t)qy * W + qx] == 1 ? 255 : 0;
                    n++;
                }
            ((unsigned*)((BYTE*)lr.pBits + (size_t)y * lr.Pitch))[x] = (unsigned)(sum / n) << 24 | 0xFFFFFF;
        }
    m.mask->UnlockRect(0);
    memcpy(m.maskBox, box, sizeof(box));
    if (hand)
        Log("mirrors: %s: the glass as checked by eye (%d points) takes up %.0f%% of the face%s", m.name.c_str(), hand->points, 100.0f * share,
            m.faceTriangles < (int)m.triangles.size() ? " and the faces next to it" : "");
    else if (rim)
        Log("mirrors: %s: the glass in its texture (%dx%d samples over %.4f %.4f .. %.4f %.4f) takes up %.0f%% of the face%s, inside its rim "
            "(running off the face in %d of %d directions)", m.name.c_str(), W, H, box[0], box[1], box[2], box[3], 100.0f * share,
            m.faceTriangles < (int)m.triangles.size() ? " and the faces next to it" : "", ranOff, kRays);
}

// For tests (the background mode's "mirror"): the player's car's mirror on that side knocked loose (state 1) or off
// (2), as the hits do (its part's own calls, see game.h kCarParts).
void KnockMirror(char side, int state)
{
    for (SideMirror& m : g_mirrors.list)
    {
        if (m.name.empty() || m.name.back() != side) continue;
        if (!m.part)
        {
            Log("mirrors: %s: no part to knock", m.name.c_str());
            return;
        }
        using PartFn = void(__thiscall*)(BYTE*);
        PartFn* vt = *(PartFn**)m.part;
        vt[state == 2 ? fo2::kPartKnockOff : fo2::kPartKnockLoose](m.part);
        Log("mirrors: %s knocked %s for the test (its part's state now %d)", m.name.c_str(), state == 2 ? "off" : "loose",
            *(const int*)(m.part + fo2::kPartState));
        return;
    }
    Log("mirrors: no %s mirror to knock", side == 'l' ? "left" : "right");
}

// The colour filter's textures (see FilterTextures) while a mirror renders: of the mirrors' size.
struct FilterSwap
{
    BYTE* wrapper[2] = {};
    int size[2][2] = {};
    IDirect3DTexture9* tex[2] = {};
};

bool UseMirrorFilterTextures(IDirect3DDevice9* dev, FilterSwap& saved)
{
    BYTE* engine = *(BYTE**)fo2::kDeviceD3d;
    BYTE* post = engine ? *(BYTE**)(engine + fo2::kDevPost) : nullptr;
    if (!post) return false;
    BYTE** wrappers = (BYTE**)(post + fo2::kPostFilterTex);
    for (int i = 0; i < 2; i++)
    {
        IDirect3DTexture9* game = wrappers[i] ? *(IDirect3DTexture9**)(wrappers[i] + fo2::kTexD3D) : nullptr;
        D3DSURFACE_DESC d;
        if (!game || FAILED(game->GetLevelDesc(0, &d))) return false;
        if (!g_mirrors.filter[i] &&
            FAILED(dev->CreateTexture(kMirrorMax, kMirrorMax, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_mirrors.filter[i], nullptr)))
            return false;
    }
    for (int i = 0; i < 2; i++)
    {
        BYTE* w = saved.wrapper[i] = wrappers[i];
        saved.size[i][0] = *(int*)(w + fo2::kTexWidth);
        saved.size[i][1] = *(int*)(w + fo2::kTexHeight);
        saved.tex[i] = *(IDirect3DTexture9**)(w + fo2::kTexD3D);
        *(int*)(w + fo2::kTexWidth) = (int)kMirrorMax;
        *(int*)(w + fo2::kTexHeight) = (int)kMirrorMax;
        *(IDirect3DTexture9**)(w + fo2::kTexD3D) = g_mirrors.filter[i];
    }
    return true;
}

// The filter's textures said to be of a picture's size (they are of its target's).
void SizeMirrorFilterTextures(const FilterSwap& swapped, UINT w, UINT h)
{
    for (BYTE* wrapper : swapped.wrapper)
        if (wrapper) *(int*)(wrapper + fo2::kTexWidth) = (int)w, *(int*)(wrapper + fo2::kTexHeight) = (int)h;
}

void RestoreFilterTextures(const FilterSwap& saved)
{
    for (int i = 0; i < 2; i++)
    {
        BYTE* w = saved.wrapper[i];
        if (!w) continue;
        *(int*)(w + fo2::kTexWidth) = saved.size[i][0];
        *(int*)(w + fo2::kTexHeight) = saved.size[i][1];
        *(IDirect3DTexture9**)(w + fo2::kTexD3D) = saved.tex[i];
    }
}

// A mirror's camera looks along the mirror's normal, at the glass well off to the side, and its frustum is off
// centre; the engine picks the objects a view shows with a frustum about the camera's direction, wide enough to
// take the glass in (some 50 degrees for a glass of 6): hundreds of objects outside the glass were drawn for
// nothing. While a mirror's objects are picked (kCullView), the camera looks straight at the glass, with a frustum
// just around it.
struct MirrorCull
{
    bool on = false;
    float matrix[16], frustum[7], fov;
} g_mirrorCull;

using CullViewFn = void(__stdcall*)(BYTE* ctx, BYTE* cam, int view);
const auto oCullView = (CullViewFn)fo2::kCullView;

void __stdcall CullViewHook(BYTE* ctx, BYTE* cam, int view)
{
    if (!g_mirrorCull.on)
    {
        oCullView(ctx, cam, view);
        return;
    }
    float* m = (float*)(cam + fo2::kCamMatrix);
    float* m2 = (float*)(cam + fo2::kCamMatrixCopy2);
    float* fr = (float*)(cam + fo2::kCamFrustum);
    float* fov = (float*)(cam + fo2::kCamFov);
    float saved[16], saved2[16], savedFr[7], savedFov = *fov;
    memcpy(saved, m, sizeof(saved));
    memcpy(saved2, m2, sizeof(saved2));
    memcpy(savedFr, fr, sizeof(savedFr));
    memcpy(m, g_mirrorCull.matrix, sizeof(saved));
    memcpy(m2, g_mirrorCull.matrix, sizeof(saved2));
    memcpy(fr, g_mirrorCull.frustum, sizeof(savedFr));
    *fov = g_mirrorCull.fov;
    oCullView(ctx, cam, view);
    memcpy(m, saved, sizeof(saved));
    memcpy(m2, saved2, sizeof(saved2));
    memcpy(fr, savedFr, sizeof(savedFr));
    *fov = savedFov;
}

// The mirrors render before the eyes, with the cars' shadows (kCarShadow) the eyes rendered the frame before.
static uintptr_t g_carShadowFn = fo2::kCarShadow;
static bool g_keepCarShadows;

// At kCarShadowCall, instead of kCarShadow (the car's shadow in eax).
static __declspec(naked) void CarShadowHook()
{
    __asm {
        cmp byte ptr [g_keepCarShadows], 0
        jne keep
        jmp dword ptr [g_carShadowFn]
    keep:
        ret
    }
}

// Before the eyes (RenderVrEyes, the cockpit; anchor, pose and fov as the eyes have them, pastAnchor the camera's past
// placements moved along with it): the pictures of the mirrors the eyes may see, with the cars' shadows of the frame
// before (CarShadowHook) and without the post-processing (PostProcessHook). Each eye then lays them on their glass as
// it draws, just before the player's windows (DynamicBatchDraw: the windows' tint, shine and cracks lie over them as
// over the rest of the view) or, in a car without them, before its own post-processing (its colour filter, glow and
// blur take the mirrors in with the rest of the view): MirrorsIntoEye. What the eyes set is put back after.
void RenderMirrors(IDirect3DDevice9* dev, BYTE* ctx, BYTE** cams, float* fovScale, BYTE* cam, const float* anchor, const xr::EyePose* pose,
                   const xr::EyeFov* fov, const float (*pastAnchor)[16])
{
    for (SideMirror& m : g_mirrors.list) m.shown[0] = m.shown[1] = false;
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    g_mirrorSkinWanted = false;
    if (!g_cfg.vrMirrors || !car || car != g_eyeCar) return;  // once the car has its driver's eyes
    if (g_mirrors.car != car) FindMirrors(car);
    int pictures = g_cfg.vrMirrors == 2 ? 2 : 1;
    if (g_mirrors.list.empty() || !EnsureMirrorTargets(dev, pictures)) return;
    double t0 = NowSeconds();
    // Where the glass is on the faces: from the housings' textures as the eyes drew them (see MakeGlassMask).
    for (SideMirror& m : g_mirrors.list)
        if (!m.maskDone)
        {
            if (m.skin)
                MakeGlassMask(dev, m);
            else
                g_mirrorSkinWanted = true;
        }
    const float* c = (const float*)(car + fo2::kCarMatrix);
    float e[2][16], mid[3];  // the eyes, and their middle
    for (int i = 0; i < 2; i++) BuildEyeMatrix(anchor, pose[i], e[i]);
    for (int k = 0; k < 3; k++) mid[k] = 0.5f * (e[0][12 + k] + e[1][12 + k]);

    float* camM = (float*)(cam + fo2::kCamMatrix);
    float* camM2 = (float*)(cam + fo2::kCamMatrixCopy2);
    float* camViews = (float*)(cam + fo2::kCamViews);
    float* camFov = (float*)(cam + fo2::kCamFov);
    float* camNear = (float*)(cam + fo2::kCamNear);
    float* camFar = (float*)(cam + fo2::kCamFar);
    float* grass = (float*)(ctx + fo2::kCtxPlantFar);
    int* screenSize = EngineScreenSize();
    float* screenShape = (float*)fo2::kScreenShape;
    float* blurSpeed = (float*)fo2::kRadialBlurSpeed;
    // What the eyes set (RenderVrEyes), put back after the mirrors.
    float savedNear = *camNear, savedFar = *camFar, savedGrass[2] = { grass[0], grass[1] }, savedShape[2] = { screenShape[0], screenShape[1] }, savedBlur[4];
    float savedCam[16], savedCam2[16], savedViews[fo2::kCamViewCount * 16], savedFov = *camFov;
    int savedScreen[2] = { screenSize[0], screenSize[1] };
    memcpy(savedBlur, blurSpeed, sizeof(savedBlur));
    memcpy(savedCam, camM, sizeof(savedCam));
    memcpy(savedCam2, camM2, sizeof(savedCam2));
    memcpy(savedViews, camViews, sizeof(savedViews));
    VrEye savedEye = g_vrEye;
    FilterSwap filter;
    float relative[16], anchorView[16];  // a mirror's camera = relative * anchor
    InvertCamera(anchor, anchorView);
    int rendered = 0;
    std::vector<float> window, flat;
    for (SideMirror& m : g_mirrors.list)
    {
        if (!m.rt[0]) continue;
        // The glass in the world: as found (flat), moved with the housing (with its door, or as it hangs knocked loose),
        // which the mirror's picture is worked out for; and as the crashes have bent it (the batch's vertices on the
        // CPU), where the picture is drawn, so that it stays on the glass however bent. Its middle, and the mirror's
        // normal as set.
        const float* w = (const float*)(m.node + fo2::kNodeWorld);
        const BYTE* cpu = *(const BYTE**)(m.batch + fo2::kBatchVertices);
        int stride = *(int*)(m.batch + fo2::kBatchStride);
        size_t nv = m.vertices.size();
        m.world.resize(3 * nv);
        flat.resize(3 * nv);
        window.resize(3 * nv);
        float C[3] = {}, n[3];
        for (size_t i = 0; i < nv; i++)
        {
            const float* p = &m.model[3 * i];
            const float* b = cpu ? (const float*)(cpu + m.vertices[i] * stride) : p;
            float* q = &flat[3 * i];
            float* bent = &m.world[3 * i];
            for (int k = 0; k < 3; k++)
            {
                q[k] = p[0] * w[k] + p[1] * w[4 + k] + p[2] * w[8 + k] + w[12 + k], C[k] += q[k] / nv;
                bent[k] = b[0] * w[k] + b[1] * w[4 + k] + b[2] * w[8 + k] + w[12 + k];
            }
        }
        for (int k = 0; k < 3; k++) n[k] = m.aim[0] * w[k] + m.aim[1] * w[4 + k] + m.aim[2] * w[8 + k];
        if (!Normalize3(n)) continue;
        // Knocked off: its part's state (without its part: the glass far from its place in the car).
        bool away;
        if (m.part)
        {
            int state = *(const int*)(m.part + fo2::kPartState);
            if (state != m.state)
                Log("mirrors: %s %s", m.name.c_str(), state >= fo2::kPartKnockedOff ? "knocked off" : state ? "knocked loose (it still shows)" : "on the car");
            m.state = state;
            away = state >= fo2::kPartKnockedOff;
        }
        else
        {
            float moved = 0;
            for (int i = 0; i < 3; i++)
            {
                float x = (C[0] - c[12]) * c[4 * i] + (C[1] - c[13]) * c[4 * i + 1] + (C[2] - c[14]) * c[4 * i + 2] - m.rest[i];
                moved += x * x;
            }
            away = moved > 0.3f * 0.3f;
            if (away != m.away) Log("mirrors: %s %s", m.name.c_str(), away ? "knocked away" : "back in its place");
        }
        m.away = away;
        if (away) continue;
        const char* why = "";  // why it is not shown (out of view: nothing to say)
        for (int k = 0; k < pictures; k++)
        {
            // Seen from the eyes' middle (one picture for both), or from eye k: in front of the mirror by d.
            const float* from = pictures == 2 ? e[k] + 12 : mid;
            float toFrom[3] = { from[0] - C[0], from[1] - C[1], from[2] - C[2] };
            float d = Dot3(toFrom, n);
            if (d < 0.05f)
            {
                why = "the eyes are behind its glass";
                continue;
            }
            // In view of the eye (either eye, for one picture), and how many of its pixels the glass takes up (the
            // larger of the two for one picture; a glass partly behind the eye as many as a picture has).
            bool inView = false;
            float pixels[2] = {};
            for (int i = pictures == 2 ? k : 0; i < (pictures == 2 ? k + 1 : 2); i++)
            {
                float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
                bool behind = false;
                for (size_t v = 0; v < nv; v++)
                {
                    const float* q = &m.world[3 * v];
                    float dq[3] = { q[0] - e[i][12], q[1] - e[i][13], q[2] - e[i][14] };
                    float z = Dot3(dq, e[i] + 8);
                    if (z < 0.01f)
                    {
                        behind = true;
                        continue;
                    }
                    float x = Dot3(dq, e[i]) / z, y = Dot3(dq, e[i] + 4) / z;
                    lo[0] = std::min(lo[0], x), hi[0] = std::max(hi[0], x), lo[1] = std::min(lo[1], y), hi[1] = std::max(hi[1], y);
                }
                bool seen = lo[0] <= hi[0] && (behind || (hi[0] > fov[i].left && lo[0] < fov[i].right && hi[1] > fov[i].down && lo[1] < fov[i].up));
                if (!seen) continue;
                inView = true;
                pixels[0] = std::max(pixels[0], behind ? (float)kMirrorMax : (hi[0] - lo[0]) / (fov[i].right - fov[i].left) * g_vrt.w);
                pixels[1] = std::max(pixels[1], behind ? (float)kMirrorMax : (hi[1] - lo[1]) / (fov[i].up - fov[i].down) * g_vrt.h);
            }
            if (!inView) continue;
            UINT pw = std::clamp((UINT)ceilf(kMirrorSharp * pixels[0] / kMirrorStep) * kMirrorStep, kMirrorLeast, kMirrorMax);
            UINT ph = std::clamp((UINT)ceilf(kMirrorSharp * pixels[1] / kMirrorStep) * kMirrorStep, kMirrorLeast, kMirrorMax);
            // The window: the glass as seen from there, on the mirror's plane (through the glass's middle).
            bool edgeOn = false;
            for (size_t v = 0; v < nv && !edgeOn; v++)
            {
                const float* q = &flat[3 * v];
                float dq[3] = { q[0] - from[0], q[1] - from[1], q[2] - from[2] };
                float along = Dot3(dq, n);
                edgeOn = along > -0.01f;
                for (int j = 0; j < 3 && !edgeOn; j++) window[3 * v + j] = from[j] - d / along * dq[j];
            }
            if (edgeOn)
            {
                why = "seen edge on";
                continue;
            }
            // The camera: there reflected in the mirror's plane, looking through it along its normal, upright as the car
            // is.
            float mc[16] = {};
            float* r = mc;
            float* u = mc + 4;
            float* f = mc + 8;
            float* at = mc + 12;
            memcpy(f, n, sizeof(n));
            for (int j = 0; j < 3; j++) u[j] = c[4 + j] - Dot3(c + 4, n) * n[j], at[j] = from[j] - 2.0f * d * n[j];
            if (!Normalize3(u))
            {
                why = "lying flat";
                continue;
            }
            Cross3(u, f, r);
            mc[15] = 1.0f;
            // The window's tangents from there: the frustum through it, and where each vertex lies in the picture.
            std::vector<float>& uv = m.uv[k];
            uv.resize(2 * nv);
            float left = 1e9f, right = -1e9f, down = 1e9f, up = -1e9f;
            for (size_t v = 0; v < nv; v++)
            {
                const float* q = &window[3 * v];
                float dq[3] = { q[0] - at[0], q[1] - at[1], q[2] - at[2] };
                float z = std::max(Dot3(dq, f), 0.01f), x = Dot3(dq, r) / z, y = Dot3(dq, u) / z;
                uv[2 * v] = x, uv[2 * v + 1] = y;
                left = std::min(left, x), right = std::max(right, x), down = std::min(down, y), up = std::max(up, y);
            }
            // A convex glass ([VR] MirrorRadius) shows more about its middle: its normal turns across it by the way off
            // the middle over the radius, and the ray twice that, so in these tangents what it shows is gx and gy times
            // as wide as the glass (d the eyes' way to its plane, 1 + tan^2 the tangent's slope there; the two differ
            // seen aslant, as a sphere's focal lengths do). The rays come off the glass as from its virtual eye, the
            // reflected one moved towards the glass's middle to 1/g of its way (g the larger): the window is g times
            // as wide from there, its picture squeezed across the other way to gx and gy, and nothing is seen but
            // through the glass (not past it, into the car).
            float aspect = (float)pw / (float)ph, view[4] = { left, right, up, down }, depth = d;
            if (g_cfg.vrMirrorRadius > 0.0f)
            {
                float mx = 0.5f * (left + right), my = 0.5f * (up + down);
                float gx = 1.0f + 2.0f * d * (1.0f + mx * mx) / g_cfg.vrMirrorRadius, gy = 1.0f + 2.0f * d * (1.0f + my * my) / g_cfg.vrMirrorRadius;
                float g = std::max(gx, gy);
                for (int j = 0; j < 3; j++) at[j] += (1.0f - 1.0f / g) * d * (f[j] + mx * r[j] + my * u[j]);
                depth = d / g;
                view[0] = mx + gx * (left - mx), view[1] = mx + gx * (right - mx), view[2] = my + gy * (up - my), view[3] = my + gy * (down - my);
            }
            float cover = std::max({ -view[0], view[1], view[2] * aspect, -view[3] * aspect }) * 1.02f;
            if (right - left < 1e-4f || up - down < 1e-4f || cover > 6.0f)
            {
                why = "seen edge on";
                continue;
            }
            // (within the picture's corner of its target, from its first pixel's middle to its last's)
            for (size_t v = 0; v < nv; v++)
            {
                uv[2 * v] = (0.5f + (uv[2 * v] - left) / (right - left) * (pw - 1)) / kMirrorMax;
                uv[2 * v + 1] = (0.5f + (up - uv[2 * v + 1]) / (up - down) * (ph - 1)) / kMirrorMax;
            }
            m.size[k][0] = pw, m.size[k][1] = ph;
            g_mirrors.pixels += (double)pw * ph;
            // The camera the objects are picked with (CullViewHook): from there straight at the window, upright, its
            // frustum just around it.
            float* o = g_mirrorCull.matrix;
            float* of = o + 8;
            float* ou = o + 4;
            for (int j = 0; j < 3; j++) of[j] = f[j] + 0.5f * (left + right) * r[j] + 0.5f * (up + down) * u[j];
            Normalize3(of);
            for (int j = 0; j < 3; j++) ou[j] = c[4 + j] - Dot3(c + 4, of) * of[j];
            Normalize3(ou);
            Cross3(ou, of, o);
            memcpy(o + 12, at, 3 * sizeof(float));
            o[3] = o[7] = o[11] = 0.0f;
            o[15] = 1.0f;
            float around = 0.0f, nearest = 1e9f;
            for (size_t v = 0; v < nv; v++)
            {
                const float* q = &window[3 * v];
                float dq[3] = { q[0] - at[0], q[1] - at[1], q[2] - at[2] };
                float z = std::max(Dot3(dq, of), 0.01f);
                around = std::max({ around, fabsf(Dot3(dq, o)) / z, fabsf(Dot3(dq, ou)) / z * aspect });
                nearest = std::min(nearest, z);
            }
            around *= 1.05f;
            float cullNear = 0.9f * nearest;
            const float cullFrustum[7] = { -cullNear * around, cullNear * around, -cullNear * around / aspect, cullNear * around / aspect, cullNear,
                                           *camFar, 0.0f };
            if (!rendered)
            {
                share::EndEyes();
                UseMirrorFilterTextures(dev, filter);
                memset(blurSpeed, 0, sizeof(savedBlur));  // no speed blur
                // The world and the grass as far out as the game draws them ([VR] ViewDistance and GrassDistance are for
                // the eyes): far away things are small in a mirror, and cost as much as near ones.
                if (g_cfg.vrViewDistance > 0.0f) *camFar = savedFar / g_cfg.vrViewDistance;
                if (g_cfg.vrGrassDistance > 0.0f) grass[0] = savedGrass[0] / g_cfg.vrGrassDistance, grass[1] = savedGrass[1] / g_cfg.vrGrassDistance;
            }
            screenSize[0] = (int)pw;
            screenSize[1] = (int)ph;
            screenShape[0] = (float)pw;
            screenShape[1] = (float)ph;
            SizeMirrorFilterTextures(filter, pw, ph);
            // Every copy of the camera's placement; its past ones moved along as the eyes' are.
            memcpy(camM, mc, sizeof(mc));
            memcpy(camM2, mc, sizeof(mc));
            InvertCamera(mc, camViews);
            MulMatrix(mc, anchorView, relative);
            for (int i = 1; i < fo2::kCamViewCount; i++)
            {
                float past[16];
                MulMatrix(relative, pastAnchor[i], past);
                InvertCamera(past, camViews + 16 * i);
            }
            *camFov = 2.0f * atanf(cover);
            *camNear = 0.995f * depth;
            SetFullViewportGlobals();
            dev->SetRenderTarget(0, m.rt[k]);
            dev->SetDepthStencilSurface(g_mirrors.ds);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
            g_vrEye.active = true;
            g_vrEye.index = 0;
            g_vrEye.cockpit = false;
            g_vrEye.mirror = true;
            g_vrEye.w = pw;
            g_vrEye.h = ph;
            g_vrEye.coverTan = cover;
            g_vrEye.fov = { view[0], view[1], view[2], view[3] };
            // The mirror's own housing is left out of its picture (it lies about the mirror's plane as set).
            DWORD* flags = (DWORD*)(m.node + fo2::kNodeFlags);
            DWORD savedFlags = *flags;
            *flags |= fo2::kNodeHidden;
            memcpy(g_mirrorCull.frustum, cullFrustum, sizeof(cullFrustum));
            g_mirrorCull.frustum[5] = *camFar;
            g_mirrorCull.fov = 2.0f * atanf(around);
            g_mirrorCull.on = true;
            g_keepCarShadows = true;
            oRenderViews(ctx, cams, fovScale);
            g_keepCarShadows = false;
            g_mirrorCull.on = false;
            *flags = savedFlags;
            g_vrEye.active = false;
            g_vrEye.mirror = false;
            m.shown[k] = true;
            rendered++;
            m.tex[k]->GenerateMipSubLevels();
            if (g_saveEyesThisFrame)
            {
                std::string tag = std::string("vr_mirror_") + m.name.back() + (pictures == 2 ? (k ? "_right" : "_left") : "");
                SaveSurfacePng(dev, m.rt[k], ShotPath(tag.c_str()).c_str());
            }
        }
        if (m.shown[0] || m.shown[1]) why = "";
        if (why != m.why && why[0] && m.whyLogged < 20)
        {
            Log("mirrors: %s not shown: %s", m.name.c_str(), why);
            m.whyLogged++;
        }
        m.why = why;
    }
    if (rendered)
    {
        RestoreFilterTextures(filter);
        memcpy(blurSpeed, savedBlur, sizeof(savedBlur));
        screenSize[0] = savedScreen[0];
        screenSize[1] = savedScreen[1];
        screenShape[0] = savedShape[0];
        screenShape[1] = savedShape[1];
        *camNear = savedNear;
        *camFar = savedFar;
        grass[0] = savedGrass[0];
        grass[1] = savedGrass[1];
        memcpy(camM, savedCam, sizeof(savedCam));
        memcpy(camM2, savedCam2, sizeof(savedCam2));
        memcpy(camViews, savedViews, sizeof(savedViews));
        *camFov = savedFov;
        g_vrEye = savedEye;
    }
    double now = NowSeconds();
    g_mirrors.time.Add(now - t0);
    g_mirrors.renders += rendered;
    if (now - g_mirrors.lastLog >= g_cfg.logIntervalSeconds)
    {
        if (g_mirrors.lastLog > 0)
            Log("mirrors: %.2f pictures rendered a frame, of %.0f pixels on average (%.0f a side), %.2f/%.2f ms avg/max",
                (double)g_mirrors.renders / std::max(g_mirrors.time.n, 1), g_mirrors.pixels / std::max(g_mirrors.renders, 1),
                sqrt(g_mirrors.pixels / std::max(g_mirrors.renders, 1)), g_mirrors.time.AvgMs(), g_mirrors.time.MaxMs());
        g_mirrors.time = Timing();
        g_mirrors.renders = 0;
        g_mirrors.pixels = 0;
        g_mirrors.lastLog = now;
    }
}

// In an eye (after its RenderViews): the mirrors' pictures over their glass, with the eye's depth (and where the glass
// is on the face, see MakeGlassMask).
void DrawMirrors(IDirect3DDevice9* dev, BYTE* cam, int eye)
{
    struct V { float x, y, z, rhw; DWORD colour; float u, v, mu, mv; };
    const float* e = (const float*)(cam + fo2::kCamMatrix);  // the eye's
    float n = *(const float*)(cam + fo2::kCamNear), f = *(const float*)(cam + fo2::kCamFar);
    const xr::EyeFov& fov = g_vrEye.fov;
    float W = (float)g_vrt.w, H = (float)g_vrt.h;
    DWORD shade = (DWORD)lroundf(255.0f * kMirrorShade);
    DWORD colour = 0xFF000000 | shade << 16 | shade << 8 | shade;
    int k = g_mirrors.pictures == 2 ? eye : 0;
    IDirect3DStateBlock9* saved = nullptr;
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    std::vector<V> verts;
    for (const SideMirror& m : g_mirrors.list)
    {
        if (!m.shown[k]) continue;
        verts.clear();
        const float* box = m.maskBox;
        for (size_t i = 0; i < m.vertices.size(); i++)
        {
            const float* p = &m.world[3 * i];
            float d[3] = { p[0] - e[12], p[1] - e[13], p[2] - e[14] };
            float x = Dot3(d, e), y = Dot3(d, e + 4), z = Dot3(d, e + 8);
            if (z < n * 1.01f) break;  // (not clipped: left out)
            float zd = std::max(z - kMirrorDepthPull, n * 1.001f);
            float mu = m.mask ? (m.skinUv[2 * i] - box[0]) / (box[2] - box[0]) : 0.0f, mv = m.mask ? (m.skinUv[2 * i + 1] - box[1]) / (box[3] - box[1]) : 0.0f;
            verts.push_back({ (x / z - fov.left) / (fov.right - fov.left) * W - 0.5f, (fov.up - y / z) / (fov.up - fov.down) * H - 0.5f,
                              EyeDepth(n, f, zd), 1.0f / z, colour, m.uv[k][2 * i], m.uv[k][2 * i + 1], mu, mv });
        }
        if (verts.size() != m.vertices.size()) continue;
        if (!saved)
        {
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;
            dev->GetRenderTarget(0, &oldRT);
            dev->GetDepthStencilSurface(&oldDS);
            dev->SetRenderTarget(0, g_vrt.rt[eye]);
            dev->SetDepthStencilSurface(g_vrt.ds[eye]);
            D3DVIEWPORT9 vp = { 0, 0, g_vrt.w, g_vrt.h, 0.0f, 1.0f };
            dev->SetViewport(&vp);
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
            dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            // The glass's mask (when it has one): its alpha.
            dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
            dev->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            for (DWORD s = 0; s < 2; s++)
            {
                dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(s, D3DSAMP_MIPFILTER, s ? D3DTEXF_NONE : D3DTEXF_LINEAR);
                dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
                dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
                dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
            }
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        }
        dev->SetTexture(0, m.tex[k]);
        dev->SetTexture(1, m.mask);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, m.mask ? D3DTOP_SELECTARG2 : D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, m.mask ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, m.mask ? TRUE : FALSE);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(), (UINT)(m.triangles.size() / 3), m.triangles.data(), D3DFMT_INDEX16,
                                    verts.data(), sizeof(V));
    }
    if (!saved) return;
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    saved->Apply();
    saved->Release();
}

// The eye being rendered, for its mirrors (MirrorsIntoEye): its camera (null: not the cockpit's), and whether they are
// on its picture yet.
struct MirrorEye
{
    BYTE* cam = nullptr;
    int index = 0;
    bool drawn = false;
} g_mirrorEye;

void MirrorsIntoEye()
{
    if (!g_mirrorEye.cam || g_mirrorEye.drawn || g_vrEye.mirror) return;
    g_mirrorEye.drawn = true;
    DrawMirrors(*(IDirect3DDevice9**)fo2::kD3DDevice, g_mirrorEye.cam, g_mirrorEye.index);
}

// The renderer's post-processing (see game.h, kSlotPostProcess): none in the mirrors' pictures (the eye's takes them in);
// in the cockpit's eyes the mirrors go on before it, if the car's windows have not brought them on yet.
using PostProcessFn = void(__thiscall*)(void* self);
const auto oPostProcess = (PostProcessFn)fo2::kPostProcess;

static void __fastcall PostProcessHook(void* self, void*)
{
    if (g_vrEye.active && g_vrEye.mirror) return;
    if (g_vrEye.active) MirrorsIntoEye();
    oPostProcess(self);
}

// Renders both eyes for the headset (they are submitted at Present, with the HUD). Returns true
// when the eye textures hold this frame's eyes. The caller renders (or mirrors) the monitor view
// after.
bool RenderVrEyes(IDirect3DDevice9* dev, BYTE* ctx, BYTE** cams, float* fovScale, BYTE* cam)
{
    double t0 = NowSeconds();
    // Test (PaceHz with VrEyesWithoutHeadset): the eyes are rendered at the pretend headset's pace
    // with no OpenXR frame at all, for the full load without an idle headset holding frames up.
    bool noXr = g_cfg.testPaceHz > 0 && g_cfg.vrEyesWithoutHeadset;
    bool render = !noXr && xr::BeginFrame();
    double t1 = NowSeconds();
    g_vrStats.wait += t1 - t0;
    // A screenshot request renders the eyes even when the headset wants no frame (asleep), so
    // they can be checked without wearing it.
    bool dumpOnly = !render && (g_saveEyesThisFrame || g_cfg.vrEyesWithoutHeadset) && (noXr || xr::FrameOpen());
    if ((!render && !dumpOnly) || !EnsureVrEyeTargets(dev))
    {
        xr::EndFrameEmpty();
        return false;
    }

    g_gpu.Begin(dev);
    float* camM = (float*)(cam + fo2::kCamMatrix);
    // Every copy of the camera's placement follows the eye (see game.h). The past views too, each the
    // eye where it would have been with the head as it is now and the camera where it was then: a
    // streak shows the particle's motion against the car, as on the monitor, and not the head's. The
    // stunt cars' jet is all streaks that keep up with the car: only the camera's motion draws its
    // flame out behind the nozzle.
    float* camM2 = (float*)(cam + fo2::kCamMatrixCopy2);
    float* camViews = (float*)(cam + fo2::kCamViews);
    float* camFov = (float*)(cam + fo2::kCamFov);
    float anchor[16], anchor2[16], views[fo2::kCamViewCount * 16];
    float savedCam[16];
    memcpy(savedCam, camM, sizeof(savedCam));
    memcpy(anchor, camM, sizeof(anchor));
    bool flying = FlightEye(anchor);  // the driver thrown out: the eyes in his head ([VR] EjectView)
    bool cockpit = !flying && CockpitEye(anchor);
    FlightSeen(anchor, cockpit);
    memcpy(anchor2, camM2, sizeof(anchor2));
    memcpy(views, camViews, sizeof(views));
    float savedFov = *camFov;
    float savedScale = fovScale ? fovScale[0] : 1.0f;
    int savedVp[4];
    memcpy(savedVp, (const void*)fo2::kViewportX, sizeof(savedVp));

    // The engine culls with a symmetric frustum built from the camera's horizontal field of view
    // and aspect ratio: widen it until it contains both eyes' frustums. RenderViews takes the
    // aspect ratio from the game's screen shape (16:9); for the eyes it gets theirs (about
    // square). With 16:9 the field of view would have to grow to some 138 degrees to reach as low
    // and high as the eyes do, and the engine picks its levels of detail by the field of view too:
    // the wider it is, the smaller and simpler everything is drawn.
    xr::EyeFov fov[2] = { xr::Fov(0), xr::Fov(1) };
    xr::EyePose pose[2] = { xr::Pose(0), xr::Pose(1) };
    ApplyHeadOffsets(pose[0]);
    ApplyHeadOffsets(pose[1]);
    SameDirection(pose);
    if (!rec::ReplayView(pose, fov)) rec::RecordView(pose, fov);
    // Mirror=2: the eyes take in the room beside the headset's picture (SteadyRoom), the right eye's left for right.
    if (g_vrt.w != g_vrt.eyeW || g_vrt.h != g_vrt.eyeH)
        for (int eye = 0; eye < 2; eye++)
        {
            xr::EyeFov& f = fov[eye];
            float px = (f.right - f.left) / (float)g_vrt.eyeW, py = (f.up - f.down) / (float)g_vrt.eyeH;
            f.left -= px * (float)g_vrt.room[eye ? 1 : 0];
            f.right += px * (float)g_vrt.room[eye ? 0 : 1];
            f.up += py * (float)g_vrt.room[2];
            f.down -= py * (float)g_vrt.room[3];
        }
    g_steady.eye = pose[0];
    g_steady.eyeFov = fov[0];  // (as the headset shows it: the speed zoom only shrinks what the picture shows)
    if (cockpit) MoveCockpitCards(anchor);
    // The speed zoom ([VR] SpeedZoom): at speed the game widens its horizontal field of view (by fovScale,
    // up to some 1.3 times the angle) and stretches its picture sideways by half as much, and the world
    // shrinks into the distance. The headset's field of view is its own: instead the eyes are rendered
    // with their angles widened as the monitor's are, and shown in the headset as they are, so the
    // world shrinks the same. The game's own widening stays off in the eyes (fovScale 1, below).
    float zoom = flying ? 1.0f : 1.0f + (savedScale - 1.0f) * g_cfg.vrSpeedZoom;
    if (zoom > 1.0f)
    {
        float half = 0.5f * savedFov;
        float kH = tanf(std::min(half * zoom, 1.5f)) / tanf(half);
        float kV = kH / (1.0f + 0.5f * (zoom - 1.0f));
        for (auto& f : fov)
        {
            f.left *= kH;
            f.right *= kH;
            f.up *= kV;
            f.down *= kV;
        }
    }
    float aspect = (float)g_vrt.w / (float)g_vrt.h;
    float* screenShape = (float*)fo2::kScreenShape;
    float savedShape[2] = { screenShape[0], screenShape[1] };
    float tanH = 0, tanV = 0;
    for (const auto& f : fov)
    {
        tanH = std::max(tanH, std::max(-f.left, f.right));
        tanV = std::max(tanV, std::max(f.up, -f.down));
    }
    float cover = std::max(tanH, tanV * aspect) * 1.02f;

    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    if (fovScale) fovScale[0] = 1.0f;
    int* screenSize = EngineScreenSize();
    int savedScreen[2] = { screenSize[0], screenSize[1] };
    screenSize[0] = (int)g_vrt.w;
    screenSize[1] = (int)g_vrt.h;
    screenShape[0] = (float)g_vrt.w;
    screenShape[1] = (float)g_vrt.h;
    OwnScreenShape(true);
    // The far clipping plane: the track's visibility (MaxVisibility, 300-700 m) or the camera's
    // 1000 m. Beyond it the haze is not yet thick enough to hide what pops in there: [VR]
    // ViewDistance moves it out.
    float* camFar = (float*)(cam + fo2::kCamFar);
    float savedFar = *camFar;
    if (g_cfg.vrViewDistance > 0.0f) *camFar = savedFar * g_cfg.vrViewDistance;
    // The near clipping plane: the camera inside the car leaves out what is nearer than 0.3 m, which in
    // the cockpit is the roof over one's head and the pillars beside it (CockpitNear; [VR] NearClip the
    // nearest it may come).
    float* camNear = (float*)(cam + fo2::kCamNear);
    float savedNear = *camNear;
    if (cockpit && g_cfg.vrNearClip > 0.0f) *camNear = CockpitNear(anchor, pose, fov, savedNear);
    if (flying) *camNear = std::min(savedNear, kFlightNear);
    // Grass ends (fading out) a fixed distance ahead, which in the headset is plainly in view:
    // [VR] GrassDistance moves both ends of it.
    float* grass = (float*)(ctx + fo2::kCtxPlantFar);
    float savedGrass[2] = { grass[0], grass[1] };
    if (g_cfg.vrGrassDistance > 0.0f)
    {
        grass[0] *= g_cfg.vrGrassDistance;
        grass[1] *= g_cfg.vrGrassDistance;
    }
    // The lens flare's monitor and the speed blur's centre are measured against the game camera's own
    // view (see LensFlare and EyeBlurCenter).
    g_vrEye.monitorFov = savedFov;
    g_vrEye.monitorTanH = tanf(0.5f * savedFov);
    g_vrEye.monitorTanV = g_vrEye.monitorTanH * savedShape[1] / savedShape[0];
    g_vrEye.flareTanV = std::max(tanV, tanH * (float)g_screenH / (float)g_screenW) * 1.02f;
    g_vrEye.flareTanH = g_vrEye.flareTanV * (float)g_screenW / (float)g_screenH;
    // The camera's past placements, each moved along with the camera's present one where that differs
    // from the game's own (C_k * V_0 * anchor).
    float pastAnchor[fo2::kCamViewCount][16];
    for (int i = 1; i < fo2::kCamViewCount; i++)
    {
        float c[16], t[16];
        InvertCamera(views + 16 * i, c);
        MulMatrix(c, views, t);
        MulMatrix(t, anchor, pastAnchor[i]);
    }
    UseEyeFilterTextures(dev);
    // The speed blur ([VR] SpeedBlur): from where the game camera looks, in each eye (EyeBlurCenter), with
    // the eyes' speed factor SpeedBlur times the game's (which it sets anew every frame); 0 is off. The
    // blur's settings are only filled in at its first use, inside an eye's render: until then they are
    // left alone.
    float* blurSpeed = (float*)fo2::kRadialBlurSpeed;
    float* blurCenter = (float*)fo2::kRadialBlurCenter;
    float savedBlurSpeed[4], savedBlurCenter[2] = {};
    bool blurCenterSaved = false;
    memcpy(savedBlurSpeed, blurSpeed, sizeof(savedBlurSpeed));
    float blurScale = flying ? 0.0f : g_cfg.vrSpeedBlur;
    for (int i = 0; i < 4; i++) blurSpeed[i] *= blurScale;
    NoteParticleLists();
    ReverseEyeDepth(dev, true);  // ([VR] FloatDepth) the eyes' and the mirrors'
    // The side mirrors' pictures for this frame, laid on their glass in each eye as it renders (MirrorsIntoEye).
    if (cockpit) RenderMirrors(dev, ctx, cams, fovScale, cam, anchor, pose, fov, pastAnchor);
    for (int eye = 0; eye < 2; eye++)
    {
        float e[16];
        BuildEyeMatrix(anchor, pose[eye], e);
        if (eye == 0)
        {
            memcpy(g_markers.eye, e, sizeof(e));  // (the HUD's markers chosen as this eye looks)
            g_markers.eyeValid = true;
        }
        float ev[16];
        InvertCamera(e, ev);
        memcpy(camM, e, sizeof(e));
        memcpy(camM2, e, sizeof(e));
        memcpy(camViews, ev, sizeof(ev));
        for (int i = 1; i < fo2::kCamViewCount; i++)
        {
            float pe[16];
            BuildEyeMatrix(pastAnchor[i], pose[eye], pe);
            InvertCamera(pe, camViews + 16 * i);
        }
        if (blurScale > 0 && *(const BYTE*)fo2::kPostSettingsLoaded)
        {
            if (!blurCenterSaved) memcpy(savedBlurCenter, blurCenter, sizeof(savedBlurCenter));
            blurCenterSaved = true;
            EyeBlurCenter(savedBlurCenter, pose[eye], fov[eye], blurCenter);
        }
        *camFov = 2.0f * atanf(cover);
        CopyGameScreenTexture(dev, eye == 0);
        SetFullViewportGlobals();
        dev->SetRenderTarget(0, g_vrt.rt[eye]);
        dev->SetDepthStencilSurface(g_vrt.ds[eye]);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
        g_vrEye.active = true;
        g_vrEye.index = eye;
        g_vrEye.cockpit = cockpit;
        g_vrEye.mirror = false;
        g_vrEye.w = g_vrt.w;
        g_vrEye.h = g_vrt.h;
        g_vrEye.coverTan = cover;
        g_vrEye.fov = fov[eye];
        share::BeginEye(eye);
        g_mirrorEye = { cockpit ? cam : nullptr, eye, false };
        oRenderViews(ctx, cams, fovScale);
        MirrorsIntoEye();  // (if neither the windows nor the post-processing brought them on)
        g_mirrorEye.cam = nullptr;
        prof::TraceDraws(false);  // a frame trace lists the left eye's draws only
        if (cockpit) DrawCockpitCards(dev, cam, eye);
        DrawMarkers(dev, cam);
        g_vrEye.active = false;
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(g_vrt.tex[eye]->GetSurfaceLevel(0, &s)))
        {
            dev->StretchRect(g_vrt.rt[eye], nullptr, s, nullptr, D3DTEXF_NONE);
            s->Release();
        }
    }
    share::EndEyes();
    ReverseEyeDepth(dev, false);
    if (flying) FlightDriverBack();
    RestoreFilterTextures();
    memcpy(blurSpeed, savedBlurSpeed, sizeof(savedBlurSpeed));
    if (blurCenterSaved) memcpy(blurCenter, savedBlurCenter, sizeof(savedBlurCenter));
    g_gpu.EyesDone();
    screenSize[0] = savedScreen[0];
    screenSize[1] = savedScreen[1];
    screenShape[0] = savedShape[0];
    screenShape[1] = savedShape[1];
    OwnScreenShape(false);
    grass[0] = savedGrass[0];
    grass[1] = savedGrass[1];
    *camFar = savedFar;
    *camNear = savedNear;
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    if (g_saveEyesThisFrame)
    {
        g_saveEyesThisFrame = false;
        g_eyeShotPending = true;  // saved at Present, as they go to the headset
        Log("VR eye fov: left %.3f %.3f %.3f %.3f, right %.3f %.3f %.3f %.3f (tan l r u d), cover %.3f%s", fov[0].left, fov[0].right,
            fov[0].up, fov[0].down, fov[1].left, fov[1].right, fov[1].up, fov[1].down, cover, cockpit ? ", cockpit" : flying ? ", in the thrown driver's head" : "");
        if (cockpit) Log("cockpit: the sun's share past the car %.2f, near clipping plane %.3f m", g_sunShare, g_cockpitNear);
    }
    memcpy(camM, savedCam, sizeof(savedCam));
    memcpy(camM2, anchor2, sizeof(anchor2));
    memcpy(camViews, views, sizeof(views));
    *camFov = savedFov;
    if (fovScale) fovScale[0] = savedScale;
    oSetViewport(*(void**)fo2::kDeviceD3d, savedVp[0], savedVp[1], savedVp[2], savedVp[3], -1);

    g_vrStats.eyesT.Add(NowSeconds() - t1);
    if (dumpOnly)
    {
        xr::EndFrameEmpty();
        return true;
    }
    g_vrStats.eyes += NowSeconds() - t1;
    g_eyesPending = true;
    return true;
}

void __stdcall RenderViewsHook(BYTE* ctx, BYTE** cams, float* fovScale)
{
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)fo2::kD3DDevice;
    HookDevice(dev);

    double t0 = NowSeconds();
    bool race = InRace();
    int views = *(int*)(ctx + fo2::kCtxViewCount);
    BYTE* cam = views == 1 && cams ? cams[0] : nullptr;
    // The cockpit's eyes and instruments are worked out once a car, and the next race may have its car
    // where the last one's was.
    if (!race)
    {
        g_eyeCar = g_dash.car = g_cabin.car = g_mirrors.car = nullptr;
        g_steady.valid = false;  // (the monitor's steady view starts anew)
    }

    if (dev && g_screenW == 0)
    {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            g_screenW = d.Width;
            g_screenH = d.Height;
            bb->Release();
            SetRetryTextWidth(g_screenW);
        }
    }
    if (race && dev && !g_loggedTargets)
    {
        g_loggedTargets = true;
        LogRenderTargets(dev);
    }
    bool logNow = race && t0 - g_lastLog >= g_cfg.logIntervalSeconds;
    if (race && cam && g_cfg.stepProbe) ProbeCamera((const float*)(cam + fo2::kCamMatrix));
    double frameStart = FrameStartTime();
    if (race && frameStart > 0 && t0 > frameStart && t0 - frameStart < 1.0)
    {
        g_frameGame = t0 - frameStart;
        g_vrStats.game.Add(g_frameGame);
    }
    prof::SetPhase(prof::Eyes);
    if (race && g_cfg.testSampling) prof::StartSampling();

    g_inRenderViews = true;
    bool testPace = g_cfg.testPaceHz > 0 && !g_cfg.vrEyesWithoutHeadset;  // pretend headset only (see pacing.cpp)
    if (race && cam && dev && xr::Running() && !testPace)
    {
        bool eyes = RenderVrEyes(dev, ctx, cams, fovScale, cam);
        g_vrFrameDone = true;
        g_steady.shown = false;
        if (eyes && g_cfg.vrMirror == 2)
            SteadyMonitorView(dev);
        else if (eyes && g_cfg.vrMirror)
            MirrorLeftEye(dev, xr::Fov(0));
        else
            oRenderViews(ctx, cams, fovScale);  // the monitor's own view
        if (eyes && g_cfg.vrHud) BeginHudCapture(dev);
        if (eyes) g_renderViewsEnd = NowSeconds();
        if (g_cfg.testPaceHz > 0) g_vrFrameDone = true;  // no flat screen in the pacing tests
    }
    else if (race && testPace)
    {
        g_vrFrameDone = true;  // no flat screen either: the headset gets no frames in this test
        oRenderViews(ctx, cams, fovScale);
    }
    else
        oRenderViews(ctx, cams, fovScale);
    g_inRenderViews = false;

    if (race && !g_seenLogged && g_vpSeenCount + g_frSeenCount > 0)
    {
        g_seenLogged = true;
        for (int i = 0; i < g_vpSeenCount; i++)
        {
            const VpCall& v = g_vpSeen[i];
            Log("viewport inside RenderViews: x %d y %d w %d h %d view %d", v.x, v.y, v.w, v.h, v.view);
        }
        for (int i = 0; i < g_frSeenCount; i++)
        {
            const float* f = g_frSeenVal[i];
            Log("frustum inside RenderViews: %p%s  l %.4f r %.4f b %.4f t %.4f n %.3f f %.1f", (const void*)g_frSeen[i],
                cam && g_frSeen[i] == (const float*)(cam + fo2::kCamFrustum) ? " (player camera)" : "", f[0], f[1], f[2], f[3],
                f[4], f[5]);
        }
    }

    double dt = NowSeconds() - t0;
    g_frameRenderViews = dt;
    prof::SetPhase(prof::Hud);
    g_stats.renderViews++;
    g_stats.rvTime += dt;
    if (dt > g_stats.rvTimeMax) g_stats.rvTimeMax = dt;
    if (logNow) LogCamera(ctx, cams, fovScale);
}

}  // namespace

bool SteeringWheel(float hub[3], float axes[9]) { return WheelPose(hub, axes); }

void OnDeviceCreated(IDirect3DDevice9* dev)
{
    HookDevice(dev);
    xr::OnDevice(dev);
}

bool InstallRenderHooks()
{
    if (!PatchCall(fo2::kRenderViewsCall, fo2::kRenderViews, (const void*)&RenderViewsHook)) return false;
    bool ok = PatchCall(fo2::kLensFlareCall, fo2::kLensFlare, (const void*)&LensFlareHook);
    ok &= PatchCall(fo2::kSunGlowCall, fo2::kSunGlow, (const void*)&SunGlowHook);
    ok &= PatchPointer(fo2::kSlotSpriteRect, fo2::kSpriteRect, (const void*)&SpriteRectHook);
    ok &= PatchPointer(fo2::kSlotSpriteQuad, fo2::kSpriteQuad, (const void*)&SpriteQuadHook);
    if (!prof::HookParticles()) Log("profiler: particle call differs - not timed");
    if (!PatchTrailFill()) Log("particles: trail fill differs - not patched");
    if (g_cfg.vrEnabled && !PatchLodDistance(g_cfg.vrLodDistance)) Log("level of detail: code differs - distances unchanged");
    if (g_cfg.vrEnabled) Log(share::Install() ? "eyes: the right eye reuses the left eye's leaf and grass cards" : "eyes: leaf pass differs - each eye builds its own");
    if (g_cfg.vrEnabled && !PatchCall(fo2::kCullViewCall, fo2::kCullView, (const void*)&CullViewHook))
        Log("mirrors: the culling call differs - a mirror draws what a wide frustum takes in");
    if (g_cfg.vrEnabled && !PatchCall(fo2::kCarShadowCall, fo2::kCarShadow, (const void*)&CarShadowHook))
        Log("mirrors: the car shadow call differs - each mirror renders the shadows again");
    if (g_cfg.vrEnabled && !PatchPointer(fo2::kSlotPostProcess, fo2::kPostProcess, (const void*)&PostProcessHook))
        Log("mirrors: the post-processing differs - the mirrors go on after the eyes' windows");
    if (g_cfg.vrEnabled && !PatchCall(fo2::kCarViewCall, fo2::kSetCarView, (const void*)&CarViewHook))
        Log("cockpit: the car view call differs - the camera inside the car shows no driver");
    if (g_cfg.vrEnabled && !PatchCall(fo2::kDynamicBatchDrawCall, fo2::kBatchDraw, (const void*)&DynamicBatchDrawHook))
        Log("cockpit: the batch draw call differs - the windows are drawn as the game draws them");
    if (g_cfg.vrEnabled)
        Log("eject: %s", g_cfg.vrEjectView == 0 ? "the game's camera when the driver is thrown out (EjectView=0)"
                         : g_cfg.vrEjectView == 1 ? "the eyes stay in the driver's head when he is thrown out of the cockpit"
                                                  : "the eyes go into the driver's head when he is thrown out (EjectView=2)");
    if (g_cfg.vrEnabled && !PatchCall(fo2::kSampleAnimCall, fo2::kSampleAnim, (const void*)&SampleAnimHook))
        Log("drivers: the animation call differs - the hands move in the animation's steps");
    if (g_cfg.vrEnabled && (g_cfg.vrWheelRange > 0 || g_cfg.vrGrabWheel))
    {
        // (the driver's first: with only the wheel's hooked, the hands would leave it)
        g_ownWheel = PatchCall(fo2::kPoseDriverCall, fo2::kPoseDriver, (const void*)&PoseDriverHook) &&
                     PatchCall(fo2::kPoseCarPartsCall, fo2::kPoseCarParts, (const void*)&PoseCarPartsHook);
        if (!g_ownWheel)
            Log("cockpit: the car's posing calls differ - the steering wheel turns as the car's steering");
        else if (g_cfg.vrWheelRange > 0)
            Log("cockpit: the steering wheel turns as a game controller's of %.0f degrees lock to lock (WheelRange), up to its 90", g_cfg.vrWheelRange);
    }
    // (the camera is turned for the markers only when their triangles are taken: on the HUD they would stand wrong)
    if (g_cfg.vrEnabled && (!HookMarkerDraw() || !PatchCall(fo2::kMarkersCall, fo2::kMarkers, (const void*)&MarkersHook)))
        Log("markers: the HUD's calls differ - the rivals' markers stay on the HUD");
    ok &= PatchPointer(fo2::kSlotSetViewport, fo2::kSetViewport, (const void*)&SetViewportHook);
    ok &= PatchPointer(fo2::kSlotSetCamera, fo2::kSetCamera, (const void*)&SetCameraHook);
    ok &= PatchPointer(fo2::kSlotSetFrustum, fo2::kSetFrustum, (const void*)&SetFrustumHook);
    ok &= PatchPointer(fo2::kSlotEnginePresent, fo2::kEnginePresent, (const void*)&EnginePresentHook);
    return ok;
}
