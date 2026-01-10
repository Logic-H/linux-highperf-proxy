#pragma once

#include "proxy/balancer/Balancer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy {
namespace balancer {

// Least-Connections balancer (supports weights as a tie-break/normalizer).
// score = active_connections / max(1, weight)
class LeastConnectionsBalancer : public Balancer {
public:
    LeastConnectionsBalancer() = default;
    ~LeastConnectionsBalancer() override = default;

    void AddNode(const std::string& node, int weight = 1) override;
    void RemoveNode(const std::string& node) override;
    std::string GetNode(const std::string& key) override;

    void OnConnectionStart(const std::string& node) override;
    void OnConnectionEnd(const std::string& node) override;

private:
    struct NodeState {
        int weight{1};
        int active{0};
        bool present{false};
    };

    std::mutex mutex_;
    std::unordered_map<std::string, NodeState> state_;
    std::vector<std::string> nodes_;
    std::atomic<size_t> rr_{0};
};

} // namespace balancer
} // namespace proxy

