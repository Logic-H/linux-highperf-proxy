#pragma once

#include "proxy/network/TcpServer.h"
#include "proxy/network/EventLoop.h"
#include "proxy/balancer/BackendManager.h"
#include "proxy/balancer/BackendSession.h"
#include "proxy/balancer/BackendConnectionPool.h"
#include "proxy/ProxySessionContext.h"
#include "proxy/monitor/TokenBucket.h"
#include "proxy/monitor/CongestionControl.h"
#include "proxy/monitor/AccessControl.h"
#include "proxy/monitor/AuditLogger.h"
#include "proxy/monitor/PerKeyRateLimiter.h"
#include "proxy/monitor/PerKeyConnectionLimiter.h"
#include "proxy/monitor/HistoryStore.h"
#include "proxy/protocol/HttpBatcher.h"
#include "proxy/protocol/RewriteRules.h"
#include "proxy/protocol/TrafficMirror.h"
#include "proxy/protocol/Cache.h"
#include "proxy/common/PluginManager.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace proxy {

class PriorityDispatcher;

class ProxyServer {
public:
    ProxyServer(network::EventLoop* loop,
                uint16_t port,
                const std::string& strategy = "roundrobin",
                const std::string& name = "ProxyServer",
                bool reusePort = false);
    
    void Start();
    void AddBackend(const std::string& ip, uint16_t port, int weight = 1);
    bool UpdateBackendMetrics(const std::string& id,
                              int queueLen,
                              double gpuUtil01,
                              int vramUsedMb,
                              int vramTotalMb);
    void EnableAutoWeightAdjust(bool on);

    // Health check configuration: mode=tcp/http (http checks status line).
    void ConfigureHealthCheck(const std::string& mode,
                              double timeoutSec = 2.0,
                              const std::string& httpHost = "127.0.0.1",
                              const std::string& httpPath = "/health",
                              const std::string& scriptCmd = "");
    void StartHealthCheck(double intervalSec = 5.0);

    // AI service check: GPU/VRAM/model state via HTTP.
    void ConfigureAiServiceCheck(double timeoutSec = 2.0,
                                 const std::string& httpHost = "127.0.0.1",
                                 const std::string& httpPath = "/ai/status");
    void StartAiServiceCheck(double intervalSec = 5.0);

    // Warmup management: gate new backends until model warmup succeeds.
    void ConfigureWarmup(bool enable,
                         const std::string& model,
                         double timeoutSec = 2.0,
                         const std::string& httpHost = "127.0.0.1",
                         const std::string& httpPath = "/ai/warmup");

    // Set number of threads for I/O loop
    void SetThreadNum(int numThreads);

    // Enable a simple global request rate limit (token bucket).
    // qps <= 0 disables rate limiting.
    void EnableRateLimit(double qps, double burstTokens);

    // Connection limits (0 means unlimited).
    void SetConnectionLimits(int maxConnections, int maxConnectionsPerIp);
    void SetIdleTimeout(double idleTimeoutSec, double cleanupIntervalSec = 1.0);

    // Basic DDoS protection: accept rate limiting (qps <= 0 disables).
    void SetAcceptRateLimit(double qps, double burst);
    void SetPerIpAcceptRateLimit(double qps, double burst, double idleSec = 60.0, size_t maxEntries = 10000);

    // Session affinity (sticky sessions): none/ip/header/cookie
    void SetSessionAffinity(const std::string& mode, const std::string& headerName = "", const std::string& cookieName = "");

    // Access control (IP allow/deny list + token)
    void SetAccessControl(const monitor::AccessControl::Config& cfg);
    void EnableAuditLog(const std::string& path);
    void EnablePerIpRateLimit(double qps, double burst, double idleSec = 60.0, size_t maxEntries = 10000);
    void EnablePerPathRateLimit(double qps, double burst, double idleSec = 60.0, size_t maxEntries = 10000);

    // Congestion control (sliding window + AIMD) for backend forwarding concurrency.
    void ConfigureCongestionControl(const monitor::CongestionControl::Config& cfg);

    // Connection limits based on user/service (0 disables).
    void SetMaxConnectionsPerUser(int maxConnections, const std::string& headerName = "X-Api-Token", size_t maxEntries = 10000);
    void SetMaxConnectionsPerService(int maxConnections, size_t maxEntries = 10000);

    // Expose backend manager for management-plane components (service discovery).
    balancer::BackendManager* GetBackendManager() { return &backendManager_; }

    // Batch processing optimization: merge small JSON POST requests into a larger batch per backend/model.
    void ConfigureHttpBatching(const protocol::HttpBatcher::Config& cfg);

    // Request/response rewrite rules (header/body modifications).
    void ConfigureRewriteRules(const std::vector<protocol::RewriteRule>& rules);

