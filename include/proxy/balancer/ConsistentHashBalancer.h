#pragma once

#include "proxy/balancer/Balancer.h"
#include <map>
#include <mutex>

namespace proxy {
namespace balancer {

class ConsistentHashBalancer : public Balancer {
public:
    explicit ConsistentHashBalancer(int virtualNodesPerWeight = 100);
    ~ConsistentHashBalancer() override = default;

    void AddNode(const std::string& node, int weight = 1) override;
    void RemoveNode(const std::string& node) override;
    std::string GetNode(const std::string& key) override;

private:
    uint32_t Hash(const std::string& key);

    int virtualNodesPerWeight_;
    std::mutex mutex_;
    
    // Hash Ring: <Hash Value, Physical Node Name>
    std::map<uint32_t, std::string> ring_;
    
    // Track physical nodes and their weights to avoid redundant additions
    std::map<std::string, int> nodes_;
};

} // namespace balancer
} // namespace proxy
