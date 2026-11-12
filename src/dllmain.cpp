#include "background.h"
#include "controllers.h"
#include "d3dcreate.h"
#include "dialog.h"
#include "game.h"
#include "largeaddress.h"
#include "pacing.h"
#include "record.h"
#include "render.h"
#include "sound.h"
#include "util.h"
#include "xr.h"

#include <windows.h>

#include <cstdio>
#include <ctime>
#include <string>

// "FlatOut2.exe setup" (or -setup, the launchers' Settings): WinMain compares its whole command line
// with these two, case aside (0x520EFC), and then only shows the settings dialog (render device,
// resolution, antialiasing) and exits. The mod stays out of it: OpenXR would wait for the headset
// (and start SteamVR) before the dialog shows, a recording would be started, and the last game's log
// would make room for an empty one.
static bool SettingsDialog()
{
    // The command line after the program name (quoted or not) and the blanks after it, as WinMain gets it.
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"')
    {
        p++;
        while (*p && *p != L'"') p++;
        if (*p) p++;
    }
    else
        while (*p > L' ') p++;
    while (*p && *p <= L' ') p++;
    return _wcsicmp(p, L"setup") == 0 || _wcsicmp(p, L"-setup") == 0;
}

// The game's exit (game.h, kIatExitProcess). Through Virtual Desktop the game crashed in VirtualDesktop.LibOVRRT32_1.dll
// as it exited (0xC0000409; the system's log had it at nearly every exit from VR), and once it hung in the OpenXR
// loader's teardown instead (freeing memory in a loop: one core busy, no window, until the process was ended by hand):
// the VR session was still open, and the runtime's and the loader's DLLs took it down as Windows unloaded them, in
// whatever order. The mod ends the session and the instance itself first (xr::Shutdown), while all the threads still
// run, then the process, without the DLLs' unloading (the game's own shutdown is done by then; the mod's files are
// flushed). Without VR the game exits as it always did.
using ExitProcessFn = void(WINAPI*)(UINT);
static ExitProcessFn g_exitProcess;

static void WINAPI ExitProcessHook(UINT code)
{
    Log("the game exits (code %u)", code);
    if (xr::Shutdown())
    {
        Log("the process ends without unloading its DLLs");
        _flushall();
        TerminateProcess(GetCurrentProcess(), code);
    }
    g_exitProcess(code);
}

