#include "sound.h"

#include "game.h"
#include "util.h"

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <cstring>

// FlatOut 2's sound is FMOD 3.75's (built into the exe) on DirectSound, which it loads itself: dsound.dll's
// DirectSoundCreate8 through the game's GetProcAddress (0x629C78). FMOD makes its buffers without
// DSBCAPS_GLOBALFOCUS (the game does not ask for FSOUND_INIT_GLOBALFOCUS), and DirectSound silences such buffers
// while another program's window is in front. In VR the game's window on the monitor is not what the player looks
// at: a game started from the headset, which may never have had the focus, played without sound until its window
// was clicked on the desktop. With VR on ([VR] Enabled) the buffers are made with global focus: the sound plays
// whichever window is in front.

namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using CreateFn = HRESULT(WINAPI*)(LPCGUID, void**, LPUNKNOWN);
using CreateBufferFn = HRESULT(__stdcall*)(void* self, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer);
constexpr int kSlotCreateSoundBuffer = 3;  // IDirectSound and IDirectSound8

GetProcAddressFn oGetProcAddress;
CreateFn oCreate, oCreate8;
CreateBufferFn oCreateBuffer;
int g_globalBuffers;

HRESULT __stdcall CreateBufferHook(void* self, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer)
{
    if (!desc || desc->dwSize < sizeof(DSBUFFERDESC1) || desc->dwSize > sizeof(DSBUFFERDESC) || (desc->dwFlags & DSBCAPS_PRIMARYBUFFER))
        return oCreateBuffer(self, desc, out, outer);
    DSBUFFERDESC d = {};
    memcpy(&d, desc, desc->dwSize);
    d.dwFlags = (d.dwFlags & ~(DWORD)DSBCAPS_STICKYFOCUS) | DSBCAPS_GLOBALFOCUS;
    HRESULT hr = oCreateBuffer(self, &d, out, outer);
    if (FAILED(hr)) return oCreateBuffer(self, desc, out, outer);
    if (g_globalBuffers++ == 0) Log("sound: the game's sound plays whichever window is in front (its buffers with global focus)");
    return hr;
}

// Every DirectSound object of dsound.dll shares its interface's table: hooked once (IDirectSound's and
// IDirectSound8's may be the same table).
void HookDirectSound(void* ds)
{
    void** vtbl = *(void***)ds;
    if (vtbl[kSlotCreateSoundBuffer] == (void*)&CreateBufferHook) return;
    oCreateBuffer = (CreateBufferFn)vtbl[kSlotCreateSoundBuffer];
    DWORD old;
    VirtualProtect(&vtbl[kSlotCreateSoundBuffer], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[kSlotCreateSoundBuffer] = (void*)&CreateBufferHook;
    VirtualProtect(&vtbl[kSlotCreateSoundBuffer], sizeof(void*), old, &old);
}

HRESULT WINAPI Create8Hook(LPCGUID device, void** out, LPUNKNOWN outer)
{
    HRESULT hr = oCreate8(device, out, outer);
    if (SUCCEEDED(hr) && out && *out) HookDirectSound(*out);
    return hr;
}

HRESULT WINAPI CreateHook(LPCGUID device, void** out, LPUNKNOWN outer)
{
    HRESULT hr = oCreate(device, out, outer);
    if (SUCCEEDED(hr) && out && *out) HookDirectSound(*out);
    return hr;
}

FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR name)
{
    FARPROC p = oGetProcAddress(module, name);
    if (!p || IS_INTRESOURCE(name)) return p;
    if (strcmp(name, "DirectSoundCreate8") == 0)
    {
        oCreate8 = (CreateFn)p;
        return (FARPROC)&Create8Hook;
    }
    if (strcmp(name, "DirectSoundCreate") == 0)
    {
        oCreate = (CreateFn)p;
        return (FARPROC)&CreateHook;
    }
    return p;
}

}  // namespace

bool InstallSound()
{
    if (!g_cfg.vrEnabled) return true;
    FARPROC real = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
    void** slot = (void**)fo2::kIatGetProcAddress;
    if (*slot != (void*)real)
    {
        Log("sound: the game's GetProcAddress import differs - its sound may stop while its window is not in front");
        return false;
    }
    oGetProcAddress = (GetProcAddressFn)real;
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = (void*)&GetProcAddressHook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}
