#include "profile.h"

#include "util.h"

#include <windows.h>
#include <d3d9.h>
#include <timeapi.h>

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace prof {
namespace {

enum Kind { Draw, Lock, Create, Upload, KindCount };
const char* const kKindNames[KindCount] = { "draws", "locks", "creates", "uploads" };
const char* const kPhaseNames[PhaseCount] = { "game", "eyes", "HUD" };

struct Counter
{
    int count = 0;
    double time = 0, max = 0;
    const char* maxWhat = "";
    unsigned maxArg = 0;
};

Counter g_frame[PhaseCount][KindCount];
Phase g_phase = Game;
double g_phaseStart[PhaseCount], g_phaseEnd[PhaseCount];

// ---- sampling ---------------------------------------------------------------------------------
// Where the game thread was: its instruction pointer and the first return addresses into the
// game's code found on its stack (who called the DXVK or Windows code it was in).
struct Sample
{
    double t;
    uint32_t eip;
    uint32_t ret[3];
};
constexpr int kSamples = 16384;
Sample g_samples[kSamples];
volatile LONG g_sampleCount;
HANDLE g_gameThread;

bool IsGameCode(uint32_t a) { return a >= 0x401000 && a < 0x650000; }

// A return address: the bytes before it are a call (E8 rel32, FF /2 with modrm forms).
bool LooksLikeReturn(uint32_t a)
{
    if (!IsGameCode(a)) return false;
    const BYTE* p = (const BYTE*)a;
    return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) ||
           (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10);
}

void ScanStack(uint32_t esp, uint32_t* ret)
{
    __try
    {
        const uint32_t* sp = (const uint32_t*)esp;
        int n = 0;
        for (int i = 0; i < 1024 && n < 3; i++)
            if (LooksLikeReturn(sp[i])) ret[n++] = sp[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

DWORD WINAPI SamplerThread(void*)
{
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;)
    {
        Sleep(1);
        if (SuspendThread(g_gameThread) == (DWORD)-1) continue;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_gameThread, &c))
        {
            Sample s = { NowSeconds(), c.Eip, {} };
            if (!IsGameCode(c.Eip)) ScanStack(c.Esp, s.ret);
            LONG i = g_sampleCount;
            g_samples[i % kSamples] = s;
            InterlockedIncrement(&g_sampleCount);
        }
        ResumeThread(g_gameThread);
    }
}

const char* ModuleName(uint32_t a)
{
    static char name[64];
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &m))
        return "?";
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* b = strrchr(path, '\\');
    strncpy(name, b ? b + 1 : path, sizeof(name) - 1);
    return name;
}

// The samples of one phase of the slow frame: the places they fell in, most frequent first.
void LogSamples(Phase p)
{
    if (!g_gameThread) return;
    struct Place
    {
        uint32_t eip, ret;
        int n;
    } places[64];
    int count = 0, total = 0;
    LONG end = g_sampleCount;
    for (LONG i = end - 1; i >= 0 && i > end - kSamples; i--)
    {
        const Sample& s = g_samples[i % kSamples];
        if (s.t < g_phaseStart[p]) break;
        if (s.t > g_phaseEnd[p]) continue;
        total++;
        // Game code: by address (64-byte granularity); elsewhere: by the module and its caller in the game.
        uint32_t eip = IsGameCode(s.eip) ? (s.eip & ~0x3Fu) : 0, ret = IsGameCode(s.eip) ? 0 : s.ret[0];
        HMODULE m = nullptr;
        if (!eip) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)s.eip, &m);
        uint32_t key = eip ? eip : (uint32_t)(uintptr_t)m;
        int j = 0;
        while (j < count && !(places[j].eip == key && places[j].ret == ret)) j++;
        if (j == count && count < 64) places[count++] = { key, ret, 0 };
        if (j < count) places[j].n++;
    }
    if (total == 0) return;
    std::sort(places, places + count, [](const Place& a, const Place& b) { return a.n > b.n; });
    char line[700];
    int n = snprintf(line, sizeof(line), "   %s samples %d:", kPhaseNames[p], total);
    for (int j = 0; j < count && j < 8 && n < (int)sizeof(line) - 80; j++)
    {
        if (IsGameCode(places[j].eip))
            n += snprintf(line + n, sizeof(line) - n, " game %06X x%d;", places[j].eip, places[j].n);
        else
            n += snprintf(line + n, sizeof(line) - n, " %s from %06X x%d;", places[j].eip ? ModuleName(places[j].eip) : "?", places[j].ret,
                          places[j].n);
    }
    Log("%s", line);
}
double g_lastSlowLog;
int g_slowLogged;

