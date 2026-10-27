#include "xr.h"

#include "background.h"
#include "dialog.h"
#include "dxvk_interop.h"
#include "util.h"

#include <windows.h>
#include <d3d9.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace xr {
namespace {

// The OpenXR functions the mod calls, all through xrGetInstanceProcAddr: the loader's (scripts\openxr_loader.dll), or
// the runtime's own when the loader would not take Runtime= (LoadEntryPoints).
#define FO2VR_XR_FUNCTIONS(X)                                                                                              \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) X(xrGetSystem) X(xrGetSystemProperties)            \
    X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrBeginSession) X(xrEndSession) X(xrPollEvent)               \
    X(xrCreateReferenceSpace) X(xrCreateActionSpace) X(xrLocateSpace) X(xrLocateViews) X(xrEnumerateSwapchainFormats)      \
    X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage)                    \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrStringToPath)      \
    X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets)             \
    X(xrSyncActions) X(xrGetActionStateFloat) X(xrGetActionStateBoolean) X(xrGetActionStateVector2f)                       \
    X(xrApplyHapticFeedback) X(xrStopHapticFeedback)
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr;
PFN_xrCreateInstance xrCreateInstance;
#define FO2VR_XR_DECLARE(f) PFN_##f f;
FO2VR_XR_FUNCTIONS(FO2VR_XR_DECLARE)

// An instance's functions (once it is made).
bool LoadFunctions(XrInstance instance)
{
    bool ok = true;
#define FO2VR_XR_LOAD(f) ok &= (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, #f, (PFN_xrVoidFunction*)&f)) && f != nullptr);
    FO2VR_XR_FUNCTIONS(FO2VR_XR_LOAD)
    return ok;
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
bool g_running;
XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;
bool g_testUnfocused;  // [Test] FakeHeadset: the focus taken away (SetTestFocus)

// DXVK's Vulkan objects.
ID3D9VkInteropDevice* g_interop;
VkInstance g_vkInstance;
VkPhysicalDevice g_vkPhys;
VkDevice g_vkDevice;
VkQueue g_queue;
uint32_t g_queueFamily, g_queueIndex;

struct VkFns
{
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkGetFenceStatus GetFenceStatus;
} vk;

struct Slot
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};
Slot g_slots[3];
int g_slot;
VkCommandPool g_pool;

struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t w = 0, h = 0;
    bool released = false;  // an image was released: a layer can show it again without a new one
};
Swapchain g_eyeSc[2], g_screenSc, g_hudSc;
int64_t g_format;
uint32_t g_eyeW, g_eyeH;
uint32_t g_eyeOffset[2][2];  // where the headset's picture begins in each eye's texture (SetEyeOffsets)

XrFrameState g_frameState;
bool g_frameOpen;
XrView g_views[2];
bool g_viewsValid;

// Recenter origin (OpenXR LOCAL space): position and rotation.
bool g_recenterPending = true;
float g_originPos[3];
XrQuaternionf g_originRot = { 0, 0, 0, 1 };  // a turn about the vertical (see ApplyRecenter)

bool g_loggedImage;
SubmitTimes g_times;

// The controllers' actions (CreateInput) and their state for the open frame (ReadInput).
XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_aimAction, g_holdAction, g_triggerAction, g_gripAction, g_buttonAction, g_stickAction, g_lowerAction, g_menuAction, g_thumbAction, g_hapticAction;
XrPath g_handPath[2];
XrSpace g_aimSpace[2], g_holdSpace[2];
Controller g_hands[2], g_testHands[2];

// A Quest switches to tracking the hands with its cameras when the controllers are put down, and Virtual Desktop then
// makes Touch controllers of the hands (a pinch pulls the trigger): with the hands on the keyboard their triggers sat
// some way off their rest most of the time and took the throttle and the brake from W and S. A hand the headset's
// cameras track (XR_EXT_hand_tracking with XR_EXT_hand_tracking_data_source's unobstructed source only: not the hand
// joints a runtime makes up from a controller held) has its controller left out (ReadInput).
bool g_handExtensions;  // the runtime has both extensions (enabled at xrCreateInstance)
PFN_xrCreateHandTrackerEXT g_createHandTracker;
PFN_xrLocateHandJointsEXT g_locateHandJoints;
XrHandTrackerEXT g_handTracker[2];
bool g_handSeen[2];  // the hand is tracked by the cameras (its controller put down)

// The controllers' vibration (Haptics): what each hand plays, and when it was last sent to the runtime.
struct Vibration
{
    float amplitude = 0;
    double sent = -1;
};
Vibration g_vibration[2];

// No tracking (headset asleep, or no frame located yet): eyes straight ahead with a Quest 3-like
// field of view, for test renders.
void DefaultViews()
{
    for (int i = 0; i < 2; i++)
    {
        g_views[i] = { XR_TYPE_VIEW };
        g_views[i].pose.orientation.w = 1.0f;
        g_views[i].pose.position = { i == 0 ? -0.032f : 0.032f, 0.0f, 0.0f };
        g_views[i].fov = i == 0 ? XrFovf{ -0.94f, 0.70f, 0.77f, -0.96f } : XrFovf{ -0.70f, 0.94f, 0.77f, -0.96f };
    }
}

// The results a runtime's start may end with, by name (xrResultToString needs an instance).
const char* ResultName(XrResult r)
{
    switch (r)
    {
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
    case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
    case XR_ERROR_FILE_ACCESS_ERROR: return "XR_ERROR_FILE_ACCESS_ERROR";
    case XR_ERROR_FILE_CONTENTS_INVALID: return "XR_ERROR_FILE_CONTENTS_INVALID";
    case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
    default: return "";
    }
}

bool Check(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (g_instance)
        xrResultToString(g_instance, r, name);
    else
        strcpy(name, ResultName(r));
    Log("xr: %s failed: %d %s", what, (int)r, name);
    return false;
}

// The runtime may use DXVK's queue inside these calls; DXVK must not submit meanwhile. Not only in the frame's calls:
// twice the GPU stopped with an error through Virtual Desktop (nvlddmkm 13 "Graphics Exception", then 153: a reset)
// just as the HUD's swapchain was made, unlocked, while DXVK submitted a race's first frame (VDXR likely readies a new
// swapchain's images on that queue); earlier runs through it ended so now and then at a race's start.
struct QueueLock
{
    QueueLock() { g_interop->LockSubmissionQueue(); }
    ~QueueLock() { g_interop->ReleaseSubmissionQueue(); }
};

void Rotate(const XrQuaternionf& q, const float v[3], float out[3])
{
    float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
    float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
    float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

// Into the recenter origin's axes (its rotation undone), then OpenXR axes (right-handed, -z forward) to the game's
// (left-handed, +z forward).
void ToGame(const float v[3], float out[3])
{
    const XrQuaternionf undo = { -g_originRot.x, -g_originRot.y, -g_originRot.z, g_originRot.w };
    float w[3];
    Rotate(undo, v, w);
    out[0] = w[0];
    out[1] = w[1];
    out[2] = -w[2];
}

// A pose in the LOCAL space relative to the recenter origin, in game axes.
EyePose ToGamePose(const XrPosef& p)
{
    const float x[3] = { 1, 0, 0 }, y[3] = { 0, 1, 0 }, fwd[3] = { 0, 0, -1 };
    float r[3], u[3], f[3];
    Rotate(p.orientation, x, r);
    Rotate(p.orientation, y, u);
    Rotate(p.orientation, fwd, f);
    float pos[3] = { p.position.x - g_originPos[0], p.position.y - g_originPos[1], p.position.z - g_originPos[2] };
    EyePose out;
    ToGame(r, out.right);
    ToGame(u, out.up);
    ToGame(f, out.forward);
    ToGame(pos, out.position);
    return out;
}

// The head as it is now becomes the origin: where it is, and its turn about the vertical (the horizon stays level).
// A head upside down (its up more than 120 degrees off the vertical; a headset left on the desk is not) means the
// runtime's space is upside down: with SteamVR through Virtual Desktop (which gives SteamVR its headset as an Oculus
// one, SteamVR's "oculus" driver) the game came in upside down at every start, until the headset was recentered (the
// Meta button held: SteamVR then sets its zero pose anew). Its space was turned half a
// turn about its forward axis (the head's up -0.02 -0.95 0.32 before, 0.01 0.95 0.32 after), with the origin on the
// floor: that half turn is undone first, then the head's turn about the vertical, so its tilt stays its own.
void ApplyRecenter()
{
    const XrPosef& p0 = g_views[0].pose;
    const XrPosef& p1 = g_views[1].pose;
    g_originPos[0] = 0.5f * (p0.position.x + p1.position.x);
    g_originPos[1] = 0.5f * (p0.position.y + p1.position.y);
    g_originPos[2] = 0.5f * (p0.position.z + p1.position.z);
    const float fwd[3] = { 0, 0, -1 }, up[3] = { 0, 1, 0 };  // OpenXR forward, up
    float f[3], u[3];
    Rotate(p0.orientation, fwd, f);
    Rotate(p0.orientation, up, u);
    bool upright = u[1] >= -0.5f;
    // (upside down: the head's forward in the space turned back is -f.x, -f.y, f.z)
    float yaw = upright ? atan2f(-f[0], -f[2]) : atan2f(f[0], -f[2]);
    float s = sinf(0.5f * yaw), c = cosf(0.5f * yaw);
    g_originRot = upright ? XrQuaternionf{ 0, s, 0, c } : XrQuaternionf{ -s, 0, c, 0 };  // (the half turn about z after the yaw)
    Log("xr: recentered at %.2f %.2f %.2f, yaw %.1f deg (the head's up %.2f %.2f %.2f%s)", g_originPos[0], g_originPos[1], g_originPos[2],
        yaw * 57.29578f, u[0], u[1], u[2], upright ? "" : ": the runtime's space is upside down - turned back");
}

template <class T>
bool LoadDeviceFn(T& fn, const char* name)
{
    fn = (T)vk.GetDeviceProcAddr(g_vkDevice, name);
    if (!fn) Log("xr: Vulkan function %s missing", name);
    return fn != nullptr;
}

bool LoadVulkan()
{
    HMODULE lib = GetModuleHandleW(L"vulkan-1.dll");
    if (!lib) lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) return false;
    vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!vk.GetInstanceProcAddr) return false;
    vk.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vk.GetInstanceProcAddr(g_vkInstance, "vkGetDeviceProcAddr");
    if (!vk.GetDeviceProcAddr) return false;
    bool ok = LoadDeviceFn(vk.CreateCommandPool, "vkCreateCommandPool");
    ok &= LoadDeviceFn(vk.AllocateCommandBuffers, "vkAllocateCommandBuffers");
    ok &= LoadDeviceFn(vk.BeginCommandBuffer, "vkBeginCommandBuffer");
    ok &= LoadDeviceFn(vk.EndCommandBuffer, "vkEndCommandBuffer");
    ok &= LoadDeviceFn(vk.ResetCommandBuffer, "vkResetCommandBuffer");
    ok &= LoadDeviceFn(vk.CmdPipelineBarrier, "vkCmdPipelineBarrier");
    ok &= LoadDeviceFn(vk.CmdCopyImage, "vkCmdCopyImage");
    ok &= LoadDeviceFn(vk.CmdBlitImage, "vkCmdBlitImage");
    ok &= LoadDeviceFn(vk.QueueSubmit, "vkQueueSubmit");
    ok &= LoadDeviceFn(vk.CreateFence, "vkCreateFence");
    ok &= LoadDeviceFn(vk.WaitForFences, "vkWaitForFences");
    ok &= LoadDeviceFn(vk.ResetFences, "vkResetFences");
    ok &= LoadDeviceFn(vk.GetFenceStatus, "vkGetFenceStatus");
    return ok;
}

