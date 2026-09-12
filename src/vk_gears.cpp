#include "vk_gears.h"

#include "math3d.h"
#include "platform.h"
#include "spirv_embedded.h"

// GLFW must not drag in the OpenGL headers: this demo is Vulkan only.
// Defined here rather than on the command line so every build system agrees.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vkg {

namespace {

const int   kFramesInFlight    = 2;
const int   kGearCount         = 3;
const int   kCheckerTextureSize = 256;
const int   kCheckerCells      = 8;

const float kClearColor[4] = { 0.045f, 0.052f, 0.070f, 1.0f };

const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation";

struct SceneUniforms {
    float viewProj[16];
    float lightDir[4];
    float cameraPos[4];
    float params[4];
};

struct PushConstants {
    float model[16];
    float color[4];
    float uvParams[4];
};

static_assert(sizeof(PushConstants) == 96,
              "push constant block must fit in the 128 bytes every Vulkan implementation guarantees");

// ---------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------

double steadySeconds() {
    const std::chrono::duration<double> since(
        std::chrono::steady_clock::now().time_since_epoch());
    return since.count();
}

bool directoryExists(const std::string& path) {
    if (path.empty()) { return false; }
    std::FILE* file = std::fopen((path + "/VkLayer_khronos_validation.json").c_str(), "rb");
    if (file != 0) { std::fclose(file); return true; }
    return false;
}

std::string apiVersionString(uint32_t version) {
    return diag::format("%u.%u.%u", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                        VK_VERSION_PATCH(version));
}

std::string deviceTypeString(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU / software";
        default:                                     return "other";
    }
}

std::string vendorName(uint32_t vendorId) {
    switch (vendorId) {
        case 0x1002: return "AMD";
        case 0x1010: return "Imagination";
        case 0x10DE: return "NVIDIA";
        case 0x13B5: return "ARM";
        case 0x14E4: return "Broadcom";
        case 0x1AE0: return "Google";
        case 0x5143: return "Qualcomm";
        case 0x8086: return "Intel";
        case 0x10005: return "Mesa";
        default:     return "unknown";
    }
}

// Driver versions are packed differently by each vendor; decode the common ones.
std::string driverVersionString(uint32_t vendorId, uint32_t version) {
    switch (vendorId) {
        case 0x10DE: // NVIDIA
            return diag::format("%u.%u.%u.%u",
                                (version >> 22) & 0x3ff, (version >> 14) & 0x0ff,
                                (version >> 6) & 0x0ff, (version) & 0x003f);
        case 0x8086: // Intel
            return diag::format("%u.%u", version >> 14, version & 0x3fff);
        case 0x1002: // AMD
        case 0x1022:
            return diag::format("%u.%u.%u",
                                (version >> 22) & 0x3ff, (version >> 12) & 0x3ff, version & 0xfff);
        default:
            return diag::format("%u.%u.%u",
                                VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                                VK_VERSION_PATCH(version));
    }
}

std::string formatString(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:            return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:             return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM:            return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB:             return "B8G8R8A8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:  return "A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_D16_UNORM:                 return "D16_UNORM";
        case VK_FORMAT_D24_UNORM_S8_UINT:         return "D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT:                return "D32_SFLOAT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT:        return "D32_SFLOAT_S8_UINT";
        default:                                  return diag::format("VkFormat(%d)", static_cast<int>(format));
    }
}

std::string presentModeString(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE (no vsync, may tear)";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX (no vsync, no tearing)";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO (vsync)";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR: return "SHARED_DEMAND_REFRESH";
        case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR: return "SHARED_CONTINUOUS_REFRESH";
        default:                               return diag::format("VkPresentModeKHR(%d)", static_cast<int>(mode));
    }
}

std::string colorSpaceString(VkColorSpaceKHR space) {
    switch (space) {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "SRGB_NONLINEAR";
        case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "DISPLAY_P3_NONLINEAR";
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "EXTENDED_SRGB_LINEAR";
        case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10_ST2084";
        default: return diag::format("VkColorSpaceKHR(%d)", static_cast<int>(space));
    }
}

std::string queueFamilyString(VkQueueFlags flags) {
    std::string out;
    if (flags & VK_QUEUE_GRAPHICS_BIT) { out += "graphics "; }
    if (flags & VK_QUEUE_COMPUTE_BIT)  { out += "compute "; }
    if (flags & VK_QUEUE_TRANSFER_BIT) { out += "transfer "; }
    if (flags & VK_QUEUE_SPARSE_BINDING_BIT) { out += "sparse "; }
    if (!out.empty() && out[out.size() - 1] == ' ') { out.erase(out.size() - 1); }
    return out.empty() ? std::string("none") : out;
}

std::string sampleCountString(VkSampleCountFlagBits samples) {
    return diag::format("%dx", static_cast<int>(samples));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) {
    VulkanGears::ValidationCounters* counters =
        static_cast<VulkanGears::ValidationCounters*>(userData);

    const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (counters != 0) {
        if (isError) { counters->errors += 1; }
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { counters->warnings += 1; }
    }

    const char* kind = "validation";
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) { kind = "performance"; }
    else if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) { kind = "general"; }

    std::string message = diag::format("[vk/%s] ", kind);
    if (data != 0) {
        if (data->pMessageIdName != 0) {
            message += data->pMessageIdName;
            message += ": ";
        }
        if (data->pMessage != 0) { message += data->pMessage; }
    }

    if (isError) { diag::logError(message); }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { diag::logWarn(message); }
    else { diag::logDebug(message); }

    return VK_FALSE;
}

// Finds a queue family supporting the given flags, and optionally presentation.
bool findQueueFamily(VkPhysicalDevice device,
                     VkSurfaceKHR surface,
                     VkQueueFlags requiredFlags,
                     bool needPresent,
                     uint32_t& outFamily) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, 0);
    if (count == 0) { return false; }

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, &families[0]);

    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & requiredFlags) != requiredFlags) { continue; }
        if (needPresent) {
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported) != VK_SUCCESS) { continue; }
            if (!supported) { continue; }
        }
        outFamily = i;
        return true;
    }
    return false;
}

bool deviceHasExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, 0, &count, 0) != VK_SUCCESS) { return false; }
    std::vector<VkExtensionProperties> extensions(count);
    if (count > 0 && vkEnumerateDeviceExtensionProperties(device, 0, &count, &extensions[0]) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(extensions[i].extensionName, name) == 0) { return true; }
    }
    return false;
}

bool instanceHasExtension(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(0, &count, 0) != VK_SUCCESS) { return false; }
    std::vector<VkExtensionProperties> extensions(count);
    if (count > 0 && vkEnumerateInstanceExtensionProperties(0, &count, &extensions[0]) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(extensions[i].extensionName, name) == 0) { return true; }
    }
    return false;
}

bool instanceHasLayer(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, 0) != VK_SUCCESS) { return false; }
    std::vector<VkLayerProperties> layers(count);
    if (count > 0 && vkEnumerateInstanceLayerProperties(&count, &layers[0]) != VK_SUCCESS) { return false; }
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(layers[i].layerName, name) == 0) { return true; }
    }
    return false;
}

bool isSrgbFormat(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

bool isBgraFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SNORM;
}

VkFormat findSupportedDepthFormat(VkPhysicalDevice device) {
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(device, candidates[i], &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return candidates[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat chooseOffscreenColorFormat(VkPhysicalDevice device) {
    const VkFormat candidates[] = {
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM
    };
    const VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(device, candidates[i], &properties);
        if ((properties.optimalTilingFeatures & needed) == needed) { return candidates[i]; }
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}

VkSampleCountFlagBits chooseSampleCount(const VkPhysicalDeviceProperties& properties, int requested) {
    VkSampleCountFlags counts = properties.limits.framebufferColorSampleCounts &
                                properties.limits.framebufferDepthSampleCounts;
    const VkSampleCountFlagBits ordered[] = {
        VK_SAMPLE_COUNT_64_BIT, VK_SAMPLE_COUNT_32_BIT, VK_SAMPLE_COUNT_16_BIT,
        VK_SAMPLE_COUNT_8_BIT,  VK_SAMPLE_COUNT_4_BIT,  VK_SAMPLE_COUNT_2_BIT
    };
    for (size_t i = 0; i < sizeof(ordered) / sizeof(ordered[0]); ++i) {
        if (static_cast<int>(ordered[i]) <= requested && (counts & ordered[i]) != 0) {
            return ordered[i];
        }
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

} // namespace

// ---------------------------------------------------------------------------
// AppOptions
// ---------------------------------------------------------------------------

AppOptions::AppOptions()
    : width(800),
      height(600),
      vsync(false),
      headless(false),
      frames(0),
      samples(4),
      validation(true),
      gpuIndex(-1),
      gpuNameFilter(),
      fpsIntervalMs(1000),
      backfaceCulling(true),
      checkerScale(1),
      speed(1.15f),
      listDevices(false),
      verbose(false) {}

// ---------------------------------------------------------------------------
// listVulkanDevices
// ---------------------------------------------------------------------------

bool listVulkanDevices(std::string& error) {
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "vulkangears";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "vulkangears";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = vkCreateInstance(&createInfo, 0, &instance);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateInstance failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, 0);
    diag::logRaw(diag::format("Vulkan devices: %u", deviceCount));

    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (deviceCount > 0) { vkEnumeratePhysicalDevices(instance, &deviceCount, &devices[0]); }

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(devices[i], &memory);

        diag::logRaw(diag::format("  [%u] %s", i, properties.deviceName));
        diag::logRaw(diag::format("      type %s, API %s, driver %s (%u.%u.%u), vendor %s (0x%04x)",
                                  deviceTypeString(properties.deviceType).c_str(),
                                  apiVersionString(properties.apiVersion).c_str(),
                                  driverVersionString(properties.vendorID, properties.driverVersion).c_str(),
                                  VK_VERSION_MAJOR(properties.driverVersion),
                                  VK_VERSION_MINOR(properties.driverVersion),
                                  VK_VERSION_PATCH(properties.driverVersion),
                                  vendorName(properties.vendorID).c_str(),
                                  properties.vendorID));
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, 0);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount > 0) { vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, &families[0]); }
        for (uint32_t f = 0; f < familyCount; ++f) {
            diag::logRaw(diag::format("      queue family %u: %u queues [%s] timestamp bits %u",
                                      f, families[f].queueCount,
                                      queueFamilyString(families[f].queueFlags).c_str(),
                                      families[f].timestampValidBits));
        }
        for (uint32_t h = 0; h < memory.memoryHeapCount; ++h) {
            diag::logRaw(diag::format("      heap %u: %s%s", h,
                                      diag::humanBytes(memory.memoryHeaps[h].size).c_str(),
                                      (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                                          ? " (device local)" : ""));
        }
    }

    vkDestroyInstance(instance, 0);
    return true;
}

