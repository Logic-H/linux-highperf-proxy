#pragma once

#include <chrono>
#include <mutex>

namespace proxy {
namespace monitor {

// Simple token bucket rate limiter.
// - capacity: maximum tokens in bucket (burst)
// - refill_rate: tokens added per second
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    TokenBucket(double refill_rate_tokens_per_sec, double capacity_tokens);

    // Returns true if at least `tokens` are available and consumed.
    bool Allow(double tokens = 1.0);

    // Same as Allow(), but caller provides time point (useful for tests).
    bool AllowAt(Clock::time_point now, double tokens = 1.0);

    double refill_rate() const { return refill_rate_tokens_per_sec_; }
    double capacity() const { return capacity_tokens_; }

private:
    void RefillLocked(Clock::time_point now);

    const double refill_rate_tokens_per_sec_;
    const double capacity_tokens_;

    mutable std::mutex mutex_;
    double tokens_;
    Clock::time_point last_refill_;
};

} // namespace monitor
} // namespace proxy

