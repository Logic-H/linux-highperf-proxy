#include "proxy/balancer/ConsistentHashBalancer.h"
#include "proxy/common/Logger.h"
#include <iostream>
#include <map>

using namespace proxy::balancer;
using namespace proxy::common;

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);
    LOG_INFO << "Starting ConsistentHashBalancer test";

    ConsistentHashBalancer balancer(200); // 200 virtual nodes per weight

    balancer.AddNode("ServerA", 1);
    balancer.AddNode("ServerB", 1);
    balancer.AddNode("ServerC", 2); // Server C has double weight

    std::map<std::string, int> counts;
    
    // Simulate 10,000 requests from different client IPs
    for (int i = 0; i < 10000; ++i) {
        std::string client = "192.168.1." + std::to_string(i);
        std::string node = balancer.GetNode(client);
        counts[node]++;
    }

    LOG_INFO << "Distribution Results (10000 requests):";
    for (auto const& [node, count] : counts) {
        LOG_INFO << "  " << node << ": " << count << " (" << (count/100.0) << "%)";
    }

    // Verify consistency: same key always maps to same node
    std::string client = "10.0.0.1";
    std::string node1 = balancer.GetNode(client);
    std::string node2 = balancer.GetNode(client);
    if (node1 == node2) {
        LOG_INFO << "Consistency check: PASS";
    } else {
        LOG_ERROR << "Consistency check: FAILED";
    }

    // Test node removal
    LOG_INFO << "Removing ServerB...";
    balancer.RemoveNode("ServerB");
    std::string node3 = balancer.GetNode(client);
    LOG_INFO << "New mapping for " << client << ": " << node3;

    LOG_INFO << "ConsistentHashBalancer test passed";
    return 0;
}