bool CreateCommandObjects()
{
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g_queueFamily;
    if (vk.CreateCommandPool(g_vkDevice, &pi, nullptr, &g_pool) != VK_SUCCESS) return false;
    for (Slot& s : g_slots)
    {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = g_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vk.AllocateCommandBuffers(g_vkDevice, &ai, &s.cmd) != VK_SUCCESS) return false;
        if (vk.CreateFence(g_vkDevice, &fi, nullptr, &s.fence) != VK_SUCCESS) return false;
    }
    return true;
}

bool CreateSwapchain(Swapchain& sc, uint32_t w, uint32_t h, const char* name)
{
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r;
    {
        QueueLock lock;
        r = xrCreateSwapchain(g_session, &ci, &sc.handle);
    }
    if (!Check(r, "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> images(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
    xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)images.data());
    for (auto& i : images) sc.images.push_back(i.image);
    sc.w = w;
    sc.h = h;
    Log("xr: %s swapchain %ux%u, %u images", name, w, h, n);
    return true;
}

void DestroySwapchain(Swapchain& sc)
{
    {
        QueueLock lock;
        xrDestroySwapchain(sc.handle);
    }
    sc = Swapchain();
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, const VkImageSubresourceRange& range)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    vk.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool SameChannelOrder(VkFormat a, int64_t b)
{
    auto bgra = [](int64_t f) { return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB; };
    auto rgba = [](int64_t f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB; };
    return (bgra(a) && bgra(b)) || (rgba(a) && rgba(b));
}

// The GPU was lost (VK_ERROR_DEVICE_LOST): it stopped with an error and Windows reset its driver (the system's log has
// it: nvlddmkm 13 and 153 on an NVIDIA card, at a race's start). Neither DXVK nor the runtime can go on after
// it: Virtual Desktop failed every frame and the game stood still in DXVK soon after, frozen in the headset. The game
// ends instead, saying why: the VR session first (the headset goes back to its own view, where the window can be
// seen), then the window, over the game's minimized one.
[[noreturn]] void DeviceLost(const char* what)
{
    Log("xr: %s: the GPU was lost (VK_ERROR_DEVICE_LOST) - its driver was reset; the game ends", what);
    if (!BackgroundMode())
    {
        HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD { return xrDestroyInstance(g_instance); }, nullptr, 0, nullptr);
        if (t && WaitForSingleObject(t, 2000) != WAIT_OBJECT_0) Log("xr: the runtime did not end the session in 2 s");
        EnumWindows(
            [](HWND w, LPARAM) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                if (pid == GetCurrentProcessId() && IsWindowVisible(w)) ShowWindow(w, SW_MINIMIZE);
                return TRUE;
            },
            0);
        TASKDIALOGCONFIG c = { sizeof(c) };
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        c.pszWindowTitle = L"FlatOut 2 VR";
        c.pszMainIcon = TD_ERROR_ICON;
        c.pszMainInstruction = L"The graphics driver was reset";
        c.pszContent = L"The graphics card stopped with an error and Windows reset its driver (the system's event log has it, as "
                       L"nvlddmkm on an NVIDIA card). The game cannot draw after that and closes now: start it again.\n\n"
                       L"Details: scripts\\FO2VR.log";
        c.dwCommonButtons = TDCBF_OK_BUTTON;
        ShowTaskDialog(c, nullptr);
    }
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) Sleep(INFINITE);
}

// After the runtime failed a frame's call: whether the GPU was lost meanwhile (the runtime then fails every frame, and
// the mod's own copy, which would see it, is not reached).
void CheckDeviceLost(const char* what)
{
    VkFence f = g_slots[0].fence;
    if (g_vkDevice && f && vk.GetFenceStatus && vk.GetFenceStatus(g_vkDevice, f) == VK_ERROR_DEVICE_LOST) DeviceLost(what);
}

