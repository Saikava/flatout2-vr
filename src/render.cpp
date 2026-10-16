// Probe build: hooks the race render path, logs what the engine does with cameras, frustums,
// viewports and render targets, and offers test modes (head sway, side-by-side stereo on the
// monitor, eyes rendered into offscreen targets, double render) to verify that the engine can
// be driven per eye.
#include "render.h"

#include "background.h"
#include "dxvk_interop.h"
#include "eyeshare.h"
#include "game.h"
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

// Lens flare call in RenderViews (register arguments, so the hook is naked). The flare is placed
// with the camera's symmetric field of view and would sit in the wrong place in an eye; it is
// skipped while VR eyes render.
static bool g_skipLensFlare;
static uintptr_t g_lensFlareFn = fo2::kLensFlare;

static __declspec(naked) void LensFlareHook()
{
    __asm {
        cmp g_skipLensFlare, 0
        jne skip
        jmp dword ptr [g_lensFlareFn]
    skip:
        ret
    }
}

// The screen-space crash sparks (particle object in eax) fly at the monitor's camera; on the VR HUD
// panel they would fly across a flat sheet in front of the player, so they are left out while the
// HUD goes to the headset.
static bool g_skipHudParticles;
static uintptr_t g_hudParticlesFn = fo2::kHudParticles;

static __declspec(naked) void HudParticlesHook()
{
    __asm {
        cmp g_skipHudParticles, 0
        jne skip
        jmp dword ptr [g_hudParticlesFn]
    skip:
        ret
    }
}

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
              BeginScene = 41, EndScene = 42, Clear = 43, SetRenderState = 57;
}
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using StretchRectFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                          D3DTEXTUREFILTERTYPE);
using SetRenderTargetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepthStencilSurfaceFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
using SceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ClearFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetRenderStateFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);

ResetFn oReset;
StretchRectFn oStretchRect;
SetRenderTargetFn oSetRenderTarget;
SetDepthStencilSurfaceFn oSetDepthStencilSurface;
SceneFn oBeginScene, oEndScene;
ClearFn oClear;
SetRenderStateFn oSetRenderState;
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
    float coverTan = 0;  // tan(fov/2) the camera was given so the engine's culling covers the eye
    xr::EyeFov fov = {};
} g_vrEye;
// VR races: the game draws its HUD (everything after RenderViews until Present: HUD, pause menu,
// crash sparks) into this target instead of the back buffer, over transparent black, so that it
// can go to the headset on its own panel and be laid over the monitor's picture. The colours end
// up premultiplied by alpha; the alpha channel is blended on its own ("over" for normal blending,
// left alone for additive blending, see SetRenderStateHook).
struct HudTargets
{
    IDirect3DTexture9* tex = nullptr;    // back buffer size
    IDirect3DSurface9* rt = nullptr;     // its level 0
    IDirect3DSurface9* ds = nullptr;     // single-sample depth of the same size
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
        w = (int)g_vrt.w;
        h = (int)g_vrt.h;
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
            return;
        }
    }
    oSetFrustum(self, fr);
}

// ---- device hooks ---------------------------------------------------------------------------

void ReleaseHudTargets()
{
    IUnknown* all[6] = { g_hud.tex, g_hud.rt, g_hud.ds, g_hud.panel, g_hud.gameRT, g_hud.gameDS };
    for (IUnknown* r : all)
        if (r) r->Release();
    g_hud = HudTargets();
}

