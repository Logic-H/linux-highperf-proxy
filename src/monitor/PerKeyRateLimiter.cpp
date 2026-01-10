#include "proxy/monitor/PerKeyRateLimiter.h"

#include <algorithm>
#include <vector>

namespace proxy {
namespace monitor {

PerKeyRateLimiter::Entry::Entry(double qps, double burst)
    : refillRate(qps),
      capacity(burst),
      tokens(burst),
      lastRefill(std::chrono::steady_clock::now()),
      lastActive(std::chrono::steady_clock::now()) {}

bool PerKeyRateLimiter::Entry::AllowAt(std::chrono::steady_clock::time_point now) {
    if (now > lastRefill) {
        const std::chrono::duration<double> elapsed = now - lastRefill;
        tokens = std::min(capacity, tokens + elapsed.count() * refillRate);
        lastRefill = now;
    }
    if (tokens >= 1.0) {
        tokens -= 1.0;
        return true;
    }
    return false;
}

PerKeyRateLimiter::PerKeyRateLimiter(Config cfg) : cfg_(cfg) {
    if (cfg_.qps > 0.0 && cfg_.burst <= 0.0) cfg_.burst = cfg_.qps;
    if (cfg_.idleSec <= 0.0) cfg_.idleSec = 60.0;
    if (cfg_.maxEntries == 0) cfg_.maxEntries = 1;
    if (cfg_.cleanupEvery == 0) cfg_.cleanupEvery = 1;
}

size_t PerKeyRateLimiter::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

bool PerKeyRateLimiter::Allow(const std::string& key) {
    if (!Enabled()) return true;
    if (key.empty()) return true;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    calls_++;

    auto it = map_.find(key);
    if (it == map_.end()) {
        it = map_.emplace(key, Entry(cfg_.qps, cfg_.burst)).first;
    }

    it->second.lastActive = now;
    const bool ok = it->second.AllowAt(now);

    if ((calls_ % cfg_.cleanupEvery) == 0) {
        CleanupLocked(now);
        EnforceCapLocked();
    } else if (map_.size() > cfg_.maxEntries) {
        // quick cap enforcement
        EnforceCapLocked();
    }

    return ok;
}

void PerKeyRateLimiter::CleanupLocked(std::chrono::steady_clock::time_point now) {
    if (cfg_.idleSec <= 0.0) return;
    const auto ttl = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(cfg_.idleSec));
    for (auto it = map_.begin(); it != map_.end();) {
        if (now - it->second.lastActive > ttl) {
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
}

void PerKeyRateLimiter::EnforceCapLocked() {
    if (map_.size() <= cfg_.maxEntries) return;

    // Evict oldest entries.
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> items;
    items.reserve(map_.size());
    for (const auto& kv : map_) {
        items.emplace_back(kv.first, kv.second.lastActive);
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

    const size_t needRemove = map_.size() - cfg_.maxEntries;
    for (size_t i = 0; i < needRemove; ++i) {
        map_.erase(items[i].first);
    }
}

} // namespace monitor
} // namespace proxy
