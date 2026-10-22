#pragma once
// The game's 32-bit address space ([VR] LargeAddressAware): FlatOut2.exe makes the game use 4 GB of it instead of 2,
// marked by the mod itself, and the log follows how much of it is used.

// First thing in Init: a game the mod started anew (EnsureLargeAddressAware) waits for the old one to end.
void WaitForOldGame();
// As VR starts (Direct3DCreate9, on the game's thread): marks FlatOut2.exe large address aware if it is not, and starts
// the game anew with it (this process ends). Says so only when the file cannot be marked.
void EnsureLargeAddressAware();
// Once a log interval: logs the address space in use when it has grown by 128 MB since it was last logged.
void NoteAddressSpace();