// Copies D3D9 textures (from the given offsets in them, if any) into newly acquired images of the given swapchains on
// DXVK's queue.
bool CopyToSwapchains(IDirect3DTexture9* const* textures, Swapchain* const* chains, int count, const uint32_t (*offsets)[2] = nullptr)
{
    struct Src
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    } src[3];
    for (int i = 0; i < count; i++)
    {
        ID3D9VkInteropTexture* it = nullptr;
        if (FAILED(textures[i]->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&it)))
        {
            Log("xr: texture without Vulkan image");
            return false;
        }
        HRESULT hr = it->GetVulkanImageInfo(&src[i].image, &src[i].layout, &src[i].info);
        it->Release();
        if (FAILED(hr)) return false;
        if (!g_loggedImage)
        {
            g_loggedImage = true;
            Log("xr: D3D9 texture image %ux%u format %d layout %d -> swapchain format %lld", src[i].info.extent.width,
                src[i].info.extent.height, (int)src[i].info.format, (int)src[i].layout, (long long)g_format);
        }
    }

    // DXVK has to hand the eye rendering to the queue before our copy is queued behind it.
    double t0 = NowSeconds();
    g_interop->FlushRenderingCommands();
    double t1 = NowSeconds();
    g_times.flush += t1 - t0;

    uint32_t index[3];
    for (int i = 0; i < count; i++)
    {
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrResult r;
        double a0 = NowSeconds();
        {
            QueueLock lock;
            r = xrAcquireSwapchainImage(chains[i]->handle, &ai, &index[i]);
        }
        double a1 = NowSeconds();
        g_times.acquire += a1 - a0;
        if (!Check(r, "xrAcquireSwapchainImage")) return false;
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        {
            QueueLock lock;
            r = xrWaitSwapchainImage(chains[i]->handle, &wi);
        }
        Check(r, "xrWaitSwapchainImage");
        g_times.wait += NowSeconds() - a1;
    }

    g_slot = (g_slot + 1) % 3;
    Slot& s = g_slots[g_slot];
    double f0 = NowSeconds();
    if (vk.WaitForFences(g_vkDevice, 1, &s.fence, VK_TRUE, 1000000000ull) == VK_ERROR_DEVICE_LOST) DeviceLost("vkWaitForFences");
    double f1 = NowSeconds();
    g_times.fence += f1 - f0;
    vk.ResetFences(g_vkDevice, 1, &s.fence);
    vk.ResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(s.cmd, &bi);
    const VkImageSubresourceRange color = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (int i = 0; i < count; i++)
    {
        VkImage dst = chains[i]->images[index[i]];
        ImageBarrier(s.cmd, src[i].image, src[i].layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, color);
        ImageBarrier(s.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, color);
        uint32_t x = offsets ? std::min(offsets[i][0], src[i].info.extent.width) : 0;
        uint32_t y = offsets ? std::min(offsets[i][1], src[i].info.extent.height) : 0;
        uint32_t w = std::min(src[i].info.extent.width - x, chains[i]->w);
        uint32_t h = std::min(src[i].info.extent.height - y, chains[i]->h);
        if (SameChannelOrder(src[i].info.format, g_format))
        {
            VkImageCopy region = {};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffset = { (int32_t)x, (int32_t)y, 0 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { w, h, 1 };
            vk.CmdCopyImage(s.cmd, src[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        else
        {
            VkImageBlit region = {};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffsets[0] = { (int32_t)x, (int32_t)y, 0 };
            region.srcOffsets[1] = { (int32_t)(x + w), (int32_t)(y + h), 1 };
            region.dstOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
            vk.CmdBlitImage(s.cmd, src[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                            VK_FILTER_NEAREST);
        }
        // Give the image back to DXVK in its layout, and the swapchain image to the runtime as a colour attachment.
        ImageBarrier(s.cmd, src[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src[i].layout, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     color);
        ImageBarrier(s.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, color);
    }
    vk.EndCommandBuffer(s.cmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    VkResult vr;
    {
        QueueLock lock;
        vr = vk.QueueSubmit(g_queue, 1, &si, s.fence);
        for (int i = 0; i < count; i++)
        {
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            chains[i]->released |= XR_SUCCEEDED(xrReleaseSwapchainImage(chains[i]->handle, &ri));
        }
    }
    g_times.submit += NowSeconds() - f1;
    g_times.count++;
    if (vr == VK_ERROR_DEVICE_LOST) DeviceLost("vkQueueSubmit");
    if (vr != VK_SUCCESS) Log("xr: vkQueueSubmit failed %d", (int)vr);
    return vr == VK_SUCCESS;
}

std::vector<std::string> SplitList(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// xrGetVulkanInstanceExtensionsKHR or xrGetVulkanDeviceExtensionsKHR (same signature).
std::vector<std::string> RuntimeVulkanExtensions(const char* function)
{
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(g_instance, function, (PFN_xrVoidFunction*)&fn);
    uint32_t n = 0;
    if (!fn || !Check(fn(g_instance, g_system, 0, &n, nullptr), function) || n == 0) return {};
    std::string s(n, '\0');
    if (!Check(fn(g_instance, g_system, n, &n, s.data()), function)) return {};
    s.resize(strlen(s.c_str()));
    return SplitList(s);
}

// Extensions the runtime asks for that this machine's Vulkan does not have would make DXVK's
// vkCreateInstance / vkCreateDevice fail, so they are left out (the session then fails instead of
// the game).
void FilterSupported(std::vector<std::string>& instanceExts, std::vector<std::string>& deviceExts)
{
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    auto gipa = lib ? (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa)
    {
        instanceExts.clear();
        deviceExts.clear();
        return;
    }
    auto enumInst = (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
    auto createInst = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    std::vector<std::string> have;
    uint32_t n = 0;
    if (enumInst && enumInst(nullptr, &n, nullptr) == VK_SUCCESS)
    {
        std::vector<VkExtensionProperties> props(n);
        enumInst(nullptr, &n, props.data());
        for (auto& p : props) have.push_back(p.extensionName);
    }
    auto keep = [&](std::vector<std::string>& list, const char* what) {
        std::vector<std::string> kept;
        for (auto& e : list)
            if (std::find(have.begin(), have.end(), e) != have.end())
                kept.push_back(e);
            else
                Log("xr: %s extension %s is not supported here - left out", what, e.c_str());
        list = kept;
    };
    keep(instanceExts, "Vulkan instance");

    // Device extensions: supported by any of the GPUs.
    have.clear();
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (createInst && createInst(&ci, nullptr, &inst) == VK_SUCCESS)
    {
        auto enumPhys = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
        auto enumDev = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
        auto destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
        uint32_t count = 0;
        if (enumPhys && enumDev && enumPhys(inst, &count, nullptr) == VK_SUCCESS)
        {
            std::vector<VkPhysicalDevice> phys(count);
            enumPhys(inst, &count, phys.data());
            for (VkPhysicalDevice pd : phys)
            {
                n = 0;
                enumDev(pd, nullptr, &n, nullptr);
                std::vector<VkExtensionProperties> props(n);
                enumDev(pd, nullptr, &n, props.data());
                for (auto& p : props) have.push_back(p.extensionName);
            }
        }
        if (destroy) destroy(inst, nullptr);
    }
    keep(deviceExts, "Vulkan device");
}

// DXVK creates its Vulkan instance and device with the extensions listed by wineopenxr.dll
// (src/wineopenxr), which must be loaded before Direct3DCreate9.
void PrepareDxvkExtensions()
{
    std::vector<std::string> inst = RuntimeVulkanExtensions("xrGetVulkanInstanceExtensionsKHR");
    std::vector<std::string> dev = RuntimeVulkanExtensions("xrGetVulkanDeviceExtensionsKHR");
    FilterSupported(inst, dev);
    std::string instList, devList;
    for (auto& e : inst) instList += (instList.empty() ? "" : " ") + e;
    for (auto& e : dev) devList += (devList.empty() ? "" : " ") + e;
    Log("xr: Vulkan instance extensions for the runtime: %s", instList.c_str());
    Log("xr: Vulkan device extensions for the runtime: %s", devList.c_str());

    std::wstring path = ModuleDir() + L"wineopenxr.dll";
    HMODULE shim = LoadLibraryW(path.c_str());
    using SetFn = void (*)(const char*, const char*);
    auto set = shim ? (SetFn)GetProcAddress(shim, "fo2vr_SetVulkanExtensions") : nullptr;
    if (!set)
    {
        Log("xr: cannot load %ls (error %u) - DXVK will lack the runtime's Vulkan extensions", path.c_str(), (unsigned)GetLastError());
        return;
    }
    set(instList.c_str(), devList.c_str());
}

// The hand trackers (see g_handExtensions), when the headset tracks hands.
void CreateHandTrackers()
{
    if (!g_handExtensions)
    {
        Log("xr: the runtime cannot tell the hands tracked by the headset's cameras from the controllers");
        return;
    }
    // SteamVR (2.18.4, through Virtual Desktop) says the cameras track the hands also while the controllers are held,
    // which left them out: Virtual Desktop's own runtime only.
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g_instance, &ip);
    if (!strstr(ip.runtimeName, "VirtualDesktop"))
    {
        Log("xr: %s's hand tracking does not tell the hands from the controllers held - not used", ip.runtimeName);
        return;
    }
    XrSystemHandTrackingPropertiesEXT hp = { XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES, &hp };
    xrGetInstanceProcAddr(g_instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&g_createHandTracker);
    xrGetInstanceProcAddr(g_instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&g_locateHandJoints);
    if (!XR_SUCCEEDED(xrGetSystemProperties(g_instance, g_system, &sp)) || !hp.supportsHandTracking || !g_createHandTracker || !g_locateHandJoints)
    {
        Log("xr: the headset does not track hands");
        return;
    }
    XrHandTrackingDataSourceEXT cameras = XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
    XrHandTrackingDataSourceInfoEXT ds = { XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT };
    ds.requestedDataSourceCount = 1;
    ds.requestedDataSources = &cameras;
    for (int h = 0; h < 2; h++)
    {
        XrHandTrackerCreateInfoEXT ci = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, &ds };
        ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!Check(g_createHandTracker(g_session, &ci, &g_handTracker[h]), "xrCreateHandTrackerEXT")) g_handTracker[h] = XR_NULL_HANDLE;
    }
    if (g_handTracker[0] && g_handTracker[1]) Log("xr: hands tracked by the headset's cameras are told from the controllers");
}

// Whether the headset's cameras track the hand at the open frame's display time (its controller put down).
bool HandSeen(int h)
{
    if (!g_handTracker[h]) return false;
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandTrackingDataSourceStateEXT source = { XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT };
    XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT, &source };
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;
    XrHandJointsLocateInfoEXT li = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    li.baseSpace = g_local;
    li.time = g_frameState.predictedDisplayTime;
    return XR_SUCCEEDED(g_locateHandJoints(g_handTracker[h], &li, &locations)) && locations.isActive && source.isActive &&
           source.dataSource == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
}

// The controllers: one action set with the aim pose, the trigger, the grip, B/Y, A/X, the menu button, the thumbstick
// and its click of either hand, and their vibration, bound for the Touch controllers (Virtual Desktop gives the game
// other headsets' controllers as these too), the Index and Windows Mixed Reality controllers (what they have of it) and
// any other through the simple profile (the trigger, the menu button and the pose).
void CreateInput()
{
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(si.actionSetName, "cockpit");
    strcpy(si.localizedActionSetName, "Cockpit");
    if (!Check(xrCreateActionSet(g_instance, &si, &g_actionSet), "xrCreateActionSet")) return;
    xrStringToPath(g_instance, "/user/hand/left", &g_handPath[0]);
    xrStringToPath(g_instance, "/user/hand/right", &g_handPath[1]);
    struct
    {
        XrAction* action;
        const char *name, *label;
        XrActionType type;
    } actions[] = {
        { &g_aimAction, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT },
        { &g_holdAction, "hold", "Hold", XR_ACTION_TYPE_POSE_INPUT },
        { &g_triggerAction, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT },
        { &g_gripAction, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT },
        { &g_buttonAction, "reset", "Reset", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &g_stickAction, "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
        { &g_lowerAction, "lower", "A or X", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &g_menuAction, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &g_thumbAction, "thumbclick", "Thumbstick press", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &g_hapticAction, "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT },
    };
    for (auto& a : actions)
    {
        XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
        ai.actionType = a.type;
        strcpy(ai.actionName, a.name);
        strcpy(ai.localizedActionName, a.label);
        ai.countSubactionPaths = 2;
        ai.subactionPaths = g_handPath;
        if (!Check(xrCreateAction(g_actionSet, &ai, a.action), "xrCreateAction")) return;
    }
    auto suggest = [](const char* profile, std::initializer_list<std::pair<XrAction, const char*>> list) {
        std::vector<XrActionSuggestedBinding> b;
        for (auto& [action, path] : list)
        {
            XrPath p;
            if (XR_SUCCEEDED(xrStringToPath(g_instance, path, &p))) b.push_back({ action, p });
        }
        XrPath p;
        xrStringToPath(g_instance, profile, &p);
        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile = p;
        sb.countSuggestedBindings = (uint32_t)b.size();
        sb.suggestedBindings = b.data();
        Check(xrSuggestInteractionProfileBindings(g_instance, &sb), profile);
    };
    suggest("/interaction_profiles/oculus/touch_controller",
            { { g_aimAction, "/user/hand/left/input/aim/pose" }, { g_aimAction, "/user/hand/right/input/aim/pose" },
              { g_holdAction, "/user/hand/left/input/grip/pose" }, { g_holdAction, "/user/hand/right/input/grip/pose" },
              { g_triggerAction, "/user/hand/left/input/trigger/value" }, { g_triggerAction, "/user/hand/right/input/trigger/value" },
              { g_gripAction, "/user/hand/left/input/squeeze/value" }, { g_gripAction, "/user/hand/right/input/squeeze/value" },
              { g_buttonAction, "/user/hand/left/input/y/click" }, { g_buttonAction, "/user/hand/right/input/b/click" },
              { g_stickAction, "/user/hand/left/input/thumbstick" }, { g_stickAction, "/user/hand/right/input/thumbstick" },
              { g_lowerAction, "/user/hand/left/input/x/click" }, { g_lowerAction, "/user/hand/right/input/a/click" },
              { g_menuAction, "/user/hand/left/input/menu/click" },
              { g_thumbAction, "/user/hand/left/input/thumbstick/click" }, { g_thumbAction, "/user/hand/right/input/thumbstick/click" },
              { g_hapticAction, "/user/hand/left/output/haptic" }, { g_hapticAction, "/user/hand/right/output/haptic" } });
    suggest("/interaction_profiles/valve/index_controller",
            { { g_aimAction, "/user/hand/left/input/aim/pose" }, { g_aimAction, "/user/hand/right/input/aim/pose" },
              { g_holdAction, "/user/hand/left/input/grip/pose" }, { g_holdAction, "/user/hand/right/input/grip/pose" },
              { g_triggerAction, "/user/hand/left/input/trigger/value" }, { g_triggerAction, "/user/hand/right/input/trigger/value" },
              { g_gripAction, "/user/hand/left/input/squeeze/value" }, { g_gripAction, "/user/hand/right/input/squeeze/value" },
              { g_buttonAction, "/user/hand/left/input/b/click" }, { g_buttonAction, "/user/hand/right/input/b/click" },
              { g_stickAction, "/user/hand/left/input/thumbstick" }, { g_stickAction, "/user/hand/right/input/thumbstick" },
              { g_lowerAction, "/user/hand/left/input/a/click" }, { g_lowerAction, "/user/hand/right/input/a/click" },
              { g_thumbAction, "/user/hand/left/input/thumbstick/click" }, { g_thumbAction, "/user/hand/right/input/thumbstick/click" },
              { g_hapticAction, "/user/hand/left/output/haptic" }, { g_hapticAction, "/user/hand/right/output/haptic" } });
    suggest("/interaction_profiles/microsoft/motion_controller",
            { { g_aimAction, "/user/hand/left/input/aim/pose" }, { g_aimAction, "/user/hand/right/input/aim/pose" },
              { g_holdAction, "/user/hand/left/input/grip/pose" }, { g_holdAction, "/user/hand/right/input/grip/pose" },
              { g_triggerAction, "/user/hand/left/input/trigger/value" }, { g_triggerAction, "/user/hand/right/input/trigger/value" },
              { g_gripAction, "/user/hand/left/input/squeeze/click" }, { g_gripAction, "/user/hand/right/input/squeeze/click" },
              { g_stickAction, "/user/hand/left/input/thumbstick" }, { g_stickAction, "/user/hand/right/input/thumbstick" },
              { g_menuAction, "/user/hand/left/input/menu/click" },
              { g_thumbAction, "/user/hand/left/input/thumbstick/click" }, { g_thumbAction, "/user/hand/right/input/thumbstick/click" },
              { g_hapticAction, "/user/hand/left/output/haptic" }, { g_hapticAction, "/user/hand/right/output/haptic" } });
    suggest("/interaction_profiles/khr/simple_controller",
            { { g_aimAction, "/user/hand/left/input/aim/pose" }, { g_aimAction, "/user/hand/right/input/aim/pose" },
              { g_holdAction, "/user/hand/left/input/grip/pose" }, { g_holdAction, "/user/hand/right/input/grip/pose" },
              { g_triggerAction, "/user/hand/left/input/select/click" }, { g_triggerAction, "/user/hand/right/input/select/click" },
              { g_menuAction, "/user/hand/left/input/menu/click" },
              { g_hapticAction, "/user/hand/left/output/haptic" }, { g_hapticAction, "/user/hand/right/output/haptic" } });
    for (int h = 0; h < 2; h++)
    {
        XrActionSpaceCreateInfo ci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ci.action = g_aimAction;
        ci.subactionPath = g_handPath[h];
        ci.poseInActionSpace.orientation.w = 1.0f;
        Check(xrCreateActionSpace(g_session, &ci, &g_aimSpace[h]), "xrCreateActionSpace");
        ci.action = g_holdAction;
        Check(xrCreateActionSpace(g_session, &ci, &g_holdSpace[h]), "xrCreateActionSpace");
    }
    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    if (Check(xrAttachSessionActionSets(g_session, &ai), "xrAttachSessionActionSets")) Log("xr: controller actions ready");
    CreateHandTrackers();
}

// The controllers at the open frame's display time.
void ReadInput()
{
    if (!g_actionSet) return;
    XrActiveActionSet active = { g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    XrResult r = xrSyncActions(g_session, &sync);
    for (int h = 0; h < 2; h++)
    {
        Controller c;
        if (r == XR_SUCCESS)  // (XR_SESSION_NOT_FOCUSED: the system has the controllers)
        {
            XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
            const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (g_aimSpace[h] && XR_SUCCEEDED(xrLocateSpace(g_aimSpace[h], g_local, g_frameState.predictedDisplayTime, &loc)) &&
                (loc.locationFlags & valid) == valid)
            {
                c.tracked = true;
                c.aim = ToGamePose(loc.pose);
            }
            loc = { XR_TYPE_SPACE_LOCATION };
            c.hold = c.aim;
            if (g_holdSpace[h] && XR_SUCCEEDED(xrLocateSpace(g_holdSpace[h], g_local, g_frameState.predictedDisplayTime, &loc)) &&
                (loc.locationFlags & valid) == valid)
                c.hold = ToGamePose(loc.pose);
            XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
            gi.subactionPath = g_handPath[h];
            XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
            gi.action = g_triggerAction;
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &f)) && f.isActive) c.trigger = f.currentState;
            f = { XR_TYPE_ACTION_STATE_FLOAT };
            gi.action = g_gripAction;
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &f)) && f.isActive) c.grip = f.currentState;
            auto pressed = [&](XrAction action) {
                XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
                gi.action = action;
                return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &b)) && b.isActive && b.currentState != XR_FALSE;
            };
            c.button = pressed(g_buttonAction);
            c.lower = pressed(g_lowerAction);
            c.menu = pressed(g_menuAction);
            c.thumb = pressed(g_thumbAction);
            XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
            gi.action = g_stickAction;
            if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &v)) && v.isActive) c.stick[0] = v.currentState.x, c.stick[1] = v.currentState.y;
            bool seen = HandSeen(h);
            if (seen != g_handSeen[h])
            {
                g_handSeen[h] = seen;
                Log(seen ? "xr: the %s controller is put down, the headset tracks the hand - its controller is left out"
                         : "xr: the %s controller is back", h == 0 ? "left" : "right");
            }
            if (seen) c = Controller();
        }
        g_hands[h] = c;
    }
}

