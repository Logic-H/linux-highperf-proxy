#include "proxy/balancer/ResponseTimeWeightedBalancer.h"

#include <algorithm>
#include <limits>

namespace proxy {
namespace balancer {

ResponseTimeWeightedBalancer::ResponseTimeWeightedBalancer(double ewmaAlpha)
    : alpha_((ewmaAlpha > 0.0 && ewmaAlpha <= 1.0) ? ewmaAlpha : 0.2) {}

void ResponseTimeWeightedBalancer::AddNode(const std::string& node, int weight) {
    if (weight <= 0) weight = 1;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& st = state_[node];
    st.weight = weight;
    if (!st.present) {
        st.present = true;
        nodes_.push_back(node);
    }
}

void ResponseTimeWeightedBalancer::RemoveNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(node);
    if (it != state_.end()) {
        it->second.present = false;
        it->second.active = 0;
    }
    auto endIt = std::remove(nodes_.begin(), nodes_.end(), node);
    nodes_.erase(endIt, nodes_.end());
}

std::string ResponseTimeWeightedBalancer::GetNode(const std::string& /*key*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.empty()) return "";

    double bestScore = std::numeric_limits<double>::infinity();
    std::vector<std::string> best;
    best.reserve(nodes_.size());

    for (const auto& node : nodes_) {
        auto it = state_.find(node);
        if (it == state_.end() || !it->second.present) continue;
        const int w = std::max(1, it->second.weight);
        const double latency = it->second.hasSample ? it->second.ewmaMs : it->second.ewmaMs;
        const double score = latency * (1.0 + static_cast<double>(std::max(0, it->second.active))) / static_cast<double>(w);
        if (score < bestScore) {
            bestScore = score;
            best.clear();
            best.push_back(node);
        } else if (score == bestScore) {
            best.push_back(node);
        }
    }
    if (best.empty()) return "";

    const size_t idx = rr_.fetch_add(1, std::memory_order_relaxed);
    return best[idx % best.size()];
}

void ResponseTimeWeightedBalancer::OnConnectionStart(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(node);
    if (it != state_.end() && it->second.present) {
        it->second.active += 1;
    }
}

void ResponseTimeWeightedBalancer::OnConnectionEnd(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(node);
    if (it != state_.end() && it->second.present) {
        it->second.active -= 1;
        if (it->second.active < 0) it->second.active = 0;
    }
}

void ResponseTimeWeightedBalancer::RecordResponseTimeMs(const std::string& node, double ms) {
    if (ms <= 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = state_.find(node);
    if (it == state_.end() || !it->second.present) return;
    if (!it->second.hasSample) {
        it->second.ewmaMs = ms;
        it->second.hasSample = true;
        return;
    }
    it->second.ewmaMs = alpha_ * ms + (1.0 - alpha_) * it->second.ewmaMs;
}

} // namespace balancer
} // namespace proxy

