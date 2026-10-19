#pragma once
#include <cstdint>
#include <string>

void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);

// Redirect the E8 call at `site` (which must currently call `expected`) to `hook`.
bool PatchCall(uintptr_t site, uintptr_t expected, const void* hook);
// Replace the pointer in a vtable slot (which must currently hold `expected`); returns the old value.
bool PatchPointer(uintptr_t slot, uintptr_t expected, const void* hook);
// Replace n bytes of code at `addr` (which must currently be `expected`) with `code`.
bool PatchCode(uintptr_t addr, const void* expected, const void* code, size_t n);

// Directory of this ASI (with trailing backslash).
const std::wstring& ModuleDir();

struct Config
{
    // [Test]
    float autoShotSeconds = 0.0f;
    bool vrEyesWithoutHeadset = false;  // render the eyes (and the mirror) even when the headset wants no frames
    float testPaceHz = 0;    // races without a headset: pace the game like a headset at this rate (0 = off)
    bool testPaceJitter = false;  // the pretend headset's display times wander and jump like SteamVR's
    bool stepProbe = false;  // log how evenly the player's car moves from frame to frame
    float testHeadOffset[3] = {};  // metres added to the eye positions (x right, y up, z forward)
    float testHeadYawDeg = 0;      // the head turned right by this much (background mode command "head")
    bool testBackground = false;  // window on another monitor, no sound, keys from a file (background.h)
    bool testSampling = false;    // profiler: sample where the game thread runs (slow frames are logged)
    bool testShareEyes = true;     // the right eye copies the left eye's leaf and grass cards (0: each eye builds its own)
    bool testDeviceTiming = false;  // profiler: time every draw call and buffer lock (listed in the slow frames' logs)
    bool testRecord = false;      // record the next race's input (record.h)
    bool testReplay = false;      // play the recorded input back in the race
    bool testFrameTimes = false;  // write each race frame's display time and start to scripts\fo2vr_frametimes.csv
    int testReplayStop = 0;
    int testReplayShots[8] = {};
    int testReplayTrace = 0;  // replay: log one frame of render calls at this race frame (background mode)  // replay: screenshots at these race frames (background mode)       // replay: log the benchmark totals and close the game after this many race frames (0 = whole race)
    // [VR]
    bool vrEnabled = true;
    std::wstring vrRuntime = L"default";  // "default", "steamvr" or the path of a runtime .json
    float vrRenderScale = 1.0f;
    int vrMsaa = 4;
    float vrScreenDistance = 2.0f;
    float vrScreenWidth = 2.4f;
    float vrWorldScale = 1.0f;
    bool vrMirror = true;  // in races the monitor shows the left eye instead of its own render
    bool vrPhysicsAtHeadsetRate = true;  // races: one physics step per headset frame
    int vrHud = 1;                 // races: the game's HUD on a panel in the headset (2: not while racing)
    float vrHudDistance = 1.2f;    // metres in front of the recenter origin
    float vrHudWidth = 1.3f;       // metres
    float vrHudHeight = 0.0f;      // metres above the recenter origin
    float vrLodDistance = 0.0f;    // level-of-detail and draw distances times this (0 = no limit)
    float vrGrassDistance = 5.0f;  // how far grass is drawn, times this
    float vrViewDistance = 3.0f;   // the far clipping distance times this
    bool vrSpeedBlur = true;       // races: the game's speed blur in the eyes
    float vrSpeedZoom = 1.0f;      // races: the game's widening of the field of view at speed, times this
    // [Log]
    float logIntervalSeconds = 2.0f;
    // [Keys]
    int keyScreenshot = 0x7A;  // F11
    int keyRecenter = 0x77;    // F8
    int keyHud = 0x78;         // F9: Hud 1 <-> 2
};
extern Config g_cfg;
void LoadConfig();

double NowSeconds();

// Timestamp of this build (util.cpp is always recompiled, see CMakeLists.txt).
const char* BuildStamp();
