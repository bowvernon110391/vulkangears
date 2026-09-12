#include "platform.h"

#include <cstddef>
#include <cstdio>

#if defined(_WIN32)

// Keep windows.h from defining min/max macros (which break std::min/std::max)
// and from pulling in winsock and the rest of the kitchen sink.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>

#else

#include <unistd.h>

#endif

namespace vkg {
namespace platform {

std::string executableDir() {
#if defined(_WIN32)
    char buffer[4096];
    const DWORD length = GetModuleFileNameA(0, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length == 0 || length >= static_cast<DWORD>(sizeof(buffer))) {
        return std::string();
    }
    const std::string path(buffer, static_cast<std::size_t>(length));
    const std::size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) { return std::string(); }
    buffer[length] = '\0';
    const std::string path(buffer);
    const std::size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
#endif
}

bool setEnvironmentIfUnset(const char* name, const std::string& value) {
#if defined(_WIN32)
    // A zero-length query returns the size of an existing value, or 0.
    if (GetEnvironmentVariableA(name, 0, 0) != 0) { return false; }
    return SetEnvironmentVariableA(name, value.c_str()) != 0;
#else
    return ::setenv(name, value.c_str(), 0 /* do not clobber */) == 0;
#endif
}

bool stdoutSupportsColor() {
#if defined(_WIN32)
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == 0 || handle == INVALID_HANDLE_VALUE) { return false; }
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) == 0) { return false; } // redirected to a file/pipe
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) { return true; }
    // Windows 10+: ask the console to interpret ANSI escapes.
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

} // namespace platform
} // namespace vkg