void Count(Kind k, double t, const char* what, unsigned arg)
{
    Counter& c = g_frame[g_phase][k];
    c.count++;
    c.time += t;
    if (t > c.max)
    {
        c.max = t;
        c.maxWhat = what;
        c.maxArg = arg;
    }
}

struct Timer
{
    double t0 = NowSeconds();
    double Elapsed() const { return NowSeconds() - t0; }
};

// A draw that took long (over 2 ms): which kind of shaders it used.
void LogSlowDraw(IDirect3DDevice9* d, const char* what, unsigned prims, double t)
{
    static int logged;
    if (logged >= 200) return;
    logged++;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    DWORD fvf = 0;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    d->GetFVF(&fvf);
    Log("slow draw %.2f ms: %s %u primitives, vertex shader %s, pixel shader %s, FVF %08X", 1000.0 * t, what, prims,
        vs ? "game" : "fixed function", ps ? "game" : "fixed function", (unsigned)fvf);
    if (vs) vs->Release();
    if (ps) ps->Release();
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

// ---- device -------------------------------------------------------------------------------------
using CreateTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateCubeTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
using CreateVertexBufferFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using CreateIndexBufferFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
using CreateSurfaceFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using UpdateSurfaceFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);
using UpdateTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
using DrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using CreateShaderFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const DWORD*, void**);

namespace slot {
constexpr int CreateTexture = 23, CreateCubeTexture = 25, CreateVertexBuffer = 26, CreateIndexBuffer = 27, CreateRenderTarget = 28,
              CreateDepthStencilSurface = 29, UpdateSurface = 30, UpdateTexture = 31, DrawPrimitive = 81, DrawIndexedPrimitive = 82,
              DrawPrimitiveUP = 83, DrawIndexedPrimitiveUP = 84, CreateVertexShader = 91, CreatePixelShader = 106;
constexpr int BufferLock = 11;   // IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9::Lock
constexpr int TextureLock = 19;  // IDirect3DTexture9::LockRect
}

CreateTextureFn oCreateTexture;
CreateCubeTextureFn oCreateCubeTexture;
CreateVertexBufferFn oCreateVertexBuffer;
CreateIndexBufferFn oCreateIndexBuffer;
CreateSurfaceFn oCreateRenderTarget, oCreateDepthStencilSurface;
UpdateSurfaceFn oUpdateSurface;
UpdateTextureFn oUpdateTexture;
DrawPrimitiveFn oDrawPrimitive;
DrawIndexedPrimitiveFn oDrawIndexedPrimitive;
DrawPrimitiveUPFn oDrawPrimitiveUP;
DrawIndexedPrimitiveUPFn oDrawIndexedPrimitiveUP;
CreateShaderFn oCreateVertexShader, oCreatePixelShader;

