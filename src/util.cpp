#include "util.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

Config g_cfg;

namespace {
FILE* g_log;
std::mutex g_logMutex;
}  // namespace

void LogInit(const std::wstring& path)
{
    // The previous run's log stays as FO2VR.prev.log.
    std::wstring prev = path.substr(0, path.size() - 4) + L".prev.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfopen(path.c_str(), L"w");
}

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static bool WriteProtected(uintptr_t addr, const void* src, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, src, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}

bool PatchCall(uintptr_t site, uintptr_t expected, const void* hook)
{
    const uint8_t* p = (const uint8_t*)site;
    if (p[0] != 0xE8 || site + 5 + *(const int32_t*)(p + 1) != expected)
    {
        Log("PatchCall %08X: unexpected code, not patched", (unsigned)site);
        return false;
    }
    int32_t rel = (int32_t)((uintptr_t)hook - (site + 5));
    return WriteProtected(site + 1, &rel, 4);
}

bool PatchPointer(uintptr_t slot, uintptr_t expected, const void* hook)
{
    if (*(const uintptr_t*)slot != expected)
    {
        Log("PatchPointer %08X: holds %08X, expected %08X, not patched", (unsigned)slot,
            (unsigned)*(const uintptr_t*)slot, (unsigned)expected);
        return false;
    }
    uintptr_t v = (uintptr_t)hook;
    return WriteProtected(slot, &v, sizeof(v));
}

bool PatchCode(uintptr_t addr, const void* expected, const void* code, size_t n)
{
    if (memcmp((const void*)addr, expected, n) != 0)
    {
        Log("PatchCode %08X: unexpected code, not patched", (unsigned)addr);
        return false;
    }
    return WriteProtected(addr, code, n);
}

const std::wstring& ModuleDir()
{
    static std::wstring dir = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&ModuleDir, &self);
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(self, buf, MAX_PATH);
        std::wstring s = buf;
        return s.substr(0, s.find_last_of(L"\\/") + 1);
    }();
    return dir;
}

static float IniFloat(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
    return buf[0] ? (float)_wtof(buf) : def;
}