// ---------------------------------------------------------------------------
// construction / destruction
// ---------------------------------------------------------------------------

VulkanGears::VulkanGears()
    : windowed_(false),
      initialized_(false),
      framebufferResized_(false),
      window_(0),
      instance_(VK_NULL_HANDLE),
      debugMessenger_(VK_NULL_HANDLE),
      validationEnabled_(false),
      instanceApiVersion_(VK_API_VERSION_1_0),
      loaderApiVersion_(VK_API_VERSION_1_0),
      canQueryProperties2_(false),
      surface_(VK_NULL_HANDLE),
      physicalDevice_(VK_NULL_HANDLE),
      hasMemoryBudget_(false),
      hasDriverProperties_(false),
      physicalDeviceCount_(0),
      deviceIndex_(0),
      graphicsQueueFamily_(0),
      presentQueueFamily_(0),
      separatePresentQueue_(false),
      graphicsQueue_(VK_NULL_HANDLE),
      presentQueue_(VK_NULL_HANDLE),
      device_(VK_NULL_HANDLE),
      samplerAnisotropy_(false),
      swapchain_(VK_NULL_HANDLE),
      colorFormat_(VK_FORMAT_UNDEFINED),
      colorSpace_(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR),
      presentMode_(VK_PRESENT_MODE_FIFO_KHR),
      swapchainImageCount_(0),
      depthFormat_(VK_FORMAT_UNDEFINED),
      sampleCount_(VK_SAMPLE_COUNT_1_BIT),
      msaaActive_(false),
      depthImage_(VK_NULL_HANDLE),
      depthMemory_(VK_NULL_HANDLE),
      depthImageView_(VK_NULL_HANDLE),
      msaaImage_(VK_NULL_HANDLE),
      msaaMemory_(VK_NULL_HANDLE),
      msaaImageView_(VK_NULL_HANDLE),
      offscreenImage_(VK_NULL_HANDLE),
      offscreenMemory_(VK_NULL_HANDLE),
      offscreenImageView_(VK_NULL_HANDLE),
      readbackBuffer_(VK_NULL_HANDLE),
      readbackMemory_(VK_NULL_HANDLE),
      readbackMapped_(0),
      readbackSize_(0),
      renderPass_(VK_NULL_HANDLE),
      descriptorSetLayout_(VK_NULL_HANDLE),
      pipelineLayout_(VK_NULL_HANDLE),
      pipeline_(VK_NULL_HANDLE),
      checkerImage_(VK_NULL_HANDLE),
      checkerMemory_(VK_NULL_HANDLE),
      checkerImageView_(VK_NULL_HANDLE),
      checkerSampler_(VK_NULL_HANDLE),
      checkerSize_(kCheckerTextureSize),
      checkerCells_(kCheckerCells),
      checkerLevels_(0),
      descriptorPool_(VK_NULL_HANDLE),
      uniformBuffer_(VK_NULL_HANDLE),
      uniformMemory_(VK_NULL_HANDLE),
      uniformMapped_(0),
      uniformStride_(0),
      uniformBytes_(0),
      commandPool_(VK_NULL_HANDLE),
      currentFrame_(0),
      cameraDirty_(true),
      lastStatusTime_(-1.0),
      framesRendered_(0),
      drawCalls_(0),
      startTime_(-1.0),
      endTime_(-1.0),
      gearTriangleTotal_(0),
      gearVertexTotal_(0) {
    std::memset(&deviceProperties_, 0, sizeof(deviceProperties_));
    std::memset(&memoryProperties_, 0, sizeof(memoryProperties_));
    std::memset(&driverProperties_, 0, sizeof(driverProperties_));
    std::memset(&surfaceCapabilities_, 0, sizeof(surfaceCapabilities_));
    std::memset(cameraPosition_, 0, sizeof(cameraPosition_));
    std::memset(viewProj_, 0, sizeof(viewProj_));
    for (int i = 0; i < kGearCount; ++i) {
        std::memset(&specs_[i], 0, sizeof(GearSpec));
        gearBuffers_[i].vertexBuffer = VK_NULL_HANDLE;
        gearBuffers_[i].vertexMemory = VK_NULL_HANDLE;
        gearBuffers_[i].vertexBytes = 0;
        gearBuffers_[i].indexBuffer = VK_NULL_HANDLE;
        gearBuffers_[i].indexMemory = VK_NULL_HANDLE;
        gearBuffers_[i].indexBytes = 0;
        gearBuffers_[i].indexCount = 0;
    }
    // The gear train: a big driver, a small idler and a medium output gear,
    // all with the same module so the teeth really mesh.
    specs_[0].teeth = 30; specs_[0].module = 1.0f; specs_[0].thickness = 3.4f;
    specs_[0].label = "gear A 30T";
    specs_[0].color[0] = 0.34f; specs_[0].color[1] = 0.56f; specs_[0].color[2] = 1.00f;

    specs_[1].teeth = 14; specs_[1].module = 1.0f; specs_[1].thickness = 3.4f;
    specs_[1].label = "gear B 14T";
    specs_[1].color[0] = 1.00f; specs_[1].color[1] = 0.44f; specs_[1].color[2] = 0.20f;

    specs_[2].teeth = 22; specs_[2].module = 1.0f; specs_[2].thickness = 3.4f;
    specs_[2].label = "gear C 22T";
    specs_[2].color[0] = 0.36f; specs_[2].color[1] = 0.95f; specs_[2].color[2] = 0.46f;
}

VulkanGears::~VulkanGears() { shutdown(); }

// ---------------------------------------------------------------------------
// instance / device
// ---------------------------------------------------------------------------

