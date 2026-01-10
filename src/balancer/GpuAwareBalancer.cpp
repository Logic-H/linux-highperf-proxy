#include "proxy/balancer/GpuAwareBalancer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace proxy {
namespace balancer {

void GpuAwareBalancer::AddNode(const std::string& node, int weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (weights_.find(node) == weights_.end()) {
        nodes_.push_back(node);
    }
    weights_[node] = std::max(1, weight);
    fallback_.AddNode(node, weight);
}

void GpuAwareBalancer::RemoveNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
    weights_.erase(node);
    metrics_.erase(node);
    fallback_.RemoveNode(node);
}

void GpuAwareBalancer::RecordGpuUtil(const std::string& node,
                                     double gpuUtil01,
                                     int vramUsedMb,
                                     int vramTotalMb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (gpuUtil01 < 0.0) gpuUtil01 = 0.0;
    if (gpuUtil01 > 1.0) gpuUtil01 = 1.0;
    if (vramUsedMb < 0) vramUsedMb = 0;
    if (vramTotalMb < 0) vramTotalMb = 0;

    GpuMetric m;
    m.util01 = gpuUtil01;
    m.usedMb = vramUsedMb;
    m.totalMb = vramTotalMb;
    m.valid = true;
    metrics_[node] = m;
}

double GpuAwareBalancer::Score(const GpuMetric& m) {
    // Lower is better.
    double memRatio = 0.0;
    if (m.totalMb > 0) {
        memRatio = static_cast<double>(m.usedMb) / static_cast<double>(m.totalMb);
        if (memRatio < 0.0) memRatio = 0.0;
        if (memRatio > 1.0) memRatio = 1.0;
    }

    // Weighted score: utilization is dominant; VRAM ratio contributes.
    return 0.7 * m.util01 + 0.3 * memRatio;
}

std::string GpuAwareBalancer::GetNode(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (nodes_.empty()) {
        return "";
    }

    double bestScore = std::numeric_limits<double>::infinity();
    std::vector<std::string> best;
    best.reserve(nodes_.size());

    for (const auto& n : nodes_) {
        auto it = metrics_.find(n);
        if (it == metrics_.end() || !it->second.valid) {
            continue;
        }
        double s = Score(it->second);
        if (s < bestScore - 1e-9) {
            bestScore = s;
            best.clear();
            best.push_back(n);
        } else if (std::fabs(s - bestScore) <= 1e-9) {
            best.push_back(n);
        }
    }

    if (best.empty()) {
        lock.unlock();
        return fallback_.GetNode(key);
    }
    if (best.size() == 1) {
        return best[0];
    }
    size_t h = std::hash<std::string>{}(key);
    return best[h % best.size()];
}

} // namespace balancer
} // namespace proxy
