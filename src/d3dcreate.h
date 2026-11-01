#pragma once

// Redirects FlatOut2.exe's calls to Direct3DCreate9: starts OpenXR before Direct3D exists and
// hooks IDirect3D9::CreateDevice (no vsync in VR, VR session once the device exists).
bool InstallCreateHook();

// Earlier versions of the mod put DXVK into the game folder as d3d9.dll; now it is scripts\dxvk_d3d9.dll (see
// Direct3DModule in d3dcreate.cpp). Updated over an earlier version, the mod puts the old one out of the way itself: the
// running game has it loaded, which a rename does not disturb (Windows lets a loaded file be renamed, not deleted), and
// the next start deletes it. Only DXVK, and only with scripts\dxvk_d3d9.dll there. A Steam release that the old one hung
// never gets this far: README's Troubleshooting says to delete it.
void PutAwayOldDxvk();