bool VulkanGears::createInstance(std::string& error) {
    loaderApiVersion_ = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerateVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion != 0) {
        uint32_t version = 0;
        if (enumerateVersion(&version) == VK_SUCCESS && version >= VK_API_VERSION_1_1) {
            loaderApiVersion_ = version;
        }
    }
    // Request 1.1 when the loader can provide it (that makes the *2 property
    // queries core), otherwise stay on 1.0 for maximum compatibility.
    instanceApiVersion_ = (loaderApiVersion_ >= VK_API_VERSION_1_1)
                              ? VK_MAKE_VERSION(1, 1, 0)
                              : VK_API_VERSION_1_0;

    // If the validation layer is not installed system wide, point the loader at
    // the copy that scripts/fetch_deps.sh unpacked next to the build.
    if (options_.validation && !instanceHasLayer(kValidationLayer)) {
        const std::string exeDir = platform::executableDir();
        const char* candidates[] = {
            "/third_party/sysroot/usr/share/vulkan/explicit_layer.d",
            "/../third_party/sysroot/usr/share/vulkan/explicit_layer.d"
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
            const std::string path = exeDir + candidates[i];
            if (directoryExists(path)) {
                platform::setEnvironmentIfUnset("VK_LAYER_PATH", path);
                break;
            }
        }
    }

    std::vector<const char*> extensions;
    std::vector<const char*> layers;

    const bool hasDebugUtils = instanceHasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (hasDebugUtils) { extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

    bool khrProperties2 = false;
    if (instanceApiVersion_ < VK_API_VERSION_1_1) {
        khrProperties2 = instanceHasExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (khrProperties2) {
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
    }
    canQueryProperties2_ = (instanceApiVersion_ >= VK_API_VERSION_1_1) || khrProperties2;

    if (windowed_) {
        uint32_t glfwCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
        if (glfwExtensions == 0) {
            error = "GLFW could not provide the instance extensions needed for a window "
                    "(is a display available? try --headless)";
            return false;
        }
        for (uint32_t i = 0; i < glfwCount; ++i) {
            extensions.push_back(glfwExtensions[i]);
        }
    }

    if (options_.validation) {
        if (instanceHasLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            validationEnabled_ = true;
        } else {
            diag::logWarn("validation layer '" + std::string(kValidationLayer) +
                          "' not found - run scripts/fetch_deps.sh to vendor it, continuing without it");
        }
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "vulkangears";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "vulkangears";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = instanceApiVersion_;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.empty() ? 0 : &extensions[0];
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.empty() ? 0 : &layers[0];

    const VkResult result = vkCreateInstance(&createInfo, 0, &instance_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateInstance failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    enabledLayers_.clear();
    for (size_t i = 0; i < layers.size(); ++i) { enabledLayers_.push_back(layers[i]); }
    enabledInstanceExtensions_.clear();
    for (size_t i = 0; i < extensions.size(); ++i) { enabledInstanceExtensions_.push_back(extensions[i]); }

    if (hasDebugUtils) {
        PFN_vkCreateDebugUtilsMessengerEXT createMessenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger != 0) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            if (options_.verbose) {
                messengerInfo.messageSeverity |=
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
            }
            messengerInfo.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = &debugUtilsCallback;
            messengerInfo.pUserData = &validationCounters_;
            if (createMessenger(instance_, &messengerInfo, 0, &debugMessenger_) != VK_SUCCESS) {
                debugMessenger_ = VK_NULL_HANDLE;
            }
        }
    }

    return true;
}

bool VulkanGears::createSurface(GLFWwindow* window, std::string& error) {
    const VkResult result = glfwCreateWindowSurface(instance_, window, 0, &surface_);
    if (result != VK_SUCCESS) {
        error = diag::format("glfwCreateWindowSurface failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanGears::pickPhysicalDevice(std::string& error) {
    uint32_t deviceCount = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance_, &deviceCount, 0);
    if (result != VK_SUCCESS || deviceCount == 0) {
        error = "no Vulkan capable device found (is a driver installed?)";
        return false;
    }
    physicalDeviceCount_ = deviceCount;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(instance_, &deviceCount, &devices[0]);
    if (result != VK_SUCCESS) {
        error = diag::format("vkEnumeratePhysicalDevices failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    int bestIndex = -1;
    int bestScore = -1;

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        if (options_.gpuIndex >= 0 && static_cast<int>(i) != options_.gpuIndex) { continue; }
        if (!options_.gpuNameFilter.empty() &&
            std::string(properties.deviceName).find(options_.gpuNameFilter) == std::string::npos) {
            continue;
        }
        if (windowed_) {
            if (!deviceHasExtension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { continue; }
            uint32_t family = 0;
            if (!findQueueFamily(devices[i], surface_, VK_QUEUE_GRAPHICS_BIT, true, family)) { continue; }
        } else {
            uint32_t family = 0;
            if (!findQueueFamily(devices[i], VK_NULL_HANDLE, VK_QUEUE_GRAPHICS_BIT, false, family)) { continue; }
        }

        int score = 1;
        switch (properties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 500; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 400; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 300; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            score = 200; break;
            default:                                     score = 100; break;
        }
        if (static_cast<int>(i) == options_.gpuIndex) { score += 1000; }

        if (score > bestScore) {
            bestScore = score;
            bestIndex = static_cast<int>(i);
        }
    }

    if (bestIndex < 0) {
        error = "no usable Vulkan device matched the selection";
        return false;
    }

    physicalDevice_ = devices[static_cast<uint32_t>(bestIndex)];
    deviceIndex_ = static_cast<uint32_t>(bestIndex);
    vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties_);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);

    if (!findQueueFamily(physicalDevice_, windowed_ ? surface_ : VK_NULL_HANDLE,
                         VK_QUEUE_GRAPHICS_BIT, windowed_, graphicsQueueFamily_)) {
        error = "could not find a graphics queue family";
        return false;
    }
    presentQueueFamily_ = graphicsQueueFamily_;
    separatePresentQueue_ = false;
    if (windowed_) {
        uint32_t presentFamily = 0;
        if (findQueueFamily(physicalDevice_, surface_, 0, true, presentFamily) &&
            presentFamily != graphicsQueueFamily_) {
            presentQueueFamily_ = presentFamily;
            separatePresentQueue_ = true;
        }
    }

    hasMemoryBudget_ = deviceHasExtension(physicalDevice_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    hasDriverProperties_ = deviceHasExtension(physicalDevice_, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    if (hasDriverProperties_ && canQueryProperties2_) {
        VkPhysicalDeviceDriverProperties driverInfo = {};
        driverInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2 = {};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &driverInfo;
        vkGetPhysicalDeviceProperties2(physicalDevice_, &properties2);
        if (driverInfo.driverName[0] != '\0') { driverProperties_ = driverInfo; }
        else { hasDriverProperties_ = false; }
    } else {
        hasDriverProperties_ = false;
    }

    return true;
}

bool VulkanGears::createLogicalDevice(std::string& error) {
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;

    uint32_t families[2] = { graphicsQueueFamily_, presentQueueFamily_ };
    const uint32_t familyCount = separatePresentQueue_ ? 2u : 1u;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkDeviceQueueCreateInfo queueInfo = {};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = families[i];
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        queueInfos.push_back(queueInfo);
    }

    std::vector<const char*> extensions;
    if (windowed_) { extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME); }
    if (hasMemoryBudget_) { extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME); }
    if (hasDriverProperties_) { extensions.push_back(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME); }

    VkPhysicalDeviceFeatures available;
    vkGetPhysicalDeviceFeatures(physicalDevice_, &available);

    VkPhysicalDeviceFeatures enabled = {};
    if (available.samplerAnisotropy) {
        enabled.samplerAnisotropy = VK_TRUE;
        samplerAnisotropy_ = true;
    }

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = &queueInfos[0];
    createInfo.pEnabledFeatures = &enabled;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.empty() ? 0 : &extensions[0];

    const VkResult result = vkCreateDevice(physicalDevice_, &createInfo, 0, &device_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateDevice failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    enabledDeviceExtensions_.clear();
    for (size_t i = 0; i < extensions.size(); ++i) { enabledDeviceExtensions_.push_back(extensions[i]); }

    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    if (separatePresentQueue_) {
        vkGetDeviceQueue(device_, presentQueueFamily_, 0, &presentQueue_);
    } else {
        presentQueue_ = graphicsQueue_;
    }
    return true;
}

// ---------------------------------------------------------------------------
// presentation targets
// ---------------------------------------------------------------------------

bool VulkanGears::createSwapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR capabilities;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);
    if (result != VK_SUCCESS) {
        error = diag::format("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (%d)", static_cast<int>(result));
        return false;
    }
    surfaceCapabilities_ = capabilities;

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, 0);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount > 0) { vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, &formats[0]); }

    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR() : formats[0];
    {
        bool found = false;
        for (size_t i = 0; i < formats.size() && !found; ++i) {
            if ((formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
                 formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = formats[i];
                found = true;
            }
        }
        for (size_t i = 0; i < formats.size() && !found; ++i) {
            if (isSrgbFormat(formats[i].format)) { chosen = formats[i]; found = true; }
        }
    }
    if (formatCount == 0) {
        chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
        chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, 0);
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (modeCount > 0) { vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, &modes[0]); }

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!options_.vsync) {
        for (size_t i = 0; i < modes.size(); ++i) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = modes[i]; break; }
        }
        if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
            for (size_t i = 0; i < modes.size(); ++i) {
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) { presentMode = modes[i]; break; }
            }
        }
    }

    int width = options_.width;
    int height = options_.height;
    if (windowed_) {
        glfwGetFramebufferSize(window_, &width, &height);
    } else {
        width = options_.width;
        height = options_.height;
    }

    VkExtent2D extent;
    if (capabilities.currentExtent.width != 0xFFFFFFFFu) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = static_cast<uint32_t>(width);
        extent.height = static_cast<uint32_t>(height);
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, extent.width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, extent.height));
    }
    if (extent.width == 0) { extent.width = static_cast<uint32_t>(options_.width); }
    if (extent.height == 0) { extent.height = static_cast<uint32_t>(options_.height); }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
            compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
            compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        } else {
            compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }
    }

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosen.format;
    createInfo.imageColorSpace = chosen.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = compositeAlpha;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    const uint32_t indices[2] = { graphicsQueueFamily_, presentQueueFamily_ };
    if (separatePresentQueue_) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = indices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    result = vkCreateSwapchainKHR(device_, &createInfo, 0, &swapchain_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateSwapchainKHR failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, 0);
    swapchainImages_.resize(actualCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, &swapchainImages_[0]);

    colorFormat_ = chosen.format;
    colorSpace_ = chosen.colorSpace;
    colorExtent_ = extent;
    presentMode_ = presentMode;
    swapchainImageCount_ = actualCount;
    return true;
}

bool VulkanGears::createSwapchainImageViews(std::string& error) {
    (void)error;
    swapchainImageViews_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        swapchainImageViews_[i] = createImageView(swapchainImages_[i], colorFormat_,
                                                  VK_IMAGE_ASPECT_COLOR_BIT, 1);
        if (swapchainImageViews_[i] == VK_NULL_HANDLE) {
            error = "failed to create a swapchain image view";
            return false;
        }
    }
    return true;
}

