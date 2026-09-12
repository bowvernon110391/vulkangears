// vulkangears - a small Vulkan demo in the spirit of glxgears:
// three meshing, checker-textured gears of different sizes, a resizable
// 800x600 window, and a running console report of what the GPU is doing.

#include "diag.h"
#include "platform.h"
#include "vk_gears.h"

// GLFW must not drag in the OpenGL headers: this demo is Vulkan only.
// Defined here rather than on the command line so every build system agrees.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The console helpers live in vkg::diag; alias them for readability below.
namespace diag = vkg::diag;

namespace {

volatile std::sig_atomic_t g_quit = 0;

void handleSignal(int) { g_quit = 1; }

void printUsage(const char* executable) {
    std::printf(
        "vulkangears - three meshing checker textured gears, rendered with Vulkan\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "window\n"
        "  --width N            window width in pixels (default 800)\n"
        "  --height N           window height in pixels (default 600)\n"
        "  --vsync              force FIFO presentation (default is uncapped MAILBOX)\n"
        "  --novsync            prefer an uncapped present mode (default)\n"
        "  --frames N           stop after N frames (default: run until the window closes)\n"
        "\n"
        "rendering\n"
        "  --samples N          MSAA sample count, 1 disables it (default 4)\n"
        "  --speed F            angular speed of the first gear in rad/s (default 1.15)\n"
        "  --checker-scale N    integer checker frequency multiplier (default 1)\n"
        "  --no-cull            disable backface culling\n"
        "  --no-validation      do not enable the Khronos validation layer\n"
        "  --verbose            also print informational messages from Vulkan layers\n"
        "\n"
        "device selection\n"
        "  --gpu N              use physical device N (see --list-devices)\n"
        "  --gpu-name TEXT      pick the first device whose name contains TEXT\n"
        "  --list-devices       print every Vulkan device and exit\n"
        "\n"
        "headless / offscreen\n"
        "  --headless           render offscreen (no window) and write a PPM file\n"
        "  --out FILE           output file for --headless (default vulkangears.ppm)\n"
        "\n"
        "misc\n"
        "  --fps-interval MS    console status line period (default 1000)\n"
        "  --help               show this help\n"
        "\n"
        "examples\n"
        "  %s                             # 800x600 window, console diagnostics\n"
        "  %s --frames 600                # render 600 frames and quit\n"
        "  %s --headless --out shot.ppm   # offscreen render, no display needed\n",
        executable, executable, executable, executable);
}

void glfwErrorCallback(int code, const char* description) {
    diag::logError(diag::format("GLFW error %d: %s", code, description != 0 ? description : "?"));
}

bool needsValue(const std::string& flag, int index, int argc) {
    if (index + 1 >= argc) {
        diag::logError("option " + flag + " needs a value");
        return false;
    }
    return true;
}

bool parseInteger(const char* text, int& out) {
    char* end = 0;
    const long value = std::strtol(text, &end, 10);
    if (end == text || (end != 0 && *end != '\0')) { return false; }
    out = static_cast<int>(value);
    return true;
}

bool parseFloat(const char* text, float& out) {
    char* end = 0;
    const double value = std::strtod(text, &end);
    if (end == text || (end != 0 && *end != '\0')) { return false; }
    out = static_cast<float>(value);
    return true;
}

bool parseArgs(int argc, char** argv, vkg::AppOptions& options, std::string& outputPath) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--width") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.width)) {
                diag::logError("--width needs an integer");
                return false;
            }
        } else if (arg == "--height") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.height)) {
                diag::logError("--height needs an integer");
                return false;
            }
        } else if (arg == "--frames") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.frames)) {
                diag::logError("--frames needs an integer");
                return false;
            }
        } else if (arg == "--samples") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.samples)) {
                diag::logError("--samples needs an integer");
                return false;
            }
        } else if (arg == "--gpu") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.gpuIndex)) {
                diag::logError("--gpu needs an integer");
                return false;
            }
        } else if (arg == "--gpu-name") {
            if (!needsValue(arg, i, argc)) { return false; }
            options.gpuNameFilter = argv[++i];
        } else if (arg == "--fps-interval") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.fpsIntervalMs)) {
                diag::logError("--fps-interval needs an integer");
                return false;
            }
        } else if (arg == "--checker-scale") {
            if (!needsValue(arg, i, argc) || !parseInteger(argv[++i], options.checkerScale)) {
                diag::logError("--checker-scale needs an integer");
                return false;
            }
            if (options.checkerScale < 1) {
                diag::logError("--checker-scale must be at least 1 (a whole number keeps the pattern seamless)");
                return false;
            }
        } else if (arg == "--speed") {
            if (!needsValue(arg, i, argc) || !parseFloat(argv[++i], options.speed)) {
                diag::logError("--speed needs a number");
                return false;
            }
        } else if (arg == "--out") {
            if (!needsValue(arg, i, argc)) { return false; }
            outputPath = argv[++i];
        } else if (arg == "--vsync") {
            options.vsync = true;
        } else if (arg == "--novsync") {
            options.vsync = false;
        } else if (arg == "--no-cull") {
            options.backfaceCulling = false;
        } else if (arg == "--no-validation") {
            options.validation = false;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--list-devices") {
            options.listDevices = true;
        } else {
            diag::logError("unknown option '" + arg + "' (try --help)");
            return false;
        }
    }

    if (options.width < 16 || options.height < 16) {
        diag::logError("window size must be at least 16x16");
        return false;
    }
    if (options.samples <= 0) { options.samples = 4; }
    return true;
}

