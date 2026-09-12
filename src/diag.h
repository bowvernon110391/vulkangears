#ifndef VKGEARS_DIAG_H
#define VKGEARS_DIAG_H

// Console diagnostics for the demo: logging, a frame-rate counter and a small
// ledger that tracks how much device memory the application itself allocates.

#include <cstdint>
#include <string>
#include <vector>

namespace vkg {
namespace diag {

// printf-style formatting into a std::string (C++11: no <format>).
std::string format(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
;

void logRaw(const std::string& message);
void logInfo(const std::string& message);
void logDebug(const std::string& message);
void logWarn(const std::string& message);
void logError(const std::string& message);

void setColored(bool enabled);
bool colored();

std::string humanBytes(uint64_t bytes);
std::string humanCount(uint64_t count);

// ---------------------------------------------------------------------------
// Rolling frame-rate / frame-time statistics.
// ---------------------------------------------------------------------------
class FpsCounter {
public:
    FpsCounter();

    void reset();

    // Call once per rendered frame with a monotonically increasing timestamp
    // in seconds (std::chrono::steady_clock based).
    void tick(double nowSeconds);

    // True exactly once per "intervalSeconds" of wall clock, in which case the
    // statistics of the interval that just ended are written to the out
    // parameters.
    bool consumeInterval(double nowSeconds,
                         double intervalSeconds,
                         double& outFps,
                         double& outAvgMs,
                         double& outMinMs,
                         double& outMaxMs,
                         uint64_t& outFrames);

    double   smoothedFps() const;
    double   smoothedFrameMs() const;
    double   lastIntervalFps() const;
    double   bestIntervalFps() const;
    double   worstIntervalFps() const;
    double   elapsedSeconds() const;
    uint64_t frameCount() const;

private:
    double   start_;
    double   last_;
    double   intervalStart_;
    double   smoothedMs_;
    double   intervalMsSum_;
    double   intervalMinMs_;
    double   intervalMaxMs_;
    double   lastIntervalFps_;
    double   bestIntervalFps_;
    double   worstIntervalFps_;
    uint64_t frames_;
    uint64_t intervalFrames_;
};

// ---------------------------------------------------------------------------
// Bookkeeping for the app's own vkAllocateMemory calls.
// ---------------------------------------------------------------------------
class MemoryLedger {
public:
    void add(const std::string& label, uint64_t bytes);
    void clear();

    uint64_t total() const;
    size_t   allocationCount() const;

    // Multi-line, aligned, grouped per label.
    std::string report(const std::string& indent) const;

private:
    struct Entry {
        std::string label;
        uint64_t    bytes;
    };
    std::vector<Entry> entries_;
};

// Write an image as a binary PPM (P6).  Accepts RGBA or BGRA byte order.
bool writePPM(const std::string& path,
              int width,
              int height,
              const unsigned char* pixels,
              bool bgra,
              std::string& error);

} // namespace diag
} // namespace vkg

#endif // VKGEARS_DIAG_H