void EndFrame(const XrCompositionLayerBaseHeader* layer, const XrCompositionLayerBaseHeader* overlay = nullptr)
{
    const XrCompositionLayerBaseHeader* layers[2] = { layer, overlay };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = g_frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layer ? (overlay ? 2 : 1) : 0;
    ei.layers = layer ? layers : nullptr;
    XrResult r;
    double t0 = NowSeconds();
    {
        QueueLock lock;
        r = xrEndFrame(g_session, &ei);
    }
    if (layer) g_times.endFrame += NowSeconds() - t0;
    if (!Check(r, "xrEndFrame")) CheckDeviceLost("xrEndFrame");
    g_frameOpen = false;
}

// ---- The loader's implicit API layers ----------------------------------------------------------------------------
// Programs register implicit API layers (in HKLM's or HKCU's Software\Khronos\OpenXR\1\ApiLayers\Implicit, the 32-bit
// registry's for a 32-bit game) that the loader puts into every OpenXR program. When a layer's library does not load and
// no layer loaded before it, the loader fails xrCreateInstance (XR_ERROR_FILE_ACCESS_ERROR): ReShade set up for OpenXR
// in some game registers its 32-bit layer for all, its library refuses to load into a program ReShade is not set up
// for (its DllMain returns FALSE), and VR did not start (issue #3). Each implicit layer names an environment variable
// that turns it off (disable_environment, read at each xrCreateInstance): after such a failure the start is tried again
// with them all off.
bool g_viaLoader;  // xrCreateInstance is the loader's (LoadEntryPoints)