void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

void resizeCallback(GLFWwindow* window, int, int) {
    void* user = glfwGetWindowUserPointer(window);
    if (user != 0) { static_cast<vkg::VulkanGears*>(user)->notifyResized(); }
}

int runWindowed(vkg::AppOptions& options, std::string& error) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        diag::logError("glfwInit failed - no display available? "
                       "Use --headless for an offscreen render instead.");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(options.width, options.height, "vulkangears", 0, 0);
    if (window == 0) {
        diag::logError("glfwCreateWindow failed");
        glfwTerminate();
        return 1;
    }

    vkg::VulkanGears renderer;
    if (!renderer.initWindowed(window, options, error)) {
        diag::logError(error);
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    renderer.printDiagnostics();
    diag::logInfo("window is open - press ESC or close it to quit");

    glfwSetWindowUserPointer(window, &renderer);
    glfwSetFramebufferSizeCallback(window, resizeCallback);
    glfwSetKeyCallback(window, keyCallback);

    int status = 0;
    uint64_t rendered = 0;
    while (!glfwWindowShouldClose(window) && g_quit == 0) {
        glfwPollEvents();
        if (!renderer.drawFrame(error)) {
            diag::logError(error);
            status = 1;
            break;
        }
        ++rendered;
        if (options.frames > 0 && static_cast<int>(rendered) >= options.frames) { break; }
    }

    renderer.waitIdle();
    renderer.printSummary();
    renderer.shutdown();

    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

int runHeadless(vkg::AppOptions& options, const std::string& outputPath, std::string& error) {
    vkg::VulkanGears renderer;
    if (!renderer.initHeadless(options, error)) {
        diag::logError(error);
        return 1;
    }

    renderer.printDiagnostics();

    const int frameCount = (options.frames > 0) ? options.frames : 1;
    if (!renderer.renderOffscreenFrames(frameCount, error)) {
        diag::logError(error);
        renderer.shutdown();
        return 1;
    }

    std::vector<unsigned char> pixels;
    if (!renderer.copyOffscreenImage(pixels, error)) {
        diag::logError(error);
        renderer.shutdown();
        return 1;
    }

    if (!diag::writePPM(outputPath, renderer.colorWidth(), renderer.colorHeight(),
                        pixels.empty() ? 0 : &pixels[0], false, error)) {
        diag::logError(error);
        renderer.shutdown();
        return 1;
    }

    diag::logInfo(diag::format("wrote %s (%dx%d, %d frame%s)",
                               outputPath.c_str(), renderer.colorWidth(), renderer.colorHeight(),
                               frameCount, frameCount == 1 ? "" : "s"));

    renderer.printSummary();
    renderer.shutdown();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    vkg::AppOptions options;
    std::string outputPath = "vulkangears.ppm";
    std::string error;

    if (!parseArgs(argc, argv, options, outputPath)) {
        std::fprintf(stderr, "run with --help for the option list\n");
        return 2;
    }

    diag::setColored(vkg::platform::stdoutSupportsColor());

    if (options.listDevices) {
        if (!vkg::listVulkanDevices(error)) {
            diag::logError(error);
            return 1;
        }
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    return options.headless ? runHeadless(options, outputPath, error)
                            : runWindowed(options, error);
}
