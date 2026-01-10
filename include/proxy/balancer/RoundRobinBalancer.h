#pragma once

#include "proxy/balancer/Balancer.h"
#include <vector>
#include <mutex>
#include <atomic>

namespace proxy {
namespace balancer {

class RoundRobinBalancer : public Balancer {
public:
    RoundRobinBalancer() : index_(0) {}
    ~RoundRobinBalancer() override = default;

    void AddNode(const std::string& node, int weight = 1) override;
    void RemoveNode(const std::string& node) override;
    std::string GetNode(const std::string& key) override;

private:
    std::mutex mutex_;
    std::vector<std::string> nodes_; // Flattened list for weighted RR or just simple list
    // optimization: for weighted, we could store pairs, but flattening is simplest for small weights
    // If we want simple RR, just list of nodes. If weighted RR, we can use GCD algorithm or flatten.
    // Let's implement Weighted Round Robin by flattening for now (simplest for high performance if list isn't huge).
    
    std::atomic<size_t> index_;
};

} // namespace balancer
} // namespace proxy