bool VulkanGears::createOffscreenTarget(int width, int height, std::string& error) {
    colorFormat_ = chooseOffscreenColorFormat(physicalDevice_);
    colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    colorExtent_.width = static_cast<uint32_t>(width);
    colorExtent_.height = static_cast<uint32_t>(height);

    if (!createImage2D(colorExtent_.width, colorExtent_.height, 1, VK_SAMPLE_COUNT_1_BIT,
                       colorFormat_, VK_IMAGE_TILING_OPTIMAL,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       0, "offscreen colour target", offscreenImage_, offscreenMemory_)) {
        error = "could not allocate the offscreen colour image";
        return false;
    }
    offscreenImageView_ = createImageView(offscreenImage_, colorFormat_, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (offscreenImageView_ == VK_NULL_HANDLE) {
        error = "could not create the offscreen colour image view";
        return false;
    }
    return true;
}

bool VulkanGears::createReadbackBuffer(std::string& error) {
    readbackSize_ = static_cast<VkDeviceSize>(colorExtent_.width) *
                    static_cast<VkDeviceSize>(colorExtent_.height) * 4u;
    if (!createBuffer(readbackSize_, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      0, "offscreen readback buffer", readbackBuffer_, readbackMemory_)) {
        error = "could not allocate the offscreen readback buffer";
        return false;
    }
    const VkResult result = vkMapMemory(device_, readbackMemory_, 0, readbackSize_, 0, &readbackMapped_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkMapMemory failed for the readback buffer (%d)", static_cast<int>(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// render pass / pipeline
// ---------------------------------------------------------------------------

bool VulkanGears::createRenderPass(std::string& error) {
    (void)error;
    const bool resolving = msaaActive_;
    const VkImageLayout finalLayout = windowed_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentDescription attachments[3];
    std::memset(attachments, 0, sizeof(attachments));

    // 0: multisampled colour (or the only colour attachment when MSAA is off)
    attachments[0].format = colorFormat_;
    attachments[0].samples = sampleCount_;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = resolving ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                       : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = resolving ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : finalLayout;

    uint32_t attachmentCount = 1;
    if (resolving) {
        attachments[1].format = colorFormat_;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = finalLayout;
        attachmentCount = 2;
    }

    attachments[attachmentCount].format = depthFormat_;
    attachments[attachmentCount].samples = sampleCount_;
    attachments[attachmentCount].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[attachmentCount].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const uint32_t depthIndex = attachmentCount;
    ++attachmentCount;

    // The colour attachment is the multisampled image; the resolve target is
    // referenced *only* through pResolveAttachments (every entry of
    // pColorAttachments must share the same sample count).
    VkAttachmentReference colorRef;
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference resolveRef;
    resolveRef.attachment = resolving ? 1 : VK_ATTACHMENT_UNUSED;
    resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef;
    depthRef.attachment = depthIndex;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = resolving ? &resolveRef : 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = attachmentCount;
    createInfo.pAttachments = attachments;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    const VkResult result = vkCreateRenderPass(device_, &createInfo, 0, &renderPass_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateRenderPass failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanGears::createDepthResources(std::string& error) {
    if (!createImage2D(colorExtent_.width, colorExtent_.height, 1, sampleCount_, depthFormat_,
                       VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "depth buffer",
                       depthImage_, depthMemory_)) {
        error = "could not allocate the depth buffer";
        return false;
    }
    depthImageView_ = createImageView(depthImage_, depthFormat_, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    if (depthImageView_ == VK_NULL_HANDLE) {
        error = "could not create the depth image view";
        return false;
    }
    return true;
}

bool VulkanGears::createMultisampleResources(std::string& error) {
    destroyMultisampleResources();
    if (!msaaActive_) { return true; }

    if (!createImage2D(colorExtent_.width, colorExtent_.height, 1, sampleCount_, colorFormat_,
                       VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "MSAA colour target",
                       msaaImage_, msaaMemory_)) {
        error = "could not allocate the multisampled colour image";
        return false;
    }
    msaaImageView_ = createImageView(msaaImage_, colorFormat_, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (msaaImageView_ == VK_NULL_HANDLE) {
        error = "could not create the multisampled colour image view";
        return false;
    }
    return true;
}

bool VulkanGears::createFramebuffers(std::string& error) {
    (void)error;
    const size_t count = windowed_ ? swapchainImageViews_.size() : 1u;
    framebuffers_.resize(count, VK_NULL_HANDLE);

    for (size_t i = 0; i < count; ++i) {
        VkImageView views[3];
        uint32_t viewCount = 0;
        if (msaaActive_) {
            views[viewCount++] = msaaImageView_;
            views[viewCount++] = windowed_ ? swapchainImageViews_[i] : offscreenImageView_;
        } else {
            views[viewCount++] = windowed_ ? swapchainImageViews_[i] : offscreenImageView_;
        }
        views[viewCount++] = depthImageView_;

        VkFramebufferCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = viewCount;
        createInfo.pAttachments = views;
        createInfo.width = colorExtent_.width;
        createInfo.height = colorExtent_.height;
        createInfo.layers = 1;

        const VkResult result = vkCreateFramebuffer(device_, &createInfo, 0, &framebuffers_[i]);
        if (result != VK_SUCCESS) {
            error = diag::format("vkCreateFramebuffer failed (VkResult %d)", static_cast<int>(result));
            return false;
        }
    }
    return true;
}

bool VulkanGears::createDescriptorSetLayout(std::string& error) {
    (void)error;
    VkDescriptorSetLayoutBinding bindings[2];
    std::memset(bindings, 0, sizeof(bindings));

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].pImmutableSamplers = 0;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = 0;

    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = 2;
    createInfo.pBindings = bindings;

    const VkResult result = vkCreateDescriptorSetLayout(device_, &createInfo, 0, &descriptorSetLayout_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateDescriptorSetLayout failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanGears::createGraphicsPipeline(std::string& error) {
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv::gear_vert_spv_size;
    moduleInfo.pCode = spirv::gear_vert_spv;
    VkResult result = vkCreateShaderModule(device_, &moduleInfo, 0, &vertexModule);
    if (result != VK_SUCCESS) {
        error = diag::format("could not create the vertex shader module (VkResult %d)", static_cast<int>(result));
        return false;
    }

    moduleInfo.codeSize = spirv::gear_frag_spv_size;
    moduleInfo.pCode = spirv::gear_frag_spv;
    result = vkCreateShaderModule(device_, &moduleInfo, 0, &fragmentModule);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vertexModule, 0);
        error = diag::format("could not create the fragment shader module (VkResult %d)", static_cast<int>(result));
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2];
    std::memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3];
    std::memset(attributes, 0, sizeof(attributes));
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = static_cast<uint32_t>(offsetof(Vertex, position));
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = static_cast<uint32_t>(offsetof(Vertex, normal));
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = static_cast<uint32_t>(offsetof(Vertex, texCoord));

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(colorExtent_.width);
    viewport.height = static_cast<float>(colorExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent = colorExtent_;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = options_.backfaceCulling ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // The gear meshes are wound counter-clockwise as seen from outside of the
    // solid, which is what VK_FRONT_FACE_COUNTER_CLOCKWISE expects (verified by
    // comparing a culled against an unculled render, and by tests/mesh_check.cpp).
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = sampleCount_;
    multisample.sampleShadingEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable = VK_FALSE;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    result = vkCreatePipelineLayout(device_, &layoutInfo, 0, &pipelineLayout_);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vertexModule, 0);
        vkDestroyShaderModule(device_, fragmentModule, 0);
        error = diag::format("vkCreatePipelineLayout failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, 0, &pipeline_);

    vkDestroyShaderModule(device_, vertexModule, 0);
    vkDestroyShaderModule(device_, fragmentModule, 0);

    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateGraphicsPipelines failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------

uint32_t VulkanGears::pickMemoryType(uint32_t typeBits,
                                     VkMemoryPropertyFlags required,
                                     VkMemoryPropertyFlags preferred) const {
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) { continue; }
        const VkMemoryPropertyFlags flags = memoryProperties_.memoryTypes[i].propertyFlags;
        if ((flags & required) == required && (preferred == 0 || (flags & preferred) == preferred)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) { continue; }
        const VkMemoryPropertyFlags flags = memoryProperties_.memoryTypes[i].propertyFlags;
        if ((flags & required) == required) { return i; }
    }
    return 0xFFFFFFFFu;
}

bool VulkanGears::createBuffer(VkDeviceSize size,
                               VkBufferUsageFlags usage,
                               VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred,
                               const char* ledgerLabel,
                               VkBuffer& buffer,
                               VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, 0, &buffer) != VK_SUCCESS) { return false; }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);

    const uint32_t typeIndex = pickMemoryType(requirements.memoryTypeBits, required, preferred);
    if (typeIndex == 0xFFFFFFFFu) { return false; }

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = typeIndex;

    if (vkAllocateMemory(device_, &allocateInfo, 0, &memory) != VK_SUCCESS) { return false; }
    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) { return false; }

    ledger_.add(ledgerLabel, static_cast<uint64_t>(requirements.size));
    return true;
}

bool VulkanGears::createImage2D(uint32_t width,
                                uint32_t height,
                                uint32_t mipLevels,
                                VkSampleCountFlagBits samples,
                                VkFormat format,
                                VkImageTiling tiling,
                                VkImageUsageFlags usage,
                                VkMemoryPropertyFlags required,
                                VkMemoryPropertyFlags preferred,
                                const char* ledgerLabel,
                                VkImage& image,
                                VkDeviceMemory& memory) {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, 0, &image) != VK_SUCCESS) { return false; }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device_, image, &requirements);

    const uint32_t typeIndex = pickMemoryType(requirements.memoryTypeBits, required, preferred);
    if (typeIndex == 0xFFFFFFFFu) { return false; }

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = typeIndex;

    if (vkAllocateMemory(device_, &allocateInfo, 0, &memory) != VK_SUCCESS) { return false; }
    if (vkBindImageMemory(device_, image, memory, 0) != VK_SUCCESS) { return false; }

    ledger_.add(ledgerLabel, static_cast<uint64_t>(requirements.size));
    return true;
}

VkImageView VulkanGears::createImageView(VkImage image,
                                         VkFormat format,
                                         VkImageAspectFlags aspect,
                                         uint32_t mipLevels) {
    VkImageViewCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = image;
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = format;
    createInfo.subresourceRange.aspectMask = aspect;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = mipLevels;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &createInfo, 0, &view) != VK_SUCCESS) { return VK_NULL_HANDLE; }
    return view;
}

VkCommandBuffer VulkanGears::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VulkanGears::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

void VulkanGears::transitionImageLayout(VkCommandBuffer commandBuffer,
                                        VkImage image,
                                        VkImageLayout oldLayout,
                                        VkImageLayout newLayout,
                                        uint32_t mipLevels) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
                         0, 0, 0, 0, 1, &barrier);
}

bool VulkanGears::createGearBuffers(std::string& error) {
    gearTriangleTotal_ = 0;
    gearVertexTotal_ = 0;

    for (int i = 0; i < kGearCount; ++i) {
        const GearMesh& mesh = train_.meshes[i];
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            error = diag::format("%s: empty mesh", specs_[i].label);
            return false;
        }
        if (mesh.vertices.size() > 65535) {
            error = diag::format("%s: mesh has too many vertices for 16 bit indices", specs_[i].label);
            return false;
        }

        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(mesh.vertices.size()) * sizeof(Vertex);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(mesh.indices.size()) * sizeof(uint16_t);

        if (!createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          "gear vertex buffers", gearBuffers_[i].vertexBuffer, gearBuffers_[i].vertexMemory)) {
            error = diag::format("%s: could not create the vertex buffer", specs_[i].label);
            return false;
        }
        void* mapped = 0;
        if (vkMapMemory(device_, gearBuffers_[i].vertexMemory, 0, vertexBytes, 0, &mapped) != VK_SUCCESS) {
            error = diag::format("%s: could not map the vertex buffer", specs_[i].label);
            return false;
        }
        std::memcpy(mapped, &mesh.vertices[0], static_cast<size_t>(vertexBytes));
        vkUnmapMemory(device_, gearBuffers_[i].vertexMemory);

        if (!createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          "gear index buffers", gearBuffers_[i].indexBuffer, gearBuffers_[i].indexMemory)) {
            error = diag::format("%s: could not create the index buffer", specs_[i].label);
            return false;
        }
        if (vkMapMemory(device_, gearBuffers_[i].indexMemory, 0, indexBytes, 0, &mapped) != VK_SUCCESS) {
            error = diag::format("%s: could not map the index buffer", specs_[i].label);
            return false;
        }
        std::memcpy(mapped, &mesh.indices[0], static_cast<size_t>(indexBytes));
        vkUnmapMemory(device_, gearBuffers_[i].indexMemory);

        gearBuffers_[i].vertexBytes = vertexBytes;
        gearBuffers_[i].indexBytes = indexBytes;
        gearBuffers_[i].indexCount = static_cast<uint32_t>(mesh.indices.size());

        gearTriangleTotal_ += mesh.triangleCount;
        gearVertexTotal_ += mesh.vertices.size();
    }
    return true;
}

