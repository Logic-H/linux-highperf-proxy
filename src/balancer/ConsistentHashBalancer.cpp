#include "proxy/balancer/ConsistentHashBalancer.h"
#include <sstream>

namespace proxy {
namespace balancer {

ConsistentHashBalancer::ConsistentHashBalancer(int virtualNodesPerWeight)
    : virtualNodesPerWeight_(virtualNodesPerWeight) {
}

// FNV-1a hash algorithm
uint32_t ConsistentHashBalancer::Hash(const std::string& key) {
    uint32_t hash = 2166136261U;
    for (char c : key) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619U;
    }
    return hash;
}

void ConsistentHashBalancer::AddNode(const std::string& node, int weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.count(node)) {
        RemoveNode(node); // Update weight by re-adding
    }
    
    nodes_[node] = weight;
    int totalVirtualNodes = weight * virtualNodesPerWeight_;
    
    for (int i = 0; i < totalVirtualNodes; ++i) {
        std::stringstream ss;
        ss << node << "#" << i; // Virtual node identifier
        uint32_t hash = Hash(ss.str());
        ring_[hash] = node;
    }
}

void ConsistentHashBalancer::RemoveNode(const std::string& node) {
    // Note: Mutex expected to be held if called from AddNode (re-add)
    // But for public use, we need a lock.
    // To handle both, we check if node exists.
    
    // In this simple implementation, we just clear and rebuild or iterate
    // Iterating the ring to remove is safer for this demo.
    auto it = ring_.begin();
    while (it != ring_.end()) {
        if (it->second == node) {
            it = ring_.erase(it);
        } else {
            ++it;
        }
    }
    nodes_.erase(node);
}

std::string ConsistentHashBalancer::GetNode(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.empty()) {
        return "";
    }

    uint32_t hash = Hash(key);
    
    // Find the first node with hash >= key's hash
    auto it = ring_.lower_bound(hash);
    
    if (it == ring_.end()) {
        // Wrap around to the start of the ring
        return ring_.begin()->second;
    }
    
    return it->second;
}

} // namespace balancer
} // namespace proxy
