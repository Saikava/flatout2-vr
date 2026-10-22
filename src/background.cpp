#include "background.h"

#include "record.h"
#include "util.h"
#include "xr.h"

#include <windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

// FlatOut2.exe: "call [0x8DA748]" (DirectInput8Create, fetched with GetProcAddress) at 0x5612AB.
constexpr uintptr_t kDirectInputCreateCall = 0x5612AB;
constexpr uintptr_t kDirectInputCreatePtr = 0x8DA748;
// The game's keyboard: its WH_KEYBOARD hook (0x55AAD0) keeps a state per virtual key (0x80 down,
// bit 0 toggled on every press), counts presses per key and the keys held.
constexpr uintptr_t kKeyState = 0x8D7E60;   // BYTE[256]
constexpr uintptr_t kKeyPresses = 0x8D7D60; // BYTE[256]
constexpr uintptr_t kKeysHeld = 0x8DA740;   // int
// Import address table entries (USER32).
constexpr uintptr_t kIatCreateWindowExA = 0x65229C;
constexpr uintptr_t kIatShowWindow = 0x6522A0;
constexpr uintptr_t kIatSetFocus = 0x6522A8;

bool g_on;
HWND g_prevForeground;  // the window that had the focus when the game started
HWND g_gameWindow;
WNDPROC g_gameWndProc;
RECT g_place;  // where the game window goes
double g_lastPlaceCheck;

std::wstring CommandFile() { return ModuleDir() + L"fo2vr_input.txt"; }
std::wstring MuteFlag() { return ModuleDir() + L"FO2VR.muted"; }

// ---- sound ------------------------------------------------------------------------------------
// Mutes (or unmutes) this process's audio session, as the volume mixer would. On its own thread,
// not waited for: this runs inside DllMain (the thread starts once the loader lock is free), and
// the game's thread may have initialised COM differently.
void SetProcessMute(bool mute)
{
    std::thread([mute] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioSessionManager* mgr = nullptr;
        ISimpleAudioVolume* vol = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, (void**)&mgr);
        if (SUCCEEDED(hr)) hr = mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol);
        if (SUCCEEDED(hr)) hr = vol->SetMute(mute, nullptr);
        if (vol) vol->Release();
        if (mgr) mgr->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        if (SUCCEEDED(co)) CoUninitialize();
        Log("background: sound %s: %08X", mute ? "muted" : "unmuted", (unsigned)hr);
    }).detach();
}

// ---- window ---------------------------------------------------------------------------------
BOOL CALLBACK FindOtherMonitor(HMONITOR mon, HDC, LPRECT, LPARAM out)
{
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY))
    {
        *(RECT*)out = mi.rcWork;
        return FALSE;
    }
    return TRUE;
}

// A 1280x720 window in the bottom right corner of another monitor (or of the only one).
RECT Placement()
{
    RECT work = {};
    EnumDisplayMonitors(nullptr, nullptr, FindOtherMonitor, (LPARAM)&work);
    if (work.right <= work.left) SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 1280, h = 720;
    return { work.right - w - 16, work.bottom - h - 16, work.right - 16, work.bottom - 16 };
}