// The process runs at a high integrity level (as administrator), as the loader tells it.
bool HighIntegrity()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    alignas(8) BYTE buf[64];
    DWORD size = 0;
    bool high = false;
    if (GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf), &size))
    {
        PSID sid = ((TOKEN_MANDATORY_LABEL*)buf)->Label.Sid;
        high = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) >= SECURITY_MANDATORY_HIGH_RID;
    }
    CloseHandle(token);
    return high;
}

// A runtime's or a layer's .json.
std::string ReadManifest(const std::wstring& path)
{
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0 && text.size() < 65536) text.append(buf, got);
        CloseHandle(f);
    }
    return text;
}

// "key": "..." in a manifest (the first such key, with JSON's escapes).
std::string ManifestString(const std::string& text, const char* key)
{
    std::string quoted = std::string("\"") + key + "\"";
    size_t at = text.find(quoted);
    size_t colon = at == std::string::npos ? at : text.find(':', at + quoted.size());
    size_t open = colon == std::string::npos ? colon : text.find('"', colon + 1);
    std::string value;
    for (size_t i = open == std::string::npos ? text.size() : open + 1; i < text.size() && text[i] != '"'; i++)
        value += text[i] == '\\' && i + 1 < text.size() ? text[++i] : text[i];
    return value;
}

struct ImplicitLayer
{
    std::wstring manifest;
    std::string name, disableEnv;
};

// The implicit layers the loader puts in (those turned on: 0 in the registry; HKCU's not in a process run as
// administrator).
std::vector<ImplicitLayer> ImplicitLayers()
{
    std::vector<ImplicitLayer> layers;
    for (HKEY hive : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
    {
        HKEY key;
        if (hive == HKEY_CURRENT_USER && HighIntegrity()) continue;
        if (RegOpenKeyExW(hive, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i++)
        {
            wchar_t name[1024];
            DWORD nameLen = 1024, type = 0, on = 1, size = sizeof(on);
            LSTATUS s = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, (BYTE*)&on, &size);
            if (s == ERROR_NO_MORE_ITEMS) break;
            if (s != ERROR_SUCCESS || type != REG_DWORD || on != 0) continue;
            std::string text = ReadManifest(name);
            layers.push_back({ name, ManifestString(text, "name"), ManifestString(text, "disable_environment") });
        }
        RegCloseKey(key);
    }
    return layers;
}

// After the loader failed to load a layer: turns the implicit layers off; false if they are off already.
bool ImplicitLayersOff(XrResult r)
{
    if (!g_viaLoader || (r != XR_ERROR_FILE_ACCESS_ERROR && r != XR_ERROR_FILE_CONTENTS_INVALID && r != XR_ERROR_API_LAYER_NOT_PRESENT))
        return false;
    std::string off;
    for (const ImplicitLayer& l : ImplicitLayers())
        if (!l.disableEnv.empty() && !GetEnvironmentVariableA(l.disableEnv.c_str(), nullptr, 0))
        {
            SetEnvironmentVariableA(l.disableEnv.c_str(), "1");
            off += " " + l.name;
        }
    if (off.empty()) return false;
    Log("xr: xrCreateInstance failed: %d %s - the OpenXR loader could not load an API layer; tried again with the implicit "
        "layers off:%s", (int)r, ResultName(r), off.c_str());
    return true;
}

// ---- The runtime's start -----------------------------------------------------------------------------------------
// xrCreateInstance and xrGetSystem run on a thread of their own. A runtime that cannot start may take half a minute to
// say so, or never return, and the game stood still with nothing on the screen: SteamVR 2.18 does not come up when
// its vrserver.exe runs elevated (started by an elevated Virtual Desktop Streamer, or left by an earlier start), its
// bootstrapper cannot open it and gives up after 30 s, and xrCreateInstance fails (XR_ERROR_RUNTIME_FAILURE) or
// waits on. After a few seconds a window says what the game waits for and goes away by itself when the runtime is
// ready; when the start fails, it says why and what to do, with Try again, Play without VR and Quit.
std::wstring g_runtimePath;  // the runtime's .json (empty: the system's default runtime)

struct RuntimeStart
{
    LONG refs = 2;                         // the thread's and the game's
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // set when the thread has its results
    double began = NowSeconds();
    XrInstance instance = XR_NULL_HANDLE;  // the results, read once done is set
    XrSystemId system = XR_NULL_SYSTEM_ID;
    std::wstring problem;                  // why it failed, for the player (empty: ready)

    bool Done() const { return WaitForSingleObject(done, 0) == WAIT_OBJECT_0; }
    // The last one out closes an instance nobody took (the game went on without VR while the runtime started).
    void Release()
    {
        if (InterlockedDecrement(&refs) > 0) return;
        if (instance)
        {
            Log("xr: the runtime got ready after the game went on without VR - closed");
            xrDestroyInstance(instance);
        }
        CloseHandle(done);
        delete this;
    }
};

std::wstring RuntimeName()
{
    std::wstring r = g_cfg.vrRuntime;
    for (wchar_t& c : r) c = towlower(c);
    if (r == L"steamvr" || r.find(L"steamxr") != std::wstring::npos) return L"SteamVR";
    if (r.find(L"virtualdesktop") != std::wstring::npos) return L"Virtual Desktop";
    if (r.find(L"oculus") != std::wstring::npos) return L"the Oculus runtime";
    return L"the VR runtime";
}

std::wstring Sentence(std::wstring s)
{
    if (!s.empty()) s[0] = towupper(s[0]);
    return s;
}

// What to do while the game waits for the runtime, or when it failed.
std::wstring RuntimeHint()
{
    std::wstring name = RuntimeName();
    if (name == L"SteamVR")
        return L"Start SteamVR if it does not run, and wait until it shows the headset. If SteamVR hangs while it starts, "
               L"close it (end vrserver.exe in the Task Manager if it stays) and start it again from Steam.";
    if (name == L"Virtual Desktop") return L"Connect the headset in Virtual Desktop (its Streamer runs on this PC).";
    return L"Start the VR runtime and connect the headset.";
}

std::wstring ResultText(XrResult r)
{
    wchar_t buf[64];
    const char* name = ResultName(r);
    if (*name)
        swprintf(buf, 64, L"%hs", name);
    else
        swprintf(buf, 64, L"error %d", (int)r);
    return buf;
}

DWORD WINAPI RuntimeThread(void* param)
{
    RuntimeStart* s = (RuntimeStart*)param;
    std::vector<const char*> exts = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    // The hands told from the controllers (see g_handExtensions), when the runtime can.
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    if (xrGetInstanceProcAddr) xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate);
    uint32_t count = 0;
    std::vector<XrExtensionProperties> have;
    if (enumerate && XR_SUCCEEDED(enumerate(nullptr, 0, &count, nullptr)))
    {
        have.resize(count, { XR_TYPE_EXTENSION_PROPERTIES });
        if (!XR_SUCCEEDED(enumerate(nullptr, count, &count, have.data()))) have.clear();
    }
    auto has = [&](const char* name) {
        return std::any_of(have.begin(), have.end(), [&](const XrExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
    };
    g_handExtensions = has(XR_EXT_HAND_TRACKING_EXTENSION_NAME) && has(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    if (g_handExtensions) exts.insert(exts.end(), { XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME });
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "FlatOut 2 VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "FO2VR");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    std::wstring problem;
    XrResult r = xrCreateInstance ? xrCreateInstance(&ci, &instance) : XR_ERROR_RUNTIME_UNAVAILABLE;  // (none: see LoadEntryPoints)
    if (ImplicitLayersOff(r)) r = xrCreateInstance(&ci, &instance);
    if (XR_SUCCEEDED(r) && !LoadFunctions(instance))
    {
        Log("xr: the runtime lacks some of OpenXR's functions");
        if (xrDestroyInstance) xrDestroyInstance(instance);
        r = XR_ERROR_RUNTIME_FAILURE;
    }
    if (!Check(r, "xrCreateInstance"))
    {
        instance = XR_NULL_HANDLE;
        // The loader says unavailable also when it finds the runtime but cannot load it.
        if (r == XR_ERROR_RUNTIME_UNAVAILABLE && !g_runtimePath.empty() && GetFileAttributesW(g_runtimePath.c_str()) == INVALID_FILE_ATTRIBUTES)
            problem = Sentence(RuntimeName()) + L" was not found (" + g_runtimePath + L"). The runtime is chosen by Runtime= in scripts\\FO2VR.ini.";
        else if (r == XR_ERROR_RUNTIME_UNAVAILABLE)
            problem = Sentence(RuntimeName()) + L" could not be loaded" + (g_runtimePath.empty() ? L"" : L" (" + g_runtimePath + L")") +
                      L". Is it installed? The runtime is chosen by Runtime= in scripts\\FO2VR.ini.";
        else
            problem = Sentence(RuntimeName()) + L" did not start (" + ResultText(r) + L").\n\n" + RuntimeHint();
    }
    else
    {
        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        xrGetInstanceProperties(instance, &ip);
        Log("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
            XR_VERSION_PATCH(ip.runtimeVersion));
        XrSystemGetInfo si = { XR_TYPE_SYSTEM_GET_INFO };
        si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        // SteamVR reports the headset a few seconds after it starts.
        r = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        for (int i = 0; i < 40 && r == XR_ERROR_FORM_FACTOR_UNAVAILABLE; i++)
        {
            r = xrGetSystem(instance, &si, &system);
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) Sleep(250);
        }
        if (!Check(r, "xrGetSystem (is the headset connected?)"))
        {
            problem = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? Sentence(RuntimeName()) + L" runs but has no headset.\n\n" + RuntimeHint()
                                                            : Sentence(RuntimeName()) + L" gave no headset (" + ResultText(r) + L").\n\n" + RuntimeHint();
            xrDestroyInstance(instance);
            instance = XR_NULL_HANDLE;
        }
    }
    s->instance = instance;
    s->system = system;
    s->problem = problem;
    SetEvent(s->done);
    s->Release();
    return 0;
}

RuntimeStart* BeginRuntimeStart()
{
    RuntimeStart* s = new RuntimeStart;
    HANDLE thread = CreateThread(nullptr, 0, RuntimeThread, s, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        RuntimeThread(s);
    return s;
}

enum { kTryAgain = 100, kWithoutVr, kQuit };
enum class StartChoice { NoWindow, Ready, WithoutVr, Quit };

struct StartDialog
{
    RuntimeStart* start = nullptr;
    int seconds = -1;  // shown on the waiting page
    bool failedPage = false, ready = false;
    std::wstring waitInstruction, waitContent, failContent;
    TASKDIALOG_BUTTON waitButtons[2] = { { kWithoutVr, L"Play without VR" }, { kQuit, L"Quit the game" } };
    TASKDIALOG_BUTTON failButtons[3] = { { kTryAgain, L"Try again" }, { kWithoutVr, L"Play without VR" }, { kQuit, L"Quit the game" } };
    TASKDIALOGCONFIG waiting = { sizeof(TASKDIALOGCONFIG) }, failed = { sizeof(TASKDIALOGCONFIG) };
};

void ShowWaiting(StartDialog& d, HWND hwnd)
{
    d.failedPage = false;
    d.seconds = (int)(NowSeconds() - d.start->began);
    d.waitContent = L"The game goes on in VR as soon as " + RuntimeName() + L" is ready (" + std::to_wstring(d.seconds) + L" s).\n\n" +
                    RuntimeHint();
    d.waiting.pszContent = d.waitContent.c_str();
    if (hwnd) SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&d.waiting);
}

void ShowFailed(StartDialog& d, HWND hwnd)
{
    d.failedPage = true;
    d.failContent = d.start->problem + L"\n\nDetails: scripts\\FO2VR.log";
    d.failed.pszContent = d.failContent.c_str();
    if (hwnd) SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&d.failed);
}

HRESULT CALLBACK StartDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR data)
{
    StartDialog& d = *(StartDialog*)data;
    switch (msg)
    {
    case TDN_CREATED:
        // Over the game's window, which may cover the screen.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
        break;
    case TDN_TIMER:
        if (d.failedPage) break;
        if (d.start->Done())
        {
            if (!d.start->problem.empty())
                ShowFailed(d, hwnd);
            else
            {
                d.ready = true;
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
            }
        }
        else if ((int)(NowSeconds() - d.start->began) != d.seconds)
        {
            ShowWaiting(d, nullptr);
            SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)d.waitContent.c_str());
        }
        break;
    case TDN_BUTTON_CLICKED:
        if (wp == kTryAgain)
        {
            Log("xr: the runtime's start tried again");
            d.start->Release();
            d.start = BeginRuntimeStart();
            ShowWaiting(d, hwnd);
            return S_FALSE;  // the window stays
        }
        break;
    }
    return S_OK;
}