    // Traffic mirroring: send a copy of traffic to a monitoring system (best-effort).
    void ConfigureTrafficMirror(const protocol::TrafficMirror::Config& cfg);

    // Cache integration (best-effort): Redis/Memcached.
    void ConfigureCache(const protocol::Cache::Config& cfg);

    // TLS termination (optional). Accepts both HTTPS and HTTP by sniffing the first byte.
    bool EnableTls(const std::string& certPemPath, const std::string& keyPemPath);

    // ACME HTTP-01 challenge files directory. If set, serves:
    //   /.well-known/acme-challenge/<token>  -> file content
    void SetAcmeChallengeDir(const std::string& dir);

    void ConfigureHistory(const monitor::HistoryStore::Config& cfg);

    // Plugin manager (dlopen .so). Intended for pluggable extensions.
    void ConfigurePlugins(const common::PluginManager::Config& cfg);

    // L4 raw TCP tunnel (for throughput/iperf3). When enabled, a second listener
    // accepts connections and forwards bytes to a selected backend without HTTP parsing.
    // listenPort=0 disables.
    void ConfigureL4Tunnel(uint16_t listenPort);

    struct PriorityConfig {
        bool enabled{false};
        // mode: "priority" (strict priority, default), "fair" (fair queuing), "edf" (earliest deadline first).
        std::string mode{"priority"};
        int maxInflight{0};      // 0 => unlimited (disabled effect)
        int highThreshold{8};    // >= threshold treated as high-priority
        int lowDelayMs{0};       // optional: delay low-priority enqueue to reduce priority inversion
        std::string priorityHeader{"X-Priority"};
        std::string priorityQuery{"priority"};
        // Fair-queuing flow key (used when mode="fair"): header/query preferred, fallback to client IP.
        std::string flowHeader{"X-Flow"};
        std::string flowQuery{"flow"};
        // EDF deadline (used when mode="edf"): milliseconds relative to now (smaller => earlier).
        std::string deadlineHeader{"X-Deadline-Ms"};
        std::string deadlineQuery{"deadline_ms"};
        int defaultDeadlineMs{60000};
    };
    void ConfigurePriorityScheduling(const PriorityConfig& cfg);

private:
    void OnConnection(const network::TcpConnectionPtr& conn);
    void OnConnectionL4(const network::TcpConnectionPtr& conn);
    void OnMessage(const network::TcpConnectionPtr& conn,
                   network::Buffer* buf,
                   std::chrono::system_clock::time_point);
    std::shared_ptr<protocol::HttpBatcher> GetOrCreateBatcher(network::EventLoop* loop);

    network::EventLoop* loop_;
    network::TcpServer server_;
    std::unique_ptr<network::TcpServer> l4Server_;
    uint16_t l4ListenPort_{0};
    bool reusePort_{false};
    balancer::BackendManager backendManager_;
    std::unique_ptr<balancer::BackendConnectionPool> backendPool_;

    std::unique_ptr<monitor::TokenBucket> requestRateLimiter_;
    std::unique_ptr<monitor::CongestionControl> congestion_;

    std::string affinityMode_{"none"};
    std::string affinityHeader_;
    std::string affinityCookie_;

    std::unique_ptr<monitor::AccessControl> accessControl_;
    std::unique_ptr<monitor::AuditLogger> auditLogger_;
    std::unique_ptr<monitor::PerKeyRateLimiter> perIpRateLimiter_;
    std::unique_ptr<monitor::PerKeyRateLimiter> perPathRateLimiter_;

    std::unique_ptr<monitor::PerKeyConnectionLimiter> perUserConnLimiter_;
    std::unique_ptr<monitor::PerKeyConnectionLimiter> perServiceConnLimiter_;
    std::string userConnHeader_{"X-Api-Token"};

    protocol::HttpBatcher::Config batchCfg_{};
    std::mutex batchMu_;
    std::map<network::EventLoop*, std::shared_ptr<protocol::HttpBatcher>> batchers_;

    protocol::RewriteEngine rewrite_;
    protocol::TrafficMirror mirror_;
    protocol::Cache cache_;

    PriorityConfig prioCfg_{};
    std::mutex prioMu_;

    std::string acmeChallengeDir_;

    std::unique_ptr<monitor::HistoryStore> history_;

    std::unique_ptr<common::PluginManager> plugins_;
    
    // Manage active sessions to keep them alive
    // Key: frontend connection name? Or just shared_ptr binding?
    // Using shared_ptr in callbacks is usually enough, but we need to break cycles.
    // BackendSession holds weak_ptr to frontend.
    // Frontend connection context can hold BackendSession.
};

} // namespace proxy
