#pragma once
// Work the two eyes share. The eyes look the same way from 6 cm apart, so some of what the game
// builds on the CPU for each view comes out the same for both: the trees' leaf cards (turned to the
// camera's direction, not its position) and the grass cards (faded by the distance along the view
// direction). The left eye keeps what it builds; the right eye copies it into its own vertex
// buffers instead of building it again.
namespace share {

// Patches the game's code (once, at startup). False if the code differs.
bool Install();
// Around each eye's RenderViews: eye 0 keeps what it builds, eye 1 reuses it. EndEyes: neither
// (the monitor's own view, menus).
void BeginEye(int eye);
void EndEyes();

// Since the last call: what the right eye copied, and what it built itself because the left eye
// had not built it (or not from the same direction): leaf chunks of up to 64 trees, grass batches.
struct Stats
{
    int reused = 0, rebuilt = 0;
    int grassCopied = 0, grassBuilt = 0;  // grass batches
};
Stats TakeStats();

}  // namespace share
