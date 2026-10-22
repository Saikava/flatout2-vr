#pragma once
// Windows' task dialogs, for what the player must know or choose at the game's start (the VR runtime not starting,
// the game's memory). They come from comctl32 6, which the mod's manifest asks for (the game's own is 5).
#include <windows.h>
#include <commctrl.h>

// Shows the dialog on a thread of its own while the calling thread waits (the game's window is not pumped meanwhile,
// as while it loads: its window procedure never runs before the game expects it). Returns what TaskDialogIndirect
// returned, or an error when comctl32 6 could not be had (logged).
HRESULT ShowTaskDialog(const TASKDIALOGCONFIG& config, int* button, BOOL* checked = nullptr);
