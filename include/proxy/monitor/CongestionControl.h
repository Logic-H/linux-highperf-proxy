#pragma once

#include "proxy/common/noncopyable.h"

#include <atomic>
#include <cstddef>
#include <mutex>

namespace proxy {
namespace monitor {

// Sliding window congestion control with AIMD (additive increase, multiplicative decrease).
// The "window" is the max number of in-flight backend requests allowed concurrently.
class CongestionControl : public proxy::common::noncopyable {
public:
    struct Config {
        bool enabled{false};
        int initialWindow{64};
        int minWindow{1};
        int maxWindow{1024};
        int additiveIncrease{1};      // increase cwnd by this when enough acks observed
        double multiplicativeDecrease{0.7}; // cwnd = max(min, floor(cwnd * beta)) on loss
    };

    struct Stats {
        int cwnd{0};
        int inflight{0};
        unsigned long long acks{0};
        unsigned long long losses{0};
    };

    explicit CongestionControl(const Config& cfg);

    bool enabled() const { return enabled_; }

    // Try acquire one slot in the congestion window.
    // Returns false if inflight >= cwnd.
    bool TryAcquire();

    // Mark one request complete and update AIMD.
    // Must be called exactly once per successful TryAcquire().
    void OnComplete(bool success);

    // Congestion drop without acquiring a slot (e.g. shed load when window is full).
    void OnDrop();

    Stats GetStats() const;

private:
    void OnAckLocked();
    void OnLossLocked();

    bool enabled_{false};
    int cwnd_{0};
    int inflight_{0};
    int minWindow_{1};
    int maxWindow_{1024};
    int additiveIncrease_{1};
    double beta_{0.7};
    unsigned long long ackCounter_{0};

    mutable std::mutex mu_;
    std::atomic<unsigned long long> acks_{0};
    std::atomic<unsigned long long> losses_{0};
};

} // namespace monitor
} // namespace proxy