static void InstallExitHook()
{
    ExitProcessFn current = *(ExitProcessFn*)fo2::kIatExitProcess;
    if (current != (ExitProcessFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess"))
        Log("exit: the game's ExitProcess import is another's (%p) - the hook goes before it", (void*)current);
    g_exitProcess = current;
    if (!PatchPointer(fo2::kIatExitProcess, (uintptr_t)current, (const void*)&ExitProcessHook)) Log("exit: hook FAILED");
}

// ---- a game the mod does not know ----------------------------------------------------------------------------------
// InstallRenderHooks found other code than the mod needs: the mod stays off and the game goes on without VR. The
// player is told so in a window, and where a newer version of the mod is: before 1.6.14 only the log said it. (Steam
// updates the game by itself: after its update of 10 November 2026 the mod's earlier versions did not know the game.)
// Init runs as the ASI is loaded, inside Windows' loader, where a window cannot be shown and waited for. The window
// comes as the game makes its first window instead, on the game's own thread, before that one: its own window
// (CreateWindowExA) or, before it, its settings' (DialogBoxParamA: with the mod off the game draws on Windows'
// Direct3D, not DXVK, and where it then sees another graphics card than its settings were saved with, as the GOG
// release does, it shows them first). The game's imports of the two are looked up by name (FindImport: the addresses
// the mod knows may not be this game's) and put back as the window shows.

static const IMAGE_NT_HEADERS* Headers(const void* module)
{
    const BYTE* img = (const BYTE*)module;
    return (const IMAGE_NT_HEADERS*)(img + ((const IMAGE_DOS_HEADER*)img)->e_lfanew);
}

// Whether FlatOut2.exe is the file the mod knows (game.h): then its code was changed after it loaded.
static bool KnownExe()
{
    const IMAGE_NT_HEADERS* nt = Headers(GetModuleHandleW(nullptr));
    return nt->FileHeader.TimeDateStamp == fo2::kExeTimeStamp && nt->OptionalHeader.AddressOfEntryPoint == fo2::kExeEntryPoint;
}

static const wchar_t kDownloads[] = L"https://github.com/Saikava/flatout2-vr/releases/latest";

// The mod's downloads in the player's browser, through Windows' Explorer: the game may run as administrator (started
// by a Steam that does), and a browser it started itself would too.
static void OpenDownloads()
{
    wchar_t explorer[MAX_PATH + 16] = L"";
    GetWindowsDirectoryW(explorer, MAX_PATH);
    wcscat(explorer, L"\\explorer.exe");
    std::wstring command = L"\"" + std::wstring(explorer) + L"\" \"" + kDownloads + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(explorer, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        Log("the downloads could not be opened (error %u): %ls", (unsigned)GetLastError(), kDownloads);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

enum { kNewVersion = 100, kWithoutVr, kQuit };

static HRESULT CALLBACK UnknownGameProc(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR)
{
    if (msg == TDN_CREATED)
    {
        // Over whatever is on the screen as the game starts (Steam's window, a launcher's).
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
    }
    return S_OK;
}

static void TellUnknownGame()
{
    std::string v = ModVersion();
    std::wstring mod = L"FlatOut 2 VR" + (v == "unknown" ? std::wstring() : L" " + std::wstring(v.begin(), v.end()));
    std::wstring content =
        (KnownExe() ? L"The game's code is not as " + mod + L" knows it. Most likely the game was updated after this version of the "
                      L"mod came out (Steam updates it by itself); another mod that changes the same code does it too."
                    : L"This FlatOut2.exe is not the one " + mod + L" is made for: version 1.2 of the game, as the GOG release and "
                      L"the Steam release have it.") +
        L"\n\nThe mod stays off: the game starts without VR. A newer version of the mod may know this game.\n\n"
        L"Details: scripts\\FO2VR.log";
    static const TASKDIALOG_BUTTON buttons[] = {
        { kNewVersion, L"Look for a newer version of the mod\nOpens its downloads in the browser and closes the game" },
        { kWithoutVr, L"Play without VR" },
        { kQuit, L"Quit the game" } };
    TASKDIALOGCONFIG c = { sizeof(c) };
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    c.pszWindowTitle = L"FlatOut 2 VR";
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = L"FlatOut 2 VR does not know this version of the game";
    c.pszContent = content.c_str();
    c.cButtons = 3;
    c.pButtons = buttons;
    c.nDefaultButton = kNewVersion;
    c.pfCallback = UnknownGameProc;
    int button = 0;
    if (FAILED(ShowTaskDialog(c, &button))) return;
    Log("the player's choice: %s", button == kNewVersion ? "the mod's downloads - the game closes" : button == kQuit ? "quit" : "play without VR");
    if (button == kNewVersion) OpenDownloads();
    if (button == kNewVersion || button == kQuit)
    {
        _flushall();
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using DialogBoxParamAFn = INT_PTR(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static void **g_createWindowSlot, **g_dialogBoxSlot;  // the game's two imports while the mod's hooks are in them
static CreateWindowExAFn g_createWindow;
static DialogBoxParamAFn g_dialogBox;

static void TellAtFirstWindow();

static HWND WINAPI FirstWindowHook(DWORD exStyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                   HMENU menu, HINSTANCE inst, LPVOID param)
{
    TellAtFirstWindow();
    return g_createWindow(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

static INT_PTR WINAPI FirstDialogHook(HINSTANCE inst, LPCSTR dialog, HWND parent, DLGPROC proc, LPARAM param)
{
    TellAtFirstWindow();
    return g_dialogBox(inst, dialog, parent, proc, param);
}

static void TellAtFirstWindow()
{
    static bool told;
    if (told) return;
    told = true;
    if (g_createWindowSlot) PatchPointer((uintptr_t)g_createWindowSlot, (uintptr_t)&FirstWindowHook, (const void*)g_createWindow);
    if (g_dialogBoxSlot) PatchPointer((uintptr_t)g_dialogBoxSlot, (uintptr_t)&FirstDialogHook, (const void*)g_dialogBox);
    TellUnknownGame();
}

static void TellUnknownGameAtFirstWindow()
{
    if (g_cfg.testBackground) return;  // (a test nobody watches: the log says it)
    if ((g_createWindowSlot = FindImport("user32.dll", "CreateWindowExA")) != nullptr)
    {
        g_createWindow = (CreateWindowExAFn)*g_createWindowSlot;
        if (!PatchPointer((uintptr_t)g_createWindowSlot, (uintptr_t)g_createWindow, (const void*)&FirstWindowHook)) g_createWindowSlot = nullptr;
    }
    if ((g_dialogBoxSlot = FindImport("user32.dll", "DialogBoxParamA")) != nullptr)
    {
        g_dialogBox = (DialogBoxParamAFn)*g_dialogBoxSlot;
        if (!PatchPointer((uintptr_t)g_dialogBoxSlot, (uintptr_t)g_dialogBox, (const void*)&FirstDialogHook)) g_dialogBoxSlot = nullptr;
    }
    if (g_createWindowSlot || g_dialogBoxSlot) return;
    // No such import to wait at: the window on a thread of its own, beside the game as it starts.
    Log("the game imports neither CreateWindowExA nor DialogBoxParamA - the window about it comes as the game starts");
    if (HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { TellUnknownGame(); return 0; }, nullptr, 0, nullptr))
        CloseHandle(thread);
}

static void Init()
{
    if (SettingsDialog()) return;
    WaitForOldGame();
    LogInit(ModuleDir() + L"FO2VR.log");
    LoadConfig();

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const IMAGE_NT_HEADERS* nt = Headers(GetModuleHandleW(nullptr));
    Log("FO2VR %s, build %s", ModVersion(), BuildStamp());
    Log("game: %s, timestamp %08X, image size %08X, entry %08X", exe, (unsigned)nt->FileHeader.TimeDateStamp,
        (unsigned)nt->OptionalHeader.SizeOfImage, (unsigned)nt->OptionalHeader.AddressOfEntryPoint);
    // The Steam release's fo2_zoom.dll changes the game's code as it loads, and Steam updates it by itself: which one
    // this is, by the day it was built.
    if (HMODULE zoom = GetModuleHandleW(L"fo2_zoom.dll"))
    {
        const IMAGE_NT_HEADERS* zn = Headers(zoom);
        time_t built = zn->FileHeader.TimeDateStamp;
        tm day = {};
        gmtime_s(&day, &built);
        Log("game: the Steam release's fo2_zoom.dll of %04d-%02d-%02d (timestamp %08X, image size %08X)", day.tm_year + 1900,
            day.tm_mon + 1, day.tm_mday, (unsigned)zn->FileHeader.TimeDateStamp, (unsigned)zn->OptionalHeader.SizeOfImage);
    }
    Log("config: AutoShotSeconds %.1f", g_cfg.autoShotSeconds);
    Log("config: VR %d, runtime %ls, render scale %.2f, msaa %d", g_cfg.vrEnabled, g_cfg.vrRuntime.c_str(), g_cfg.vrRenderScale, g_cfg.vrMsaa);

    if (!InstallRenderHooks())
    {
        Log("unsupported FlatOut2.exe - mod inactive: %s", KnownExe() ? "the file is the v1.2 the mod knows, with its code changed in the "
                                                                         "places above (an update of the game, or another mod)"
                                                                       : "not the v1.2 the mod knows (GOG's and Steam's)");
        TellUnknownGameAtFirstWindow();
        return;
    }
    Log("render hooks installed");
    Log("config: Mirror %d (monitor %.0f deg, %.0f down, smoothing %.2f s), PhysicsAtHeadsetRate %d, Hud %d (%.2f m wide at %.2f m, "
        "height %.2f), PaceHz %.1f (jitter %d), StepProbe %d",
        g_cfg.vrMirror, g_cfg.vrMonitorFov, g_cfg.vrMonitorPitch, g_cfg.vrMonitorSmoothing, g_cfg.vrPhysicsAtHeadsetRate, g_cfg.vrHud, g_cfg.vrHudWidth, g_cfg.vrHudDistance, g_cfg.vrHudHeight,
        g_cfg.testPaceHz, g_cfg.testPaceJitter, g_cfg.stepProbe);
    const float* co = g_cfg.vrCameraOffset;
    Log("config: CameraOffset %.3f %.3f %.3f (right, up, forward)", co[0], co[1], co[2]);
    if (!InstallPacingHooks()) Log("frame gate hook FAILED");
    InstallBackground();
    InstallControllers();
    InstallSound();
    rec::Install();
    StartInCockpit();
    PutAwayOldDxvk();
    if (g_cfg.vrEnabled) InstallExitHook();
    if (g_cfg.vrEnabled || BackgroundMode()) Log("Direct3DCreate9 hook %s", InstallCreateHook() ? "installed" : "FAILED");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
