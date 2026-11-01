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

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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

// A module loaded from the game's folder (not from Windows').
bool InGameFolder(HMODULE module)
{
    wchar_t path[MAX_PATH] = L"", exe[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* a = wcsrchr(path, L'\\');
    const wchar_t* b = wcsrchr(exe, L'\\');
    return a && b && a - path == b - exe && _wcsnicmp(path, exe, a - path) == 0;
}

// The game's Direct3D: DXVK, scripts\dxvk_d3d9.dll, loaded here for the game's own Direct3DCreate9 - not a d3d9.dll in
// the game folder. The Steam release's fo2_zoom.dll calls Direct3DCreate9 as it loads, in its DllMain (0x10017BE0 there:
// it compares the graphics card's identifier with one), with Windows' loader lock held. With DXVK as the game folder's
// d3d9.dll that started Vulkan there, its driver and layers, and a player's game hung for good before it showed a window:
// its FlatOut2_d3d9.log ended with that first start's instance extensions, while the GOG release (no fo2_zoom.dll) ran on
// DXVK and the Steam one on Windows' d3d9.dll. FO2VR.asi is loaded after every DllMain, too late to step in. So
// fo2_zoom.dll (and the game without VR) has Windows' d3d9.dll and the game's Direct3D comes from DXVK here. A d3d9.dll of
// the game folder's own (DXVK where the mod had it before, or another wrapper) is still used as it is, while it is loaded
// (the old DXVK: PutAwayOldDxvk).
HMODULE Direct3DModule()
{
    static HMODULE module;
    if (module) return module;
    HMODULE loaded = GetModuleHandleW(L"d3d9.dll");
    if (loaded && InGameFolder(loaded)) return module = loaded;
    std::wstring dxvk = ModuleDir() + L"dxvk_d3d9.dll";
    module = LoadLibraryW(dxvk.c_str());
    if (!module)
    {
        Log("Direct3D: %ls not loaded (error %u) - Windows' d3d9.dll", dxvk.c_str(), (unsigned)GetLastError());
        module = loaded ? loaded : LoadLibraryW(L"d3d9.dll");
    }
    return module;
}

using GetAdapterIdentifierFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, DWORD, D3DADAPTER_IDENTIFIER9*);
GetAdapterIdentifierFn oGetAdapterIdentifier;
std::vector<GUID> g_windowsCards;  // DeviceIdentifier of each of Windows' Direct3D's adapters