HRESULT __stdcall CreateTextureHook(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* s)
{
    Timer tm;
    HRESULT hr = oCreateTexture(d, w, h, l, u, f, p, t, s);
    Count(Create, tm.Elapsed(), "texture", w * 65536 + h);
    return hr;
}
HRESULT __stdcall CreateCubeTextureHook(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* s)
{
    Timer tm;
    HRESULT hr = oCreateCubeTexture(d, e, l, u, f, p, t, s);
    Count(Create, tm.Elapsed(), "cube texture", e);
    return hr;
}
HRESULT __stdcall CreateVertexBufferHook(IDirect3DDevice9* d, UINT n, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** b, HANDLE* s)
{
    Timer tm;
    HRESULT hr = oCreateVertexBuffer(d, n, u, fvf, p, b, s);
    Count(Create, tm.Elapsed(), "vertex buffer", n);
    return hr;
}
HRESULT __stdcall CreateIndexBufferHook(IDirect3DDevice9* d, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** b, HANDLE* s)
{
    Timer tm;
    HRESULT hr = oCreateIndexBuffer(d, n, u, f, p, b, s);
    Count(Create, tm.Elapsed(), "index buffer", n);
    return hr;
}
HRESULT __stdcall CreateRenderTargetHook(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL l,
                                         IDirect3DSurface9** o, HANDLE* s)
{
    Timer tm;
    HRESULT hr = oCreateRenderTarget(d, w, h, f, m, q, l, o, s);
    Count(Create, tm.Elapsed(), "render target", w * 65536 + h);
    return hr;
}
HRESULT __stdcall CreateDepthStencilSurfaceHook(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL x,
                                                IDirect3DSurface9** o, HANDLE* s)
{
    Timer tm;
    HRESULT hr = oCreateDepthStencilSurface(d, w, h, f, m, q, x, o, s);
    Count(Create, tm.Elapsed(), "depth surface", w * 65536 + h);
    return hr;
}
HRESULT __stdcall UpdateSurfaceHook(IDirect3DDevice9* d, IDirect3DSurface9* a, const RECT* r, IDirect3DSurface9* b, const POINT* p)
{
    Timer tm;
    HRESULT hr = oUpdateSurface(d, a, r, b, p);
    Count(Upload, tm.Elapsed(), "UpdateSurface", 0);
    return hr;
}
HRESULT __stdcall UpdateTextureHook(IDirect3DDevice9* d, IDirect3DBaseTexture9* a, IDirect3DBaseTexture9* b)
{
    Timer tm;
    HRESULT hr = oUpdateTexture(d, a, b);
    Count(Upload, tm.Elapsed(), "UpdateTexture", 0);
    return hr;
}
bool g_traceDraws;
int g_tracedDraws;

void TraceDraw(IDirect3DDevice9* d, const char* what, UINT count, const void* frame)
{
    if (!g_traceDraws || g_tracedDraws >= 6000) return;
    g_tracedDraws++;
    uint32_t ret[6] = {};
    __try
    {
        const uint32_t* sp = (const uint32_t*)frame;
        int n = 0;
        for (int i = 0; i < 512 && n < 6; i++)
            if (LooksLikeReturn(sp[i])) ret[n++] = sp[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    IDirect3DBaseTexture9* tex = nullptr;
    d->GetTexture(0, &tex);
    IDirect3DVertexShader9* vs = nullptr;
    d->GetVertexShader(&vs);
    Log("draw %s %u tex %p vs %p from %06X < %06X < %06X < %06X < %06X < %06X", what, count, (void*)tex, (void*)vs, ret[0], ret[1], ret[2],
        ret[3], ret[4], ret[5]);
    if (tex) tex->Release();
    if (vs) vs->Release();
}

HRESULT __stdcall DrawPrimitiveHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT n)
{
    TraceDraw(d, "DP", n, _AddressOfReturnAddress());
    Timer tm;
    HRESULT hr = oDrawPrimitive(d, t, a, n);
    double e = tm.Elapsed();
    Count(Draw, e, "DrawPrimitive", n);
    if (e > 0.002) LogSlowDraw(d, "DrawPrimitive", n, e);
    return hr;
}
HRESULT __stdcall DrawIndexedPrimitiveHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT n)
{
    TraceDraw(d, "DIP", n, _AddressOfReturnAddress());
    Timer tm;
    HRESULT hr = oDrawIndexedPrimitive(d, t, b, mi, nv, si, n);
    double e = tm.Elapsed();
    Count(Draw, e, "DrawIndexedPrimitive", n);
    if (e > 0.002) LogSlowDraw(d, "DrawIndexedPrimitive", n, e);
    return hr;
}
HRESULT __stdcall DrawPrimitiveUPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT s)
{
    TraceDraw(d, "DPUP", n, _AddressOfReturnAddress());
    Timer tm;
    HRESULT hr = oDrawPrimitiveUP(d, t, n, v, s);
    double e = tm.Elapsed();
    Count(Draw, e, "DrawPrimitiveUP", n);
    if (e > 0.002) LogSlowDraw(d, "DrawPrimitiveUP", n, e);
    return hr;
}
HRESULT __stdcall DrawIndexedPrimitiveUPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n, const void* i, D3DFORMAT f,
                                             const void* v, UINT s)
{
    Timer tm;
    HRESULT hr = oDrawIndexedPrimitiveUP(d, t, mi, nv, n, i, f, v, s);
    double e = tm.Elapsed();
    Count(Draw, e, "DrawIndexedPrimitiveUP", n);
    if (e > 0.002) LogSlowDraw(d, "DrawIndexedPrimitiveUP", n, e);
    return hr;
}
HRESULT __stdcall CreateVertexShaderHook(IDirect3DDevice9* d, const DWORD* code, void** out)
{
    Timer tm;
    HRESULT hr = oCreateVertexShader(d, code, out);
    Count(Create, tm.Elapsed(), "vertex shader", 0);
    return hr;
}
HRESULT __stdcall CreatePixelShaderHook(IDirect3DDevice9* d, const DWORD* code, void** out)
{
    Timer tm;
    HRESULT hr = oCreatePixelShader(d, code, out);
    Count(Create, tm.Elapsed(), "pixel shader", 0);
    return hr;
}

