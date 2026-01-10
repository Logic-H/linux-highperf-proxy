#include "proxy/balancer/LeastQueueBalancer.h"

#include <algorithm>
#include <functional>

namespace proxy {
namespace balancer {

void LeastQueueBalancer::AddNode(const std::string& node, int weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (weights_.find(node) == weights_.end()) {
        nodes_.push_back(node);
    }
    weights_[node] = std::max(1, weight);
    fallback_.AddNode(node, weight);
}

void LeastQueueBalancer::RemoveNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
    weights_.erase(node);
    queueLen_.erase(node);
    fallback_.RemoveNode(node);
}

void LeastQueueBalancer::RecordQueueLength(const std::string& node, int queueLen) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Clamp to non-negative.
    if (queueLen < 0) queueLen = 0;
    queueLen_[node] = queueLen;
}

std::string LeastQueueBalancer::GetNode(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (nodes_.empty()) {
        return "";
    }

    // Find minimal queue length among nodes that have metrics.
    int bestQ = std::numeric_limits<int>::max();
    std::vector<std::string> best;
    best.reserve(nodes_.size());

    for (const auto& n : nodes_) {
        auto it = queueLen_.find(n);
        if (it == queueLen_.end()) {
            continue;
        }
        int q = it->second;
        if (q < bestQ) {
            bestQ = q;
            best.clear();
            best.push_back(n);
        } else if (q == bestQ) {
            best.push_back(n);
        }
    }

    if (best.empty()) {
        // No queue metrics: fall back to RR (locks inside fallback_).
        lock.unlock();
        return fallback_.GetNode(key);
    } else if (best.size() == 1) {
        return best[0];
    } else {
        size_t h = std::hash<std::string>{}(key);
        return best[h % best.size()];
    }
}

} // namespace balancer
} // namespace proxy