// The window while the game waits for the runtime, or after it failed. start may be replaced by a new one (Try again).
StartChoice AskAboutRuntime(RuntimeStart*& start)
{
    StartDialog d;
    d.start = start;
    d.waitInstruction = L"Waiting for " + RuntimeName() + L"...";
    for (TASKDIALOGCONFIG* c : { &d.waiting, &d.failed })
    {
        c->dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER;
        c->pszWindowTitle = L"FlatOut 2 VR";
        c->pfCallback = StartDialogProc;
        c->lpCallbackData = (LONG_PTR)&d;
    }
    d.waiting.pszMainIcon = TD_INFORMATION_ICON;
    d.waiting.pszMainInstruction = d.waitInstruction.c_str();
    d.waiting.cButtons = 2;
    d.waiting.pButtons = d.waitButtons;
    d.waiting.nDefaultButton = kWithoutVr;
    d.failed.pszMainIcon = TD_WARNING_ICON;
    d.failed.pszMainInstruction = L"VR did not start";
    d.failed.cButtons = 3;
    d.failed.pButtons = d.failButtons;
    d.failed.nDefaultButton = kTryAgain;
    if (start->Done())
        ShowFailed(d, nullptr);
    else
        ShowWaiting(d, nullptr);
    int button = 0;
    HRESULT hr = ShowTaskDialog(d.failedPage ? d.failed : d.waiting, &button);
    start = d.start;
    if (FAILED(hr)) return StartChoice::NoWindow;
    if (d.ready) return StartChoice::Ready;
    return button == kQuit ? StartChoice::Quit : StartChoice::WithoutVr;  // (or the window closed)
}

// The runtime's own xrGetInstanceProcAddr (see LoadEntryPoints), loaded as the loader does it: the library its .json
// names (relative to the .json's folder), and the loader's handshake with it.
PFN_xrGetInstanceProcAddr LoadRuntime(const std::wstring& json)
{
    std::string lib = ManifestString(ReadManifest(json), "library_path");
    if (lib.empty())
    {
        Log("xr: %ls names no runtime library", json.c_str());
        return nullptr;
    }
    wchar_t wide[MAX_PATH] = L"";
    MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), -1, wide, MAX_PATH);
    std::wstring path = wide;
    bool absolute = path.size() > 1 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\'));
    if (!absolute) path = json.substr(0, json.find_last_of(L"/\\") + 1) + path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto negotiate = m ? (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface") : nullptr;
    if (!negotiate)
    {
        Log("xr: cannot load the runtime %ls (error %u)", path.c_str(), (unsigned)GetLastError());
        return nullptr;
    }
    XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li) };
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);  // (the loader's: any 1.x)
    XrNegotiateRuntimeRequest rr = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(rr) };
    XrResult r = negotiate(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr)
    {
        Log("xr: the runtime %ls refused the loader's handshake (%d)", path.c_str(), (int)r);
        return nullptr;
    }
    Log("xr: runtime library %ls loaded (interface %u, API %u.%u.%u)", path.c_str(), rr.runtimeInterfaceVersion,
        XR_VERSION_MAJOR(rr.runtimeApiVersion), XR_VERSION_MINOR(rr.runtimeApiVersion), XR_VERSION_PATCH(rr.runtimeApiVersion));
    return rr.getInstanceProcAddr;
}

// xrGetInstanceProcAddr and xrCreateInstance: the loader's, or with Runtime= in a game run as administrator the
// runtime's own. The loader ignores XR_RUNTIME_JSON in an elevated process (another program could otherwise make an
// administrator's load its library through it) and starts the system's active runtime instead: the Steam release,
// started by a Steam run as administrator, went to Virtual Desktop (the active runtime) with Runtime=steamvr. Runtime=
// comes from the mod's own ini, so the runtime it names is loaded without the loader then (and without the loader's
// API layers). False: no loader (the runtime's failure is the start's: RuntimeThread).
bool LoadEntryPoints()
{
    if (!g_runtimePath.empty() && HighIntegrity())
    {
        Log("xr: the game runs as administrator: the OpenXR loader would start the system's active runtime, not Runtime= - "
            "the runtime is loaded without it");
        xrGetInstanceProcAddr = LoadRuntime(g_runtimePath);
    }
    else
    {
        std::wstring loader = ModuleDir() + L"openxr_loader.dll";
        HMODULE m = LoadLibraryW(loader.c_str());
        if (!m)
        {
            Log("xr: cannot load %ls (error %u)", loader.c_str(), (unsigned)GetLastError());
            return false;
        }
        xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(m, "xrGetInstanceProcAddr");
        g_viaLoader = true;
        for (const ImplicitLayer& l : ImplicitLayers())
            Log("xr: the OpenXR loader's implicit API layer %s (%ls)", l.name.c_str(), l.manifest.c_str());
    }
    if (xrGetInstanceProcAddr) xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    return true;
}

}  // namespace