bool EnsureHudTargets(IDirect3DDevice9* dev)
{
    if (g_hud.tex && g_hud.w == g_screenW && g_hud.h == g_screenH) return true;
    if (g_hud.failed || g_screenW == 0) return false;
    ReleaseHudTargets();
    UINT w = g_screenW, h = g_screenH;
    // The panel covers about 60 degrees of view: 1920 pixels across are sharper than the headset.
    UINT pw = std::min(w, 1920u), ph = (UINT)((unsigned long long)h * pw / w);
    bool ok = SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hud.tex, nullptr)) &&
              SUCCEEDED(g_hud.tex->GetSurfaceLevel(0, &g_hud.rt)) &&
              SUCCEEDED(dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_hud.ds, nullptr)) &&
              SUCCEEDED(dev->CreateTexture(pw, ph, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hud.panel, nullptr));
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
    oSetRenderTarget(dev, 0, g_hud.rt);
    oSetDepthStencilSurface(dev, g_hud.ds);
    oClear(dev, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    oSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    oSetRenderState(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    SetHudAlphaBlend(dev, destBlend);
    oSetRenderState(dev, D3DRS_COLORWRITEENABLE, colorWrite | D3DCOLORWRITEENABLE_ALPHA);
    g_hud.active = true;
    g_skipHudParticles = true;
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
    return oSetRenderState(dev, state, value);
}

void ReleaseVrTargets()
{
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

bool EnsureVrEyeTargets(IDirect3DDevice9* dev)
{
    if (g_vrt.tex[0]) return true;
    if (g_vrt.failed) return false;
    unsigned w, h;
    xr::EyeSize(&w, &h);
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
    }
    else
        xr::EndFrameEmpty();
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
    return oStretchRect(dev, src, sr, dst, dr, f);
}

HRESULT __stdcall SetRenderTargetHook(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
    if (g_trace == Trace::Running) TraceLine("SetRenderTarget %u %s", (unsigned)index, Describe(s).c_str());
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
    return oClear(dev, count, rects, flags, color, z, stencil);
}

bool KeyPressed(int vk, bool& wasDown)
{
    bool down = HotkeysAllowed() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
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
        }
    }

    if (g_eyesPending)
    {
        g_eyesPending = false;
        double s0 = NowSeconds();
        bool hud = g_cfg.vrHud && g_hud.captured;
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
    if (KeyPressed(g_cfg.keyRecenter, g_recenterKeyWasDown)) xr::Recenter();
    if (xr::Running() && !g_vrFrameDone)
    {
        double w0 = NowSeconds();
        bool render = xr::BeginFrame();
        g_vrStats.wait += NowSeconds() - w0;
        if (render)
            SubmitVrScreen(dev);
        else
            xr::EndFrameEmpty();
    }
    g_vrFrameDone = false;

    if (now - g_lastLog >= g_cfg.logIntervalSeconds && g_stats.frames > 0)
    {
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

// Test: the head moved by [Test] HeadOffset and turned about the vertical axis (the background
// mode's "head" command).
void ApplyTestHead(xr::EyePose& p)
{
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
    for (int i = 0; i < 3; i++) p.position[i] += g_cfg.testHeadOffset[i];
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

// The engine's back buffer size (DeviceD3d fields), which its post-processing uses for the
// target of its final passes.
int* EngineScreenSize() { return (int*)(*(BYTE**)fo2::kDeviceD3d + fo2::kDevWidth); }

// Draws a texture (or the part u0..u1, v0..v1 of it) over the whole bound render target, opaque
// or as premultiplied alpha over what is there. Every device state is put back, so the engine's
// state cache stays valid.
void DrawScreenQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float u0, float v0, float u1, float v1, bool premultiplied)
{
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt))) return;
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    rt->Release();
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) return;

    struct V { float x, y, z, rhw, u, v; };
    float W = (float)d.Width - 0.5f, H = (float)d.Height - 0.5f;
    const V quad[4] = { { -0.5f, -0.5f, 0, 1, u0, v0 }, { W, -0.5f, 0, 1, u1, v0 }, { -0.5f, H, 0, 1, u0, v1 }, { W, H, 0, 1, u1, v1 } };

    D3DVIEWPORT9 vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
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
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
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
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    saved->Apply();
    saved->Release();
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

