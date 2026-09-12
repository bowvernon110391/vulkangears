#ifndef VKGEARS_PLATFORM_H
#define VKGEARS_PLATFORM_H

// Tiny portability layer: everything that differs between POSIX and Win32
// lives here, so the rest of the demo stays plain portable C++11.

#include <string>

namespace vkg {
namespace platform {

// Directory that contains the running executable, without a trailing
// separator.  Used to find the vendored validation layer next to the build
// tree; empty if it cannot be determined.
std::string executableDir();

// Sets an environment variable but never overwrites an existing value.  The
// Vulkan loader reads VK_LAYER_PATH while creating the instance, which is why
// this has to happen before vkCreateInstance.
bool setEnvironmentIfUnset(const char* name, const std::string& value);

// True when stdout is a terminal that can render ANSI colour escapes.  On
// Windows this also tries to switch the console into virtual terminal mode.
bool stdoutSupportsColor();

} // namespace platform
} // namespace vkg

#endif // VKGEARS_PLATFORM_H
