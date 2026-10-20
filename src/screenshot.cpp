#include "screenshot.h"

#include "util.h"

#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static void WritePng(std::wstring path, std::vector<BYTE> pixels, UINT w, UINT h)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    // The PNG encoder takes 32bpp BGRA as is (it would silently switch 32bpp BGR to 24bpp).
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(h, w * 4, (UINT)pixels.size(), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    Log("screenshot %s: %ls", SUCCEEDED(hr) ? "saved" : "FAILED", path.c_str());
}

// A screenshot on its way. GetRenderTargetData only queues the GPU's copy into system memory, and
// locking the copy right away would wait for the GPU to finish the whole frame: the copy is locked
// a few frames later, a worker thread takes the pixels from it (and writes the PNG), and it is
// unlocked once the worker has them. So taking a screenshot does not hold up the headset's frames.
struct PendingShot
{
    IDirect3DSurface9* sys = nullptr;
    std::wstring path;
    bool keepAlpha = false;
    UINT w = 0, h = 0;
    int wait = 3;  // frames before it is locked
    bool locked = false;
    std::shared_ptr<std::atomic<bool>> copied = std::make_shared<std::atomic<bool>>(false);
};
static std::vector<PendingShot> g_shots;

// System memory copies kept for the next screenshots: making one the size of an eye takes some
// 11 ms (DXVK allocates its staging memory), too long for a headset frame.
static std::vector<IDirect3DSurface9*> g_copies;

static bool Matches(IDirect3DSurface9* s, UINT w, UINT h, D3DFORMAT fmt)
{
    D3DSURFACE_DESC d;
    s->GetDesc(&d);
    return d.Width == w && d.Height == h && d.Format == fmt;
}

static IDirect3DSurface9* TakeCopy(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt)
{
    for (size_t i = 0; i < g_copies.size(); i++)
        if (Matches(g_copies[i], w, h, fmt))
        {
            IDirect3DSurface9* s = g_copies[i];
            g_copies.erase(g_copies.begin() + i);
            return s;
        }
    IDirect3DSurface9* s = nullptr;
    return SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &s, nullptr)) ? s : nullptr;
}

void PrepareScreenshots(IDirect3DDevice9* dev, unsigned w, unsigned h, int format, int count)
{
    D3DFORMAT fmt = (D3DFORMAT)format;
    int have = 0;
    for (IDirect3DSurface9* s : g_copies) have += Matches(s, w, h, fmt) ? 1 : 0;
    for (; have < count; have++)
    {
        IDirect3DSurface9* s = nullptr;
        if (FAILED(dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &s, nullptr))) return;
        g_copies.push_back(s);
    }
}

static void CopyAndWrite(const BYTE* bits, int pitch, UINT w, UINT h, bool keepAlpha, std::wstring path, std::shared_ptr<std::atomic<bool>> copied)
{
    std::vector<BYTE> px((size_t)w * h * 4);
    for (UINT y = 0; y < h; y++)
    {
        BYTE* row = &px[(size_t)y * w * 4];
        memcpy(row, bits + (size_t)y * pitch, w * 4);
        if (!keepAlpha)
            for (UINT x = 0; x < w; x++) row[4 * x + 3] = 0xFF;
    }
    copied->store(true);
    WritePng(std::move(path), std::move(px), w, h);
}

void SaveSurfacePng(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const wchar_t* path, bool keepAlpha)
{
    IDirect3DSurface9* resolved = nullptr;
    D3DSURFACE_DESC d;
    surface->GetDesc(&d);
    IDirect3DSurface9* src = surface;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE)
    {
        if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
            SUCCEEDED(dev->StretchRect(surface, nullptr, resolved, nullptr, D3DTEXF_NONE)))
            src = resolved;
    }
    PendingShot shot;
    bool ok = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) && (shot.sys = TakeCopy(dev, d.Width, d.Height, d.Format)) != nullptr &&
              SUCCEEDED(dev->GetRenderTargetData(src, shot.sys));
    if (resolved) resolved->Release();
    if (!ok)
    {
        if (shot.sys) shot.sys->Release();
        Log("screenshot failed (format %d, %ux%u, msaa %d)", (int)d.Format, d.Width, d.Height, (int)d.MultiSampleType);
        return;
    }
    shot.path = path;
    shot.keepAlpha = keepAlpha;
    shot.w = d.Width;
    shot.h = d.Height;
    g_shots.push_back(std::move(shot));
}

void ScreenshotTick()
{
    bool lockedOne = false;
    for (size_t i = 0; i < g_shots.size();)
    {
        PendingShot& s = g_shots[i];
        if (!s.locked)
        {
            if (s.wait > 0 || lockedOne)
            {
                s.wait--;
                i++;
                continue;
            }
            lockedOne = true;  // one a frame
            D3DLOCKED_RECT lr;
            if (FAILED(s.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            {
                Log("screenshot failed: %ls", s.path.c_str());
                s.sys->Release();
                g_shots.erase(g_shots.begin() + i);
                continue;
            }
            s.locked = true;
            std::thread(CopyAndWrite, (const BYTE*)lr.pBits, (int)lr.Pitch, s.w, s.h, s.keepAlpha, s.path, s.copied).detach();
            i++;
            continue;
        }
        if (s.copied->load())
        {
            s.sys->UnlockRect();
            g_copies.push_back(s.sys);
            g_shots.erase(g_shots.begin() + i);
            continue;
        }
        i++;
    }
}

void SavePixelsPng(const wchar_t* path, const unsigned* bgra, unsigned w, unsigned h)
{
    std::vector<BYTE> px((const BYTE*)bgra, (const BYTE*)(bgra + (size_t)w * h));
    std::thread(WritePng, std::wstring(path), std::move(px), w, h).detach();
}

void SaveBackBufferPng(IDirect3DDevice9* dev, const wchar_t* path)
{
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    SaveSurfacePng(dev, bb, path);
    bb->Release();
}
