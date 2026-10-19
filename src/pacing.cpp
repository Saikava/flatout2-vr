// Game loop timing. The game's frame gate runs a frame once a whole 10 ms tick has gathered, and
// the race simulation (0x4725C0) cuts a frame's time into pieces of at most 10 ms with one physics
// step of a fixed 0.01 s each. So the game shows at most 100 frames a second, and on a 90 or 120 Hz
// headset the world moves by one step in some shown frames and by two in others.
//
// In VR races the game follows the headset instead: the gate waits for the headset's frame, and
// each headset frame simulates exactly one physics step as long as the headset's frame period
// (the step and the piece limit are patched for that, and the cars take the same step). The game
// clock gets the same time in milliseconds, the fraction carried over, so the race and the game's
// millisecond timers run in real time.
#include "pacing.h"

#include "game.h"
#include "record.h"
#include "util.h"
#include "xr.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

static int __cdecl FrameGate(BYTE* gameFlow);

static uintptr_t g_gameGateFn = fo2::kFrameGate;
static uintptr_t g_readTimerFn = fo2::kReadFrameTimer;
static float g_physicsStep = 0.01f;  // seconds per physics step, pushed for the world step
static int g_physicsSteps;           // world steps so far, for the step probe
static int g_worldMs = 10;           // the world's step in whole milliseconds while pacing
static double g_frameStart;
static float g_thousand = 1000.0f, g_milli = 0.001f;

// The game calls the gate with GameFlow in edi and reads the result from eax.
static __declspec(naked) void FrameGateHook()
{
    __asm {
        push edi
        call FrameGate
        add esp, 4
        ret
    }
}

// Replaces "push 0.01" before the world step call: pushes g_physicsStep instead (eax is free there).
static __declspec(naked) void PushPhysicsStep()
{
    __asm {
        pop eax
        push dword ptr [g_physicsStep]
        inc g_physicsSteps
        jmp eax
    }
}

// While pacing, replaces the race tick's "mov [ebp + 0x207CC], 10" (the world's step in
// milliseconds, kTickSetStepMs): this frame's milliseconds instead.
static __declspec(naked) void SetWorldMs()
{
    __asm {
        push eax
        mov eax, g_worldMs
        mov dword ptr [ebp + 0x207CC], eax
        pop eax
        ret
    }
}

// While pacing, replaces "fild [esp + 0x50]; fld 0.001" (kCarStepLoad, the world's whole
// milliseconds and the factor that makes the car's step of them): loads the world's step in seconds
// times 1000 instead, so a car steps exactly as long as the world. ebp is the world.
static __declspec(naked) void LoadCarStepMs()
{
    __asm {
        fld dword ptr [ebp + 0x20724]
        fmul dword ptr [g_thousand]
        fld dword ptr [g_milli]
        ret
    }
}

static int CallGameGate(BYTE* gameFlow)
{
    int r;
    __asm {
        mov edi, gameFlow
        call dword ptr [g_gameGateFn]
        mov r, eax
    }
    return r;
}

static unsigned ReadGameTimer()
{
    void* holder = *(void**)fo2::kFrameTimerHolder;
    unsigned r;
    __asm {
        mov esi, holder
        call dword ptr [g_readTimerFn]
        mov r, eax
    }
    return r;
}

namespace {

int& Field(BYTE* p, int offset) { return *(int*)(p + offset); }
unsigned& Global(uintptr_t a) { return *(unsigned*)a; }

bool InRace(BYTE* gameFlow) { return Field(gameFlow, fo2::kGameState) == 2; }

bool WriteCode(uintptr_t addr, const void* src, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, src, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}

// A 10-byte instruction sequence that is swapped for a call (and a 5-byte nop) while pacing.
struct CallSwap
{
    uintptr_t addr = 0;
    uint8_t original[10] = {}, call[10] = {};
    bool on = false;

