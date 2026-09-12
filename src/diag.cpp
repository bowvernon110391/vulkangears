#include "diag.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace vkg {
namespace diag {
namespace {

bool g_colored = false;

void emit(const char* tag, const std::string& message) {
    // Prefix every line so multi-line validation messages stay readable.
    std::string out;
    out.reserve(message.size() + 16);
    for (size_t i = 0; i < message.size(); ++i) {
        const bool lineStart = (i == 0) || (message[i - 1] == '\n');
        if (lineStart) { out += tag; }
        out += message[i];
    }
    std::fputs(out.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace

std::string format(const char* fmt, ...) {
    char stackBuffer[1024];
    va_list args;
    va_start(args, fmt);
    const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, args);
    va_end(args);

    if (needed < 0) { return std::string(); }
    if (static_cast<size_t>(needed) < sizeof(stackBuffer)) {
        return std::string(stackBuffer, static_cast<size_t>(needed));
    }

    std::vector<char> heapBuffer(static_cast<size_t>(needed) + 1);
    va_start(args, fmt);
    std::vsnprintf(&heapBuffer[0], heapBuffer.size(), fmt, args);
    va_end(args);
    return std::string(&heapBuffer[0], static_cast<size_t>(needed));
}

void setColored(bool enabled) { g_colored = enabled; }
bool colored() { return g_colored; }

void logRaw(const std::string& message)   { emit("", message); }
void logInfo(const std::string& message)  { emit(g_colored ? "\033[1;34m[info]\033[0m  " : "[info]  ", message); }
void logDebug(const std::string& message) { emit(g_colored ? "\033[0;36m[debug]\033[0m " : "[debug] ", message); }
void logWarn(const std::string& message)  { emit(g_colored ? "\033[1;33m[warn]\033[0m  " : "[warn]  ", message); }
void logError(const std::string& message) { emit(g_colored ? "\033[1;31m[error]\033[0m " : "[error] ", message); }

std::string humanBytes(uint64_t bytes) {
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return format("%llu B", static_cast<unsigned long long>(bytes));
    }
    return format("%.2f %s", value, units[unit]);
}

std::string humanCount(uint64_t count) {
    if (count < 1000) { return format("%llu", static_cast<unsigned long long>(count)); }
    if (count < 1000000) { return format("%.1f K", static_cast<double>(count) / 1000.0); }
    if (count < 1000000000ULL) { return format("%.2f M", static_cast<double>(count) / 1000000.0); }
    return format("%.2f G", static_cast<double>(count) / 1000000000.0);
}

// ---------------------------------------------------------------------------
// FpsCounter
// ---------------------------------------------------------------------------

FpsCounter::FpsCounter() { reset(); }

void FpsCounter::reset() {
    start_ = -1.0;
    last_ = -1.0;
    intervalStart_ = -1.0;
    smoothedMs_ = 0.0;
    intervalMsSum_ = 0.0;
    intervalMinMs_ = 0.0;
    intervalMaxMs_ = 0.0;
    lastIntervalFps_ = 0.0;
    bestIntervalFps_ = 0.0;
    worstIntervalFps_ = 0.0;
    frames_ = 0;
    intervalFrames_ = 0;
}

void FpsCounter::tick(double nowSeconds) {
    if (start_ < 0.0) {
        start_ = nowSeconds;
        last_ = nowSeconds;
        intervalStart_ = nowSeconds;
        return;
    }

    const double dt = nowSeconds - last_;
    last_ = nowSeconds;
    if (dt <= 0.0) { return; }

    ++frames_;
    const double ms = dt * 1000.0;

    // Exponential moving average with a ~0.5 s time constant: steady enough to
    // read, quick enough to react when the window is resized.
    const double tau = 0.5;
    const double alpha = 1.0 - std::exp(-dt / tau);
    if (smoothedMs_ <= 0.0) {
        smoothedMs_ = ms;
    } else {
        smoothedMs_ += (ms - smoothedMs_) * alpha;
    }

    ++intervalFrames_;
    intervalMsSum_ += ms;
    if (intervalMinMs_ <= 0.0 || ms < intervalMinMs_) { intervalMinMs_ = ms; }
    if (ms > intervalMaxMs_) { intervalMaxMs_ = ms; }
}

bool FpsCounter::consumeInterval(double nowSeconds,
                                 double intervalSeconds,
                                 double& outFps,
                                 double& outAvgMs,
                                 double& outMinMs,
                                 double& outMaxMs,
                                 uint64_t& outFrames) {
    if (intervalStart_ < 0.0) {
        intervalStart_ = nowSeconds;
        return false;
    }
    const double span = nowSeconds - intervalStart_;
    if (span < intervalSeconds || intervalFrames_ == 0) { return false; }

    outFrames = intervalFrames_;
    outFps = static_cast<double>(intervalFrames_) / span;
    outAvgMs = intervalMsSum_ / static_cast<double>(intervalFrames_);
    outMinMs = intervalMinMs_;
    outMaxMs = intervalMaxMs_;

    lastIntervalFps_ = outFps;
    if (bestIntervalFps_ <= 0.0 || outFps > bestIntervalFps_) { bestIntervalFps_ = outFps; }
    if (worstIntervalFps_ <= 0.0 || outFps < worstIntervalFps_) { worstIntervalFps_ = outFps; }

    intervalStart_ = nowSeconds;
    intervalFrames_ = 0;
    intervalMsSum_ = 0.0;
    intervalMinMs_ = 0.0;
    intervalMaxMs_ = 0.0;
    return true;
}

double FpsCounter::smoothedFps() const {
    return smoothedMs_ > 0.0 ? 1000.0 / smoothedMs_ : 0.0;
}

double FpsCounter::smoothedFrameMs() const { return smoothedMs_; }
double FpsCounter::lastIntervalFps() const { return lastIntervalFps_; }
double FpsCounter::bestIntervalFps() const { return bestIntervalFps_; }
double FpsCounter::worstIntervalFps() const { return worstIntervalFps_; }
uint64_t FpsCounter::frameCount() const { return frames_; }

double FpsCounter::elapsedSeconds() const {
    return (start_ >= 0.0 && last_ > start_) ? (last_ - start_) : 0.0;
}

// ---------------------------------------------------------------------------
// MemoryLedger
// ---------------------------------------------------------------------------

void MemoryLedger::add(const std::string& label, uint64_t bytes) {
    Entry entry;
    entry.label = label;
    entry.bytes = bytes;
    entries_.push_back(entry);
}

void MemoryLedger::clear() { entries_.clear(); }

uint64_t MemoryLedger::total() const {
    uint64_t sum = 0;
    for (size_t i = 0; i < entries_.size(); ++i) { sum += entries_[i].bytes; }
    return sum;
}

size_t MemoryLedger::allocationCount() const { return entries_.size(); }

std::string MemoryLedger::report(const std::string& indent) const {
    std::string out;

    // Group by label, preserving first-seen order.
    std::vector<std::string> labels;
    std::vector<uint64_t> sums;
    std::vector<size_t> counts;
    for (size_t i = 0; i < entries_.size(); ++i) {
        size_t found = labels.size();
        for (size_t j = 0; j < labels.size(); ++j) {
            if (labels[j] == entries_[i].label) { found = j; break; }
        }
        if (found == labels.size()) {
            labels.push_back(entries_[i].label);
            sums.push_back(0);
            counts.push_back(0);
        }
        sums[found] += entries_[i].bytes;
        counts[found] += 1;
    }

    size_t widest = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        widest = std::max(widest, labels[i].size());
    }