// Present: the game's drawing goes back to its own targets; the HUD is laid over the monitor's
// picture and copied to the headset's panel texture.
void EndHudCapture(IDirect3DDevice9* dev)
{
    g_hud.active = false;
    g_skipHudParticles = false;
    oSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    oSetRenderState(dev, D3DRS_COLORWRITEENABLE, g_hud.gameColorWrite);
    oSetRenderTarget(dev, 0, g_hud.gameRT);
    oSetDepthStencilSurface(dev, g_hud.gameDS);
    if (g_hud.gameRT) g_hud.gameRT->Release();
    if (g_hud.gameDS) g_hud.gameDS->Release();
    g_hud.gameRT = g_hud.gameDS = nullptr;
    DrawScreenQuad(dev, g_hud.tex, 0.0f, 0.0f, 1.0f, 1.0f, true);
    IDirect3DSurface9* panel = nullptr;
    if (SUCCEEDED(g_hud.panel->GetSurfaceLevel(0, &panel)))
    {
        g_hud.captured = SUCCEEDED(oStretchRect(dev, g_hud.rt, nullptr, panel, nullptr, D3DTEXF_LINEAR));
        panel->Release();
    }
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
    // Every copy of the camera's placement follows the eye (see game.h).
    // The past views too: in the eyes the streaks of moving particles show their own motion in the
    // world, not the camera's (nor the head's).
    float* camM2 = (float*)(cam + fo2::kCamMatrixCopy2);
    float* camViews = (float*)(cam + fo2::kCamViews);
    float* camFov = (float*)(cam + fo2::kCamFov);
    float anchor[16], anchor2[16], views[fo2::kCamViewCount * 16];
    memcpy(anchor, camM, sizeof(anchor));
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
    ApplyTestHead(pose[0]);
    ApplyTestHead(pose[1]);
    SameDirection(pose);
    if (!rec::ReplayView(pose, fov)) rec::RecordView(pose, fov);
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
    if (fovScale) fovScale[0] = 1.0f;  // no speed zoom in VR
    int* screenSize = EngineScreenSize();
    int savedScreen[2] = { screenSize[0], screenSize[1] };
    screenSize[0] = (int)g_vrt.w;
    screenSize[1] = (int)g_vrt.h;
    screenShape[0] = (float)g_vrt.w;
    screenShape[1] = (float)g_vrt.h;
    // The far clipping plane: the track's visibility (MaxVisibility, 300-700 m) or the camera's
    // 1000 m. Beyond it the haze is not yet thick enough to hide what pops in there: [VR]
    // ViewDistance moves it out.
    float* camFar = (float*)(cam + fo2::kCamFar);
    float savedFar = *camFar;
    if (g_cfg.vrViewDistance > 0.0f) *camFar = savedFar * g_cfg.vrViewDistance;
    // Grass ends (fading out) a fixed distance ahead, which in the headset is plainly in view:
    // [VR] GrassDistance moves both ends of it.
    float* grass = (float*)(ctx + fo2::kCtxPlantFar);
    float savedGrass[2] = { grass[0], grass[1] };
    if (g_cfg.vrGrassDistance > 0.0f)
    {
        grass[0] *= g_cfg.vrGrassDistance;
        grass[1] *= g_cfg.vrGrassDistance;
    }
    g_skipLensFlare = true;
    for (int eye = 0; eye < 2; eye++)
    {
        float e[16];
        BuildEyeMatrix(anchor, pose[eye], e);
        float ev[16];
        InvertCamera(e, ev);
        memcpy(camM, e, sizeof(e));
        memcpy(camM2, e, sizeof(e));
        for (int i = 0; i < fo2::kCamViewCount; i++) memcpy(camViews + 16 * i, ev, sizeof(ev));
        *camFov = 2.0f * atanf(cover);
        SetFullViewportGlobals();
        dev->SetRenderTarget(0, g_vrt.rt[eye]);
        dev->SetDepthStencilSurface(g_vrt.ds[eye]);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
        g_vrEye.active = true;
        g_vrEye.coverTan = cover;
        g_vrEye.fov = fov[eye];
        share::BeginEye(eye);
        oRenderViews(ctx, cams, fovScale);
        prof::TraceDraws(false);  // a frame trace lists the left eye's draws only
        g_vrEye.active = false;
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(g_vrt.tex[eye]->GetSurfaceLevel(0, &s)))
        {
            dev->StretchRect(g_vrt.rt[eye], nullptr, s, nullptr, D3DTEXF_NONE);
            s->Release();
        }
    }
    share::EndEyes();
    g_skipLensFlare = false;
    g_gpu.EyesDone();
    screenSize[0] = savedScreen[0];
    screenSize[1] = savedScreen[1];
    screenShape[0] = savedShape[0];
    screenShape[1] = savedShape[1];
    grass[0] = savedGrass[0];
    grass[1] = savedGrass[1];
    *camFar = savedFar;
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    if (g_saveEyesThisFrame)
    {
        g_saveEyesThisFrame = false;
        // The resolved eye textures: what the headset gets.
        for (int eye = 0; eye < 2; eye++)
        {
            IDirect3DSurface9* s = nullptr;
            if (SUCCEEDED(g_vrt.tex[eye]->GetSurfaceLevel(0, &s)))
            {
                SaveSurfacePng(dev, s, ShotPath(eye == 0 ? "vr_left" : "vr_right").c_str());
                s->Release();
            }
        }
        Log("VR eye fov: left %.3f %.3f %.3f %.3f, right %.3f %.3f %.3f %.3f (tan l r u d), cover %.3f", fov[0].left, fov[0].right,
            fov[0].up, fov[0].down, fov[1].left, fov[1].right, fov[1].up, fov[1].down, cover);
    }
    memcpy(camM, anchor, sizeof(anchor));
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
        if (eyes && g_cfg.vrMirror)
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

void OnDeviceCreated(IDirect3DDevice9* dev)
{
    HookDevice(dev);
    xr::OnDevice(dev);
}

bool InstallRenderHooks()
{
    if (!PatchCall(fo2::kRenderViewsCall, fo2::kRenderViews, (const void*)&RenderViewsHook)) return false;
    bool ok = PatchCall(fo2::kLensFlareCall, fo2::kLensFlare, (const void*)&LensFlareHook);
    ok &= PatchCall(fo2::kHudParticlesCall, fo2::kHudParticles, (const void*)&HudParticlesHook);
    if (!prof::HookParticles()) Log("profiler: particle call differs - not timed");
    if (!PatchTrailFill()) Log("particles: trail fill differs - not patched");
    if (g_cfg.vrEnabled && !PatchLodDistance(g_cfg.vrLodDistance)) Log("level of detail: code differs - distances unchanged");
    if (g_cfg.vrEnabled) Log(share::Install() ? "eyes: the right eye reuses the left eye's leaf and grass cards" : "eyes: leaf pass differs - each eye builds its own");
    ok &= PatchPointer(fo2::kSlotSetViewport, fo2::kSetViewport, (const void*)&SetViewportHook);
    ok &= PatchPointer(fo2::kSlotSetCamera, fo2::kSetCamera, (const void*)&SetCameraHook);
    ok &= PatchPointer(fo2::kSlotSetFrustum, fo2::kSetFrustum, (const void*)&SetFrustumHook);
    ok &= PatchPointer(fo2::kSlotEnginePresent, fo2::kEnginePresent, (const void*)&EnginePresentHook);
    return ok;
}