    void Prepare(uintptr_t a, const uint8_t* bytes, const void* target)
    {
        addr = a;
        memcpy(original, bytes, 10);
        const uint8_t tail[10] = { 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
        memcpy(call, tail, 10);
        int32_t rel = (int32_t)((uintptr_t)target - (a + 5));
        memcpy(call + 1, &rel, 4);
    }
    void Set(bool enable)
    {
        if (enable != on && WriteCode(addr, enable ? call : original, 10)) on = enable;
    }
};

// Longest piece of a frame the race simulation steps at once (milliseconds).
bool g_pieceHooked;
int g_pieceMs = 10;

void SetPieceLimit(int ms)
{
    ms = std::clamp(ms, 1, 127);
    if (!g_pieceHooked || ms == g_pieceMs) return;
    uint8_t imm8 = (uint8_t)ms;
    int32_t imm32 = ms;
    WriteCode(fo2::kRacePieceCmp + 2, &imm8, 1);
    WriteCode(fo2::kRacePieceMov + 1, &imm32, 4);
    g_pieceMs = ms;
}

// The race tick (0x472D30) sets the world's step at its start: milliseconds, seconds and the
// inverse, as immediates of three "mov [ebp+...], imm32" (6 bytes before each immediate). For any
// other step than the game's own the milliseconds come from g_worldMs and the cars take the
// world's step in seconds.
bool g_tickHooked;
float g_tickStep = 0.01f;
CallSwap g_worldMsSwap, g_carStepSwap;

void SetTickStep(float step)
{
    if (!g_tickHooked || step == g_tickStep) return;
    uint32_t sec, inv;
    bool own = step == 0.01f;
    if (own)
    {
        sec = 0x3C23D70B;  // the game's own values
        inv = 0x42C7FFFF;
    }
    else
    {
        float invf = 1.0f / step;
        memcpy(&sec, &step, 4);
        memcpy(&inv, &invf, 4);
    }
    WriteCode(fo2::kTickSetStep + 6, &sec, 4);
    WriteCode(fo2::kTickSetStepInv + 6, &inv, 4);
    g_worldMsSwap.Set(!own);
    g_carStepSwap.Set(!own);
    g_tickStep = step;
}

// The headset's frame period, from the intervals between the display times the runtime predicts.
// The runtime's own figure can jump from frame to frame (SteamVR with a Quest 3 at 120 Hz reports
// 6.6 to 18 ms), and a physics step that followed it would move the world unevenly.
// Headsets run at one of a few refresh rates. An estimate within 4% of one of them is taken to be
// that rate exactly (the nearest two are 10% apart): the noise in the runtime's display times
// pulls an estimate off by a percent or two, and a step that is off lets the frame clock drift into
// false catch-ups.
double SnapPeriod(double period)
{
    static const double rates[] = { 60.0, 72.0, 80.0, 90.0, 100.0, 120.0, 144.0 };
    for (double r : rates)
        if (fabs(period * r - 1.0) < 0.04) return 1.0 / r;
    return period;
}

// The headset's refresh period, from the intervals between the starts of consecutive game frames
// (when the runtime let them start). Not from the display times: SteamVR rounds them to steps of
// 3.28 ms, so at 120 Hz their intervals go 6.55, 9.83, 9.83, 6.55 ms and so on, with jumps of a
// frame ahead and back. The starts keep to the refresh within a few tenths of a millisecond, but
// for a second or two after a slow frame (and after loading) SteamVR runs the game at a half or a
// third of the rate. So the period is the median of the recent intervals (the mean of those near
// it), snapped to a standard rate. A shorter period is believed after a moment (a throttled game
// never sees intervals shorter than the refresh), at once when the old one was a whole multiple of
// it; a longer one only when it is not a whole multiple of the period and has lasted five seconds.
struct FramePeriod
{
    static constexpr int kRing = 31;
    double value = 0;  // seconds (0: not known yet)
    double ring[kRing] = {};
    int count = 0, next = 0;
    int shorter = 0;         // frames the intervals have said a shorter period
    double longerSince = 0;  // since when they have said a longer one (seconds)