bool VulkanGears::createCheckerTexture(std::string& error) {
    std::vector<unsigned char> pixels;
    int levels = 0;
    if (!buildCheckerTexture(checkerSize_, checkerCells_, pixels, levels, error)) { return false; }
    checkerLevels_ = levels;

    if (!createImage2D(static_cast<uint32_t>(checkerSize_), static_cast<uint32_t>(checkerSize_),
                       static_cast<uint32_t>(levels), VK_SAMPLE_COUNT_1_BIT,
                       VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "checker texture",
                       checkerImage_, checkerMemory_)) {
        error = "could not allocate the checker texture image";
        return false;
    }

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(pixels.size());
    if (!createBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      "texture staging buffer", stagingBuffer, stagingMemory)) {
        error = "could not create the texture staging buffer";
        return false;
    }

    void* mapped = 0;
    if (vkMapMemory(device_, stagingMemory, 0, stagingSize, 0, &mapped) != VK_SUCCESS) {
        error = "could not map the texture staging buffer";
        return false;
    }
    std::memcpy(mapped, &pixels[0], static_cast<size_t>(stagingSize));
    vkUnmapMemory(device_, stagingMemory);

    const VkCommandBuffer commandBuffer = beginSingleTimeCommands();
    if (commandBuffer == VK_NULL_HANDLE) {
        error = "could not allocate a command buffer for the texture upload";
        return false;
    }

    transitionImageLayout(commandBuffer, checkerImage_, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(levels));

    std::vector<VkBufferImageCopy> regions(static_cast<size_t>(levels));
    VkDeviceSize offset = 0;
    int levelSize = checkerSize_;
    for (int level = 0; level < levels; ++level) {
        std::memset(&regions[level], 0, sizeof(VkBufferImageCopy));
        regions[level].bufferOffset = offset;
        regions[level].bufferRowLength = 0;
        regions[level].bufferImageHeight = 0;
        regions[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[level].imageSubresource.mipLevel = static_cast<uint32_t>(level);
        regions[level].imageSubresource.baseArrayLayer = 0;
        regions[level].imageSubresource.layerCount = 1;
        regions[level].imageOffset.x = 0;
        regions[level].imageOffset.y = 0;
        regions[level].imageOffset.z = 0;
        regions[level].imageExtent.width = static_cast<uint32_t>(levelSize);
        regions[level].imageExtent.height = static_cast<uint32_t>(levelSize);
        regions[level].imageExtent.depth = 1;

        offset += static_cast<VkDeviceSize>(levelSize) * static_cast<VkDeviceSize>(levelSize) * 4u;
        levelSize /= 2;
    }

    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, checkerImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), &regions[0]);

    transitionImageLayout(commandBuffer, checkerImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, static_cast<uint32_t>(levels));

    endSingleTimeCommands(commandBuffer);

    vkDestroyBuffer(device_, stagingBuffer, 0);
    vkFreeMemory(device_, stagingMemory, 0);

    checkerImageView_ = createImageView(checkerImage_, VK_FORMAT_R8G8B8A8_SRGB,
                                        VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(levels));
    if (checkerImageView_ == VK_NULL_HANDLE) {
        error = "could not create the checker texture view";
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = samplerAnisotropy_ ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = samplerAnisotropy_ ? deviceProperties_.limits.maxSamplerAnisotropy : 1.0f;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(levels);
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;

    if (vkCreateSampler(device_, &samplerInfo, 0, &checkerSampler_) != VK_SUCCESS) {
        error = "could not create the checker sampler";
        return false;
    }
    return true;
}

bool VulkanGears::createUniformBuffers(std::string& error) {
    VkDeviceSize alignment = deviceProperties_.limits.minUniformBufferOffsetAlignment;
    if (alignment == 0) { alignment = 64; }

    uniformStride_ = sizeof(SceneUniforms);
    uniformStride_ = (uniformStride_ + alignment - 1) / alignment * alignment;
    uniformBytes_ = uniformStride_ * static_cast<VkDeviceSize>(kFramesInFlight);

    if (!createBuffer(uniformBytes_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      "scene uniform buffers", uniformBuffer_, uniformMemory_)) {
        error = "could not allocate the scene uniform buffer";
        return false;
    }
    if (vkMapMemory(device_, uniformMemory_, 0, uniformBytes_, 0, &uniformMapped_) != VK_SUCCESS) {
        error = "could not map the scene uniform buffer";
        return false;
    }
    return true;
}

