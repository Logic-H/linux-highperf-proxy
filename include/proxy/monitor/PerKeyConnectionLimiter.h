#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace proxy {
namespace monitor {

// Tracks active connections per key (user/service/etc).
// - TryAcquire increments active count if under limit.
// - Release decrements active count and erases entry at zero.
class PerKeyConnectionLimiter {
public:
    struct Config {
        int maxConnections{0};     // 0 means unlimited/disabled
        size_t maxEntries{10000};  // cap unique keys to avoid unbounded memory
    };

    explicit PerKeyConnectionLimiter(Config cfg) : cfg_(cfg) {}

    bool TryAcquire(const std::string& key);
    void Release(const std::string& key);

    const Config& config() const { return cfg_; }

private:
    struct Entry {
        int active{0};
    };

    Config cfg_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace monitor
} // namespace proxy

