#ifndef VKGEARS_VK_GEARS_H
#define VKGEARS_VK_GEARS_H

// The Vulkan side of the demo: instance/device setup, swapchain, render pass,
// pipeline, buffers, the checkered gear train and the frame loop.
//
// The same renderer supports two targets:
//   * windowed  - draws into a GLFW window's swapchain (the normal mode)
//   * offscreen - draws into an image and reads it back (used for --headless,
//                 which is handy on machines without a display/GPU)

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "diag.h"
#include "gear.h"

struct GLFWwindow;

namespace vkg {

struct AppOptions {
    int         width;
    int         height;
    bool        vsync;
    bool        headless;
    int         frames;             // 0 = until the window is closed
    int         samples;            // requested MSAA sample count (1 = off)
    bool        validation;
    int         gpuIndex;           // -1 = automatic
    std::string gpuNameFilter;      // substring match, empty = ignore
    int         fpsIntervalMs;      // console status line period
    bool        backfaceCulling;
    int         checkerScale;       // integer checker frequency multiplier
    float       speed;              // gear A angular speed, rad/s
    int         gearCount;          // 0 = random, otherwise clamped to [3, 15]
    uint32_t    seed;               // 0 = clock, otherwise reproducible
    bool        listDevices;
    bool        verbose;            // include informational layer messages

    AppOptions();
};

// Print every Vulkan device (and its memory heaps) without creating a device.
bool listVulkanDevices(std::string& error);

class VulkanGears {
public:
    VulkanGears();
    ~VulkanGears();

    // Non-copyable: owns Vulkan objects.
    VulkanGears(const VulkanGears&) = delete;
    VulkanGears& operator=(const VulkanGears&) = delete;

    // `window` must have been created with GLFW_CLIENT_API = GLFW_NO_API.
    bool initWindowed(GLFWwindow* window, const AppOptions& options, std::string& error);

    // No window and no surface: renders into an offscreen image.
    bool initHeadless(const AppOptions& options, std::string& error);

    // Renders (and, in windowed mode, presents) exactly one frame.
    bool drawFrame(std::string& error);

    // Offscreen only: renders `count` frames and leaves the last one available
    // to copyOffscreenImage().
    bool renderOffscreenFrames(int count, std::string& error);

    // Offscreen only: copies the last rendered image into tightly packed RGBA8.
    bool copyOffscreenImage(std::vector<unsigned char>& rgba, std::string& error);

    void notifyResized() { framebufferResized_ = true; }

    void waitIdle();
    void shutdown();

    // Console diagnostics.
    void printDiagnostics() const;
    void printSummary() const;

    bool     isHeadless() const { return !windowed_; }
    uint64_t framesRendered() const { return framesRendered_; }
    int      colorWidth() const { return static_cast<int>(colorExtent_.width); }
    int      colorHeight() const { return static_cast<int>(colorExtent_.height); }
    bool     colorIsBgra() const;

    // Counters filled in by the debug messenger callback.
    struct ValidationCounters {
        uint64_t errors;
        uint64_t warnings;
        ValidationCounters() : errors(0), warnings(0) {}
    };
    ValidationCounters validationCounters_;

private:
    // ---- setup steps ------------------------------------------------------
    bool createInstance(std::string& error);
    bool createSurface(GLFWwindow* window, std::string& error);
    bool pickPhysicalDevice(std::string& error);
    bool createLogicalDevice(std::string& error);
    bool createSwapchain(std::string& error);
    bool createSwapchainImageViews(std::string& error);
    bool createRenderPass(std::string& error);
    bool createDepthResources(std::string& error);
    bool createMultisampleResources(std::string& error);
    bool createOffscreenTarget(int width, int height, std::string& error);
    bool createFramebuffers(std::string& error);
    bool createDescriptorSetLayout(std::string& error);
    bool createGraphicsPipeline(std::string& error);
    bool buildGearTrainAndBuffers(std::string& error);
    bool createGearBuffers(std::string& error);
    bool createCheckerTexture(std::string& error);
    bool createUniformBuffers(std::string& error);
    bool createDescriptorPool(std::string& error);
    bool createDescriptorSets(std::string& error);
    bool createCommandResources(std::string& error);
    bool createSyncObjects(std::string& error);
    bool createReadbackBuffer(std::string& error);

    bool recreateSwapchain(std::string& error);
    void destroySwapchainDependent();
    void updateCamera();

    // ---- per frame --------------------------------------------------------
    bool recordCommandBuffer(VkCommandBuffer commandBuffer,
                             uint32_t imageIndex,
                             uint32_t frameIndex,
                             double time,
                             std::string& error);
    void updateSceneUniforms(uint32_t frameIndex, double time);
    void writePushConstants(VkCommandBuffer commandBuffer, uint32_t gearIndex, double time);
    void reportStatus(double now, bool force);

    // ---- helpers ----------------------------------------------------------
    std::string heapTable() const;
    std::string liveMemorySummary() const;

    uint32_t pickMemoryType(uint32_t typeBits,
                            VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred) const;
    bool createBuffer(VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred,
                      const char* ledgerLabel,
                      VkBuffer& buffer,
                      VkDeviceMemory& memory);
    bool createImage2D(uint32_t width,
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
                       VkDeviceMemory& memory);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void transitionImageLayout(VkCommandBuffer commandBuffer,
                               VkImage image,
                               VkImageLayout oldLayout,
                               VkImageLayout newLayout,
                               uint32_t mipLevels);

