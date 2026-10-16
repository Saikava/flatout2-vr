#pragma once
#include "xr.h"
// Input recording for a repeatable benchmark.
//
// [Test] Record=1: from the start of the game to the end of the next race the input goes to
// scripts\fo2vr_rec.bin, and the save folder is copied to scripts\fo2vr_rec_save\. Up to the race's
// countdown (intro videos, menus, loading) the key and game controller changes are written with a
// clock that skips stalls, and every change of game state is a sync point; from the countdown on,
// each frame's input with its number of physics steps, the game's random generator at the start
// and every 120 frames the player's car position.
//
// [Test] Replay=1 (best with Background=1 and PaceHz=120): the copied save is put back (the
// player's own saves are kept aside in scripts\fo2vr_user_save\ and return afterwards), the menus
// are played back by the clock (waiting at the sync points until the game gets there), the race
// frame by frame; the log tells how far the car strays from the recorded path.
//
// In the headset each race frame also records the eyes' poses and fields of view, and a replay
// renders its eyes with them: a benchmark sees what the player looked at.
namespace rec {

// At startup, before the game reads its saves.
void Install();
bool Replaying();

// Every Present: the menu part.
void MenuTick();
// Each race frame at the gate, with the headset frames it simulates and the physics step (seconds):
// recorded, or set by a replay. True on the first frame of the race part.
bool RaceFrame(int* frames, double* step);
// The gate outside a race: a recorded race is over.
void NotInRace();
// The view this race frame's eyes are rendered with: recorded, or (replay) replaced by the recorded
// one. False when there is none to replay (a recording made without the headset).
void RecordView(const xr::EyePose* poses, const xr::EyeFov* fovs);
bool ReplayView(xr::EyePose* poses, xr::EyeFov* fovs);
// A game controller's state as the game reads it (device: its index among the game controllers):
// recorded, or replaced by the recorded state. True when replaced.
bool ControllerState(int device, unsigned size, void* data);

}  // namespace rec
