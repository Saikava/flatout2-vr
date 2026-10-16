#pragma once
// Replaces the game loop's frame gate (see game.h, "game loop timing"). False if the exe does not
// match FlatOut 2 v1.2.
bool InstallPacingHooks();

// [Test] StepProbe: called with the race camera's matrix once per rendered frame; logs how evenly
// the camera and the player's car move from frame to frame.
void ProbeCamera(const float* camMatrix);

// When the gate last let a frame run (NowSeconds), for timing the game's own work.
double FrameStartTime();
