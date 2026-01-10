#pragma once

#include "proxy/balancer/Balancer.h"
#include "proxy/balancer/RoundRobinBalancer.h"

#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy {
namespace balancer {

// Queue-length aware balancer:
// - Prefer backend with the smallest reported queue length.
// - If no queue metrics are available, fall back to (weighted) round robin.
class LeastQueueBalancer : public Balancer {
public:
    LeastQueueBalancer() = default;
    ~LeastQueueBalancer() override = default;

    void AddNode(const std::string& node, int weight = 1) override;
    void RemoveNode(const std::string& node) override;
    std::string GetNode(const std::string& key) override;

    void RecordQueueLength(const std::string& node, int queueLen) override;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> nodes_; // unique nodes
    std::unordered_map<std::string, int> weights_;
    std::unordered_map<std::string, int> queueLen_;
    RoundRobinBalancer fallback_;
};

} // namespace balancer
} // namespace proxy