void PlaceWindow()
{
    SetWindowLongA(g_gameWindow, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongA(g_gameWindow, GWL_EXSTYLE, GetWindowLongA(g_gameWindow, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
    SetWindowPos(g_gameWindow, HWND_BOTTOM, g_place.left, g_place.top, g_place.right - g_place.left, g_place.bottom - g_place.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

// The game's window is made where it belongs, hidden, never on top; showing it does not activate it.
using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetFocusFn = HWND(WINAPI*)(HWND);
CreateWindowExAFn oCreateWindowExA;
ShowWindowFn oShowWindow;
SetFocusFn oSetFocus;

HWND WINAPI CreateWindowExAHook(DWORD exStyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
                                HINSTANCE inst, LPVOID param)
{
    if (!parent && !(style & WS_CHILD))
    {
        g_place = Placement();
        exStyle &= ~(DWORD)WS_EX_TOPMOST;
        style &= ~(DWORD)WS_VISIBLE;
        x = g_place.left;
        y = g_place.top;
        w = g_place.right - g_place.left;
        h = g_place.bottom - g_place.top;
    }
    return oCreateWindowExA(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

BOOL WINAPI ShowWindowHook(HWND wnd, int cmd)
{
    if (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_SHOWDEFAULT || cmd == SW_RESTORE || cmd == SW_SHOWMAXIMIZED) cmd = SW_SHOWNOACTIVATE;
    return oShowWindow(wnd, cmd);
}

HWND WINAPI SetFocusHook(HWND) { return GetFocus(); }

template <class T>
bool PatchImport(uintptr_t slot, const char* name, void* hook, T& orig)
{
    void* real = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    if (!real || *(void**)slot != real) return false;
    orig = (T)real;
    DWORD old;
    VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &old);
    *(void**)slot = hook;
    VirtualProtect((void*)slot, sizeof(void*), old, &old);
    return true;
}

// The game never hears that it lost the focus, so it keeps running.
LRESULT CALLBACK BackgroundWndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ACTIVATEAPP:
        if (!wp) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) return 0;
        break;
    case WM_KILLFOCUS:
        return 0;
    default:
        break;
    }
    return CallWindowProcA(g_gameWndProc, wnd, msg, wp, lp);
}

// ---- keys -------------------------------------------------------------------------------------
BYTE g_keys[256];
struct KeyEvent
{
    BYTE dik;
    bool down;
};
std::deque<KeyEvent> g_events;
struct Release
{
    double at;
    BYTE dik;
};
std::vector<Release> g_releases;
DWORD g_sequence = 0x40000000;

// A key goes into the game's own keyboard state, as its keyboard hook would put it, and to
// DirectInput (state and buffer) for anything reading the keyboard there. Runs on the game's thread.
void SetKey(BYTE dik, bool down)
{
    if (((g_keys[dik] & 0x80) != 0) == down) return;
    g_keys[dik] = down ? 0x80 : 0;
    g_events.push_back({ dik, down });
    if (g_events.size() > 64) g_events.pop_front();  // nobody reads them (the game uses its own state)

    UINT sc = dik & 0x7F;
    UINT vk = MapVirtualKeyA((dik & 0x80) ? (0xE000 | sc) : sc, MAPVK_VSC_TO_VK_EX) & 0xFF;
    // The hook (WH_KEYBOARD) is told the shift, control and alt keys without their side (VK_CONTROL, not
    // VK_LCONTROL), and the game's controls name them so (the nitro, the stunts' launch: "CONTROL").
    if (vk == VK_LSHIFT || vk == VK_RSHIFT)
        vk = VK_SHIFT;
    else if (vk == VK_LCONTROL || vk == VK_RCONTROL)
        vk = VK_CONTROL;
    else if (vk == VK_LMENU || vk == VK_RMENU)
        vk = VK_MENU;
    if (!vk) return;
    BYTE& state = ((BYTE*)kKeyState)[vk];
    int& held = *(int*)kKeysHeld;
    if (down && !(state & 0x80))
    {
        held++;
        state = (BYTE)((state ^ 1) | 0x80);
        ((BYTE*)kKeyPresses)[vk]++;
    }
    else if (!down && (state & 0x80))
    {
        held--;
        state &= 0x7F;
    }
}

int KeyCode(const std::string& name)
{
    static const struct
    {
        const char* name;
        int dik;
    } named[] = { { "ESC", 0x01 }, { "ENTER", 0x1C }, { "SPACE", 0x39 }, { "TAB", 0x0F }, { "BACKSPACE", 0x0E }, { "LSHIFT", 0x2A },
                  { "LCTRL", 0x1D }, { "UP", 0xC8 }, { "DOWN", 0xD0 }, { "LEFT", 0xCB }, { "RIGHT", 0xCD }, { "F11", 0x57 }, { "F12", 0x58 } };
    for (auto& n : named)
        if (name == n.name) return n.dik;
    if (name.size() >= 2 && name[0] == 'F' && name[1] >= '1' && name[1] <= '9')
    {
        int f = atoi(name.c_str() + 1);
        if (f >= 1 && f <= 10) return 0x3B + f - 1;
    }
    if (name.size() == 1)
    {
        const char* rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
        const int start[] = { 0x02, 0x10, 0x1E, 0x2C };
        for (int r = 0; r < 4; r++)
            if (const char* p = strchr(rows[r], name[0])) return start[r] + (int)(p - rows[r]);
    }
    if (name.rfind("0x", 0) == 0) return (int)strtol(name.c_str(), nullptr, 16) & 0xFF;
    return -1;
}

bool g_shotRequested, g_traceRequested, g_hudRequested, g_ejectRequested;
char g_mirrorKnockSide;
int g_mirrorKnockState;

void RunCommand(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t sp = line.find(' ');
    std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
    for (char& c : cmd) c = (char)tolower((unsigned char)c);
    if (cmd == "shot")
    {
        g_shotRequested = true;
        return;
    }
    if (cmd == "trace")
    {
        g_traceRequested = true;
        return;
    }
    if (cmd == "hud")
    {
        g_hudRequested = true;
        return;
    }
    if (cmd == "eject")
    {
        g_ejectRequested = true;
        return;
    }
    if (cmd == "mirror")
    {
        char side = 0, how[8] = "";
        if (sscanf(arg.c_str(), " %c %7s", &side, how) == 2 && (side == 'l' || side == 'r'))
        {
            g_mirrorKnockSide = side;
            g_mirrorKnockState = strcmp(how, "off") == 0 ? 2 : 1;
        }
        return;
    }
    if (cmd == "head")
    {
        float* o = g_cfg.testHeadOffset;
        g_cfg.testHeadPitchDeg = 0;
        sscanf(arg.c_str(), "%f %f %f %f %f", &o[0], &o[1], &o[2], &g_cfg.testHeadYawDeg, &g_cfg.testHeadPitchDeg);
        Log("input file: head offset %.2f %.2f %.2f, yaw %.1f, pitch %.1f", o[0], o[1], o[2], g_cfg.testHeadYawDeg, g_cfg.testHeadPitchDeg);
        return;
    }
    if (cmd == "hand")
    {
        // hand <0 left | 1 right> x y z yaw pitch [trigger [stick-x stick-y [button]]]: a pretend controller
        // pointing from (x, y z) from the recenter origin; "hand 1 off" puts it away.
        int h = 0, button = 0;
        float p[3] = {}, yaw = 0, pitch = 0;
        xr::Controller c;
        char word[8] = "";
        int n = sscanf(arg.c_str(), "%d %7s", &h, word);
        if (n == 2 && strcmp(word, "off") == 0)
        {
            xr::SetTestHand(h & 1, c);
            Log("input file: hand %d off", h & 1);
            return;
        }
        sscanf(arg.c_str(), "%d %f %f %f %f %f %f %f %f %d", &h, &p[0], &p[1], &p[2], &yaw, &pitch, &c.trigger, &c.stick[0], &c.stick[1], &button);
        float y = yaw * 0.017453293f, x = pitch * 0.017453293f;
        float f[3] = { sinf(y) * cosf(x), sinf(x), cosf(y) * cosf(x) }, r[3] = { cosf(y), 0.0f, -sinf(y) };
        c.tracked = true;
        c.button = button != 0;
        for (int i = 0; i < 3; i++) c.aim.position[i] = p[i], c.aim.forward[i] = f[i], c.aim.right[i] = r[i];
        c.aim.up[0] = f[1] * r[2] - f[2] * r[1], c.aim.up[1] = f[2] * r[0] - f[0] * r[2], c.aim.up[2] = f[0] * r[1] - f[1] * r[0];
        xr::SetTestHand(h & 1, c);
        Log("input file: hand %d at %.2f %.2f %.2f, yaw %.1f, pitch %.1f, trigger %.2f, stick %.2f %.2f, button %d", h & 1, p[0], p[1], p[2], yaw, pitch,
            c.trigger, c.stick[0], c.stick[1], button);
        return;
    }
    if (cmd == "camera")
    {
        float* o = g_cfg.vrCameraOffset;
        sscanf(arg.c_str(), "%f %f %f", &o[0], &o[1], &o[2]);
        SaveCameraOffset();
        Log("input file: CameraOffset=%.3f %.3f %.3f", o[0], o[1], o[2]);
        return;
    }
    if (cmd == "log")
    {
        Log("input file: %s", arg.c_str());
        return;
    }
    for (char& c : arg) c = (char)toupper((unsigned char)c);
    int dik = KeyCode(arg);
    if (dik <= 0 || (cmd != "down" && cmd != "up" && cmd != "tap"))
    {
        Log("input file: cannot do \"%s\"", line.c_str());
        return;
    }
    SetKey((BYTE)dik, cmd != "up");
    if (cmd == "tap") g_releases.push_back({ NowSeconds() + 0.08, (BYTE)dik });
}

LONGLONG g_readOffset;

void ReadCommands()
{
    HANDLE f = CreateFileW(CommandFile().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart < g_readOffset) g_readOffset = 0;  // the file was started over
    if (size.QuadPart > g_readOffset)
    {
        std::string data((size_t)(size.QuadPart - g_readOffset), '\0');
        LARGE_INTEGER pos;
        pos.QuadPart = g_readOffset;
        DWORD got = 0;
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
        ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
        data.resize(got);
        size_t start = 0, nl;
        while ((nl = data.find('\n', start)) != std::string::npos)
        {
            RunCommand(data.substr(start, nl - start));
            start = nl + 1;
        }
        g_readOffset += start;  // an unfinished last line is read again next time
    }
    CloseHandle(f);
}

// ---- DirectInput ------------------------------------------------------------------------------
using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDeviceFn = HRESULT(__stdcall*)(void* self, REFGUID guid, void** device, IUnknown* outer);
using AcquireFn = HRESULT(__stdcall*)(void* self);
using GetStateFn = HRESULT(__stdcall*)(void* self, DWORD size, void* data);
using GetDataFn = HRESULT(__stdcall*)(void* self, DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* inOut, DWORD flags);
using SetFormatFn = HRESULT(__stdcall*)(void* self, const DIDATAFORMAT* format);

namespace slot {
constexpr int CreateDevice = 3;  // IDirectInput8
constexpr int Acquire = 7, GetDeviceState = 9, GetDeviceData = 10, SetDataFormat = 11, Poll = 25;  // IDirectInputDevice8
}

CreateDeviceFn oCreateDevice;
AcquireFn oAcquire, oPoll;
GetStateFn oGetDeviceState;
GetDataFn oGetDeviceData;
SetFormatFn oSetDataFormat;

// What each device delivers, by its data format: keyboard (256 bytes) or mouse (16/20 bytes).
// Other devices (game controllers) are left alone.
enum class Kind { Other, Keyboard, Mouse };
struct DeviceKind
{
    void* device;
    Kind kind;
};
std::vector<DeviceKind> g_devices;

Kind KindOf(void* device)
{
    for (auto& d : g_devices)
        if (d.device == device) return d.kind;
    return Kind::Other;
}

HRESULT __stdcall SetDataFormatHook(void* self, const DIDATAFORMAT* format)
{
    HRESULT hr = oSetDataFormat(self, format);
    if (SUCCEEDED(hr) && format)
    {
        Kind k = format->dwDataSize == 256 ? Kind::Keyboard : format->dwDataSize == 16 || format->dwDataSize == 20 ? Kind::Mouse : Kind::Other;
        bool known = false;
        for (auto& d : g_devices)
            if (d.device == self)
            {
                d.kind = k;
                known = true;
            }
        if (!known) g_devices.push_back({ self, k });
    }
    return hr;
}

// Without the focus the keyboard and mouse cannot be acquired; the game then sees them working
// but idle, apart from the keys of the command file.
HRESULT __stdcall AcquireHook(void* self)
{
    HRESULT hr = oAcquire(self);
    return g_on && FAILED(hr) && KindOf(self) != Kind::Other ? DI_OK : hr;
}

HRESULT __stdcall PollHook(void* self)
{
    HRESULT hr = oPoll(self);
    return g_on && FAILED(hr) && KindOf(self) != Kind::Other ? DI_OK : hr;
}

// A game controller's index among the game controllers (the order the game set them up in).
int ControllerIndex(void* device)
{
    int i = 0;
    for (auto& d : g_devices)
    {
        if (d.device == device) return i;
        if (d.kind == Kind::Other) i++;
    }
    return -1;
}

HRESULT __stdcall GetDeviceStateHook(void* self, DWORD size, void* data)
{
    HRESULT hr = oGetDeviceState(self, size, data);
    Kind k = KindOf(self);
    if (k == Kind::Other)
    {
        if (data && SUCCEEDED(hr) || rec::Replaying())
            if (rec::ControllerState(ControllerIndex(self), size, data)) return DI_OK;
        return hr;
    }
    if (!data || !g_on) return hr;
    if (FAILED(hr)) memset(data, 0, size);
    if (k == Kind::Keyboard && size == 256)
        for (int i = 0; i < 256; i++) ((BYTE*)data)[i] |= g_keys[i];
    return DI_OK;
}

HRESULT __stdcall GetDeviceDataHook(void* self, DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* inOut, DWORD flags)
{
    DWORD room = inOut ? *inOut : 0;
    HRESULT hr = oGetDeviceData(self, objectSize, data, inOut, flags);
    Kind k = KindOf(self);
    if (k == Kind::Other || !inOut || !g_on) return hr;
    if (FAILED(hr)) *inOut = 0;
    if (k == Kind::Keyboard && data && objectSize >= 16)
    {
        DWORD n = *inOut;
        size_t used = 0;
        while (n < room && used < g_events.size())
        {
            const KeyEvent& e = g_events[used++];
            auto* o = (DIDEVICEOBJECTDATA*)((BYTE*)data + (size_t)n * objectSize);
            memset(o, 0, objectSize);
            o->dwOfs = e.dik;
            o->dwData = e.down ? 0x80 : 0;
            o->dwTimeStamp = GetTickCount();
            o->dwSequence = ++g_sequence;
            n++;
        }
        if (!(flags & DIGDD_PEEK)) g_events.erase(g_events.begin(), g_events.begin() + used);
        *inOut = n;
    }
    return DI_OK;
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

HRESULT __stdcall CreateDeviceHook(void* self, REFGUID guid, void** device, IUnknown* outer)
{
    HRESULT hr = oCreateDevice(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device)
    {
        // All of dinput8's devices share one vtable.
        void** vtbl = *(void***)*device;
        HookSlot(vtbl, slot::Acquire, (void*)&AcquireHook, oAcquire);
        HookSlot(vtbl, slot::GetDeviceState, (void*)&GetDeviceStateHook, oGetDeviceState);
        HookSlot(vtbl, slot::GetDeviceData, (void*)&GetDeviceDataHook, oGetDeviceData);
        HookSlot(vtbl, slot::SetDataFormat, (void*)&SetDataFormatHook, oSetDataFormat);
        HookSlot(vtbl, slot::Poll, (void*)&PollHook, oPoll);
    }
    return hr;
}

HRESULT WINAPI DirectInput8CreateHook(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    auto create = *(CreateFn*)kDirectInputCreatePtr;
    HRESULT hr = create(inst, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out)
    {
        HookSlot(*(void***)*out, slot::CreateDevice, (void*)&CreateDeviceHook, oCreateDevice);
        Log("DirectInput hooked");
    }
    return hr;
}

// The Steam release's fo2_zoom.dll makes the game's DirectInput8Create call into its own (it brings in game controllers
// through SDL). Then dinput8's own interface is hooked instead, through an object made for that as Direct3D starts
// (HookInputInterfaces): the game makes its DirectInput after its Direct3D device, and its devices come from
// dinput8's IDirectInput8A::CreateDevice either way.
bool g_hookInterfaces;

// The game's DirectInput8Create call goes through DirectInput8CreateHook (background mode, input
// recording and replay).
bool InstallInputHooks()
{
    static bool done;
    if (done) return true;
    const uint8_t expected[6] = { 0xFF, 0x15, 0x48, 0xA7, 0x8D, 0x00 };
    if (memcmp((const void*)kDirectInputCreateCall, expected, 6) != 0)
    {
        Log("input: the game's DirectInput call is another's (fo2_zoom.dll's) - DirectInput hooked as Direct3D starts");
        g_hookInterfaces = done = true;
        return true;
    }
    uint8_t call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    int32_t rel = (int32_t)((uintptr_t)&DirectInput8CreateHook - (kDirectInputCreateCall + 5));
    memcpy(call + 1, &rel, 4);
    DWORD old;
    VirtualProtect((void*)kDirectInputCreateCall, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)kDirectInputCreateCall, call, 6);
    VirtualProtect((void*)kDirectInputCreateCall, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kDirectInputCreateCall, 6);
    done = true;
    return true;
}

}  // namespace

bool InstallBackground()
{
    g_on = g_cfg.testBackground;
    if (g_cfg.testRecord || g_cfg.testReplay) InstallInputHooks();
    if (g_cfg.testRecord) Log("record: the next race's input will be recorded");
    if (!g_on)
    {
        // A mute left from an earlier background run is undone.
        if (GetFileAttributesW(MuteFlag().c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            SetProcessMute(false);
            DeleteFileW(MuteFlag().c_str());
        }
        return true;
    }
    g_prevForeground = GetForegroundWindow();
    DeleteFileW(CommandFile().c_str());
    SetProcessMute(true);
    HANDLE flag = CreateFileW(MuteFlag().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (flag != INVALID_HANDLE_VALUE) CloseHandle(flag);

    bool window = PatchImport(kIatCreateWindowExA, "CreateWindowExA", (void*)&CreateWindowExAHook, oCreateWindowExA);
    window &= PatchImport(kIatShowWindow, "ShowWindow", (void*)&ShowWindowHook, oShowWindow);
    window &= PatchImport(kIatSetFocus, "SetFocus", (void*)&SetFocusHook, oSetFocus);
    if (!window) Log("background: window functions differ - the window may show up on the main monitor first");

    if (!InstallInputHooks()) Log("background: keys stay on the keyboard");
    Log("background mode: window on another monitor, no sound, keys from %ls", CommandFile().c_str());
    return true;
}

bool BackgroundMode() { return g_on; }

void HookInputInterfaces()
{
    if (!g_hookInterfaces) return;
    g_hookInterfaces = false;
    static const GUID kDirectInput8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
    HMODULE dinput = LoadLibraryW(L"dinput8.dll");
    auto create = dinput ? (CreateFn)GetProcAddress(dinput, "DirectInput8Create") : nullptr;
    void* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), 0x0800, kDirectInput8A, &di, nullptr)) || !di)
    {
        Log("input: no DirectInput of its own - not hooked");
        return;
    }
    HookSlot(*(void***)di, slot::CreateDevice, (void*)&CreateDeviceHook, oCreateDevice);
    ((IUnknown*)di)->Release();
    Log("DirectInput hooked (its interface)");
}

