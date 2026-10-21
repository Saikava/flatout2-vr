#include "util.h"

#include "game.h"

#include <windows.h>

#include <algorithm>
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
    g_cfg.testFakeHeadset = GetPrivateProfileIntW(L"Test", L"FakeHeadset", d.testFakeHeadset, p) != 0;
    g_cfg.testMoveCards = GetPrivateProfileIntW(L"Test", L"MoveCards", d.testMoveCards, p) != 0;
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
    int mirror = GetPrivateProfileIntW(L"VR", L"Mirror", d.vrMirror, p);
    g_cfg.vrMirror = mirror >= 0 && mirror <= 2 ? mirror : d.vrMirror;
    g_cfg.vrMonitorFov = IniFloat(p, L"VR", L"MonitorFov", d.vrMonitorFov);
    g_cfg.vrMonitorPitch = std::clamp(IniFloat(p, L"VR", L"MonitorPitch", d.vrMonitorPitch), -45.0f, 45.0f);
    g_cfg.vrMonitorSmoothing = IniFloat(p, L"VR", L"MonitorSmoothing", d.vrMonitorSmoothing);
    g_cfg.vrPhysicsAtHeadsetRate = GetPrivateProfileIntW(L"VR", L"PhysicsAtHeadsetRate", d.vrPhysicsAtHeadsetRate, p) != 0;
    int hud = GetPrivateProfileIntW(L"VR", L"Hud", d.vrHud, p);
    g_cfg.vrHud = hud >= 0 && hud <= 2 ? hud : d.vrHud;
    g_cfg.vrHudDistance = IniFloat(p, L"VR", L"HudDistance", d.vrHudDistance);
    g_cfg.vrHudWidth = IniFloat(p, L"VR", L"HudWidth", d.vrHudWidth);
    g_cfg.vrHudHeight = IniFloat(p, L"VR", L"HudHeight", d.vrHudHeight);
    g_cfg.vrLodDistance = IniFloat(p, L"VR", L"LodDistance", d.vrLodDistance);
    g_cfg.vrGrassDistance = IniFloat(p, L"VR", L"GrassDistance", d.vrGrassDistance);
    g_cfg.vrViewDistance = IniFloat(p, L"VR", L"ViewDistance", d.vrViewDistance);
    g_cfg.vrFloatDepth = GetPrivateProfileIntW(L"VR", L"FloatDepth", d.vrFloatDepth, p) != 0;
    g_cfg.vrSpeedBlur = GetPrivateProfileIntW(L"VR", L"SpeedBlur", d.vrSpeedBlur, p) != 0;
    g_cfg.vrSpeedZoom = IniFloat(p, L"VR", L"SpeedZoom", d.vrSpeedZoom);
    {
        wchar_t buf[64] = L"";
        GetPrivateProfileStringW(L"VR", L"CameraOffset", L"0 0 0", buf, 64, p);
        float* o = g_cfg.vrCameraOffset;
        swscanf(buf, L"%f %f %f", &o[0], &o[1], &o[2]);
    }
    g_cfg.vrCockpit = GetPrivateProfileIntW(L"VR", L"Cockpit", d.vrCockpit, p) != 0;
    g_cfg.vrStartInCockpit = GetPrivateProfileIntW(L"VR", L"StartInCockpit", d.vrStartInCockpit, p) != 0;
    g_cfg.vrCockpitHud = GetPrivateProfileIntW(L"VR", L"CockpitHud", d.vrCockpitHud, p) != 0;
    g_cfg.vrNearClip = IniFloat(p, L"VR", L"NearClip", d.vrNearClip);
    g_cfg.vrCockpitGlass = std::clamp(IniFloat(p, L"VR", L"CockpitGlass", d.vrCockpitGlass), 0.0f, 1.0f);
    int eject = GetPrivateProfileIntW(L"VR", L"EjectView", d.vrEjectView, p);
    g_cfg.vrEjectView = eject >= 0 && eject <= 2 ? eject : d.vrEjectView;
    g_cfg.vrMirrors = std::clamp((int)GetPrivateProfileIntW(L"VR", L"Mirrors", d.vrMirrors, p), 0, 2);
    g_cfg.vrMirrorRadius = IniFloat(p, L"VR", L"MirrorRadius", d.vrMirrorRadius);
    g_cfg.vrMirrorRadius = g_cfg.vrMirrorRadius > 0.0f ? std::max(g_cfg.vrMirrorRadius, 0.3f) : 0.0f;
    g_cfg.keyRecenter = GetPrivateProfileIntW(L"Keys", L"Recenter", d.keyRecenter, p);
    g_cfg.logIntervalSeconds = IniFloat(p, L"Log", L"IntervalSeconds", d.logIntervalSeconds);
    g_cfg.keyScreenshot = GetPrivateProfileIntW(L"Keys", L"Screenshot", d.keyScreenshot, p);
    g_cfg.keyHud = GetPrivateProfileIntW(L"Keys", L"Hud", d.keyHud, p);
    g_cfg.keyCameraUp = GetPrivateProfileIntW(L"Keys", L"CameraUp", d.keyCameraUp, p);
    g_cfg.keyCameraDown = GetPrivateProfileIntW(L"Keys", L"CameraDown", d.keyCameraDown, p);
    g_cfg.keyCameraLeft = GetPrivateProfileIntW(L"Keys", L"CameraLeft", d.keyCameraLeft, p);
    g_cfg.keyCameraRight = GetPrivateProfileIntW(L"Keys", L"CameraRight", d.keyCameraRight, p);
    g_cfg.keyCameraForward = GetPrivateProfileIntW(L"Keys", L"CameraForward", d.keyCameraForward, p);
    g_cfg.keyCameraBack = GetPrivateProfileIntW(L"Keys", L"CameraBack", d.keyCameraBack, p);
    g_cfg.keyCameraReset = GetPrivateProfileIntW(L"Keys", L"CameraReset", d.keyCameraReset, p);
}

