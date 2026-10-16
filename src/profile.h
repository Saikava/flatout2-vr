#pragma once
// Frame profiler for races: times the device calls that can stall a frame (draws, creating
// resources and shaders, locking buffers and textures, uploads), by phase of the frame. A slow
// frame gets its calls logged.
struct IDirect3DDevice9;

namespace prof {

enum Phase
{
    Game,  // from the frame gate to RenderViews
    Eyes,  // RenderViews (the eyes and the monitor picture)
    Hud,   // after RenderViews: the game's HUD, then Present
    PhaseCount
};

// Hooks the device's vtable and the vtables of its buffers and textures (once per device).
// perCall: also every draw call and buffer or texture lock (each timed: some 0.1 ms per thousand
// draws, so only for tests); creates and uploads always.
void Hook(IDirect3DDevice9* dev, bool perCall);
void SetPhase(Phase p);
// End of a race frame, with the phases' times in seconds: logs the frame's calls if it was slow.
void EndFrame(double game, double eyes, double hud);

// Times the game's particle rendering (0x58EFF0, once per view and eye) through its call in
// RenderViews. False if the code differs.
bool HookParticles();

// [Test] Sampling: a thread looks about once a millisecond where the calling (game) thread is
// running; a slow frame's log tells in which code its time went.
void StartSampling();

// Totals of the race frames since ResetTotals (a replay's race part): averages, the slowest frame,
// the frames over the headset's budget.
void ResetTotals();
void LogTotals(const char* what);

// While on: every draw call is logged with the game code that led to it (return addresses found
// on the stack), its size and the textures and shaders bound.
void TraceDraws(bool on);

}  // namespace prof
