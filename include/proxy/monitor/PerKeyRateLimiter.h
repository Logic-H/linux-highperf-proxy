#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace proxy {
namespace monitor {

// Thread-safe token bucket limiter per arbitrary key (e.g. per-client IP).
class PerKeyRateLimiter {
public:
    struct Config {
        double qps{0.0};          // <=0 disables
        double burst{0.0};        // <=0 defaults to qps
        double idleSec{60.0};     // entries idle longer than this may be removed
        size_t maxEntries{10000}; // hard cap for map size
        size_t cleanupEvery{256}; // run cleanup every N Allow() calls
    };

    explicit PerKeyRateLimiter(Config cfg);

    bool Enabled() const { return cfg_.qps > 0.0; }

    // Consume 1 token for key.
    bool Allow(const std::string& key);

    // For tests/observability.
    size_t Size() const;

private:
    struct Entry {
        double refillRate;
        double capacity;
        double tokens;
        std::chrono::steady_clock::time_point lastRefill;
        std::chrono::steady_clock::time_point lastActive;
        Entry(double qps, double burst);
        bool AllowAt(std::chrono::steady_clock::time_point now);
    };

    void CleanupLocked(std::chrono::steady_clock::time_point now);
    void EnforceCapLocked();

    Config cfg_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
    size_t calls_{0};
};

} // namespace monitor
} // namespace proxy