    for (size_t i = 0; i < labels.size(); ++i) {
        out += indent;
        out += labels[i];
        out.append(widest - labels[i].size(), ' ');
        if (counts[i] > 1) {
            out += format("  x%-3zu", counts[i]);
        } else {
            out += "      ";
        }
        out += "  ";
        out += humanBytes(sums[i]);
        out += "\n";
    }
    out += indent;
    out += format("%-*s        %s", static_cast<int>(widest), "total", humanBytes(total()).c_str());
    return out;
}

// ---------------------------------------------------------------------------
// writePPM
// ---------------------------------------------------------------------------

bool writePPM(const std::string& path,
              int width,
              int height,
              const unsigned char* pixels,
              bool bgra,
              std::string& error) {
    if (width <= 0 || height <= 0 || pixels == 0) {
        error = "writePPM: invalid image";
        return false;
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == 0) {
        error = format("writePPM: cannot open '%s' for writing", path.c_str());
        return false;
    }

    std::fprintf(file, "P6\n%d %d\n255\n", width, height);

    std::vector<unsigned char> row(static_cast<size_t>(width) * 3);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = pixels + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const unsigned char r = bgra ? src[x * 4 + 2] : src[x * 4 + 0];
            const unsigned char g = src[x * 4 + 1];
            const unsigned char b = bgra ? src[x * 4 + 0] : src[x * 4 + 2];
            row[static_cast<size_t>(x) * 3 + 0] = r;
            row[static_cast<size_t>(x) * 3 + 1] = g;
            row[static_cast<size_t>(x) * 3 + 2] = b;
        }
        if (std::fwrite(&row[0], 1, row.size(), file) != row.size()) {
            std::fclose(file);
            error = format("writePPM: short write to '%s'", path.c_str());
            return false;
        }
    }

    std::fclose(file);
    return true;
}

} // namespace diag
} // namespace vkg