void LoadConfig()
{
    std::wstring ini = ModuleDir() + L"FO2VR.ini";
    const wchar_t* p = ini.c_str();
    Config d;
    g_cfg.autoShotSeconds = IniFloat(p, L"Test", L"AutoShotSeconds", d.autoShotSeconds);
    g_cfg.vrEyesWithoutHeadset = GetPrivateProfileIntW(L"Test", L"VrEyesWithoutHeadset", d.vrEyesWithoutHeadset, p) != 0;
    g_cfg.testPaceHz = IniFloat(p, L"Test", L"PaceHz", d.testPaceHz);
    g_cfg.testPaceJitter = GetPrivateProfileIntW(L"Test", L"PaceJitter", d.testPaceJitter, p) != 0;
    wchar_t offset[64] = L"";
    GetPrivateProfileStringW(L"Test", L"HeadOffset", L"0 0 0", offset, 64, p);
    swscanf(offset, L"%f %f %f", &g_cfg.testHeadOffset[0], &g_cfg.testHeadOffset[1], &g_cfg.testHeadOffset[2]);
    g_cfg.testRecord = GetPrivateProfileIntW(L"Test", L"Record", d.testRecord, p) != 0;
    g_cfg.testReplay = GetPrivateProfileIntW(L"Test", L"Replay", d.testReplay, p) != 0;
    g_cfg.testFrameTimes = GetPrivateProfileIntW(L"Test", L"FrameTimes", d.testFrameTimes, p) != 0;
    g_cfg.testReplayStop = GetPrivateProfileIntW(L"Test", L"ReplayStop", d.testReplayStop, p);
    g_cfg.testReplayTrace = GetPrivateProfileIntW(L"Test", L"ReplayTrace", d.testReplayTrace, p);
    {
        wchar_t shots[128] = L"";
        GetPrivateProfileStringW(L"Test", L"ReplayShots", L"", shots, 128, p);
        int* s = g_cfg.testReplayShots;
        swscanf(shots, L"%d %d %d %d %d %d %d %d", &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7]);
    }
    g_cfg.testSampling = GetPrivateProfileIntW(L"Test", L"Sampling", d.testSampling, p) != 0;
    g_cfg.testShareEyes = GetPrivateProfileIntW(L"Test", L"ShareEyes", d.testShareEyes, p) != 0;
    g_cfg.testDeviceTiming = GetPrivateProfileIntW(L"Test", L"DeviceTiming", d.testDeviceTiming, p) != 0;
    g_cfg.testBackground = GetPrivateProfileIntW(L"Test", L"Background", d.testBackground, p) != 0;
    g_cfg.stepProbe = GetPrivateProfileIntW(L"Test", L"StepProbe", d.stepProbe, p) != 0;
    g_cfg.vrEnabled = GetPrivateProfileIntW(L"VR", L"Enabled", d.vrEnabled, p) != 0;
    {
        wchar_t buf[MAX_PATH];
        GetPrivateProfileStringW(L"VR", L"Runtime", d.vrRuntime.c_str(), buf, MAX_PATH, p);
        g_cfg.vrRuntime = buf;
    }
    g_cfg.vrRenderScale = IniFloat(p, L"VR", L"RenderScale", d.vrRenderScale);
    g_cfg.vrMsaa = GetPrivateProfileIntW(L"VR", L"MSAA", d.vrMsaa, p);
    g_cfg.vrScreenDistance = IniFloat(p, L"VR", L"ScreenDistance", d.vrScreenDistance);
    g_cfg.vrScreenWidth = IniFloat(p, L"VR", L"ScreenWidth", d.vrScreenWidth);
    g_cfg.vrWorldScale = IniFloat(p, L"VR", L"WorldScale", d.vrWorldScale);
    g_cfg.vrMirror = GetPrivateProfileIntW(L"VR", L"Mirror", d.vrMirror, p) != 0;
    g_cfg.vrPhysicsAtHeadsetRate = GetPrivateProfileIntW(L"VR", L"PhysicsAtHeadsetRate", d.vrPhysicsAtHeadsetRate, p) != 0;
    int hud = GetPrivateProfileIntW(L"VR", L"Hud", d.vrHud, p);
    g_cfg.vrHud = hud >= 0 && hud <= 2 ? hud : d.vrHud;
    g_cfg.vrHudDistance = IniFloat(p, L"VR", L"HudDistance", d.vrHudDistance);
    g_cfg.vrHudWidth = IniFloat(p, L"VR", L"HudWidth", d.vrHudWidth);
    g_cfg.vrHudHeight = IniFloat(p, L"VR", L"HudHeight", d.vrHudHeight);
    g_cfg.vrLodDistance = IniFloat(p, L"VR", L"LodDistance", d.vrLodDistance);
    g_cfg.vrGrassDistance = IniFloat(p, L"VR", L"GrassDistance", d.vrGrassDistance);
    g_cfg.vrViewDistance = IniFloat(p, L"VR", L"ViewDistance", d.vrViewDistance);
    g_cfg.vrSpeedBlur = GetPrivateProfileIntW(L"VR", L"SpeedBlur", d.vrSpeedBlur, p) != 0;
    g_cfg.vrSpeedZoom = IniFloat(p, L"VR", L"SpeedZoom", d.vrSpeedZoom);
    g_cfg.keyRecenter = GetPrivateProfileIntW(L"Keys", L"Recenter", d.keyRecenter, p);
    g_cfg.logIntervalSeconds = IniFloat(p, L"Log", L"IntervalSeconds", d.logIntervalSeconds);
    g_cfg.keyScreenshot = GetPrivateProfileIntW(L"Keys", L"Screenshot", d.keyScreenshot, p);
    g_cfg.keyHud = GetPrivateProfileIntW(L"Keys", L"Hud", d.keyHud, p);
}

double NowSeconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

const char* BuildStamp() { return __DATE__ " " __TIME__; }