// [Test] FakeHeadset: no runtime at all. The session counts as running, never has a frame, and its
// eyes look straight ahead (DefaultViews), so races render the eyes for screenshots (the eyes' test
// path, PaceHz with VrEyesWithoutHeadset) with no headset connected.
bool g_fake;

bool InitInstance()
{
    if (g_cfg.testFakeHeadset)
    {
        g_fake = true;
        float scale = g_cfg.vrRenderScale > 0.1f ? g_cfg.vrRenderScale : 1.0f;
        g_eyeW = (uint32_t)(2064 * scale + 0.5f);  // Quest 3's recommended size
        g_eyeH = (uint32_t)(2208 * scale + 0.5f);
        DefaultViews();
        Log("xr: no runtime ([Test] FakeHeadset): eyes %ux%u straight ahead", g_eyeW, g_eyeH);
        return true;
    }
    // Runtime choice: the loader reads XR_RUNTIME_JSON when it is loaded (but see LoadEntryPoints).
    std::wstring runtime = g_cfg.vrRuntime;
    if (_wcsicmp(runtime.c_str(), L"steamvr") == 0)
    {
        wchar_t steam[MAX_PATH] = L"";
        DWORD size = sizeof(steam);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) == ERROR_SUCCESS)
            runtime = std::wstring(steam) + L"/steamapps/common/SteamVR/steamxr_win32.json";
        else
            runtime = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win32.json";
    }
    if (_wcsicmp(runtime.c_str(), L"default") != 0 && !runtime.empty())
    {
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", runtime.c_str());
        g_runtimePath = runtime;
        Log("xr: runtime %ls", runtime.c_str());
    }
    if (!LoadEntryPoints()) return false;
    // Most starts take a second or two; a runtime that keeps the game waiting longer, or fails, gets the window
    // (not in background mode: tests wait as before).
    RuntimeStart* start = BeginRuntimeStart();
    WaitForSingleObject(start->done, 3000);
    if (!start->Done() || !start->problem.empty())
    {
        StartChoice choice = BackgroundMode() ? StartChoice::NoWindow : AskAboutRuntime(start);
        if (choice == StartChoice::NoWindow) WaitForSingleObject(start->done, INFINITE);
        if (choice == StartChoice::Quit)
        {
            Log("xr: quit at the runtime's start, by the player's choice");
            TerminateProcess(GetCurrentProcess(), 0);
        }
        if (choice == StartChoice::WithoutVr)
        {
            Log("xr: the game goes on without VR, by the player's choice");
            start->Release();
            return false;
        }
    }
    if (!start->problem.empty())
    {
        start->Release();
        return false;
    }
    g_instance = start->instance;
    g_system = start->system;
    start->instance = XR_NULL_HANDLE;
    start->Release();
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(g_instance, g_system, &sp);
    XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t n = 2;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, views);
    float scale = g_cfg.vrRenderScale > 0.1f ? g_cfg.vrRenderScale : 1.0f;
    g_eyeW = std::min((uint32_t)(views[0].recommendedImageRectWidth * scale + 0.5f), views[0].maxImageRectWidth);
    g_eyeH = std::min((uint32_t)(views[0].recommendedImageRectHeight * scale + 0.5f), views[0].maxImageRectHeight);
    Log("xr: headset \"%s\", recommended %ux%u per eye, rendering %ux%u", sp.systemName, views[0].recommendedImageRectWidth,
        views[0].recommendedImageRectHeight, g_eyeW, g_eyeH);
    PrepareDxvkExtensions();
    return true;
}

bool Available() { return g_instance != XR_NULL_HANDLE || g_fake; }

void OnDevice(IDirect3DDevice9* dev)
{
    if (g_session || !g_instance) return;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&g_interop)))
    {
        g_interop = nullptr;
        Log("xr: the game does not run on DXVK - no VR (put DXVK's 32-bit d3d9.dll into the game folder)");
        return;
    }
    g_interop->GetVulkanHandles(&g_vkInstance, &g_vkPhys, &g_vkDevice);
    g_interop->GetSubmissionQueue(&g_queue, &g_queueIndex, &g_queueFamily);
    Log("xr: DXVK device: instance %p, physical device %p, device %p, queue family %u index %u", (void*)g_vkInstance, (void*)g_vkPhys,
        (void*)g_vkDevice, g_queueFamily, g_queueIndex);
    if (!LoadVulkan() || !CreateCommandObjects())
    {
        Log("xr: Vulkan setup failed - no VR");
        return;
    }

    // Required before xrCreateSession.
    PFN_xrGetVulkanGraphicsRequirementsKHR getReq = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR getDev = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&getDev);
    XrGraphicsRequirementsVulkanKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    if (getReq) Check(getReq(g_instance, g_system, &req), "xrGetVulkanGraphicsRequirementsKHR");
    VkPhysicalDevice want = VK_NULL_HANDLE;
    if (getDev) Check(getDev(g_instance, g_system, g_vkInstance, &want), "xrGetVulkanGraphicsDeviceKHR");
    Log("xr: runtime wants physical device %p (DXVK uses %p)%s", (void*)want, (void*)g_vkPhys, want == g_vkPhys ? "" : " - MISMATCH");

    XrGraphicsBindingVulkanKHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    binding.instance = g_vkInstance;
    binding.physicalDevice = g_vkPhys;
    binding.device = g_vkDevice;
    binding.queueFamilyIndex = g_queueFamily;
    binding.queueIndex = g_queueIndex;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = g_system;
    XrResult r;
    {
        QueueLock lock;
        r = xrCreateSession(g_instance, &sci, &g_session);
    }
    if (!Check(r, "xrCreateSession"))
    {
        g_session = XR_NULL_HANDLE;
        return;
    }

    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    Check(xrCreateReferenceSpace(g_session, &rs, &g_local), "xrCreateReferenceSpace");

    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
    std::string list;
    for (int64_t f : formats) list += std::to_string(f) + " ";
    Log("xr: swapchain formats: %s", list.c_str());
    // The game writes sRGB-encoded colours into B8G8R8A8 textures; copying them as they are into an
    // _SRGB swapchain shows them unchanged.
    g_format = 0;
    for (int64_t pref : { (int64_t)VK_FORMAT_B8G8R8A8_SRGB, (int64_t)VK_FORMAT_R8G8B8A8_SRGB })
        for (int64_t f : formats)
            if (!g_format && f == pref) g_format = f;
    if (!g_format)
    {
        Log("xr: runtime offers no 8-bit sRGB swapchain format - no VR");
        return;
    }
    DefaultViews();
    CreateSwapchain(g_eyeSc[0], g_eyeW, g_eyeH, "left eye");
    CreateSwapchain(g_eyeSc[1], g_eyeW, g_eyeH, "right eye");
    Log("xr: session created");
    CreateInput();
}

void PollEvents()
{
    if (!g_session) return;
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS)
    {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            Log("xr: session state %d", (int)e->state);
            g_sessionState = e->state;
            if (e->state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = Check(xrBeginSession(g_session, &bi), "xrBeginSession");
            }
            else if (e->state == XR_SESSION_STATE_STOPPING)
            {
                g_running = false;
                Check(xrEndSession(g_session), "xrEndSession");
            }
            else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING)
                g_running = false;
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            Log("xr: reference space changed (headset recentered)");
            g_recenterPending = true;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

bool Running() { return g_running || g_fake; }

bool Focused() { return g_fake ? !g_testUnfocused : g_sessionState == XR_SESSION_STATE_FOCUSED; }

void SetTestFocus(bool focused) { g_testUnfocused = !focused; }

void EyeSize(unsigned* w, unsigned* h)
{
    *w = g_eyeW;
    *h = g_eyeH;
}

void SetEyeOffsets(unsigned leftX, unsigned leftY, unsigned rightX, unsigned rightY)
{
    g_eyeOffset[0][0] = leftX, g_eyeOffset[0][1] = leftY;
    g_eyeOffset[1][0] = rightX, g_eyeOffset[1][1] = rightY;
}

bool WaitFrame()
{
    if (!g_running) return false;
    if (g_frameOpen) return true;
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    g_frameState = { XR_TYPE_FRAME_STATE };
    if (!Check(xrWaitFrame(g_session, &wi, &g_frameState), "xrWaitFrame"))
    {
        CheckDeviceLost("xrWaitFrame");
        return false;
    }
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult r;
    {
        QueueLock lock;
        r = xrBeginFrame(g_session, &bi);
    }
    if (!Check(r, "xrBeginFrame"))
    {
        CheckDeviceLost("xrBeginFrame");
        return false;
    }
    g_frameOpen = true;
    return true;
}

bool ShouldRender() { return g_frameOpen && g_frameState.shouldRender; }
double DisplayTime() { return (double)g_frameState.predictedDisplayTime * 1e-9; }
double DisplayPeriod() { return (double)g_frameState.predictedDisplayPeriod * 1e-9; }

