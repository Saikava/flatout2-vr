// wineopenxr.dll: hands DXVK the Vulkan extensions the OpenXR runtime needs.
//
// DXVK (src/dxvk/dxvk_openxr.cpp) does LoadLibrary("wineopenxr.dll") while it creates its Vulkan
// instance and enables the space-separated extension lists returned by the two __wineopenxr_*
// functions on its instance and device. Under Wine that module is Wine's OpenXR bridge; here
// FO2VR.asi loads this one from scripts\ before Direct3D starts (so DXVK finds it already loaded)
// and fills in the lists from xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::string g_instanceExts, g_deviceExts;

// Same convention as the OpenXR two-call idiom: the length includes the terminating zero.
int Copy(const std::string& list, uint32_t capacity, uint32_t* length, char* out)
{
    if (list.empty() || !length) return -1;  // nothing to add (DXVK then enables nothing extra)
    *length = (uint32_t)list.size() + 1;
    if (!out || capacity == 0) return 0;
    if (capacity < *length) return -1;
    memcpy(out, list.c_str(), *length);
    return 0;
}

}  // namespace

extern "C" {

int WINAPI GetVulkanInstanceExtensions(uint32_t capacity, uint32_t* length, char* out)
{
    return Copy(g_instanceExts, capacity, length, out);
}

int WINAPI GetVulkanDeviceExtensions(uint32_t capacity, uint32_t* length, char* out)
{
    return Copy(g_deviceExts, capacity, length, out);
}

void SetVulkanExtensions(const char* instanceExts, const char* deviceExts)
{
    g_instanceExts = instanceExts ? instanceExts : "";
    g_deviceExts = deviceExts ? deviceExts : "";
}

}  // extern "C"
