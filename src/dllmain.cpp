#include "background.h"
#include "d3dcreate.h"
#include "pacing.h"
#include "record.h"
#include "render.h"
#include "util.h"

#include <windows.h>

static void Init()
{
    LogInit(ModuleDir() + L"FO2VR.log");
    LoadConfig();

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const BYTE* img = (const BYTE*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(img + ((const IMAGE_DOS_HEADER*)img)->e_lfanew);
    Log("FO2VR build %s", BuildStamp());
    Log("game: %s, timestamp %08X, image size %08X, entry %08X", exe, (unsigned)nt->FileHeader.TimeDateStamp,
        (unsigned)nt->OptionalHeader.SizeOfImage, (unsigned)nt->OptionalHeader.AddressOfEntryPoint);
    Log("config: AutoShotSeconds %.1f", g_cfg.autoShotSeconds);
    Log("config: VR %d, runtime %ls, render scale %.2f, msaa %d", g_cfg.vrEnabled, g_cfg.vrRuntime.c_str(), g_cfg.vrRenderScale, g_cfg.vrMsaa);

    if (!InstallRenderHooks())
    {
        Log("unsupported FlatOut2.exe (needs v1.2 DRM-free) - mod inactive");
        return;
    }
    Log("render hooks installed");
    Log("config: Mirror %d, PhysicsAtHeadsetRate %d, Hud %d (%.2f m wide at %.2f m, height %.2f), PaceHz %.1f (jitter %d), StepProbe %d",
        g_cfg.vrMirror, g_cfg.vrPhysicsAtHeadsetRate, g_cfg.vrHud, g_cfg.vrHudWidth, g_cfg.vrHudDistance, g_cfg.vrHudHeight,
        g_cfg.testPaceHz, g_cfg.testPaceJitter, g_cfg.stepProbe);
    if (!InstallPacingHooks()) Log("frame gate hook FAILED");
    InstallBackground();
    rec::Install();
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