bool VulkanGears::createDescriptorPool(std::string& error) {
    VkDescriptorPoolSize sizes[2];
    std::memset(sizes, 0, sizeof(sizes));
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = static_cast<uint32_t>(kFramesInFlight);
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = static_cast<uint32_t>(kFramesInFlight);

    VkDescriptorPoolCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    createInfo.maxSets = static_cast<uint32_t>(kFramesInFlight);
    createInfo.poolSizeCount = 2;
    createInfo.pPoolSizes = sizes;

    const VkResult result = vkCreateDescriptorPool(device_, &createInfo, 0, &descriptorPool_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateDescriptorPool failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanGears::createDescriptorSets(std::string& error) {
    std::vector<VkDescriptorSetLayout> layouts(static_cast<size_t>(kFramesInFlight), descriptorSetLayout_);

    VkDescriptorSetAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(kFramesInFlight);
    allocateInfo.pSetLayouts = &layouts[0];

    descriptorSets_.resize(static_cast<size_t>(kFramesInFlight), VK_NULL_HANDLE);
    const VkResult result = vkAllocateDescriptorSets(device_, &allocateInfo, &descriptorSets_[0]);
    if (result != VK_SUCCESS) {
        error = diag::format("vkAllocateDescriptorSets failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = uniformBuffer_;
        bufferInfo.offset = uniformStride_ * static_cast<VkDeviceSize>(i);
        bufferInfo.range = sizeof(SceneUniforms);

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = checkerSampler_;
        imageInfo.imageView = checkerImageView_;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2];
        std::memset(writes, 0, sizeof(writes));
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSets_[static_cast<size_t>(i)];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSets_[static_cast<size_t>(i)];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device_, 2, writes, 0, 0);
    }
    return true;
}

bool VulkanGears::createCommandResources(std::string& error) {
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;

    VkResult result = vkCreateCommandPool(device_, &poolInfo, 0, &commandPool_);
    if (result != VK_SUCCESS) {
        error = diag::format("vkCreateCommandPool failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    VkCommandBufferAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<uint32_t>(kFramesInFlight);

    commandBuffers_.resize(static_cast<size_t>(kFramesInFlight), VK_NULL_HANDLE);
    result = vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffers_[0]);
    if (result != VK_SUCCESS) {
        error = diag::format("vkAllocateCommandBuffers failed (VkResult %d)", static_cast<int>(result));
        return false;
    }
    return true;
}

bool VulkanGears::createSyncObjects(std::string& error) {
    (void)error;
    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    imageAvailableSemaphores_.resize(static_cast<size_t>(kFramesInFlight), VK_NULL_HANDLE);
    renderFinishedSemaphores_.resize(static_cast<size_t>(kFramesInFlight), VK_NULL_HANDLE);
    inFlightFences_.resize(static_cast<size_t>(kFramesInFlight), VK_NULL_HANDLE);

    for (int i = 0; i < kFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, 0, &imageAvailableSemaphores_[static_cast<size_t>(i)]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphoreInfo, 0, &renderFinishedSemaphores_[static_cast<size_t>(i)]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, 0, &inFlightFences_[static_cast<size_t>(i)]) != VK_SUCCESS) {
            error = "could not create the frame synchronisation objects";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// init entry points
// ---------------------------------------------------------------------------

bool VulkanGears::initWindowed(GLFWwindow* window, const AppOptions& options, std::string& error) {
    window_ = window;
    windowed_ = true;
    options_ = options;

    if (!createInstance(error)) { return false; }
    if (!createSurface(window, error)) { return false; }
    if (!pickPhysicalDevice(error)) { return false; }
    if (!createLogicalDevice(error)) { return false; }

    if (!createCommandResources(error)) { return false; }

    depthFormat_ = findSupportedDepthFormat(physicalDevice_);
    if (depthFormat_ == VK_FORMAT_UNDEFINED) {
        error = "no supported depth format found";
        return false;
    }
    sampleCount_ = chooseSampleCount(deviceProperties_, options_.samples);
    msaaActive_ = sampleCount_ != VK_SAMPLE_COUNT_1_BIT;

    if (!createSwapchain(error)) { return false; }
    if (!createSwapchainImageViews(error)) { return false; }
    if (!createRenderPass(error)) { return false; }
    if (!createDepthResources(error)) { return false; }
    if (!createMultisampleResources(error)) { return false; }
    if (!createFramebuffers(error)) { return false; }
    if (!createDescriptorSetLayout(error)) { return false; }
    if (!createGraphicsPipeline(error)) { return false; }

    const float jointAngles[2] = { 12.0f, -42.0f };
    if (!buildGearTrain(specs_, jointAngles, options_.speed, train_, error)) { return false; }
    if (!createGearBuffers(error)) { return false; }
    if (!createCheckerTexture(error)) { return false; }
    if (!createUniformBuffers(error)) { return false; }
    if (!createDescriptorPool(error)) { return false; }
    if (!createDescriptorSets(error)) { return false; }
    if (!createSyncObjects(error)) { return false; }

    updateCamera();
    initialized_ = true;
    return true;
}

bool VulkanGears::initHeadless(const AppOptions& options, std::string& error) {
    window_ = 0;
    windowed_ = false;
    options_ = options;

    if (!createInstance(error)) { return false; }
    if (!pickPhysicalDevice(error)) { return false; }
    if (!createLogicalDevice(error)) { return false; }
    if (!createCommandResources(error)) { return false; }

    depthFormat_ = findSupportedDepthFormat(physicalDevice_);
    if (depthFormat_ == VK_FORMAT_UNDEFINED) {
        error = "no supported depth format found";
        return false;
    }
    sampleCount_ = chooseSampleCount(deviceProperties_, options_.samples);
    msaaActive_ = sampleCount_ != VK_SAMPLE_COUNT_1_BIT;

    if (!createOffscreenTarget(options_.width, options_.height, error)) { return false; }
    if (!createReadbackBuffer(error)) { return false; }
    if (!createRenderPass(error)) { return false; }
    if (!createDepthResources(error)) { return false; }
    if (!createMultisampleResources(error)) { return false; }
    if (!createFramebuffers(error)) { return false; }
    if (!createDescriptorSetLayout(error)) { return false; }
    if (!createGraphicsPipeline(error)) { return false; }

    const float jointAngles[2] = { 12.0f, -42.0f };
    if (!buildGearTrain(specs_, jointAngles, options_.speed, train_, error)) { return false; }
    if (!createGearBuffers(error)) { return false; }
    if (!createCheckerTexture(error)) { return false; }
    if (!createUniformBuffers(error)) { return false; }
    if (!createDescriptorPool(error)) { return false; }
    if (!createDescriptorSets(error)) { return false; }
    if (!createSyncObjects(error)) { return false; }

    updateCamera();
    initialized_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// frame production
// ---------------------------------------------------------------------------

void VulkanGears::updateCamera() {
    const float aspect = (colorExtent_.height > 0)
                             ? static_cast<float>(colorExtent_.width) / static_cast<float>(colorExtent_.height)
                             : 1.0f;
    const float fovy = toRadians(42.0f);
    const float tanHalf = std::tan(fovy * 0.5f);

    const float halfWidth = std::max(train_.halfWidth, 1.0f) * 1.03f;
    const float halfHeight = std::max(train_.halfHeight, 1.0f) * 1.03f;

    const float distanceForHeight = halfHeight / tanHalf;
    const float distanceForWidth = halfWidth / (tanHalf * aspect);
    float distance = std::max(distanceForHeight, distanceForWidth) * 1.24f;
    if (distance < 1.0f) { distance = 1.0f; }

    // A three-quarter view: enough tilt to show the extrusion and the rims
    // while keeping the gear faces readable.
    const float yaw = toRadians(-18.0f);
    const float pitch = toRadians(14.0f);
    const Vec3 direction(std::cos(pitch) * std::sin(yaw),
                         std::sin(pitch),
                         std::cos(pitch) * std::cos(yaw));
    const Vec3 eye = direction * distance;

    cameraPosition_[0] = eye.x;
    cameraPosition_[1] = eye.y;
    cameraPosition_[2] = eye.z;

    const Mat4 view = Mat4::lookAt(eye, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 projection = Mat4::perspective(fovy, aspect, distance * 0.05f, distance * 6.0f);
    const Mat4 viewProjection = projection * view;
    std::memcpy(viewProj_, viewProjection.m, sizeof(viewProj_));
    cameraDirty_ = false;
}

void VulkanGears::updateSceneUniforms(uint32_t frameIndex, double time) {
    if (uniformMapped_ == 0) { return; }

    SceneUniforms uniforms;
    std::memset(&uniforms, 0, sizeof(uniforms));
    std::memcpy(uniforms.viewProj, viewProj_, sizeof(uniforms.viewProj));

    // Slowly orbiting key light so the checkers and the extrusion keep moving.
    const float t = static_cast<float>(time);
    const Vec3 light = normalize(Vec3(0.42f * std::cos(t * 0.31f),
                                      0.55f + 0.20f * std::sin(t * 0.23f),
                                      0.90f));
    uniforms.lightDir[0] = light.x;
    uniforms.lightDir[1] = light.y;
    uniforms.lightDir[2] = light.z;
    uniforms.lightDir[3] = 0.0f;

    uniforms.cameraPos[0] = cameraPosition_[0];
    uniforms.cameraPos[1] = cameraPosition_[1];
    uniforms.cameraPos[2] = cameraPosition_[2];
    uniforms.cameraPos[3] = 1.0f;

    uniforms.params[0] = 0.30f; // ambient
    uniforms.params[1] = 0.55f; // specular strength
    uniforms.params[2] = t;
    uniforms.params[3] = 0.0f;

    char* destination = static_cast<char*>(uniformMapped_) + uniformStride_ * frameIndex;
    std::memcpy(destination, &uniforms, sizeof(uniforms));
}

void VulkanGears::writePushConstants(VkCommandBuffer commandBuffer, uint32_t gearIndex, double time) {
    const GearPlacement& placement = train_.gears[gearIndex];
    const float t = static_cast<float>(time);
    const float angle = placement.phase + placement.angularSpeed * t;

    const Mat4 model = Mat4::translation(Vec3(placement.position[0], placement.position[1], 0.0f)) *
                       Mat4::rotationZ(angle);

    PushConstants constants;
    std::memset(&constants, 0, sizeof(constants));
    std::memcpy(constants.model, model.m, sizeof(constants.model));
    constants.color[0] = specs_[gearIndex].color[0];
    constants.color[1] = specs_[gearIndex].color[1];
    constants.color[2] = specs_[gearIndex].color[2];
    constants.color[3] = 1.0f;
    constants.uvParams[0] = static_cast<float>(options_.checkerScale);
    constants.uvParams[1] = static_cast<float>(options_.checkerScale);
    constants.uvParams[2] = 0.37f * static_cast<float>(gearIndex); // checker phase per gear
    constants.uvParams[3] = 0.0f;

    vkCmdPushConstants(commandBuffer, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(constants), &constants);
}

bool VulkanGears::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                      uint32_t imageIndex,
                                      uint32_t frameIndex,
                                      double time,
                                      std::string& error) {
    (void)error;
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        error = "vkBeginCommandBuffer failed";
        return false;
    }

    std::vector<VkClearValue> clearValues;
    VkClearValue colorClear = {};
    colorClear.color.float32[0] = kClearColor[0];
    colorClear.color.float32[1] = kClearColor[1];
    colorClear.color.float32[2] = kClearColor[2];
    colorClear.color.float32[3] = kClearColor[3];
    VkClearValue depthClear = {};
    depthClear.depthStencil.depth = 1.0f;
    depthClear.depthStencil.stencil = 0;

    clearValues.push_back(colorClear);
    if (msaaActive_) { clearValues.push_back(colorClear); } // resolve target, value unused
    clearValues.push_back(depthClear);

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea.offset.x = 0;
    renderPassInfo.renderArea.offset.y = 0;
    renderPassInfo.renderArea.extent = colorExtent_;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = &clearValues[0];

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(colorExtent_.width);
    viewport.height = static_cast<float>(colorExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent = colorExtent_;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &descriptorSets_[frameIndex], 0, 0);

    for (uint32_t gear = 0; gear < static_cast<uint32_t>(kGearCount); ++gear) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &gearBuffers_[gear].vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, gearBuffers_[gear].indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        writePushConstants(commandBuffer, gear, time);
        vkCmdDrawIndexed(commandBuffer, gearBuffers_[gear].indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (!windowed_) {
        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = 0;
        region.imageOffset.y = 0;
        region.imageOffset.z = 0;
        region.imageExtent.width = colorExtent_.width;
        region.imageExtent.height = colorExtent_.height;
        region.imageExtent.depth = 1;

        vkCmdCopyImageToBuffer(commandBuffer, offscreenImage_,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readbackBuffer_, 1, &region);
    }

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        error = "vkEndCommandBuffer failed";
        return false;
    }
    return true;
}

bool VulkanGears::drawFrame(std::string& error) {
    if (!initialized_) {
        error = "drawFrame called before initialisation";
        return false;
    }
    if (!windowed_) { return renderOffscreenFrames(1, error); }

    const double now = steadySeconds();
    if (startTime_ < 0.0) {
        startTime_ = now;
        lastStatusTime_ = now;
        fps_.reset();
        fps_.tick(now);
        return true;
    }

    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                   imageAvailableSemaphores_[currentFrame_],
                                                   VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreateSwapchain(error);
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        error = diag::format("vkAcquireNextImageKHR failed (VkResult %d)", static_cast<int>(acquire));
        return false;
    }

    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
    vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

    const double time = now - startTime_;
    if (!recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, currentFrame_, time, error)) {
        return false;
    }
    updateSceneUniforms(currentFrame_, time);

    VkSemaphore waitSemaphores[1] = { imageAvailableSemaphores_[currentFrame_] };
    VkPipelineStageFlags waitStages[1] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signalSemaphores[1] = { renderFinishedSemaphores_[currentFrame_] };

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    VkResult result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]);
    if (result != VK_SUCCESS) {
        error = diag::format("vkQueueSubmit failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    VkSwapchainKHR swapchains[1] = { swapchain_ };
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue_, &presentInfo);

    ++framesRendered_;
    drawCalls_ += kGearCount;
    fps_.tick(now);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized_) {
        framebufferResized_ = false;
        if (!recreateSwapchain(error)) { return false; }
        reportStatus(now, true);
        currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
        return true;
    }
    if (result != VK_SUCCESS) {
        error = diag::format("vkQueuePresentKHR failed (VkResult %d)", static_cast<int>(result));
        return false;
    }

    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
    reportStatus(now, false);
    return true;
}