    void destroyGearBuffers();
    void destroyTexture();
    void destroyDepthResources();
    void destroyMultisampleResources();

    // ---- state ------------------------------------------------------------
    AppOptions  options_;
    bool        windowed_;
    bool        initialized_;
    bool        framebufferResized_;

    GLFWwindow* window_;

    VkInstance               instance_;
    VkDebugUtilsMessengerEXT debugMessenger_;
    bool                     validationEnabled_;
    uint32_t                 instanceApiVersion_;
    uint32_t                 loaderApiVersion_;
    bool                     canQueryProperties2_;
    std::vector<std::string> enabledLayers_;
    std::vector<std::string> enabledInstanceExtensions_;

    VkSurfaceKHR                  surface_;
    VkPhysicalDevice              physicalDevice_;
    VkPhysicalDeviceProperties    deviceProperties_;
    VkPhysicalDeviceMemoryProperties memoryProperties_;
    bool                          hasMemoryBudget_;
    bool                          hasDriverProperties_;
    VkPhysicalDeviceDriverProperties driverProperties_;
    uint32_t                      physicalDeviceCount_;
    uint32_t                      deviceIndex_;
    uint32_t                      graphicsQueueFamily_;
    uint32_t                      presentQueueFamily_;
    bool                          separatePresentQueue_;
    VkQueue                       graphicsQueue_;
    VkQueue                       presentQueue_;
    VkDevice                      device_;
    std::vector<std::string>      enabledDeviceExtensions_;
    bool                          samplerAnisotropy_;

    VkSwapchainKHR           swapchain_;
    VkFormat                 colorFormat_;
    VkColorSpaceKHR          colorSpace_;
    VkExtent2D               colorExtent_;
    std::vector<VkImage>     swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkPresentModeKHR         presentMode_;
    uint32_t                 swapchainImageCount_;
    VkSurfaceCapabilitiesKHR surfaceCapabilities_;

    VkFormat              depthFormat_;
    VkSampleCountFlagBits sampleCount_;
    bool                  msaaActive_;
    VkImage               depthImage_;
    VkDeviceMemory        depthMemory_;
    VkImageView           depthImageView_;
    VkImage               msaaImage_;
    VkDeviceMemory        msaaMemory_;
    VkImageView           msaaImageView_;

    VkImage        offscreenImage_;
    VkDeviceMemory offscreenMemory_;
    VkImageView    offscreenImageView_;
    VkBuffer       readbackBuffer_;
    VkDeviceMemory readbackMemory_;
    void*          readbackMapped_;
    VkDeviceSize   readbackSize_;

    VkRenderPass          renderPass_;
    VkDescriptorSetLayout descriptorSetLayout_;
    VkPipelineLayout      pipelineLayout_;
    VkPipeline            pipeline_;
    std::vector<VkFramebuffer> framebuffers_;

    // One entry per gear, plus per-gear GPU buffers.  Sized at init from
    // options_.gearCount, which may have been drawn at random.
    std::vector<GearSpec> specs_;
    GearTrain             train_;
    struct GearBuffers {
        VkBuffer       vertexBuffer;
        VkDeviceMemory vertexMemory;
        VkDeviceSize   vertexBytes;
        VkBuffer       indexBuffer;
        VkDeviceMemory indexMemory;
        VkDeviceSize   indexBytes;
        uint32_t       indexCount;
    };
    std::vector<GearBuffers> gearBuffers_;
    int                      gearCount_;

    VkImage        checkerImage_;
    VkDeviceMemory checkerMemory_;
    VkImageView    checkerImageView_;
    VkSampler      checkerSampler_;
    int            checkerSize_;
    int            checkerCells_;
    int            checkerLevels_;

    VkDescriptorPool             descriptorPool_;
    std::vector<VkDescriptorSet> descriptorSets_;
    VkBuffer                     uniformBuffer_;
    VkDeviceMemory               uniformMemory_;
    void*                        uniformMapped_;
    VkDeviceSize                 uniformStride_;
    VkDeviceSize                 uniformBytes_;

    VkCommandPool                commandPool_;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkSemaphore>     imageAvailableSemaphores_;
    std::vector<VkSemaphore>     renderFinishedSemaphores_;
    std::vector<VkFence>         inFlightFences_;
    uint32_t                     currentFrame_;

    float cameraPosition_[3];
    float viewProj_[16];
    bool  cameraDirty_;

    // diagnostics
    mutable double        lastStatusTime_;
    uint64_t              framesRendered_;
    uint64_t              drawCalls_;
    double                startTime_;
    double                endTime_;
    diag::FpsCounter      fps_;
    diag::MemoryLedger    ledger_;
    uint64_t              gearTriangleTotal_;
    uint64_t              gearVertexTotal_;
    // The seed the train was actually generated from, which is the clock's
    // unless the caller supplied one.  Declared with the diagnostics because
    // that is where it is reported, and the constructor's initialiser list has
    // to follow declaration order.
    uint32_t              seed_;
};

} // namespace vkg

#endif // VKGEARS_VK_GEARS_H
