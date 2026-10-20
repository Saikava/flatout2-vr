#pragma once
struct IDirect3DDevice9;
struct IDirect3DSurface9;

// Copies a render target (multisampled or not) to system memory and writes it as a PNG on a
// worker thread, without waiting for the GPU (see ScreenshotTick). Must be called on the render
// thread. keepAlpha: the PNG keeps the surface's alpha channel (otherwise opaque).
void SaveSurfacePng(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const wchar_t* path, bool keepAlpha = false);
void SaveBackBufferPng(IDirect3DDevice9* dev, const wchar_t* path);
// Writes pixels (32-bit BGRA, w x h) as a PNG on a worker thread.
void SavePixelsPng(const wchar_t* path, const unsigned* bgra, unsigned w, unsigned h);
// Once a frame on the render thread: moves the screenshots on (their copies are read a few
// frames after they were taken, when the GPU is done with them).
void ScreenshotTick();
// Makes the system memory copies for `count` screenshots of a surface this size ahead of time
// (with the render targets, not when a screenshot is taken).
void PrepareScreenshots(IDirect3DDevice9* dev, unsigned w, unsigned h, int format, int count);
