#pragma once

#include "proxy/balancer/Balancer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy {
namespace balancer {

// Response-time weighted balancer:
// - Maintains EWMA latency per node (ms)
// - Selection score = ewma_ms * (1 + active) / max(1, weight)
class ResponseTimeWeightedBalancer : public Balancer {
public:
    explicit ResponseTimeWeightedBalancer(double ewmaAlpha = 0.2);
    ~ResponseTimeWeightedBalancer() override = default;

    void AddNode(const std::string& node, int weight = 1) override;
    void RemoveNode(const std::string& node) override;
    std::string GetNode(const std::string& key) override;

    void OnConnectionStart(const std::string& node) override;
    void OnConnectionEnd(const std::string& node) override;
    void RecordResponseTimeMs(const std::string& node, double ms) override;

private:
    struct NodeState {
        int weight{1};
        int active{0};
        bool present{false};
        double ewmaMs{5.0}; // initial baseline
        bool hasSample{false};
    };

    const double alpha_;
    std::mutex mutex_;
    std::unordered_map<std::string, NodeState> state_;
    std::vector<std::string> nodes_;
    std::atomic<size_t> rr_{0};
};

} // namespace balancer
} // namespace proxy