    double Estimate() const
    {
        double v[kRing];
        std::copy(ring, ring + count, v);
        std::nth_element(v, v + count / 2, v + count);
        double median = v[count / 2], sum = 0;
        int n = 0;
        for (int i = 0; i < count; i++)
            if (fabs(ring[i] - median) < 0.1 * median)
            {
                sum += ring[i];
                n++;
            }
        return sum / n;
    }

    static bool WholeMultiple(double ratio) { return fabs(ratio - floor(ratio + 0.5)) < 0.04 * ratio; }

    void Add(double interval, double now)
    {
        ring[next] = interval;
        next = (next + 1) % kRing;
        count = std::min(count + 1, kRing);
        if (count < 9) return;
        double mean = Estimate(), e = SnapPeriod(mean);
        if (e < 0.004 || e > 0.0175) return;  // no headset refreshes slower than 60 Hz: throttled (loading)
        if (value <= 0)
        {
            value = e;
            return;
        }
        if (fabs(e - value) < 0.04 * value)  // the same rate: a standard one exactly, another smoothed
        {
            value = e != mean ? e : value + 0.05 * (e - value);
            shorter = 0;
            longerSince = 0;
        }
        else if (e < value)
        {
            longerSince = 0;
            if (++shorter >= (WholeMultiple(value / e) ? 3 : 30))
            {
                value = e;
                shorter = 0;
            }
        }
        else
        {
            shorter = 0;
            if (WholeMultiple(e / value)) longerSince = 0;  // throttled: frames are missed
            else if (longerSince == 0) longerSince = now;
            else if (now - longerSince > 5.0)
            {
                value = e;
                longerSince = 0;
            }
        }
    }
} g_period;

// The runtime's own period, when it can be believed: it has stayed the same for a while and is a
// standard rate (within 0.5%, which SteamVR's rounded 6.55 and 9.83 ms are not). Virtual Desktop
// reports the refresh exactly, but its frame starts come unevenly (at 144 Hz most intervals are
// some 6.6 ms and the rest longer), so the estimate from the starts said 151 Hz there, and the
// frame clocks took the longer intervals for missed frames and stepped the world twice.
struct RuntimePeriod
{
    double last = 0;
    int same = 0;  // frames it has stayed the same

    // The period to use, or 0.
    double Add(double reported)
    {
        same = fabs(reported - last) < 1e-6 ? same + 1 : 0;
        last = reported;
        double e = SnapPeriod(reported);
        return same >= 60 && fabs(e / reported - 1.0) < 0.005 ? e : 0;
    }
} g_runtimePeriod;

// Which headset frame each game frame is shown in. SteamVR's predicted display times are rounded to
// steps of 3.28 ms and now and then jump a whole frame ahead and come back on the next frame
// (one interval of two periods, then one of none) while the headset shows every frame. Counting
// frames from the intervals would take every such jump for a missed frame and step the world
// twice. So clocks that tick one period per frame follow the display times and the wall-clock
// time of the frames slowly; a frame counts as missed only when both clocks have been behind for
// two frames in a row: a real miss delays both, a jump in the runtime's prediction only one.
struct FrameClock
{
    double time = 0;  // when the current frame is due by this clock (0: not started)
    int late = 0;
    double minOff = 1e9, maxOff = -1e9;  // stats: actual - clock

