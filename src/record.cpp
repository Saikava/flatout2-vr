#include "record.h"

#include "background.h"
#include "game.h"
#include "profile.h"
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rec {
namespace {

// The game's keyboard state (see background.cpp) and its random generator (Mersenne twister:
// a flag, the words left, a pointer into the state, 624 words of state).
constexpr uintptr_t kKeyState = 0x8D7E60;
constexpr uintptr_t kKeyPresses = 0x8D7D60;
constexpr uintptr_t kKeysHeld = 0x8DA740;
constexpr uintptr_t kRandom = 0x69C918;
constexpr size_t kRandomSize = 0xC + 624 * 4;
constexpr uint32_t kMagic = 0x32524F46;  // "FOR2"
constexpr int kCheckpointFrames = 120;
constexpr int kRaceStateCountdown = 7;

// Records of the file, after the magic.
enum Type : uint8_t
{
    kKey = 1,    // f32 clock, u8 vk, u8 state, u8 presses
    kPad = 2,    // f32 clock, u8 controller, u16 size, state
    kSync = 3,   // f32 clock, i32 game state, i32 race state
    kRace = 4,   // u32 size, random generator state, u32 game time (ms), u32 frame count: the race part starts (countdown)
    kFrame = 5,  // u8 flags, u8 steps, [f64 step], [eye poses], [eye fields of view], u8 keys, keys (vk, state, presses),
                 // u8 pads, pads (u8, u16, state), [car position]
    kEnd = 6,
};
constexpr uint8_t kCheckpoint = 1;  // frame flag: the player's car position follows
constexpr uint8_t kStepChange = 2;  // frame flag: the physics step (seconds) follows
constexpr uint8_t kView = 4;        // frame flag: the eyes' poses follow (2 xr::EyePose)
constexpr uint8_t kViewFov = 8;     // frame flag: the eyes' fields of view follow (2 xr::EyeFov), when they changed
double g_step;                      // the physics step last recorded or replayed
xr::EyeFov g_fovs[2];               // the fields of view last recorded or replayed
bool g_haveFovs;

enum class Mode { Off, Record, Replay };
Mode g_mode;
enum class Part { Menus, Race, Done } g_part = Part::Menus;
int g_frame;

// The menu clock: real time, but a gap of more than a quarter second (loading) counts as 50 ms.
double g_clock, g_lastTick;
void AdvanceClock()
{
    double now = NowSeconds();
    if (g_lastTick > 0) g_clock += std::min(now - g_lastTick > 0.25 ? 0.05 : now - g_lastTick, 0.25);
    g_lastTick = now;
}

std::wstring File() { return ModuleDir() + L"fo2vr_rec.bin"; }
std::wstring SaveCopy() { return ModuleDir() + L"fo2vr_rec_save\\"; }
std::wstring SaveDir() { return ModuleDir() + L"..\\Savegame\\"; }

void CopyFolder(const std::wstring& from, const std::wstring& to)
{
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        n += CopyFileW((from + fd.cFileName).c_str(), (to + fd.cFileName).c_str(), FALSE) ? 1 : 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log("record: %d save files copied to %ls", n, to.c_str());
}

// A replay plays on the recording's saves: the player's own wait here meanwhile, and are put back
// at the next start without a replay (or when a benchmark replay closes the game).
std::wstring UserSaveKeep() { return ModuleDir() + L"fo2vr_user_save\\"; }

bool HasFiles(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do
        any |= !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

void RestoreUserSaves()
{
    std::wstring keep = UserSaveKeep();
    if (!HasFiles(keep)) return;
    CopyFolder(keep, SaveDir());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((keep + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((keep + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(keep.c_str());
    Log("replay: the player's own saves are back");
}

// While recording or replaying the menus, the random generator is held at a fixed state: the race
// is loaded from the same randomness both times (the starting grid, among others), whatever the
// menus drew from it. Mersenne twister seeded with 5489; the next draw regenerates.
void FixRandom()
{
    BYTE* r = (BYTE*)kRandom;
    uint32_t* mt = (uint32_t*)(r + 0xC);
    mt[0] = 5489u;
    for (uint32_t i = 1; i < 624; i++) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
    r[0] = 1;                            // seeded
    *(int32_t*)(r + 4) = 1;              // words left: the next draw regenerates
    *(uint32_t**)(r + 8) = mt;
}

void States(int* game, int* race)
{
    BYTE* gf = *(BYTE**)fo2::kGameFlow;
    *game = gf ? *(int*)(gf + fo2::kGameState) : -1;
    *race = gf ? *(int*)(gf + fo2::kRaceState) : -1;
}

bool CarPosition(float* p)
{
    BYTE* player = *(BYTE**)fo2::kLocalPlayer;
    BYTE* car = player ? *(BYTE**)(player + fo2::kPlayerCar) : nullptr;
    if (!car) return false;
    memcpy(p, car + fo2::kCarMatrix + 48, 12);
    return true;
}

// A recorded key state, as the game's keyboard hook would set it: a key going down counts as a
// press (the game takes the recorded press counts back to zero once it has seen them).
void SetKeys(uint8_t vk, uint8_t state)
{
    BYTE* keys = (BYTE*)kKeyState;
    bool was = (keys[vk] & 0x80) != 0, now = (state & 0x80) != 0;
    if (was != now) *(int*)kKeysHeld += now ? 1 : -1;
    // Down now, or down and up again in between (the hook flips bit 0 on every press).
    if ((!was && now) || (!was && !now && ((keys[vk] ^ state) & 1))) ((BYTE*)kKeyPresses)[vk]++;
    keys[vk] = state;
}

// ---- recording ----------------------------------------------------------------------------------
FILE* g_out;
BYTE g_lastKeys[256], g_lastPresses[256];
int g_lastGame = -2, g_lastRace = -2;
struct PendingFrame
{
    bool have = false;
    uint8_t steps = 1;
    double step = 0;
    bool view = false;
    xr::EyePose poses[2];
    xr::EyeFov fovs[2];
    std::vector<uint8_t> keys;  // vk, state, presses
} g_pending;
std::vector<std::vector<uint8_t>> g_padRead, g_padLast;  // states read during the pending frame, last written
std::vector<bool> g_padChanged;

void Put(const void* p, size_t n) { fwrite(p, 1, n, g_out); }
void Put8(uint8_t v) { Put(&v, 1); }
void PutF(float v) { Put(&v, 4); }

void RecordKeyChanges(std::vector<uint8_t>* frameKeys)
{
    const BYTE* keys = (const BYTE*)kKeyState;
    const BYTE* presses = (const BYTE*)kKeyPresses;
    for (int vk = 0; vk < 256; vk++)
    {
        if (keys[vk] == g_lastKeys[vk] && presses[vk] == g_lastPresses[vk]) continue;
        if (frameKeys)
        {
            frameKeys->push_back((uint8_t)vk);
            frameKeys->push_back(keys[vk]);
            frameKeys->push_back(presses[vk]);
        }
        else
        {
            Put8(kKey);
            PutF((float)g_clock);
            Put8((uint8_t)vk);
            Put8(keys[vk]);
            Put8(presses[vk]);
        }
        g_lastKeys[vk] = keys[vk];
        g_lastPresses[vk] = presses[vk];
    }
}

// A race frame's record: what the game read during it. Written once the next frame starts (the
// controllers are read after the gate).
void WritePending()
{
    if (!g_pending.have) return;
    float pos[3];
    bool checkpoint = g_frame % kCheckpointFrames == 0 && CarPosition(pos);
    bool stepChange = g_pending.step != g_step;
    bool fovChange = g_pending.view && (!g_haveFovs || memcmp(g_pending.fovs, g_fovs, sizeof(g_fovs)) != 0);
    Put8(kFrame);
    Put8((checkpoint ? kCheckpoint : 0) | (stepChange ? kStepChange : 0) | (g_pending.view ? kView : 0) | (fovChange ? kViewFov : 0));
    Put8(g_pending.steps);
    if (stepChange)
    {
        Put(&g_pending.step, 8);
        g_step = g_pending.step;
    }
    if (g_pending.view) Put(g_pending.poses, sizeof(g_pending.poses));
    if (fovChange)
    {
        Put(g_pending.fovs, sizeof(g_pending.fovs));
        memcpy(g_fovs, g_pending.fovs, sizeof(g_fovs));
        g_haveFovs = true;
    }
    Put8((uint8_t)(g_pending.keys.size() / 3));
    Put(g_pending.keys.data(), g_pending.keys.size());
    uint8_t pads = 0;
    for (size_t i = 0; i < g_padRead.size(); i++) pads += g_padChanged[i] ? 1 : 0;
    Put8(pads);
    for (size_t i = 0; i < g_padRead.size(); i++)
        if (g_padChanged[i])
        {
            Put8((uint8_t)i);
            uint16_t n = (uint16_t)g_padRead[i].size();
            Put(&n, 2);
            Put(g_padRead[i].data(), n);
            g_padLast[i] = g_padRead[i];
            g_padChanged[i] = false;
        }
    if (checkpoint) Put(pos, 12);
    g_pending = PendingFrame();
}

void RecordMenuTick()
{
    int game, race;
    States(&game, &race);
    if (game != g_lastGame || race != g_lastRace)
    {
        Put8(kSync);
        PutF((float)g_clock);
        Put(&game, 4);
        Put(&race, 4);
        g_lastGame = game;
        g_lastRace = race;
    }
    RecordKeyChanges(nullptr);
}

void EndRecording()
{
    WritePending();
    Put8(kEnd);
    fclose(g_out);
    g_out = nullptr;
    g_part = Part::Done;
    Log("record: %d race frames recorded", g_frame);
}

// ---- replay -----------------------------------------------------------------------------------
std::vector<uint8_t> g_in;
size_t g_pos;
std::vector<std::vector<uint8_t>> g_padReplay;
xr::EyePose g_replayPoses[2];  // the last replayed frame's (frames rendered without them keep the last)
bool g_replayHasView;
double g_maxOff;
bool g_waitSync;
int g_syncGame, g_syncRace;
double g_syncClock;

bool Get(void* p, size_t n)
{
    if (g_pos + n > g_in.size()) return false;
    memcpy(p, g_in.data() + g_pos, n);
    g_pos += n;
    return true;
}

bool GetPad()
{
    uint8_t index = 0;
    uint16_t n = 0;
    if (!Get(&index, 1) || !Get(&n, 2) || g_pos + n > g_in.size()) return false;
    if (g_padReplay.size() <= index) g_padReplay.resize(index + 1);
    g_padReplay[index].assign(g_in.data() + g_pos, g_in.data() + g_pos + n);
    g_pos += n;
    return true;
}

void ReplayMenuTick()
{
    int game, race;
    States(&game, &race);
    if (g_waitSync)
    {
        if (game != g_syncGame || race != g_syncRace) return;  // the clock waits for the game
        g_waitSync = false;
        g_clock = g_syncClock;
    }
    while (g_pos < g_in.size())
    {
        uint8_t type = g_in[g_pos];
        if (type == kRace || type == kEnd || type == kFrame) return;  // the race part starts at the countdown
        if (g_pos + 5 > g_in.size()) return;
        float t = 0;
        memcpy(&t, g_in.data() + g_pos + 1, 4);
        if (t > g_clock) return;
        g_pos += 5;
        if (type == kKey)
        {
            uint8_t k[3];
            if (!Get(k, 3)) return;
            SetKeys(k[0], k[1]);
        }
        else if (type == kPad)
        {
            if (!GetPad()) return;
        }
        else if (type == kSync)
        {
            int s[2];
            if (!Get(s, 8)) return;
            if (s[0] != game || s[1] != race)
            {
                g_waitSync = true;
                g_syncGame = s[0];
                g_syncRace = s[1];
                g_syncClock = t;
                return;
            }
        }
        else
        {
            Log("replay: unknown record %u at %u - stopped", type, (unsigned)g_pos);
            g_part = Part::Done;
            return;
        }
    }
}

void ReplayFrame(int* frames, double* step)
{
    uint8_t type = 0, flags = 0, steps = 1, nKeys = 0, nPads = 0;
    if (!Get(&type, 1) || type != kFrame || !Get(&flags, 1) || !Get(&steps, 1) || ((flags & kStepChange) && !Get(&g_step, 8)) ||
        ((flags & kView) && !Get(g_replayPoses, sizeof(g_replayPoses))) || ((flags & kViewFov) && !Get(g_fovs, sizeof(g_fovs))) ||
        !Get(&nKeys, 1))
    {
        g_part = Part::Done;
        Log("replay: end of the recording at race frame %d (car strayed up to %.2f m)", g_frame, g_maxOff);
        return;
    }
    *frames = steps;
    if (g_step > 0) *step = g_step;  // recordings before the step was recorded: the pace's own
    if (flags & kView) g_replayHasView = true;
    if (flags & kViewFov) g_haveFovs = true;
    for (int i = 0; i < nKeys; i++)
    {
        uint8_t k[3];
        if (!Get(k, 3)) break;
        SetKeys(k[0], k[1]);
    }
    Get(&nPads, 1);
    for (int i = 0; i < nPads; i++)
        if (!GetPad()) break;
    if (flags & kCheckpoint)
    {
        float rp[3], p[3];
        if (Get(rp, 12) && CarPosition(p))
        {
            double off = sqrt((double)(p[0] - rp[0]) * (p[0] - rp[0]) + (double)(p[1] - rp[1]) * (p[1] - rp[1]) +
                              (double)(p[2] - rp[2]) * (p[2] - rp[2]));
            if (off > g_maxOff) g_maxOff = off;
            if (g_frame % (kCheckpointFrames * 10) == 0 || off > 0.5)
                Log("replay: race frame %d, car %.3f m from the recorded path", g_frame, off);
        }
    }
}

void StartRace()
{
    if (g_mode == Mode::Record)
    {
        WritePending();
        Put8(kRace);
        uint32_t size = (uint32_t)kRandomSize;
        Put(&size, 4);
        Put((const void*)kRandom, kRandomSize);
        // The game's running totals since it started (the menus took their own time).
        Put((const void*)fo2::kSimTotalMs, 4);
        Put((const void*)fo2::kFrameCounter, 4);
        Log("record: race part starts");
    }
    else
    {
        // Menu records the game did not get to are skipped; the race part starts here.
        int skipped = 0;
        while (g_pos < g_in.size() && g_in[g_pos] != kRace)
        {
            uint8_t type = g_in[g_pos++];
            size_t n = type == kKey ? 7 : type == kSync ? 12 : 0;
            if (type == kPad && g_pos + 7 <= g_in.size())
            {
                uint16_t size = 0;
                memcpy(&size, g_in.data() + g_pos + 5, 2);
                n = 7 + size;
            }
            if (n == 0)
            {
                g_part = Part::Done;
                Log("replay: no race in the recording");
                return;
            }
            g_pos += n;
            skipped++;
        }
        uint32_t size = 0;
        g_pos++;
        if (!Get(&size, 4) || size != kRandomSize || g_pos + size > g_in.size())
        {
            g_part = Part::Done;
            Log("replay: no race in the recording");
            return;
        }
        memcpy((void*)kRandom, g_in.data() + g_pos, kRandomSize);
        g_pos += size;
        Get((void*)fo2::kSimTotalMs, 4);
        Get((void*)fo2::kFrameCounter, 4);
        g_waitSync = false;
        prof::ResetTotals();
        Log("replay: race part starts (%d menu records skipped)", skipped);
    }
    g_part = Part::Race;
    g_frame = 0;
    g_step = 0;
    g_haveFovs = false;
    g_replayHasView = false;
}

}  // namespace

void Install()
{
    if (!g_cfg.testReplay) RestoreUserSaves();
    if (g_cfg.testReplay)
    {
        FILE* f = _wfopen(File().c_str(), L"rb");
        uint32_t magic = 0;
        if (f)
        {
            fseek(f, 0, SEEK_END);
            g_in.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            fread(g_in.data(), 1, g_in.size(), f);
            fclose(f);
        }
        g_pos = 4;
        if (g_in.size() < 4 || (memcpy(&magic, g_in.data(), 4), magic != kMagic))
        {
            Log("replay: no usable recording at %ls", File().c_str());
            return;
        }
        if (!HasFiles(UserSaveKeep())) CopyFolder(SaveDir(), UserSaveKeep());
        CopyFolder(SaveCopy(), SaveDir());
        g_mode = Mode::Replay;
        Log("replay: playing back %ls (%u bytes)", File().c_str(), (unsigned)g_in.size());
    }
    else if (g_cfg.testRecord)
    {
        g_out = _wfopen(File().c_str(), L"wb");
        if (!g_out)
        {
            Log("record: cannot write %ls", File().c_str());
            return;
        }
        uint32_t magic = kMagic;
        Put(&magic, 4);
        CopyFolder(SaveDir(), SaveCopy());
        g_mode = Mode::Record;
        Log("record: recording the input to %ls", File().c_str());
    }
}

bool Replaying() { return g_mode == Mode::Replay && g_part != Part::Done; }

void MenuTick()
{
    if (g_mode == Mode::Off || g_part != Part::Menus) return;
    int game, race;
    States(&game, &race);
    if (game != 2) FixRandom();
    AdvanceClock();
    if (g_mode == Mode::Record)
        RecordMenuTick();
    else
        ReplayMenuTick();
}

bool RaceFrame(int* frames, double* step)
{
    if (g_mode == Mode::Off || g_part == Part::Done) return false;
    bool start = false;
    if (g_part == Part::Menus)
    {
        int game, race;
        States(&game, &race);
        if (race < kRaceStateCountdown) return false;
        StartRace();
        if (g_part != Part::Race) return false;
        start = true;
    }
    if (g_mode == Mode::Record)
    {
        WritePending();
        g_pending.have = true;
        g_pending.steps = (uint8_t)*frames;
        g_pending.step = *step;
        RecordKeyChanges(&g_pending.keys);
    }
    else
    {
        ReplayFrame(frames, step);
        for (int shot : g_cfg.testReplayShots)
            if (shot > 0 && shot == g_frame) RequestShot();
        if (g_cfg.testReplayTrace > 0 && g_cfg.testReplayTrace == g_frame) RequestTrace();
    }
    g_frame++;
    if (g_mode == Mode::Replay && g_cfg.testReplayStop > 0 && g_frame > g_cfg.testReplayStop)
    {
        // Benchmark over: the totals of the race frames so far, then the game is closed.
        char what[64];
        snprintf(what, sizeof(what), "benchmark, race frames 0-%d", g_frame - 2);
        prof::LogTotals(what);
        Log("replay: stopped at race frame %d (car strayed up to %.2f m) - closing the game", g_frame - 1, g_maxOff);
        RestoreUserSaves();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return start;
}

void RecordView(const xr::EyePose* poses, const xr::EyeFov* fovs)
{
    if (g_mode != Mode::Record || g_part != Part::Race || !g_pending.have) return;
    g_pending.view = true;
    memcpy(g_pending.poses, poses, sizeof(g_pending.poses));
    memcpy(g_pending.fovs, fovs, sizeof(g_pending.fovs));
}

bool ReplayView(xr::EyePose* poses, xr::EyeFov* fovs)
{
    if (g_mode != Mode::Replay || g_part != Part::Race || !g_replayHasView || !g_haveFovs) return false;
    memcpy(poses, g_replayPoses, sizeof(g_replayPoses));
    memcpy(fovs, g_fovs, sizeof(g_fovs));
    return true;
}

void NotInRace()
{
    if (g_mode != Mode::Off && g_part == Part::Menus) FixRandom();  // each menu frame, before the game's update
    if (g_part != Part::Race) return;
    if (g_mode == Mode::Record && g_out) EndRecording();
    if (g_mode == Mode::Replay) Log("replay: left the race at race frame %d (car strayed up to %.2f m)", g_frame, g_maxOff);
    g_part = Part::Done;
}

bool ControllerState(int device, unsigned size, void* data)
{
    if (device < 0 || device > 15 || !data || g_mode == Mode::Off || g_part == Part::Done) return false;
    if (g_mode == Mode::Record)
    {
        if (g_padRead.size() <= (size_t)device)
        {
            g_padRead.resize(device + 1);
            g_padChanged.resize(device + 1);
            g_padLast.resize(device + 1);
        }
        std::vector<uint8_t>& read = g_padRead[device];
        read.assign((const uint8_t*)data, (const uint8_t*)data + size);
        if (read == g_padLast[device]) return false;
        if (g_part == Part::Menus)
        {
            Put8(kPad);
            PutF((float)g_clock);
            Put8((uint8_t)device);
            uint16_t n = (uint16_t)size;
            Put(&n, 2);
            Put(data, size);
            g_padLast[device] = read;
        }
        else
            g_padChanged[device] = true;
        return false;
    }
    if ((size_t)device < g_padReplay.size() && g_padReplay[device].size() == size)
    {
        memcpy(data, g_padReplay[device].data(), size);
        return true;
    }
    return false;
}

}  // namespace rec
