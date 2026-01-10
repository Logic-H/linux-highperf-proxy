#include "proxy/monitor/TokenBucket.h"

#include <algorithm>
#include <stdexcept>

namespace proxy {
namespace monitor {

TokenBucket::TokenBucket(double refill_rate_tokens_per_sec, double capacity_tokens)
    : refill_rate_tokens_per_sec_(refill_rate_tokens_per_sec),
      capacity_tokens_(capacity_tokens),
      tokens_(capacity_tokens),
      last_refill_(Clock::now()) {
    if (refill_rate_tokens_per_sec_ < 0.0) {
        throw std::invalid_argument("TokenBucket refill_rate must be >= 0");
    }
    if (capacity_tokens_ <= 0.0) {
        throw std::invalid_argument("TokenBucket capacity must be > 0");
    }
}

bool TokenBucket::Allow(double tokens) {
    return AllowAt(Clock::now(), tokens);
}

void TokenBucket::RefillLocked(Clock::time_point now) {
    if (now <= last_refill_) return;
    const std::chrono::duration<double> elapsed = now - last_refill_;
    tokens_ = std::min(capacity_tokens_, tokens_ + elapsed.count() * refill_rate_tokens_per_sec_);
    last_refill_ = now;
}

bool TokenBucket::AllowAt(Clock::time_point now, double tokens) {
    if (tokens <= 0.0) return true;

    std::lock_guard<std::mutex> lock(mutex_);
    RefillLocked(now);
    if (tokens_ >= tokens) {
        tokens_ -= tokens;
        return true;
    }
    return false;
}

} // namespace monitor
} // namespace proxy