// ---- buffers and textures -------------------------------------------------------------------
using BufferLockFn = HRESULT(__stdcall*)(void*, UINT, UINT, void**, DWORD);
using TextureLockFn = HRESULT(__stdcall*)(void*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
BufferLockFn oVertexLock, oIndexLock;
TextureLockFn oTextureLock;

HRESULT __stdcall VertexLockHook(void* b, UINT off, UINT size, void** data, DWORD flags)
{
    Timer tm;
    HRESULT hr = oVertexLock(b, off, size, data, flags);
    Count(Lock, tm.Elapsed(), "vertex buffer", flags);
    return hr;
}
HRESULT __stdcall IndexLockHook(void* b, UINT off, UINT size, void** data, DWORD flags)
{
    Timer tm;
    HRESULT hr = oIndexLock(b, off, size, data, flags);
    Count(Lock, tm.Elapsed(), "index buffer", flags);
    return hr;
}
HRESULT __stdcall TextureLockHook(void* t, UINT level, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags)
{
    Timer tm;
    HRESULT hr = oTextureLock(t, level, lr, r, flags);
    Count(Lock, tm.Elapsed(), "texture", flags);
    return hr;
}

IDirect3DDevice9* g_device;

// ---- the game's particle rendering --------------------------------------------------------------
constexpr uintptr_t kParticlesCall = 0x4CBA0F;  // call 0x58EFF0 (view in eax, camera in edi, near pushed)
constexpr uintptr_t kParticles = 0x58EFF0;
uintptr_t g_particlesFn = kParticles;
double g_particlesStart;
struct { int count = 0; double time = 0, max = 0; } g_particles[PhaseCount];

void __cdecl ParticlesBegin() { g_particlesStart = NowSeconds(); }
void __cdecl ParticlesEnd()
{
    double t = NowSeconds() - g_particlesStart;
    auto& p = g_particles[g_phase];
    p.count++;
    p.time += t;
    p.max = std::max(p.max, t);
}

void LogPhase(Phase p, double t)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "   %s %.2f ms:", kPhaseNames[p], 1000.0 * t);
    for (int k = 0; k < KindCount; k++)
    {
        const Counter& c = g_frame[p][k];
        if (c.count == 0) continue;
        n += snprintf(line + n, sizeof(line) - n, " %s %d / %.2f ms (slowest %.2f ms: %s %u);", kKindNames[k], c.count, 1000.0 * c.time,
                      1000.0 * c.max, c.maxWhat, c.maxArg);
        if (n >= (int)sizeof(line)) break;
    }
    Log("%s", line);
}

// Keeps eax, ecx, edx and the stack argument for the game's function.
__declspec(naked) void ParticlesThunk()
{
    __asm {
        push eax
        push ecx
        push edx
        call ParticlesBegin
        pop edx
        pop ecx
        pop eax
        push dword ptr [esp + 4]
        call dword ptr [g_particlesFn]
        add esp, 4
        push eax
        push ecx
        push edx
        call ParticlesEnd
        pop edx
        pop ecx
        pop eax
        ret
    }
}

}  // namespace