bool VulkanGears::renderOffscreenFrames(int count, std::string& error) {
    if (!initialized_) {
        error = "renderOffscreenFrames called before initialisation";
        return false;
    }

    const double now = steadySeconds();
    if (startTime_ < 0.0) {
        startTime_ = now;
        lastStatusTime_ = now;
        fps_.reset();
        fps_.tick(now);
    }

    for (int frame = 0; frame < count; ++frame) {
        // Advance the animation by a fixed step so multi frame runs keep moving.
        const double time = static_cast<double>(frame) * (1.0 / 60.0);

        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

        if (!recordCommandBuffer(commandBuffers_[currentFrame_], 0, currentFrame_, time, error)) {
            return false;
        }
        updateSceneUniforms(currentFrame_, time);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];

        const VkResult result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo,
                                              inFlightFences_[currentFrame_]);
        if (result != VK_SUCCESS) {
            error = diag::format("vkQueueSubmit failed (VkResult %d)", static_cast<int>(result));
            return false;
        }
        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

        ++framesRendered_;
        drawCalls_ += kGearCount;
        currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;

        // Offscreen rendering waits for the GPU every frame, so the frame time
        // reported here is real GPU work rather than just the submit cost.
        const double frameTime = steadySeconds();
        fps_.tick(frameTime);
        reportStatus(frameTime, false);
    }
    return true;
}

bool VulkanGears::copyOffscreenImage(std::vector<unsigned char>& rgba, std::string& error) {
    if (windowed_) {
        error = "copyOffscreenImage is only available in headless mode";
        return false;
    }
    if (readbackMapped_ == 0) {
        error = "no readback buffer";
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(colorExtent_.width) * static_cast<size_t>(colorExtent_.height);
    rgba.resize(pixelCount * 4u);

    const unsigned char* source = static_cast<const unsigned char*>(readbackMapped_);
    const bool bgra = isBgraFormat(colorFormat_);
    for (size_t i = 0; i < pixelCount; ++i) {
        if (bgra) {
            rgba[i * 4 + 0] = source[i * 4 + 2];
            rgba[i * 4 + 1] = source[i * 4 + 1];
            rgba[i * 4 + 2] = source[i * 4 + 0];
            rgba[i * 4 + 3] = 255;
        } else {
            rgba[i * 4 + 0] = source[i * 4 + 0];
            rgba[i * 4 + 1] = source[i * 4 + 1];
            rgba[i * 4 + 2] = source[i * 4 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// swapchain upkeep
// ---------------------------------------------------------------------------

void VulkanGears::destroySwapchainDependent() {
    for (size_t i = 0; i < framebuffers_.size(); ++i) {
        if (framebuffers_[i] != VK_NULL_HANDLE) { vkDestroyFramebuffer(device_, framebuffers_[i], 0); }
    }
    framebuffers_.clear();

    destroyDepthResources();
    destroyMultisampleResources();

    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        if (swapchainImageViews_[i] != VK_NULL_HANDLE) { vkDestroyImageView(device_, swapchainImageViews_[i], 0); }
    }
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, 0);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanGears::recreateSwapchain(std::string& error) {
    if (!windowed_) { return true; }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    while (width == 0 || height == 0) {
        glfwWaitEvents();
        if (glfwWindowShouldClose(window_)) { return true; } // shutting down
        glfwGetFramebufferSize(window_, &width, &height);
    }

    vkDeviceWaitIdle(device_);
    destroySwapchainDependent();

    const VkFormat previousFormat = colorFormat_;
    if (!createSwapchain(error)) { return false; }
    if (!createSwapchainImageViews(error)) { return false; }

    if (colorFormat_ != previousFormat) {
        if (renderPass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, renderPass_, 0);
            renderPass_ = VK_NULL_HANDLE;
        }
        if (!createRenderPass(error)) { return false; }

        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline_, 0);
            pipeline_ = VK_NULL_HANDLE;
        }
        if (!createGraphicsPipeline(error)) { return false; }
    }

    if (!createDepthResources(error)) { return false; }
    if (!createMultisampleResources(error)) { return false; }
    if (!createFramebuffers(error)) { return false; }

    updateCamera();
    return true;
}

// ---------------------------------------------------------------------------
// lifetime helpers
// ---------------------------------------------------------------------------

void VulkanGears::destroyGearBuffers() {
    for (int i = 0; i < kGearCount; ++i) {
        if (gearBuffers_[i].vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, gearBuffers_[i].vertexBuffer, 0);
            gearBuffers_[i].vertexBuffer = VK_NULL_HANDLE;
        }
        if (gearBuffers_[i].vertexMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, gearBuffers_[i].vertexMemory, 0);
            gearBuffers_[i].vertexMemory = VK_NULL_HANDLE;
        }
        if (gearBuffers_[i].indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, gearBuffers_[i].indexBuffer, 0);
            gearBuffers_[i].indexBuffer = VK_NULL_HANDLE;
        }
        if (gearBuffers_[i].indexMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, gearBuffers_[i].indexMemory, 0);
            gearBuffers_[i].indexMemory = VK_NULL_HANDLE;
        }
    }
}

void VulkanGears::destroyTexture() {
    if (checkerSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, checkerSampler_, 0);
        checkerSampler_ = VK_NULL_HANDLE;
    }
    if (checkerImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, checkerImageView_, 0);
        checkerImageView_ = VK_NULL_HANDLE;
    }
    if (checkerImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, checkerImage_, 0);
        checkerImage_ = VK_NULL_HANDLE;
    }
    if (checkerMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, checkerMemory_, 0);
        checkerMemory_ = VK_NULL_HANDLE;
    }
}

void VulkanGears::destroyDepthResources() {
    if (depthImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depthImageView_, 0);
        depthImageView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, depthImage_, 0);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, depthMemory_, 0);
        depthMemory_ = VK_NULL_HANDLE;
    }
}

void VulkanGears::destroyMultisampleResources() {
    if (msaaImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, msaaImageView_, 0);
        msaaImageView_ = VK_NULL_HANDLE;
    }
    if (msaaImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, msaaImage_, 0);
        msaaImage_ = VK_NULL_HANDLE;
    }
    if (msaaMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, msaaMemory_, 0);
        msaaMemory_ = VK_NULL_HANDLE;
    }
}

void VulkanGears::waitIdle() {
    if (device_ != VK_NULL_HANDLE) { vkDeviceWaitIdle(device_); }
}

void VulkanGears::shutdown() {
    if (device_ != VK_NULL_HANDLE) { vkDeviceWaitIdle(device_); }
    endTime_ = steadySeconds();

    for (int i = 0; i < kFramesInFlight && device_ != VK_NULL_HANDLE; ++i) {
        if (!imageAvailableSemaphores_.empty() && imageAvailableSemaphores_[static_cast<size_t>(i)] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, imageAvailableSemaphores_[static_cast<size_t>(i)], 0);
        }
        if (!renderFinishedSemaphores_.empty() && renderFinishedSemaphores_[static_cast<size_t>(i)] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, renderFinishedSemaphores_[static_cast<size_t>(i)], 0);
        }
        if (!inFlightFences_.empty() && inFlightFences_[static_cast<size_t>(i)] != VK_NULL_HANDLE) {
            vkDestroyFence(device_, inFlightFences_[static_cast<size_t>(i)], 0);
        }
    }
    imageAvailableSemaphores_.clear();
    renderFinishedSemaphores_.clear();
    inFlightFences_.clear();

    if (device_ != VK_NULL_HANDLE) {
        destroyGearBuffers();
        destroyTexture();
    }

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, 0);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (uniformMapped_ != 0 && uniformMemory_ != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, uniformMemory_);
        uniformMapped_ = 0;
    }
    if (uniformBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, uniformBuffer_, 0);
        uniformBuffer_ = VK_NULL_HANDLE;
    }
    if (uniformMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, uniformMemory_, 0);
        uniformMemory_ = VK_NULL_HANDLE;
    }
    if (readbackMapped_ != 0 && readbackMemory_ != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, readbackMemory_);
        readbackMapped_ = 0;
    }
    if (readbackBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, readbackBuffer_, 0);
        readbackBuffer_ = VK_NULL_HANDLE;
    }
    if (readbackMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, readbackMemory_, 0);
        readbackMemory_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, 0);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, 0);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, 0);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, 0);
        renderPass_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, 0);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        destroySwapchainDependent();
        if (offscreenImageView_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, offscreenImageView_, 0);
            offscreenImageView_ = VK_NULL_HANDLE;
        }
        if (offscreenImage_ != VK_NULL_HANDLE) {
            vkDestroyImage(device_, offscreenImage_, 0);
            offscreenImage_ = VK_NULL_HANDLE;
        }
        if (offscreenMemory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, offscreenMemory_, 0);
            offscreenMemory_ = VK_NULL_HANDLE;
        }
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, 0);
        device_ = VK_NULL_HANDLE;
    }

    if (debugMessenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != 0) { destroyMessenger(instance_, debugMessenger_, 0); }
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, 0);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, 0);
        instance_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------

bool VulkanGears::colorIsBgra() const { return isBgraFormat(colorFormat_); }

std::string VulkanGears::heapTable() const {
    VkPhysicalDeviceMemoryProperties2 properties2 = {};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    bool haveBudget = false;

    if (hasMemoryBudget_ && canQueryProperties2_) {
        properties2.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physicalDevice_, &properties2);
        haveBudget = true;
    }

    const VkPhysicalDeviceMemoryProperties& memory = memoryProperties_;
    std::string out;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        const VkMemoryHeap& heap = memory.memoryHeaps[i];
        out += diag::format("    heap %u  %-10s %s",
                            i, diag::humanBytes(heap.size).c_str(),
                            (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device-local" : "host");
        if (haveBudget) {
            out += diag::format("   used %s / budget %s",
                                diag::humanBytes(budget.heapUsage[i]).c_str(),
                                diag::humanBytes(budget.heapBudget[i]).c_str());
        }
        out += "\n";

        for (uint32_t t = 0; t < memory.memoryTypeCount; ++t) {
            if (memory.memoryTypes[t].heapIndex != i) { continue; }
            std::string flags;
            const VkMemoryPropertyFlags f = memory.memoryTypes[t].propertyFlags;
            if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)  { flags += "device-local "; }
            if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)  { flags += "host-visible "; }
            if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) { flags += "coherent "; }
            if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)   { flags += "cached "; }
            if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) { flags += "lazy "; }
            if (!flags.empty()) { flags.erase(flags.size() - 1); }
            out += diag::format("      type %u  %s\n", t, flags.c_str());
        }
    }
    return out;
}

