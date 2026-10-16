#pragma once

// Redirects FlatOut2.exe's calls to Direct3DCreate9: starts OpenXR before Direct3D exists and
// hooks IDirect3D9::CreateDevice (no vsync in VR, VR session once the device exists).
bool InstallCreateHook();
