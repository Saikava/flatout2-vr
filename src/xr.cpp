#include "xr.h"

#include "background.h"
#include "dialog.h"
#include "dxvk_interop.h"
#include "util.h"

#include <windows.h>
#include <d3d9.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace xr {
namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
bool g_running;

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
};
Swapchain g_eyeSc[2], g_screenSc, g_hudSc;
int64_t g_format;
uint32_t g_eyeW, g_eyeH;
uint32_t g_eyeOffset[2][2];  // where the headset's picture begins in each eye's texture (SetEyeOffsets)

XrFrameState g_frameState;
bool g_frameOpen;
XrView g_views[2];
bool g_viewsValid;

// Recenter origin (OpenXR LOCAL space): position and yaw.
bool g_recenterPending = true;
float g_originPos[3];
float g_originYaw;

bool g_loggedImage;
SubmitTimes g_times;

// The controllers' actions (CreateInput) and their state for the open frame (ReadInput).
XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_aimAction, g_triggerAction, g_gripAction, g_buttonAction, g_stickAction;
XrPath g_handPath[2];
XrSpace g_aimSpace[2];
Controller g_hands[2], g_testHands[2];

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

// The runtime may use DXVK's queue inside these calls; DXVK must not submit meanwhile.
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

// Rotates about +y by -g_originYaw, then converts OpenXR axes (right-handed, -z forward) to the
// game's (left-handed, +z forward).
void ToGame(const float v[3], float out[3])
{
    float c = cosf(-g_originYaw), s = sinf(-g_originYaw);
    out[0] = v[0] * c + v[2] * s;
    out[1] = v[1];
    out[2] = -(-v[0] * s + v[2] * c);
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

void ApplyRecenter()
{
    const XrPosef& p0 = g_views[0].pose;
    const XrPosef& p1 = g_views[1].pose;
    g_originPos[0] = 0.5f * (p0.position.x + p1.position.x);
    g_originPos[1] = 0.5f * (p0.position.y + p1.position.y);
    g_originPos[2] = 0.5f * (p0.position.z + p1.position.z);
    const float fwd[3] = { 0, 0, -1 };  // OpenXR forward
    float f[3];
    Rotate(p0.orientation, fwd, f);
    g_originYaw = atan2f(-f[0], -f[2]);
    Log("xr: recentered at %.2f %.2f %.2f, yaw %.1f deg", g_originPos[0], g_originPos[1], g_originPos[2], g_originYaw * 57.29578f);
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
    if (!Check(xrCreateSwapchain(g_session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
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
        Check(xrWaitSwapchainImage(chains[i]->handle, &wi), "xrWaitSwapchainImage");
        g_times.wait += NowSeconds() - a1;
    }

    g_slot = (g_slot + 1) % 3;
    Slot& s = g_slots[g_slot];
    double f0 = NowSeconds();
    vk.WaitForFences(g_vkDevice, 1, &s.fence, VK_TRUE, 1000000000ull);
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
            xrReleaseSwapchainImage(chains[i]->handle, &ri);
        }
    }
    g_times.submit += NowSeconds() - f1;
    g_times.count++;
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

// The controllers: one action set with the aim pose, the trigger, the grip, B/Y and the thumbstick of
// either hand, bound for the Touch controllers (and the trigger and pose of any controller through the
// simple profile).
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
        { &g_triggerAction, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT },
        { &g_gripAction, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT },
        { &g_buttonAction, "reset", "Reset", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &g_stickAction, "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
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
              { g_triggerAction, "/user/hand/left/input/trigger/value" }, { g_triggerAction, "/user/hand/right/input/trigger/value" },
              { g_gripAction, "/user/hand/left/input/squeeze/value" }, { g_gripAction, "/user/hand/right/input/squeeze/value" },
              { g_buttonAction, "/user/hand/left/input/y/click" }, { g_buttonAction, "/user/hand/right/input/b/click" },
              { g_stickAction, "/user/hand/left/input/thumbstick" }, { g_stickAction, "/user/hand/right/input/thumbstick" } });
    suggest("/interaction_profiles/khr/simple_controller",
            { { g_aimAction, "/user/hand/left/input/aim/pose" }, { g_aimAction, "/user/hand/right/input/aim/pose" },
              { g_triggerAction, "/user/hand/left/input/select/click" }, { g_triggerAction, "/user/hand/right/input/select/click" } });
    for (int h = 0; h < 2; h++)
    {
        XrActionSpaceCreateInfo ci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ci.action = g_aimAction;
        ci.subactionPath = g_handPath[h];
        ci.poseInActionSpace.orientation.w = 1.0f;
        Check(xrCreateActionSpace(g_session, &ci, &g_aimSpace[h]), "xrCreateActionSpace");
    }
    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    if (Check(xrAttachSessionActionSets(g_session, &ai), "xrAttachSessionActionSets")) Log("xr: controller actions ready");
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
            XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
            gi.subactionPath = g_handPath[h];
            XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
            gi.action = g_triggerAction;
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &f)) && f.isActive) c.trigger = f.currentState;
            f = { XR_TYPE_ACTION_STATE_FLOAT };
            gi.action = g_gripAction;
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &f)) && f.isActive) c.grip = f.currentState;
            XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
            gi.action = g_buttonAction;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &b)) && b.isActive) c.button = b.currentState != XR_FALSE;
            XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
            gi.action = g_stickAction;
            if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &v)) && v.isActive) c.stick[0] = v.currentState.x, c.stick[1] = v.currentState.y;
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
    Check(r, "xrEndFrame");
    g_frameOpen = false;
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
    const char* exts[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "FlatOut 2 VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "FO2VR");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    std::wstring problem;
    XrResult r = xrCreateInstance(&ci, &instance);
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
    // Runtime choice: the loader reads XR_RUNTIME_JSON when it is loaded.
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
    std::wstring loader = ModuleDir() + L"openxr_loader.dll";
    if (!LoadLibraryW(loader.c_str()))
    {
        Log("xr: cannot load %ls (error %u)", loader.c_str(), (unsigned)GetLastError());
        return false;
    }
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
    if (!Check(xrWaitFrame(g_session, &wi, &g_frameState), "xrWaitFrame")) return false;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult r;
    {
        QueueLock lock;
        r = xrBeginFrame(g_session, &bi);
    }
    if (!Check(r, "xrBeginFrame")) return false;
    g_frameOpen = true;
    return true;
}