std::string VulkanGears::liveMemorySummary() const {
    std::string out = diag::format("app %s in %zu allocations",
                                   diag::humanBytes(ledger_.total()).c_str(),
                                   ledger_.allocationCount());
    if (hasMemoryBudget_ && canQueryProperties2_) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 properties2 = {};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        properties2.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physicalDevice_, &properties2);

        // Report the device-local heap(s) only, that is where the real budget is.
        for (uint32_t i = 0; i < memoryProperties_.memoryHeapCount; ++i) {
            if ((memoryProperties_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) { continue; }
            out += diag::format(" | heap%u %s/%s", i,
                                diag::humanBytes(budget.heapUsage[i]).c_str(),
                                diag::humanBytes(budget.heapBudget[i]).c_str());
        }
    }
    return out;
}

void VulkanGears::printDiagnostics() const {
    diag::logRaw("");
    diag::logRaw("================================================================");
    diag::logRaw(" vulkangears - three meshing, checker-textured spinning gears");
    diag::logRaw("================================================================");
    diag::logRaw(diag::format(" Vulkan loader       : %s   (instance API %s, %u device(s) present)",
                              apiVersionString(loaderApiVersion_).c_str(),
                              apiVersionString(instanceApiVersion_).c_str(),
                              physicalDeviceCount_));
    diag::logRaw(diag::format(" Instance layers     : %s",
                              enabledLayers_.empty() ? "(none)" : enabledLayers_[0].c_str()));
    {
        std::string extensions;
        for (size_t i = 0; i < enabledInstanceExtensions_.size(); ++i) {
            if (i != 0) { extensions += " "; }
            extensions += enabledInstanceExtensions_[i];
        }
        diag::logRaw(diag::format(" Instance extensions : %s",
                                  extensions.empty() ? "(none)" : extensions.c_str()));
    }
    diag::logRaw(diag::format(" Device [%u]          : %s", deviceIndex_, deviceProperties_.deviceName));
    diag::logRaw(diag::format("   API version       : %s", apiVersionString(deviceProperties_.apiVersion).c_str()));
    diag::logRaw(diag::format("   driver            : %s (%s) %s",
                              hasDriverProperties_ ? driverProperties_.driverName : "unknown",
                              hasDriverProperties_ ? driverProperties_.driverInfo : "no driver properties",
                              hasDriverProperties_
                                  ? diag::format("conformance %u.%u.%u",
                                                 driverProperties_.conformanceVersion.major,
                                                 driverProperties_.conformanceVersion.minor,
                                                 driverProperties_.conformanceVersion.subminor).c_str()
                                  : ""));
    diag::logRaw(diag::format("   vendor / device   : %s (0x%04x) / 0x%04x",
                              vendorName(deviceProperties_.vendorID).c_str(),
                              deviceProperties_.vendorID, deviceProperties_.deviceID));
    diag::logRaw(diag::format("   type              : %s", deviceTypeString(deviceProperties_.deviceType).c_str()));
    diag::logRaw(diag::format("   driver version    : %s (raw 0x%08x)",
                              driverVersionString(deviceProperties_.vendorID,
                                                  deviceProperties_.driverVersion).c_str(),
                              deviceProperties_.driverVersion));
    diag::logRaw(diag::format("   queue families    : graphics %u%s, present %u%s",
                              graphicsQueueFamily_,
                              (graphicsQueueFamily_ == presentQueueFamily_) ? " (shared)" : "",
                              presentQueueFamily_,
                              separatePresentQueue_ ? " (separate)" : " (shared)"));
    {
        std::string extensions;
        for (size_t i = 0; i < enabledDeviceExtensions_.size(); ++i) {
            if (i != 0) { extensions += " "; }
            extensions += enabledDeviceExtensions_[i];
        }
        diag::logRaw(diag::format(" Device extensions   : %s",
                                  extensions.empty() ? "(none)" : extensions.c_str()));
    }
    diag::logRaw(diag::format(" Target              : %s %ux%u",
                              windowed_ ? "window" : "offscreen",
                              colorExtent_.width, colorExtent_.height));
    diag::logRaw(diag::format(" Colour format       : %s (%s)",
                              formatString(colorFormat_).c_str(),
                              colorSpaceString(colorSpace_).c_str()));
    if (windowed_) {
        diag::logRaw(diag::format(" Present mode        : %s, %u swapchain images, transform %u",
                                  presentModeString(presentMode_).c_str(),
                                  swapchainImageCount_,
                                  static_cast<unsigned>(surfaceCapabilities_.currentTransform)));
    }
    diag::logRaw(diag::format(" Depth format        : %s", formatString(depthFormat_).c_str()));
    diag::logRaw(diag::format(" Multisampling       : %s%s",
                              sampleCountString(sampleCount_).c_str(),
                              msaaActive_ ? " (MSAA, resolved before present)" : " (disabled)"));
    diag::logRaw(diag::format(" Backface culling    : %s", options_.backfaceCulling ? "on" : "off"));
    diag::logRaw(diag::format(" Checker texture     : %dx%d, %dx%d cells, %d mip levels, %s filtering",
                              checkerSize_, checkerSize_, checkerCells_, checkerCells_, checkerLevels_,
                              samplerAnisotropy_ ? "anisotropic + trilinear" : "trilinear"));
    diag::logRaw(diag::format(" Gear train          : %dT (%.2f r) -> %dT (%.2f r) -> %dT (%.2f r), module %.2f",
                              specs_[0].teeth, train_.meshes[0].pitchRadius,
                              specs_[1].teeth, train_.meshes[1].pitchRadius,
                              specs_[2].teeth, train_.meshes[2].pitchRadius,
                              specs_[0].module));
    diag::logRaw(diag::format(" Mesh timing error   : joint A-B %+.4f %%, joint B-C %+.4f %% of a tooth pitch",
                              train_.meshResidualPercent[0], train_.meshResidualPercent[1]));
    diag::logRaw(diag::format(" Geometry            : %llu triangles, %llu vertices, %llu draw calls per frame",
                              static_cast<unsigned long long>(gearTriangleTotal_),
                              static_cast<unsigned long long>(gearVertexTotal_),
                              static_cast<unsigned long long>(kGearCount)));
    diag::logRaw(" Memory heaps:");
    diag::logRaw(heapTable());
    diag::logRaw(" Application allocations:");
    diag::logRaw(ledger_.report("    "));
    diag::logRaw(diag::format(" Uniform buffer      : %s (%d frames in flight, %s per frame)",
                              diag::humanBytes(static_cast<uint64_t>(uniformBytes_)).c_str(),
                              kFramesInFlight,
                              diag::humanBytes(static_cast<uint64_t>(uniformStride_)).c_str()));
    diag::logRaw("================================================================");
    diag::logRaw("");
}

void VulkanGears::reportStatus(double now, bool force) {
    if (startTime_ < 0.0) { return; }

    const double interval = static_cast<double>(options_.fpsIntervalMs) / 1000.0;
    double fps = 0.0;
    double averageMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    uint64_t frames = 0;

    if (!fps_.consumeInterval(now, interval, fps, averageMs, minMs, maxMs, frames) && !force) {
        return;
    }
    if (force && averageMs <= 0.0) {
        fps = fps_.lastIntervalFps() > 0.0 ? fps_.lastIntervalFps() : fps_.smoothedFps();
        averageMs = fps_.smoothedFrameMs();
        minMs = averageMs;
        maxMs = averageMs;
        frames = 0;
    }

    diag::logRaw(diag::format("[fps] %7.1f fps | %6.2f ms avg (min %6.2f, max %6.2f) | %llu frames | %s | %s",
                              fps, averageMs, minMs, maxMs,
                              static_cast<unsigned long long>(fps_.frameCount()),
                              liveMemorySummary().c_str(),
                              diag::format("%.1f s", fps_.elapsedSeconds()).c_str()));

    if (windowed_ && window_ != 0) {
        glfwSetWindowTitle(window_, diag::format("vulkangears  -  %s  -  %.1f fps  -  %ux%u",
                                                 deviceProperties_.deviceName,
                                                 fps, colorExtent_.width, colorExtent_.height).c_str());
    }
}

void VulkanGears::printSummary() const {
    const double elapsed = fps_.elapsedSeconds();
    diag::logRaw("");
    diag::logRaw("=== session summary ================================");
    diag::logRaw(diag::format("  frames rendered  : %llu",
                              static_cast<unsigned long long>(framesRendered_)));
    diag::logRaw(diag::format("  elapsed          : %.2f s", elapsed));
    if (elapsed > 0.0 && framesRendered_ > 0) {
        diag::logRaw(diag::format("  average fps      : %.1f", static_cast<double>(framesRendered_) / elapsed));
    }
    diag::logRaw(diag::format("  best/worst 1 s   : %.1f / %.1f fps",
                              fps_.bestIntervalFps(), fps_.worstIntervalFps()));
    diag::logRaw(diag::format("  draw calls       : %llu (3 gears per frame)",
                              static_cast<unsigned long long>(drawCalls_)));
    diag::logRaw(diag::format("  triangles drawn  : %s",
                              diag::humanCount(gearTriangleTotal_ * framesRendered_).c_str()));
    diag::logRaw(diag::format("  device memory    : %s", liveMemorySummary().c_str()));
    diag::logRaw(" Application allocations at exit:");
    diag::logRaw(ledger_.report("    "));
    diag::logRaw(diag::format("  validation       : %llu errors, %llu warnings",
                              static_cast<unsigned long long>(validationCounters_.errors),
                              static_cast<unsigned long long>(validationCounters_.warnings)));
    diag::logRaw("===================================================");
}

} // namespace vkg