bool HookParticles() { return PatchCall(kParticlesCall, kParticles, (const void*)&ParticlesThunk); }

void Hook(IDirect3DDevice9* dev, bool perCall)
{
    if (!dev || dev == g_device) return;
    void** vt = *(void***)dev;
    HookSlot(vt, slot::CreateTexture, (void*)&CreateTextureHook, oCreateTexture);
    HookSlot(vt, slot::CreateCubeTexture, (void*)&CreateCubeTextureHook, oCreateCubeTexture);
    HookSlot(vt, slot::CreateVertexBuffer, (void*)&CreateVertexBufferHook, oCreateVertexBuffer);
    HookSlot(vt, slot::CreateIndexBuffer, (void*)&CreateIndexBufferHook, oCreateIndexBuffer);
    HookSlot(vt, slot::CreateRenderTarget, (void*)&CreateRenderTargetHook, oCreateRenderTarget);
    HookSlot(vt, slot::CreateDepthStencilSurface, (void*)&CreateDepthStencilSurfaceHook, oCreateDepthStencilSurface);
    HookSlot(vt, slot::UpdateSurface, (void*)&UpdateSurfaceHook, oUpdateSurface);
    HookSlot(vt, slot::UpdateTexture, (void*)&UpdateTextureHook, oUpdateTexture);
    HookSlot(vt, slot::CreateVertexShader, (void*)&CreateVertexShaderHook, oCreateVertexShader);
    HookSlot(vt, slot::CreatePixelShader, (void*)&CreatePixelShaderHook, oCreatePixelShader);
    g_device = dev;
    if (!perCall)
    {
        Log("profiler: device creates and uploads timed");
        return;
    }
    // The buffer and texture classes' vtables, from throwaway objects (before the create hooks).
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr)))
    {
        HookSlot(*(void***)vb, slot::BufferLock, (void*)&VertexLockHook, oVertexLock);
        vb->Release();
    }
    if (SUCCEEDED(dev->CreateIndexBuffer(64, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr)))
    {
        HookSlot(*(void***)ib, slot::BufferLock, (void*)&IndexLockHook, oIndexLock);
        ib->Release();
    }
    if (SUCCEEDED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)))
    {
        HookSlot(*(void***)tex, slot::TextureLock, (void*)&TextureLockHook, oTextureLock);
        tex->Release();
    }
    HookSlot(vt, slot::DrawPrimitive, (void*)&DrawPrimitiveHook, oDrawPrimitive);
    HookSlot(vt, slot::DrawIndexedPrimitive, (void*)&DrawIndexedPrimitiveHook, oDrawIndexedPrimitive);
    HookSlot(vt, slot::DrawPrimitiveUP, (void*)&DrawPrimitiveUPHook, oDrawPrimitiveUP);
    HookSlot(vt, slot::DrawIndexedPrimitiveUP, (void*)&DrawIndexedPrimitiveUPHook, oDrawIndexedPrimitiveUP);
    Log("profiler: device calls timed");
}

void SetPhase(Phase p)
{
    double now = NowSeconds();
    g_phaseEnd[g_phase] = now;
    g_phaseStart[p] = now;
    g_phase = p;
}

void StartSampling()
{
    if (g_gameThread) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gameThread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    {
        g_gameThread = nullptr;
        return;
    }
    CloseHandle(CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr));
    Log("profiler: sampling the game thread");
}

struct Totals
{
    int frames = 0, slow = 0, overBudget = 0;
    double game = 0, eyes = 0, hud = 0, particles = 0;
    double maxFrame = 0, maxEyes = 0, maxParticles = 0;
    long long eyeDraws = 0;  // with DeviceTiming
    int maxEyeDraws = 0;
    // With sampling on: where the eye renders spent their time (game code by 16 bytes; other
    // modules by the game code that called them).
    std::unordered_map<uint32_t, int> eyeSamples;
    int eyeSampleCount = 0;
} g_totals;
LONG g_sweptSamples;

void ResetTotals()
{
    g_totals = Totals();
    g_sweptSamples = g_sampleCount;
}