// The eyes as the runtime gives them, logged when they change: how far apart, the angle between them and each one's
// field of view. (With SteamVR through Virtual Desktop the picture seemed cross-eyed after the game had lost the focus
// and got it back.)
void NoteViews()
{
    static float last[10];
    static bool logged;
    static double lastLog;
    const XrVector3f& a = g_views[0].pose.position;
    const XrVector3f& b = g_views[1].pose.position;
    const float fwd[3] = { 0, 0, -1 };
    float f0[3], f1[3];
    Rotate(g_views[0].pose.orientation, fwd, f0);
    Rotate(g_views[1].pose.orientation, fwd, f1);
    float dot = std::clamp(f0[0] * f1[0] + f0[1] * f1[1] + f0[2] * f1[2], -1.0f, 1.0f);
    const XrFovf& l = g_views[0].fov;
    const XrFovf& r = g_views[1].fov;
    const float now[10] = { sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z)), acosf(dot),
                            l.angleLeft, l.angleRight, l.angleUp, l.angleDown, r.angleLeft, r.angleRight, r.angleUp, r.angleDown };
    bool changed = !logged || fabsf(now[0] - last[0]) > 0.0005f;  // half a millimetre
    for (int i = 1; i < 10; i++) changed |= fabsf(now[i] - last[i]) > 0.0035f;  // a fifth of a degree
    double t = NowSeconds();
    if (!changed || t - lastLog < 1.0) return;
    memcpy(last, now, sizeof(last));
    logged = true;
    lastLog = t;
    const float deg = 57.29578f;
    Log("xr: the eyes %.1f mm apart, %.2f deg between them; fields of view (left right up down, deg): left eye %.1f %.1f %.1f %.1f, "
        "right eye %.1f %.1f %.1f %.1f", 1000.0f * now[0], deg * now[1], deg * now[2], deg * now[3], deg * now[4], deg * now[5],
        deg * now[6], deg * now[7], deg * now[8], deg * now[9]);
}

bool BeginFrame()
{
    if (!WaitFrame()) return false;

    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_frameState.predictedDisplayTime;
    li.space = g_local;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    g_views[0] = g_views[1] = { XR_TYPE_VIEW };
    uint32_t n = 2;
    g_viewsValid = XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &n, g_views)) && n == 2 &&
                   (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (!g_viewsValid) DefaultViews();
    if (g_viewsValid) NoteViews();
    if (g_viewsValid && g_recenterPending)
    {
        g_recenterPending = false;
        ApplyRecenter();
    }
    ReadInput();
    return g_frameState.shouldRender && g_viewsValid;
}

bool FrameOpen() { return g_frameOpen; }

EyePose Pose(int eye) { return ToGamePose(g_views[eye].pose); }

Controller Hand(int hand) { return g_fake ? g_testHands[hand] : g_hands[hand]; }

void SetTestHand(int hand, const Controller& c) { g_testHands[hand] = c; }

// Each vibration is sent for a little longer than it is sent again (the game's force feedback changes from frame to
// frame), so a frame that comes late does not leave a gap and a vibration the game stops ends by itself.
void Haptics(int hand, float amplitude)
{
    Vibration& v = g_vibration[hand];
    amplitude = amplitude > 1.0f ? 1.0f : roundf(amplitude * 20.0f) / 20.0f;  // steps of 5%: not a call every frame
    if (amplitude < 0.05f) amplitude = 0;
    double now = NowSeconds();
    if (amplitude == v.amplitude && (amplitude == 0 || now - v.sent < 0.05)) return;
    v.amplitude = amplitude;
    v.sent = now;
    if (!g_running || !g_hapticAction) return;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = g_hapticAction;
    hi.subactionPath = g_handPath[hand];
    if (amplitude == 0)
    {
        xrStopHapticFeedback(g_session, &hi);
        return;
    }
    XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
    vib.duration = 100000000;  // 0.1 s
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    vib.amplitude = amplitude;
    xrApplyHapticFeedback(g_session, &hi, (const XrHapticBaseHeader*)&vib);
}

float HapticsAmplitude(int hand) { return g_vibration[hand].amplitude; }

EyeFov Fov(int eye)
{
    const XrFovf& f = g_views[eye].fov;
    return { tanf(f.angleLeft), tanf(f.angleRight), tanf(f.angleUp), tanf(f.angleDown) };
}

// A quad facing the recenter origin's straight-ahead direction, `distance` in front of it and
// `height` above it.
XrPosef PanelPose(float distance, float height)
{
    XrPosef p;
    p.orientation = g_originRot;
    const float ahead[3] = { 0, height, -distance };
    float a[3];
    Rotate(g_originRot, ahead, a);
    p.position = { g_originPos[0] + a[0], g_originPos[1] + a[1], g_originPos[2] + a[2] };
    return p;
}

// The HUD panel's swapchain, made anew for another size.
bool EnsureHudSwapchain(unsigned w, unsigned h)
{
    if (g_hudSc.handle && (g_hudSc.w != w || g_hudSc.h != h)) DestroySwapchain(g_hudSc);
    return g_hudSc.handle || CreateSwapchain(g_hudSc, w, h, "HUD");
}

// The flat screen in front of the recenter origin, showing its swapchain's last released image.
XrCompositionLayerQuad ScreenQuad()
{
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.space = g_local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_screenSc.handle;
    quad.subImage.imageRect = { { 0, 0 }, { (int32_t)g_screenSc.w, (int32_t)g_screenSc.h } };
    quad.pose = PanelPose(g_cfg.vrScreenDistance, 0.0f);
    quad.size = { g_cfg.vrScreenWidth, g_cfg.vrScreenWidth * (float)g_screenSc.h / (float)g_screenSc.w };
    return quad;
}

void SubmitEyes(IDirect3DTexture9* left, IDirect3DTexture9* right, IDirect3DTexture9* hud, unsigned hudW, unsigned hudH)
{
    if (!g_frameOpen) return;
    if (hud && !EnsureHudSwapchain(hudW, hudH)) hud = nullptr;
    IDirect3DTexture9* tex[3] = { left, right, hud };
    Swapchain* chains[3] = { &g_eyeSc[0], &g_eyeSc[1], &g_hudSc };
    const uint32_t offsets[3][2] = { { g_eyeOffset[0][0], g_eyeOffset[0][1] }, { g_eyeOffset[1][0], g_eyeOffset[1][1] }, { 0, 0 } };
    if (!g_eyeSc[0].handle || !CopyToSwapchains(tex, chains, hud ? 3 : 2, offsets))
    {
        EndFrame(nullptr);
        return;
    }
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    for (int i = 0; i < 2; i++)
    {
        pv[i].pose = g_views[i].pose;
        pv[i].fov = g_views[i].fov;
        pv[i].subImage.swapchain = g_eyeSc[i].handle;
        pv[i].subImage.imageRect = { { 0, 0 }, { (int32_t)g_eyeW, (int32_t)g_eyeH } };
    }
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = g_local;
    layer.viewCount = 2;
    layer.views = pv;
    // The HUD texture holds premultiplied colour over a transparent background.
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_hudSc.handle;
    quad.subImage.imageRect = { { 0, 0 }, { (int32_t)hudW, (int32_t)hudH } };
    quad.pose = PanelPose(g_cfg.vrHudDistance, g_cfg.vrHudHeight);
    quad.size = { g_cfg.vrHudWidth, g_cfg.vrHudWidth * (float)hudH / (float)hudW };
    EndFrame((const XrCompositionLayerBaseHeader*)&layer, hud ? (const XrCompositionLayerBaseHeader*)&quad : nullptr);
}

void SubmitScreen(IDirect3DTexture9* screen, unsigned w, unsigned h)
{
    if (!g_frameOpen) return;
    if (g_screenSc.handle && (g_screenSc.w != w || g_screenSc.h != h)) DestroySwapchain(g_screenSc);
    if (!g_screenSc.handle && !CreateSwapchain(g_screenSc, w, h, "screen"))
    {
        EndFrame(nullptr);
        return;
    }
    Swapchain* chains[1] = { &g_screenSc };
    if (!CopyToSwapchains(&screen, chains, 1))
    {
        EndFrame(nullptr);
        return;
    }
    XrCompositionLayerQuad quad = ScreenQuad();
    EndFrame((const XrCompositionLayerBaseHeader*)&quad);
}

void SubmitLastScreen(unsigned hudW, unsigned hudH)
{
    if (!g_frameOpen) return;
    if (hudW && hudH) EnsureHudSwapchain(hudW, hudH);
    if (!g_screenSc.released)
    {
        EndFrame(nullptr);
        return;
    }
    XrCompositionLayerQuad quad = ScreenQuad();
    EndFrame((const XrCompositionLayerBaseHeader*)&quad);
}

void PrepareHud(unsigned w, unsigned h)
{
    static unsigned tried[2];  // (once a size: a failure is not retried every frame)
    if (!g_session || !w || !h || (tried[0] == w && tried[1] == h)) return;
    tried[0] = w, tried[1] = h;
    EnsureHudSwapchain(w, h);
}

void EndFrameEmpty()
{
    if (g_frameOpen) EndFrame(nullptr);
}

void Recenter() { g_recenterPending = true; }

SubmitTimes TakeSubmitTimes()
{
    SubmitTimes t = g_times;
    g_times = SubmitTimes();
    return t;
}

}  // namespace xr
