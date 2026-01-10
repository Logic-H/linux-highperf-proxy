#include "proxy/monitor/PerKeyConnectionLimiter.h"

namespace proxy {
namespace monitor {

bool PerKeyConnectionLimiter::TryAcquire(const std::string& key) {
    if (cfg_.maxConnections <= 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (cfg_.maxEntries > 0 && entries_.size() >= cfg_.maxEntries) return false;
        it = entries_.emplace(key, Entry{}).first;
    }
    if (it->second.active >= cfg_.maxConnections) return false;
    it->second.active += 1;
    return true;
}

void PerKeyConnectionLimiter::Release(const std::string& key) {
    if (cfg_.maxConnections <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    if (it->second.active > 0) it->second.active -= 1;
    if (it->second.active <= 0) entries_.erase(it);
}

} // namespace monitor
} // namespace proxy

