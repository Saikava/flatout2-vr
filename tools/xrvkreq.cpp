// Prints the Vulkan instance/device extensions the active OpenXR runtime requires (XR_KHR_vulkan_enable).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
int main()
{
    const char* ext = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "fo2vr vkreq");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &ext;
    XrInstance inst;
    if (XR_FAILED(xrCreateInstance(&ci, &inst))) { printf("no instance\n"); return 1; }
    XrSystemGetInfo si = { XR_TYPE_SYSTEM_GET_INFO };
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys; XrResult r = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    for (int i = 0; i < 60 && r == XR_ERROR_FORM_FACTOR_UNAVAILABLE; i++) { r = xrGetSystem(inst, &si, &sys); if (r) Sleep(500); }
    if (XR_FAILED(r)) { printf("no system %d\n", r); return 1; }
    PFN_xrGetVulkanInstanceExtensionsKHR gi; PFN_xrGetVulkanDeviceExtensionsKHR gd; PFN_xrGetVulkanGraphicsRequirementsKHR gr;
    xrGetInstanceProcAddr(inst, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&gi);
    xrGetInstanceProcAddr(inst, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&gd);
    xrGetInstanceProcAddr(inst, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&gr);
    XrGraphicsRequirementsVulkanKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    gr(inst, sys, &req);
    printf("vulkan api min %u.%u max %u.%u\n", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
           XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    uint32_t n = 0; gi(inst, sys, 0, &n, nullptr); std::vector<char> b(n + 1); gi(inst, sys, n, &n, b.data());
    printf("instance: %s\n", b.data());
    n = 0; gd(inst, sys, 0, &n, nullptr); b.assign(n + 1, 0); gd(inst, sys, n, &n, b.data());
    printf("device: %s\n", b.data());
    xrDestroyInstance(inst);
    return 0;
}
