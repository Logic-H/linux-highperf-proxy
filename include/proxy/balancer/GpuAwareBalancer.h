#pragma once

#include "proxy/balancer/Balancer.h"
#include "proxy/balancer/RoundRobinBalancer.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy {
namespace balancer {

// GPU-aware balancer:
// - Prefer backend with lower GPU utilization and lower VRAM usage ratio (reported externally).
// - If no GPU metrics are available, fall back to (weighted) round robin.
class GpuAwareBalancer : public Balancer {
public:
    GpuAwareBalancer() = default;
    ~GpuAwareBalancer() override = default;

    void AddNode(const std::string& node, int weight = 1) override;
    void RemoveNode(const std::string& node) override;
    std::string GetNode(const std::string& key) override;

    void RecordGpuUtil(const std::string& node,
                       double gpuUtil01,
                       int vramUsedMb,
                       int vramTotalMb) override;

private:
    struct GpuMetric {
        double util01{1.0}; // 0..1
        int usedMb{0};
        int totalMb{0};
        bool valid{false};
    };

    static double Score(const GpuMetric& m);

    mutable std::mutex mutex_;
    std::vector<std::string> nodes_; // unique nodes
    std::unordered_map<std::string, int> weights_;
    std::unordered_map<std::string, GpuMetric> metrics_;
    RoundRobinBalancer fallback_;
};

} // namespace balancer
} // namespace proxy