std::string GuidText(const GUID& g)
{
    char s[40];
    sprintf_s(s, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
              g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return s;
}

HRESULT __stdcall GetAdapterIdentifierHook(IDirect3D9* d3d, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id)
{
    HRESULT hr = oGetAdapterIdentifier(d3d, adapter, flags, id);
    if (SUCCEEDED(hr) && id && !g_windowsCards.empty()) id->DeviceIdentifier = g_windowsCards[adapter < g_windowsCards.size() ? adapter : 0];
    return hr;
}

// The Steam release's fo2_zoom.dll keeps Savegame\device.cfg to the graphics card it sees as it loads (Direct3DModule):
// it writes the identifier of Windows' Direct3D's adapter 0 (D3DADAPTER_IDENTIFIER9::DeviceIdentifier) into the file's
// card at 0x68 (0x10017BE0 there). The game, on DXVK, saw another card in it (DXVK's identifier comes from Vulkan): it
// took the settings for another card's, started at their resolution once and wrote its defaults back (1280x1024), every
// start. With DXVK as the game folder's d3d9.dll fo2_zoom.dll saw DXVK too and the two agreed. So where fo2_zoom.dll has
// Windows' d3d9.dll, the game's DXVK tells each adapter's identifier as Windows' Direct3D does, as the release has it
// without DXVK.
void MatchFo2ZoomCard(IDirect3D9* d3d, HMODULE module)
{
    HMODULE windows = GetModuleHandleW(L"d3d9.dll");
    if (!GetModuleHandleW(L"fo2_zoom.dll") || !windows || windows == module) return;
    static bool asked;
    if (!asked)
    {
        asked = true;
        auto create = (Direct3DCreate9Fn)GetProcAddress(windows, "Direct3DCreate9");
        if (IDirect3D9* w = create ? create(D3D_SDK_VERSION) : nullptr)
        {
            D3DADAPTER_IDENTIFIER9 id = {};
            for (UINT i = 0, n = w->GetAdapterCount(); i < n && SUCCEEDED(w->GetAdapterIdentifier(i, 0, &id)); i++)
                g_windowsCards.push_back(id.DeviceIdentifier);
            w->Release();
        }
        D3DADAPTER_IDENTIFIER9 own = {};
        d3d->GetAdapterIdentifier(0, 0, &own);
        if (g_windowsCards.empty())
            Log("Direct3D: Windows' Direct3D tells no graphics card - the game sees DXVK's (%s), which fo2_zoom.dll's device.cfg may not",
                GuidText(own.DeviceIdentifier).c_str());
        else
            Log("Direct3D: the game sees the graphics card as Windows' Direct3D does (%s, DXVK's %s), as fo2_zoom.dll keeps device.cfg",
                GuidText(g_windowsCards[0]).c_str(), GuidText(own.DeviceIdentifier).c_str());
    }
    if (g_windowsCards.empty()) return;
    void** vtbl = *(void***)d3d;
    const int kGetAdapterIdentifier = 5;
    if (vtbl[kGetAdapterIdentifier] == (void*)&GetAdapterIdentifierHook) return;
    oGetAdapterIdentifier = (GetAdapterIdentifierFn)vtbl[kGetAdapterIdentifier];
    DWORD old;
    VirtualProtect(&vtbl[kGetAdapterIdentifier], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[kGetAdapterIdentifier] = (void*)&GetAdapterIdentifierHook;
    VirtualProtect(&vtbl[kGetAdapterIdentifier], sizeof(void*), old, &old);
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
    HMODULE d3d9 = Direct3DModule();
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
        MatchFo2ZoomCard(d3d, d3d9);
    }
    return d3d;
}

}  // namespace

void PutAwayOldDxvk()
{
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\') + 1);
    std::wstring old = dir + L"d3d9.dll", away = dir + L"d3d9.dll.fo2vr-old";
    if (DeleteFileW(away.c_str())) Log("Direct3D: d3d9.dll.fo2vr-old (DXVK, put away at the last start) deleted");
    HMODULE loaded = GetModuleHandleW(L"d3d9.dll");
    if (!loaded || !InGameFolder(loaded)) return;
    // DXVK reads its log level from DXVK_LOG_LEVEL: every version has the name in it.
    bool dxvk = false;
    HANDLE f = CreateFileW(old.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size = {};
        GetFileSizeEx(f, &size);
        std::string data((size_t)std::min<LONGLONG>(size.QuadPart, 64 << 20), '\0');
        DWORD got = 0;
        dxvk = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && data.find("DXVK_LOG_LEVEL") != std::string::npos;
        CloseHandle(f);
    }
    bool ours = GetFileAttributesW((ModuleDir() + L"dxvk_d3d9.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!dxvk || !ours)
    {
        Log("Direct3D: the game folder's own d3d9.dll (%s) is used as it is", dxvk ? "DXVK; scripts\\dxvk_d3d9.dll is missing" : "not DXVK");
        return;
    }
    if (MoveFileExW(old.c_str(), away.c_str(), MOVEFILE_REPLACE_EXISTING))
        Log("Direct3D: the game folder's d3d9.dll (DXVK, as an earlier FO2VR had it) renamed d3d9.dll.fo2vr-old, deleted at the next "
            "start, which runs on scripts\\dxvk_d3d9.dll (this one still runs on it)");
    else
        Log("Direct3D: the game folder's d3d9.dll (DXVK, as an earlier FO2VR had it) could not be renamed (error %u) - it is used "
            "as it is; delete it (DXVK is scripts\\dxvk_d3d9.dll now)", (unsigned)GetLastError());
}

bool InstallCreateHook()
{
    bool ok = PatchCall(fo2::kCreateD3DCall1, fo2::kDirect3DCreate9Thunk, (const void*)&Direct3DCreate9Hook);
    ok &= PatchCall(fo2::kCreateD3DCall2, fo2::kDirect3DCreate9Thunk, (const void*)&Direct3DCreate9Hook);
    return ok;
}