void BackgroundBeforeDevice(D3DPRESENT_PARAMETERS* pp)
{
    if (!g_on || !pp) return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
}

void BackgroundAfterDevice(HWND window)
{
    if (!g_on || !window || g_gameWindow == window) return;
    g_gameWindow = window;
    g_place = Placement();
    PlaceWindow();
    g_gameWndProc = (WNDPROC)SetWindowLongA(window, GWL_WNDPROC, (LONG)(LONG_PTR)&BackgroundWndProc);
    if (g_prevForeground && IsWindow(g_prevForeground) && g_prevForeground != window) SetForegroundWindow(g_prevForeground);
    // The window never really gets the focus: tell the game it has it (it reads no input otherwise).
    PostMessageA(window, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageA(window, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageA(window, WM_SETFOCUS, 0, 0);
    Log("background: game window at %ld,%ld %ldx%ld", g_place.left, g_place.top, g_place.right - g_place.left, g_place.bottom - g_place.top);
}

int BackgroundPoll()
{
    if (!g_on) return 0;
    double now = NowSeconds();
    for (size_t i = 0; i < g_releases.size();)
    {
        if (now >= g_releases[i].at)
        {
            SetKey(g_releases[i].dik, false);
            g_releases.erase(g_releases.begin() + i);
        }
        else
            i++;
    }
    ReadCommands();
    // The game may move its window back (a device reset): put it where it belongs.
    if (g_gameWindow && now - g_lastPlaceCheck > 1.0)
    {
        g_lastPlaceCheck = now;
        RECT r;
        GetWindowRect(g_gameWindow, &r);
        if (r.left != g_place.left || r.top != g_place.top || r.right != g_place.right || r.bottom != g_place.bottom) PlaceWindow();
    }
    int r = (g_shotRequested ? kPollShot : 0) | (g_traceRequested ? kPollTrace : 0) | (g_hudRequested ? kPollHud : 0) |
            (g_ejectRequested ? kPollEject : 0);
    g_shotRequested = g_traceRequested = g_hudRequested = g_ejectRequested = false;
    return r;
}

bool HotkeysAllowed() { return !g_on || GetForegroundWindow() == g_gameWindow; }

void RequestShot() { g_shotRequested = true; }

bool TakeMirrorKnock(char* side, int* state)
{
    if (!g_mirrorKnockSide) return false;
    *side = g_mirrorKnockSide;
    *state = g_mirrorKnockState;
    g_mirrorKnockSide = 0;
    return true;
}
void RequestTrace() { g_traceRequested = true; }
