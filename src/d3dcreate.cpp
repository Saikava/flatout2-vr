#include "d3dcreate.h"

#include "background.h"
#include "controllers.h"
#include "dxvk_interop.h"
#include "game.h"
#include "largeaddress.h"
#include "render.h"
#include "util.h"
#include "xr.h"

#include <windows.h>
#include <d3d9.h>

namespace {

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

CreateDeviceFn oCreateDevice;
bool g_xrTried;

// The game's window against Windows' display scaling: a window unaware of it is sized in scaled pixels (at 150% a full
// screen of 3840x2160 covers one and a half 4K monitors).
void LogWindow(HWND window)
{
    RECT r = {};
    GetWindowRect(window, &r);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    auto context = (HANDLE(WINAPI*)(HWND))GetProcAddress(user, "GetWindowDpiAwarenessContext");
    auto awareness = (int(WINAPI*)(HANDLE))GetProcAddress(user, "GetAwarenessFromDpiAwarenessContext");
    auto dpi = (UINT(WINAPI*)(HWND))GetProcAddress(user, "GetDpiForWindow");
    int a = context && awareness ? awareness(context(window)) : -1;
    const char* names[] = { "unaware of the display scaling", "aware of the system's display scaling", "aware of each monitor's display scaling" };
    Log("window: %ldx%ld at %ld,%ld, %s (%u dpi)", r.right - r.left, r.bottom - r.top, r.left, r.top, a >= 0 && a <= 2 ? names[a] : "(DPI awareness unknown)",
        dpi ? dpi(window) : 0);
}

HRESULT __stdcall CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                   IDirect3DDevice9** out)
{
    // In VR the headset paces the game (xrWaitFrame), so the monitor must not wait for vsync.
    if (xr::Available() && pp) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    BackgroundBeforeDevice(pp);
    HRESULT hr = oCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice %ux%u windowed %d msaa %d interval %08X: %08X", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
        pp ? pp->Windowed : -1, pp ? (int)pp->MultiSampleType : -1, pp ? (unsigned)pp->PresentationInterval : 0, (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out)
    {
        LogWindow(pp && pp->hDeviceWindow ? pp->hDeviceWindow : wnd);
        BackgroundAfterDevice(pp && pp->hDeviceWindow ? pp->hDeviceWindow : wnd);
        if (xr::Available() && !BackgroundMode()) KeepGameActive(pp && pp->hDeviceWindow ? pp->hDeviceWindow : wnd);
        FocusGameWindow(pp && pp->hDeviceWindow ? pp->hDeviceWindow : wnd);
        OnDeviceCreated(*out);
    }
    return hr;
}

void HookDirect3D(IDirect3D9* d3d)
{
    void** vtbl = *(void***)d3d;
    const int kCreateDevice = 16;
    if (vtbl[kCreateDevice] == (void*)&CreateDeviceHook) return;
    oCreateDevice = (CreateDeviceFn)vtbl[kCreateDevice];
    DWORD old;
    VirtualProtect(&vtbl[kCreateDevice], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[kCreateDevice] = (void*)&CreateDeviceHook;
    VirtualProtect(&vtbl[kCreateDevice], sizeof(void*), old, &old);
}

// [VR] FloatDepth: DXVK keeps the 24-bit depth formats the game and the mod ask for as 32-bit floats, which the eyes
// fill reversed (see g_depthReversed in render.cpp). DXVK reads its options from DXVK_CONFIG (besides dxvk.conf) as
// Direct3DCreate9 starts it; the game's own depth comes out the same in floats.
void UseFloatDepth()
{
    wchar_t old[1024] = L"";
    DWORD n = GetEnvironmentVariableW(L"DXVK_CONFIG", old, 1024);
    std::wstring config = n > 0 && n < 1024 ? std::wstring(old) + L"; " : L"";
    config += L"d3d9.useD32forD24 = True";
    SetEnvironmentVariableW(L"DXVK_CONFIG", config.c_str());
    Log("DXVK_CONFIG=%ls", config.c_str());
}

// FlatOut2.exe calls Direct3DCreate9 through its import thunk 0x5FFA80 (jmp [IAT]) from two places.
IDirect3D9* WINAPI Direct3DCreate9Hook(UINT sdk)
{
    HookInputInterfaces();
    if (g_cfg.vrEnabled && !g_xrTried)
    {
        g_xrTried = true;
        EnsureLargeAddressAware();
        // The game makes its window after Direct3D, and DXVK makes the process aware of Windows' display scaling as it
        // starts (SetProcessDPIAware; GOG's installer sets its FlatOut2.exe so too). The runtime starts first: the
        // process is made aware before it, so that nothing the runtime starts leaves the window unaware (the Steam copy
        // started with SteamVR came up stretched over two 4K monitors scaled to 150%: see LogWindow).
        SetProcessDPIAware();
        if (!xr::InitInstance()) Log("VR not available - the game runs on the monitor only");
        if (g_cfg.vrFloatDepth) UseFloatDepth();
    }
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
    auto create = (Direct3DCreate9Fn)GetProcAddress(d3d9, "Direct3DCreate9");
    IDirect3D9* d3d = create ? create(sdk) : nullptr;
    if (d3d)
    {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(d3d9, path, MAX_PATH);
        ID3D9VkInteropInterface* dxvk = nullptr;
        bool isDxvk = SUCCEEDED(d3d->QueryInterface(__uuidof(ID3D9VkInteropInterface), (void**)&dxvk));
        if (dxvk) dxvk->Release();
        Log("Direct3DCreate9(%u): %ls (%s)", sdk, path, isDxvk ? "DXVK" : "not DXVK");
        HookDirect3D(d3d);
    }
    return d3d;
}

}  // namespace

bool InstallCreateHook()
{
    bool ok = PatchCall(fo2::kCreateD3DCall1, fo2::kDirect3DCreate9Thunk, (const void*)&Direct3DCreate9Hook);
    ok &= PatchCall(fo2::kCreateD3DCall2, fo2::kDirect3DCreate9Thunk, (const void*)&Direct3DCreate9Hook);
    return ok;
}