void SaveCameraOffset()
{
    std::wstring ini = ModuleDir() + L"FO2VR.ini";
    const float* o = g_cfg.vrCameraOffset;
    wchar_t buf[64];
    swprintf(buf, 64, L"%.3f %.3f %.3f", o[0], o[1], o[2]);
    // Replaces the whole line (an ini without the key gets it at the end of [VR]).
    WritePrivateProfileStringW(L"VR", L"CameraOffset", buf, ini.c_str());
}

// The value goes between the key's '=' and its comment (';'), padded so that the comment keeps its column; a key not in the
// file is added at the end of its section (WritePrivateProfileString, which on its own would write the comment off).
void SaveIniValue(const char* section, const char* key, const char* value)
{
    std::wstring path = ModuleDir() + L"FO2VR.ini";
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    std::string sec, k = std::string(key) + "=";
    size_t at = 0;
    bool done = false;
    while (at < text.size() && !done)
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        size_t lineEnd = end > at && text[end - 1] == '\r' ? end - 1 : end;
        std::string line = text.substr(at, lineEnd - at);
        if (!line.empty() && line[0] == '[')
            sec = line.substr(1, line.find(']') - 1);
        else if (_stricmp(sec.c_str(), section) == 0 && line.size() >= k.size() && _strnicmp(line.c_str(), k.c_str(), k.size()) == 0)
        {
            size_t semi = line.find(';', k.size());
            std::string out = line.substr(0, k.size()) + value;
            if (semi != std::string::npos)
            {
                out.append(std::max<size_t>(1, semi > out.size() ? semi - out.size() : 1), ' ');
                out += line.substr(semi);
            }
            text.replace(at, lineEnd - at, out);
            done = true;
        }
        at = end + 1;
    }
    if (!done)
    {
        std::wstring s(section, section + strlen(section)), kk(key, key + strlen(key)), v(value, value + strlen(value));
        WritePrivateProfileStringW(s.c_str(), kk.c_str(), v.c_str(), path.c_str());
        return;
    }
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

// The game keeps its race camera in its settings (see game.h, kSettingCamera), read from Savegame\options.cfg at its
// start: the first start with the mod puts it inside the car, in the file and in the game's default (an options.cfg
// not made yet). After that the camera chosen with C is the game's to keep.
void StartInCockpit()
{
    if (!g_cfg.vrEnabled || !g_cfg.vrStartInCockpit || g_cfg.testReplay) return;
    *(int*)fo2::kSettingCamera = fo2::kCameraInside;
    std::wstring path = ModuleDir() + L"..\\Savegame\\options.cfg";
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    const char* key = "Settings.Game.Camera = ";
    size_t at = text.find(key);
    const char* how = "no Savegame\\options.cfg yet: the game's default";
    if (at != std::string::npos)
    {
        at += strlen(key);
        size_t end = at;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') end++;
        text.replace(at, end - at, std::to_string(fo2::kCameraInside));
        FILE* f = _wfopen(path.c_str(), L"wb");
        bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
        if (f) ok = fclose(f) == 0 && ok;
        how = ok ? "Savegame\\options.cfg" : "Savegame\\options.cfg could not be written";
    }
    g_cfg.vrStartInCockpit = false;
    SaveIniValue("VR", "StartInCockpit", "0");
    Log("cockpit: the race camera set inside the car (%s); StartInCockpit=0 from now on", how);
}

double NowSeconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

const char* BuildStamp() { return __DATE__ " " __TIME__; }
