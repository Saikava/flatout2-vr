#pragma once
struct IDirect3DDevice9;

// Hooks RenderViews and the engine's SetViewport / SetCamera / SetFrustum. False if the exe
// does not match FlatOut 2 v1.2.
bool InstallRenderHooks();

// The game created its Direct3D device: hook it and start VR.
void OnDeviceCreated(IDirect3DDevice9* dev);
