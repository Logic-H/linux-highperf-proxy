#pragma once

#include "proxy/common/noncopyable.h"
#include <string>
#include <vector>
#include <memory>

namespace proxy {
namespace balancer {

class Balancer : common::noncopyable {
public:
    virtual ~Balancer() = default;

    // 添加后端节点 (ip:port)
    virtual void AddNode(const std::string& node, int weight = 1) = 0;
    
    // 移除后端节点
    virtual void RemoveNode(const std::string& node) = 0;

    // 根据 Key（如客户端 IP）选择节点
    virtual std::string GetNode(const std::string& key) = 0;

    // Optional hooks for intelligent strategies.
    // Default implementations are no-ops so existing balancers remain compatible.
    virtual void OnConnectionStart(const std::string& /*node*/) {}
    virtual void OnConnectionEnd(const std::string& /*node*/) {}
    virtual void RecordResponseTimeMs(const std::string& /*node*/, double /*ms*/) {}

    // Optional external metrics for AI-aware strategies.
    virtual void RecordQueueLength(const std::string& /*node*/, int /*queueLen*/) {}
    virtual void RecordGpuUtil(const std::string& /*node*/,
                               double /*gpuUtil01*/,
                               int /*vramUsedMb*/,
                               int /*vramTotalMb*/) {}
};

} // namespace balancer
} // namespace proxy