bool ShouldRender() { return g_frameOpen && g_frameState.shouldRender; }
double DisplayTime() { return (double)g_frameState.predictedDisplayTime * 1e-9; }
double DisplayPeriod() { return (double)g_frameState.predictedDisplayPeriod * 1e-9; }

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
    float c = cosf(g_originYaw * 0.5f), s = sinf(g_originYaw * 0.5f);
    p.orientation = { 0, s, 0, c };
    p.position = { g_originPos[0] - sinf(g_originYaw) * distance, g_originPos[1] + height, g_originPos[2] - cosf(g_originYaw) * distance };
    return p;
}

void SubmitEyes(IDirect3DTexture9* left, IDirect3DTexture9* right, IDirect3DTexture9* hud, unsigned hudW, unsigned hudH)
{
    if (!g_frameOpen) return;
    if (hud && g_hudSc.handle && (g_hudSc.w != hudW || g_hudSc.h != hudH))
    {
        xrDestroySwapchain(g_hudSc.handle);
        g_hudSc = Swapchain();
    }
    if (hud && !g_hudSc.handle && !CreateSwapchain(g_hudSc, hudW, hudH, "HUD")) hud = nullptr;
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
    if (g_screenSc.handle && (g_screenSc.w != w || g_screenSc.h != h))
    {
        xrDestroySwapchain(g_screenSc.handle);
        g_screenSc = Swapchain();
    }
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
    // A flat screen in front of the recenter origin.
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.space = g_local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_screenSc.handle;
    quad.subImage.imageRect = { { 0, 0 }, { (int32_t)w, (int32_t)h } };
    quad.pose = PanelPose(g_cfg.vrScreenDistance, 0.0f);
    quad.size = { g_cfg.vrScreenWidth, g_cfg.vrScreenWidth * (float)h / (float)w };
    EndFrame((const XrCompositionLayerBaseHeader*)&quad);
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
