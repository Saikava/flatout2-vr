// Standalone 32-bit check of the active OpenXR runtime: creates a session with the given graphics
// API (d3d11 or d3d12) and submits a few frames. Usage: xrtest.exe d3d11|d3d12 [frames]
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static XrInstance g_inst;

static bool Ok(XrResult r, const char* what)
{
    char s[XR_MAX_RESULT_STRING_SIZE] = "";
    if (g_inst) xrResultToString(g_inst, r, s);
    if (!*what && XR_SUCCEEDED(r)) return true;
    printf("%-36s %d %s\n", what, (int)r, s);
    fflush(stdout);
    return XR_SUCCEEDED(r);
}

static int Run(bool d3d12, int frames)
{
    const char* ext = d3d12 ? XR_KHR_D3D12_ENABLE_EXTENSION_NAME : XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "fo2vr xrtest");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &ext;
    if (!Ok(xrCreateInstance(&ci, &g_inst), "xrCreateInstance")) return 1;
    XrSystemGetInfo si = { XR_TYPE_SYSTEM_GET_INFO };
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    XrResult sr = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    for (int i = 0; i < 60 && sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE; i++)  // SteamVR needs a while to start
    {
        sr = xrGetSystem(g_inst, &si, &sys);
        if (sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) Sleep(500);
    }
    if (!Ok(sr, "xrGetSystem")) return 1;

    LUID luid;
    D3D_FEATURE_LEVEL fl;
    if (d3d12)
    {
        PFN_xrGetD3D12GraphicsRequirementsKHR f;
        xrGetInstanceProcAddr(g_inst, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&f);
        XrGraphicsRequirementsD3D12KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
        if (!Ok(f(g_inst, sys, &req), "xrGetD3D12GraphicsRequirementsKHR")) return 1;
        luid = req.adapterLuid;
        fl = req.minFeatureLevel;
    }
    else
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR f;
        xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&f);
        XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        if (!Ok(f(g_inst, sys, &req), "xrGetD3D11GraphicsRequirementsKHR")) return 1;
        luid = req.adapterLuid;
        fl = req.minFeatureLevel;
    }
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));

    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx11 = nullptr;
    ID3D12Device* dev12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    XrGraphicsBindingD3D11KHR b11 = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    XrGraphicsBindingD3D12KHR b12 = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    if (d3d12)
    {
        HRESULT hr = D3D12CreateDevice(adapter, fl, IID_PPV_ARGS(&dev12));
        printf("D3D12CreateDevice %08X\n", (unsigned)hr);
        D3D12_COMMAND_QUEUE_DESC qd = {};
        dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        b12.device = dev12;
        b12.queue = queue;
    }
    else
    {
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev11, nullptr, &ctx11);
        printf("D3D11CreateDevice %08X\n", (unsigned)hr);
        b11.device = dev11;
    }
    fflush(stdout);

    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = d3d12 ? (const void*)&b12 : (const void*)&b11;
    sci.systemId = sys;
    XrSession session;
    printf("calling xrCreateSession...\n");
    fflush(stdout);
    if (!Ok(xrCreateSession(g_inst, &sci, &session), "xrCreateSession")) return 1;
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    XrSpace space;
    Ok(xrCreateReferenceSpace(session, &rs, &space), "xrCreateReferenceSpace");

    XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    sc.sampleCount = 1;
    sc.width = 1024;
    sc.height = 1024;
    sc.faceCount = sc.arraySize = sc.mipCount = 1;
    XrSwapchain chain;
    if (!Ok(xrCreateSwapchain(session, &sc, &chain), "xrCreateSwapchain")) return 1;

    bool running = false;
    int submitted = 0;
    for (int i = 0; i < 2000 && submitted < frames; i++)
    {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(g_inst, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                auto st = ((XrEventDataSessionStateChanged*)&ev)->state;
                printf("session state %d\n", (int)st);
                if (st == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = Ok(xrBeginSession(session, &bi), "xrBeginSession");
                }
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (!running)
        {
            Sleep(10);
            continue;
        }
        XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        if (!Ok(xrWaitFrame(session, &wi, &fs), submitted ? "" : "xrWaitFrame")) return 1;
        XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
        xrBeginFrame(session, &bi);
        uint32_t idx;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        xrAcquireSwapchainImage(chain, &ai, &idx);
        XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        swi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(chain, &swi);
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(chain, &ri);
        XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = space;
        quad.subImage.swapchain = chain;
        quad.subImage.imageRect = { { 0, 0 }, { 1024, 1024 } };
        quad.pose.orientation.w = 1;
        quad.pose.position.z = -2;
        quad.size = { 1, 1 };
        const XrCompositionLayerBaseHeader* layers[] = { (XrCompositionLayerBaseHeader*)&quad };
        XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = 1;
        ei.layers = layers;
        if (!Ok(xrEndFrame(session, &ei), submitted ? "" : "xrEndFrame")) return 1;
        submitted++;
    }
    printf("submitted %d frames\n", submitted);
    xrDestroySession(session);
    xrDestroyInstance(g_inst);
    printf("OK\n");
    return 0;
}

int main(int argc, char** argv)
{
    bool d3d12 = argc > 1 && strcmp(argv[1], "d3d12") == 0;
    int frames = argc > 2 ? atoi(argv[2]) : 90;
    printf("== %s, %d frames\n", d3d12 ? "D3D12" : "D3D11", frames);
    __try
    {
        return Run(d3d12, frames);
    }
    __except (printf("CRASH: exception %08X\n", GetExceptionCode()), fflush(stdout), EXCEPTION_EXECUTE_HANDLER)
    {
        return 2;
    }
}