    // Frames this clock is behind once it has been behind for two frames in a row, else 0.
    int Behind(double t, double period)
    {
        if (time == 0)
        {
            time = t;
            return 0;
        }
        time += period;
        double off = t - time;
        minOff = std::min(minOff, off);
        maxOff = std::max(maxOff, off);
        if (off < -2.0 * period)
        {
            time = t;  // the times went back: follow them
            late = 0;
            return 0;
        }
        if (off <= 0.5 * period)
        {
            late = 0;
            return 0;
        }
        return ++late >= 2 ? (int)floor(off / period + 0.5) : 0;
    }
    void Skip(int frames, double period)
    {
        time += frames * period;
        late = 0;
    }
    void Follow(double t) { time += 0.02 * (t - time); }
} g_clock, g_wallClock;

// Headset frames since the last game frame (more than one when the game missed some).
int CountFrames(double displayTime, double now, double period)
{
    int behind = std::min(g_clock.Behind(displayTime, period), g_wallClock.Behind(now, period));
    int frames = 1;
    if (behind > 3)
    {
        frames = 4;  // a long stall: no more catching up than that
        g_clock.time = displayTime;
        g_wallClock.time = now;
    }
    else if (behind > 0)
    {
        frames += behind;
        g_clock.Skip(behind, period);
        g_wallClock.Skip(behind, period);
    }
    g_clock.Follow(displayTime);
    g_wallClock.Follow(now);
    return frames;
}

// [Test] FrameTimes: the raw timing of each race frame, for trying out estimators on real runtime data.
void DumpFrameTime(double displayTime, double now, double reported, int frames, double period)
{
    static FILE* f = [] {
        FILE* out = _wfopen((ModuleDir() + L"fo2vr_frametimes.csv").c_str(), L"w");
        if (out) fputs("display,start,reported,frames,period\n", out);
        return out;
    }();
    if (f) fprintf(f, "%.6f,%.6f,%.6f,%d,%.6f\n", displayTime, now, reported, frames, period);
}

// ---- headset-paced gate -----------------------------------------------------------------------
struct Pacing
{
    bool active = false;
    double lastStart = 0;
    double step = 0;      // physics step (real seconds); follows the period within 0.1%
    bool runtimePeriod = false;  // the period is the runtime's, not g_period's
    double carryMs = 0;   // simulated milliseconds owed to the game clock
    double carryReal = 0;
    // stats
    int frames = 0, missed = 0;
    double wait = 0, lastLog = 0;
    double minReported = 1e9, maxReported = 0;
} g_pace;

void StopPacing()
{
    if (!g_pace.active) return;
    g_pace.active = false;
    g_physicsStep = 0.01f;
    g_worldMs = 10;
    SetPieceLimit(10);
    SetTickStep(0.01f);
    Log("pacing: back to the game's 10 ms ticks");
}

// One game frame per headset frame. Returns 1 (run a frame).
int HeadsetGate(BYTE* gf, double displayTime, double reported)
{
    if (Field(gf, fo2::kGfResetTimer))
    {
        ReadGameTimer();
        Field(gf, fo2::kGfResetTimer) = 0;
        g_pace.lastStart = 0;
        g_clock.time = g_wallClock.time = 0;
    }
    ReadGameTimer();  // keeps the game's timer current for when it takes over again

    // Headset frames since the last one (more than one when the game missed some).
    double now = NowSeconds();
    if (g_pace.lastStart > 0) g_period.Add(now - g_pace.lastStart, now);
    g_pace.lastStart = now;
    // Until the intervals tell (the first frames of the first race): 90 Hz. SteamVR's own period is
    // rounded like its display times (6.55 or 9.83 ms at 120 Hz).
    double period = g_runtimePeriod.Add(reported);
    g_pace.runtimePeriod = period > 0;
    if (period <= 0) period = g_period.value > 0 ? g_period.value : 1.0 / 90;
    if (g_pace.step <= 0 || fabs(period - g_pace.step) > 0.001 * g_pace.step) g_pace.step = period;
    int frames = CountFrames(displayTime, now, g_pace.step);
    if (g_cfg.testFrameTimes) DumpFrameTime(displayTime, now, reported, frames, g_period.value);
    // Recorded, or replaced by the recording; the race part of a recording starts with nothing carried.
    if (rec::RaceFrame(&frames, &g_pace.step)) g_pace.carryMs = g_pace.carryReal = 0;
    g_pace.missed += frames - 1;
    g_pace.minReported = std::min(g_pace.minReported, reported);
    g_pace.maxReported = std::max(g_pace.maxReported, reported);

    // Time scale of the game's slow motion: simulated = real >> up << down.
    int up = Field(gf, fo2::kGfTimeShiftUp), down = Field(gf, fo2::kGfTimeShiftDown);
    double scale = ldexp(1.0, down - up);
    double stepMs = g_pace.step * 1000.0 * scale;
    g_physicsStep = (float)(stepMs * 0.001);
    SetPieceLimit((int)ceil(stepMs - 1e-6));
    SetTickStep(g_physicsStep);

    g_pace.carryMs += frames * stepMs;
    unsigned dt = (unsigned)g_pace.carryMs;
    g_pace.carryMs -= dt;
    g_worldMs = std::max(1, (int)lround((double)dt / frames));
    g_pace.carryReal += frames * g_pace.step * 1000.0;
    unsigned realMs = (unsigned)g_pace.carryReal;
    g_pace.carryReal -= realMs;
    Global(fo2::kSimTotalMs) += realMs;
    Global(fo2::kLastTimerMs) = realMs;
    Global(fo2::kFrameCounter)++;
    Global(fo2::kSimAccumMs) = 0;
    Field(gf, fo2::kGfFrameDt) = (int)dt;
    Field(gf, fo2::kGfFrameDtReal) = (int)realMs;
    return 1;
}

// ---- probes: does the world move evenly from shown frame to shown frame? -----------------------
const BYTE* PlayerCar()
{
    const BYTE* player = *(const BYTE**)fo2::kLocalPlayer;
    return player ? *(const BYTE* const*)(player + fo2::kPlayerCar) : nullptr;
}

const float* CarPosition(const BYTE* car) { return (const float*)(car + fo2::kCarMatrix) + 12; }

struct Probe
{
    bool have = false;
    float pos[3] = {}, vel[3] = {};
    int steps = 0;
    int frames = 0;
    double sum = 0, minRatio = 1e9, maxRatio = 0, minStep = 1e9, maxStep = 0;
    double lastLog = 0;
} g_probe;

// Called before the gate: compares how far the car moved since the last call with its velocity
// times the physics time simulated since then.
void ProbeFrame(BYTE* gf)
{
    Probe& p = g_probe;
    const BYTE* car = PlayerCar();
    if (!car || !InRace(gf))
    {
        p.have = false;
        return;
    }
    if (p.have && g_physicsSteps == p.steps) return;  // nothing simulated since the last call
    const float* pos = CarPosition(car);
    const float* vel = (const float*)(car + fo2::kCarVelocity);
    double physics = (g_physicsSteps - p.steps) * (double)g_physicsStep;
    if (p.have)
    {
        float d[3], v[3];
        for (int i = 0; i < 3; i++)
        {
            d[i] = pos[i] - p.pos[i];
            v[i] = 0.5f * (vel[i] + p.vel[i]);
        }
        double moved = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        double expected = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * physics;
        if (expected > 0.02)
        {
            double ratio = moved / expected;
            p.frames++;
            p.sum += ratio;
            p.minRatio = std::min(p.minRatio, ratio);
            p.maxRatio = std::max(p.maxRatio, ratio);
            p.minStep = std::min(p.minStep, physics);
            p.maxStep = std::max(p.maxStep, physics);
        }
    }
    p.have = true;
    p.steps = g_physicsSteps;
    for (int i = 0; i < 3; i++)
    {
        p.pos[i] = pos[i];
        p.vel[i] = vel[i];
    }

    double now = NowSeconds();
    if (now - p.lastLog >= 2.0)
    {
        if (p.frames > 0)
            Log("step probe: %d moving frames, physics %.2f..%.2f ms per frame, moved / (velocity x physics time) mean %.3f, min %.3f, max %.3f",
                p.frames, 1000.0 * p.minStep, 1000.0 * p.maxStep, p.sum / p.frames, p.minRatio, p.maxRatio);
        p.frames = 0;
        p.sum = 0;
        p.minRatio = p.minStep = 1e9;
        p.maxRatio = p.maxStep = 0;
        p.lastLog = now;
    }
}

// Second differences of the camera's and the car's positions between rendered frames: how much
// the motion changes from one frame to the next. Even motion gives a fraction of a millimetre;
// a frame that moves further or less than its neighbours shows up as centimetres.
struct MotionProbe
{
    int have = 0, steps = 0;
    float cam[2][3] = {}, car[2][3] = {};
    int frames = 0;
    double cam2 = 0, car2 = 0, camMax = 0;
    double lastLog = 0;
} g_motion;

}  // namespace

void ProbeCamera(const float* camMatrix)
{
    MotionProbe& m = g_motion;
    const BYTE* car = PlayerCar();
    if (!g_cfg.stepProbe || !car) return;
    if (m.have && g_physicsSteps == m.steps) return;  // paused: nothing moved
    if (m.have && g_physicsSteps - m.steps > 64) m.have = 0;
    m.steps = g_physicsSteps;
    const float* cam = camMatrix + 12;
    const float* pos = CarPosition(car);
    if (m.have == 2)
    {
        double dc = 0, dp = 0;
        for (int i = 0; i < 3; i++)
        {
            double c = cam[i] - 2.0 * m.cam[1][i] + m.cam[0][i];
            double p = pos[i] - 2.0 * m.car[1][i] + m.car[0][i];
            dc += c * c;
            dp += p * p;
        }
        m.frames++;
        m.cam2 += dc;
        m.car2 += dp;
        m.camMax = std::max(m.camMax, sqrt(dc));
    }
    for (int i = 0; i < 3; i++)
    {
        m.cam[0][i] = m.cam[1][i];
        m.car[0][i] = m.car[1][i];
        m.cam[1][i] = cam[i];
        m.car[1][i] = pos[i];
    }
    m.have = std::min(m.have + 1, 2);

    double now = NowSeconds();
    if (now - m.lastLog >= 2.0)
    {
        if (m.frames > 0)
            Log("motion probe: %d frames, change of motion from frame to frame (rms): camera %.2f mm (max %.2f), car %.2f mm", m.frames,
                1000.0 * sqrt(m.cam2 / m.frames), 1000.0 * m.camMax, 1000.0 * sqrt(m.car2 / m.frames));
        m.frames = 0;
        m.cam2 = m.car2 = m.camMax = 0;
        m.lastLog = now;
    }
}

namespace {

// Test without the headset: a pretend headset at TestPaceHz, which skips the frames the game is
// too late for, like a real one. The real headset gets no frames in races meanwhile (an idle
// SteamVR headset would hold up every frame by its own timing).
double g_testClock;
bool TestPaceWait(double* displayTime, double* period)
{
    if (g_cfg.testPaceHz <= 0) return false;
    *period = 1.0 / g_cfg.testPaceHz;
    double now = NowSeconds();
    if (g_testClock == 0 || now - g_testClock > 0.25) g_testClock = now;
    g_testClock += *period;
    if (now > g_testClock) g_testClock += ceil((now - g_testClock) / *period) * *period;
    while (NowSeconds() < g_testClock)
    {
        if (g_testClock - NowSeconds() > 0.002) Sleep(1);
    }
    *displayTime = g_testClock;
    if (g_cfg.testPaceJitter)
    {
        // SteamVR's predicted times: a couple of milliseconds of wander, and one frame in 16
        // reported a whole frame late (the headset still shows it in time).
        static unsigned seed = 1;
        seed = seed * 1103515245u + 12345u;
        double r = ((seed >> 8) & 0xFFFF) / 65535.0;
        *displayTime += (r - 0.5) * 0.0032;
        if ((seed >> 24) % 16 == 0) *displayTime += *period;
    }
    return true;
}

}  // namespace

static int __cdecl FrameGate(BYTE* gameFlow)
{
    if (g_cfg.stepProbe) ProbeFrame(gameFlow);
    if (!InRace(gameFlow)) rec::NotInRace();
    int r;
    bool vrRace = InRace(gameFlow) && g_cfg.vrPhysicsAtHeadsetRate && g_pieceHooked;
    double t0 = NowSeconds();
    double displayTime = 0, period = 0;
    bool timed = false;
    bool testPace = g_cfg.testPaceHz > 0;
    if (vrRace && !testPace && xr::WaitFrame() && xr::ShouldRender())
    {
        displayTime = xr::DisplayTime();
        period = xr::DisplayPeriod();
        timed = period > 0.004 && period < 0.05;
    }
    if (vrRace && !timed) timed = TestPaceWait(&displayTime, &period);
    if (timed)
    {
        if (!g_pace.active)
        {
            g_pace = Pacing();
            g_pace.active = true;
            g_clock = g_wallClock = FrameClock();
            Log("pacing: one physics step per headset frame (headset reports %.2f ms, %.1f Hz)", period * 1000.0, 1.0 / period);
        }
        double now = NowSeconds();
        g_pace.wait += now - t0;
        g_pace.frames++;
        r = HeadsetGate(gameFlow, displayTime, period);
        if (g_pace.lastLog == 0) g_pace.lastLog = now;
        if (now - g_pace.lastLog >= g_cfg.logIntervalSeconds)
        {
            double span = now - g_pace.lastLog;
            Log("pacing: %.1f fps, step %.3f ms (%.2f Hz, %s), period from the frame starts %.3f ms, %d headset frames missed, "
                "waited %.2f ms/frame | runtime's period %.2f..%.2f ms, off the frame clocks: display times %+.2f..%+.2f ms, "
                "frame starts %+.2f..%+.2f ms",
                g_pace.frames / span, 1000.0 * g_pace.step, 1.0 / g_pace.step, g_pace.runtimePeriod ? "runtime's" : "frame starts",
                1000.0 * g_period.value, g_pace.missed,
                1000.0 * g_pace.wait / g_pace.frames,
                1000.0 * g_pace.minReported, 1000.0 * g_pace.maxReported, 1000.0 * g_clock.minOff, 1000.0 * g_clock.maxOff,
                1000.0 * g_wallClock.minOff, 1000.0 * g_wallClock.maxOff);
            g_pace.frames = g_pace.missed = 0;
            g_pace.wait = 0;
            g_pace.minReported = g_clock.minOff = g_wallClock.minOff = 1e9;
            g_pace.maxReported = g_clock.maxOff = g_wallClock.maxOff = -1e9;
            g_pace.lastLog = now;
        }
    }
    else
    {
        StopPacing();
        r = CallGameGate(gameFlow);
    }
    if (r) g_frameStart = NowSeconds();
    return r;
}

double FrameStartTime() { return g_frameStart; }

int PhysicsSteps() { return g_physicsSteps; }

namespace {

// Particles made in bulk (see game.h) come at the rate the game was tuned for: so many per physics
// step (100 steps a second) or per frame (60 frames). At the headset's 120 there are more of them,
// and the stunt stadium's emitters, which all but fill the main particle list of 8192 at the game's
// own pace, keep it full: every new particle is then dropped, whichever effect makes it, and the
// stunt cars' jet flames, exhaust, nitro flames and sparks go missing. The effects still run as
// before (they draw on the game's shared random numbers, which races and replays depend on), but
// only the share of their particles the game's own rate would make is added (a step's time over
// 10 ms, a frame's real time over 1/60 s), and none of them into the last sixteenth of a list, which
// is kept for the other effects.
using AddParticleFn = void(__cdecl*)(const void* particle, const BYTE* views);

template <int Call>
void __cdecl AddBulkParticle(const void* particle, const BYTE* views)
{
    const auto& add = fo2::kBulkParticleAdds[Call];
    static float share;
    if (add.every == fo2::Every::Step)
        share += std::min(g_physicsStep * 100.0f, 1.0f);
    else
    {
        BYTE* gf = *(BYTE**)fo2::kGameFlow;
        share += gf ? std::clamp(Field(gf, fo2::kGfFrameDtReal) * 0.06f, 0.0f, 1.0f) : 1.0f;
    }
    if (share < 1.0f) return;
    share -= 1.0f;
    BYTE room[8];
    unsigned n = *(const unsigned*)fo2::kParticleViewCount;
    if (n > std::size(room))
    {
        ((AddParticleFn)add.target)(particle, views);
        return;
    }
    for (unsigned v = 0; v < n; v++)
    {
        const int* list = (const int*)(fo2::kParticleViews + v * fo2::kParticleViewSize + add.countOffset);  // count, capacity
        room[v] = views[v] && list[0] < list[1] - list[1] / 16;
    }
    ((AddParticleFn)add.target)(particle, room);
}

template <int... Calls>
bool PatchBulkParticles(std::integer_sequence<int, Calls...>)
{
    return (PatchCall(fo2::kBulkParticleAdds[Calls].site, fo2::kBulkParticleAdds[Calls].target, (const void*)&AddBulkParticle<Calls>) & ...);
}

}  // namespace

bool InstallPacingHooks()
{
    bool ok = PatchCall(fo2::kFrameGateCall, fo2::kFrameGate, (const void*)&FrameGateHook);
    if (!PatchBulkParticles(std::make_integer_sequence<int, (int)std::size(fo2::kBulkParticleAdds)>()))
        Log("particles: the code of an effect differs - its particles are not limited");
    // Everything the physics step depends on is checked before anything is changed: the world
    // step's "push 0.01" (becomes a call of PushPhysicsStep, same length), the piece limit's two
    // immediates, the race tick's three step immediates and the cars' step load.
    const uint8_t pushStep[5] = { 0x68, 0x0A, 0xD7, 0x23, 0x3C };
    const uint8_t pieceCmp[3] = { 0x83, 0xF8, 0x0A };
    const uint8_t pieceMov[5] = { 0xB8, 0x0A, 0x00, 0x00, 0x00 };
    const uint8_t tickMs[10] = { 0xC7, 0x85, 0xCC, 0x07, 0x02, 0x00, 0x0A, 0x00, 0x00, 0x00 };
    const uint8_t tickStep[10] = { 0xC7, 0x85, 0xC4, 0x07, 0x02, 0x00, 0x0B, 0xD7, 0x23, 0x3C };
    const uint8_t tickInv[10] = { 0xC7, 0x85, 0xC8, 0x07, 0x02, 0x00, 0xFF, 0xFF, 0xC7, 0x42 };
    const uint8_t carStep[10] = { 0xDB, 0x44, 0x24, 0x50, 0xD9, 0x05, 0xD8, 0xDB, 0x67, 0x00 };
    auto same = [](uintptr_t a, const uint8_t* b, size_t n) { return memcmp((const void*)a, b, n) == 0; };
    if (same(fo2::kPushPhysicsStep, pushStep, 5) && same(fo2::kRacePieceCmp, pieceCmp, 3) && same(fo2::kRacePieceMov, pieceMov, 5) &&
        same(fo2::kTickSetStepMs, tickMs, 10) && same(fo2::kTickSetStep, tickStep, 10) && same(fo2::kTickSetStepInv, tickInv, 10) &&
        same(fo2::kCarStepLoad, carStep, 10))
    {
        g_worldMsSwap.Prepare(fo2::kTickSetStepMs, tickMs, (const void*)&SetWorldMs);
        g_carStepSwap.Prepare(fo2::kCarStepLoad, carStep, (const void*)&LoadCarStepMs);
        uint8_t call[5] = { 0xE8 };
        int32_t rel = (int32_t)((uintptr_t)&PushPhysicsStep - (fo2::kPushPhysicsStep + 5));
        memcpy(call + 1, &rel, 4);
        g_pieceHooked = g_tickHooked = WriteCode(fo2::kPushPhysicsStep, call, 5);
    }
    if (!g_pieceHooked) Log("pacing: race simulation code differs - physics stays at 10 ms steps");
    return ok;
}
