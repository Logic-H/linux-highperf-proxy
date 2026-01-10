#include "proxy/monitor/CongestionControl.h"

#include <algorithm>
#include <cmath>

namespace proxy {
namespace monitor {

CongestionControl::CongestionControl(const Config& cfg) {
    enabled_ = cfg.enabled;
    minWindow_ = std::max(1, cfg.minWindow);
    maxWindow_ = std::max(minWindow_, cfg.maxWindow);
    additiveIncrease_ = std::max(1, cfg.additiveIncrease);
    beta_ = cfg.multiplicativeDecrease;
    if (beta_ <= 0.0 || beta_ >= 1.0) beta_ = 0.7;

    cwnd_ = cfg.initialWindow;
    if (cwnd_ < minWindow_) cwnd_ = minWindow_;
    if (cwnd_ > maxWindow_) cwnd_ = maxWindow_;
}

bool CongestionControl::TryAcquire() {
    if (!enabled_) return true;
    std::lock_guard<std::mutex> lock(mu_);
    if (inflight_ >= cwnd_) return false;
    inflight_ += 1;
    return true;
}

void CongestionControl::OnAckLocked() {
    acks_.fetch_add(1, std::memory_order_relaxed);
    ackCounter_ += 1;
    // Classic AIMD: increase by 1 per cwnd ACKs (approx one RTT).
    if (ackCounter_ >= static_cast<unsigned long long>(std::max(1, cwnd_))) {
        ackCounter_ = 0;
        cwnd_ += additiveIncrease_;
        if (cwnd_ > maxWindow_) cwnd_ = maxWindow_;
    }
}

void CongestionControl::OnLossLocked() {
    losses_.fetch_add(1, std::memory_order_relaxed);
    ackCounter_ = 0;
    const int next = static_cast<int>(std::floor(static_cast<double>(cwnd_) * beta_));
    cwnd_ = std::max(minWindow_, std::min(maxWindow_, next));
}

void CongestionControl::OnComplete(bool success) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (inflight_ > 0) inflight_ -= 1;
    if (success) OnAckLocked();
    else OnLossLocked();
}

void CongestionControl::OnDrop() {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mu_);
    OnLossLocked();
}

CongestionControl::Stats CongestionControl::GetStats() const {
    Stats s;
    s.acks = acks_.load(std::memory_order_relaxed);
    s.losses = losses_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    s.cwnd = cwnd_;
    s.inflight = inflight_;
    return s;
}

} // namespace monitor
} // namespace proxy