// Adds the samples taken during this frame's eye renders to the totals.
void SweepEyeSamples()
{
    LONG end = g_sampleCount;
    for (LONG i = std::max(g_sweptSamples, end - kSamples + 1); i < end; i++)
    {
        const Sample& s = g_samples[i % kSamples];
        if (s.t < g_phaseStart[Eyes] || s.t > g_phaseEnd[Eyes]) continue;
        uint32_t key = IsGameCode(s.eip) ? (s.eip & ~0xFu) : (s.ret[0] | 0x80000000u);
        g_totals.eyeSamples[key]++;
        g_totals.eyeSampleCount++;
    }
    g_sweptSamples = end;
}

void TraceDraws(bool on)
{
    g_traceDraws = on;
    if (on) g_tracedDraws = 0;
}

void LogTotals(const char* what)
{
    const Totals& t = g_totals;
    if (t.frames == 0) return;
    double n = t.frames;
    Log("%s: %d frames, avg ms: game %.2f, eyes %.2f (particles %.2f), HUD %.2f | slowest frame %.2f ms, eyes %.2f ms, particles %.2f ms | "
        "%d slow frames, %d over 8.3 ms",
        what, t.frames, 1000.0 * t.game / n, 1000.0 * t.eyes / n, 1000.0 * t.particles / n, 1000.0 * t.hud / n, 1000.0 * t.maxFrame,
        1000.0 * t.maxEyes, 1000.0 * t.maxParticles, t.slow, t.overBudget);
    if (t.eyeDraws > 0) Log("%s: draw calls in the eye renders: %.0f a frame on average, at most %d", what, t.eyeDraws / n, t.maxEyeDraws);
    if (t.eyeSampleCount == 0) return;
    std::vector<std::pair<uint32_t, int>> places(t.eyeSamples.begin(), t.eyeSamples.end());
    std::sort(places.begin(), places.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    Log("%s: %d samples in the eye renders, the most frequent places:", what, t.eyeSampleCount);
    for (size_t i = 0; i < places.size() && i < 60; i++)
        Log("   %s %06X %d", (places[i].first & 0x80000000u) ? "called from" : "game", places[i].first & 0x7FFFFFFFu, places[i].second);
}

void EndFrame(double game, double eyes, double hud)
{
    {
        Totals& t = g_totals;
        double frame = game + eyes + hud;
        t.frames++;
        t.game += game;
        t.eyes += eyes;
        t.hud += hud;
        t.particles += g_particles[Eyes].time;
        t.maxFrame = std::max(t.maxFrame, frame);
        t.maxEyes = std::max(t.maxEyes, eyes);
        t.maxParticles = std::max(t.maxParticles, g_particles[Eyes].time);
        if (game > 0.004 || eyes > 0.006 || hud > 0.003) t.slow++;
        if (frame > 1.0 / 120) t.overBudget++;
        t.eyeDraws += g_frame[Eyes][Draw].count;
        t.maxEyeDraws = std::max(t.maxEyeDraws, g_frame[Eyes][Draw].count);
        if (g_gameThread) SweepEyeSamples();
    }
    bool slow = game > 0.004 || eyes > 0.006 || hud > 0.003;
    double now = NowSeconds();
    if (now - g_lastSlowLog > 60.0) g_slowLogged = 0;
    if (slow && g_slowLogged < 30)
    {
        if (g_slowLogged == 0) g_lastSlowLog = now;
        g_slowLogged++;
        Log("slow frame: game %.2f ms, eyes %.2f ms (of which particles %d calls %.2f ms, slowest %.2f ms), HUD %.2f ms", 1000.0 * game,
            1000.0 * eyes, g_particles[Eyes].count, 1000.0 * g_particles[Eyes].time, 1000.0 * g_particles[Eyes].max, 1000.0 * hud);
        const double t[PhaseCount] = { game, eyes, hud };
        for (int p = 0; p < PhaseCount; p++) LogPhase((Phase)p, t[p]);
        for (int p = 0; p < PhaseCount; p++)
            if (t[p] > 0.003) LogSamples((Phase)p);
    }
    for (auto& phase : g_frame)
        for (auto& c : phase) c = Counter();
    for (auto& p : g_particles) p = {};
}

}  // namespace prof
