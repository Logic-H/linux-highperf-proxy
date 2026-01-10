#include "proxy/balancer/RoundRobinBalancer.h"
#include <algorithm>

namespace proxy {
namespace balancer {

void RoundRobinBalancer::AddNode(const std::string& node, int weight) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Remove existing if present to update weight (simplistic approach)
    // In a flattened vector approach, removing is expensive.
    // Let's assume AddNode is rare (config change).
    
    // First remove all instances of this node
    auto new_end = std::remove(nodes_.begin(), nodes_.end(), node);
    nodes_.erase(new_end, nodes_.end());

    // Add 'weight' copies
    for (int i = 0; i < weight; ++i) {
        nodes_.push_back(node);
    }
}

void RoundRobinBalancer::RemoveNode(const std::string& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto new_end = std::remove(nodes_.begin(), nodes_.end(), node);
    nodes_.erase(new_end, nodes_.end());
}

std::string RoundRobinBalancer::GetNode(const std::string& /*key*/) {
    // Round Robin doesn't use key
    std::lock_guard<std::mutex> lock(mutex_); // Protect nodes_ read
    if (nodes_.empty()) {
        return "";
    }
    
    // Simple round robin
    // Note: index_ is atomic, but nodes_ size might change.
    // Ideally we copy the vector shared_ptr for lock-free read, but mutex is fine for now.
    
    size_t current = index_.fetch_add(1, std::memory_order_relaxed);
    return nodes_[current % nodes_.size()];
}

} // namespace balancer
} // namespace proxy
