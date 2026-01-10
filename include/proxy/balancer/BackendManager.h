#pragma once

#include "proxy/balancer/Balancer.h"
#include "proxy/balancer/AiServiceChecker.h"
#include "proxy/balancer/HealthChecker.h"
#include "proxy/balancer/WarmupChecker.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include <vector>
#include <memory>
#include <map>
#include <mutex>

namespace proxy {
namespace balancer {

struct BackendInfo {
    proxy::network::InetAddress addr;
    int baseWeight{1};
    int weight{1}; // effective weight
    bool healthy;
    bool online; // Admin status

    int activeConnections{0};
    double ewmaResponseMs{0.0}; // first-byte RTT EWMA
    long long failures{0};
    long long successes{0};

    // AI-aware external metrics (optional, best-effort).
    bool hasQueueLen{false};
    int queueLen{0};
	    bool hasGpu{false};
	    double gpuUtil01{0.0}; // 0..1
	    int vramUsedMb{0};
	    int vramTotalMb{0};

	    // AI service readiness (optional).
	    bool aiReadyPresent{false};
	    bool aiReady{true};
	    bool hasModelLoaded{false};
	    bool modelLoaded{false};
	    bool hasModelName{false};
	    std::string modelName;
        bool hasModelVersion{false};
        std::string modelVersion;
	    
	    std::string ToId() const { return addr.toIpPort(); }
	};

class BackendManager {
public:
    BackendManager(proxy::network::EventLoop* loop, const std::string& strategy = "roundrobin");
    ~BackendManager();

    void EnableAutoWeightAdjust(bool on) { autoWeightAdjust_ = on; }

    void AddBackend(const std::string& ip, uint16_t port, int weight = 1);
    void RemoveBackend(const std::string& ip, uint16_t port);
    bool RemoveBackendById(const std::string& id);
    
    // Select a backend for a new request
    proxy::network::InetAddress SelectBackend(const std::string& key);
    proxy::network::InetAddress SelectBackendForModel(const std::string& key, const std::string& model);
    proxy::network::InetAddress SelectBackendForModelVersion(const std::string& key,
                                                             const std::string& model,
                                                             const std::string& version);

    // Optional signals for intelligent strategies
    void OnBackendConnectionStart(const proxy::network::InetAddress& addr);
    void OnBackendConnectionEnd(const proxy::network::InetAddress& addr);
    void RecordBackendResponseTimeMs(const proxy::network::InetAddress& addr, double ms);

    // Passive health signal (fast failover): mark backend unhealthy immediately.
    // Active health checks may later mark it healthy again.
    void ReportBackendFailure(const proxy::network::InetAddress& addr);

    // Update backend metrics from an external signal (e.g., GPU inference scheduler).
    // - id: backend id in "ip:port"
    // - queueLen: backend queue length (>=0). If <0, metric is ignored.
    // - gpuUtil01: GPU utilization ratio [0..1]. If <0, metric is ignored.
    // - vramUsedMb/vramTotalMb: VRAM usage. If total <=0, memory ratio is ignored.
    bool UpdateBackendMetrics(const std::string& id,
                              int queueLen,
                              double gpuUtil01,
                              int vramUsedMb,
                              int vramTotalMb);

    // Admin operations for dynamic service discovery.
    bool SetBackendOnline(const std::string& id, bool online);
    bool SetBackendBaseWeight(const std::string& id, int baseWeight);
    bool SetBackendLoadedModel(const std::string& id, const std::string& model, bool loaded);
    bool SetBackendLoadedModel(const std::string& id, const std::string& model, const std::string& version, bool loaded);

	    struct BackendSnapshot {
	        std::string id;
	        bool healthy{false};
	        bool online{false};
	        bool aiReadyPresent{false};
	        bool aiReady{true};
	        int weight{0};
	        int baseWeight{0};
	        int activeConnections{0};
	        double ewmaResponseMs{0.0};
	        long long failures{0};
	        long long successes{0};
	        double errorRate{0.0}; // failures/(failures+successes), best-effort
	        bool hasQueueLen{false};
	        int queueLen{0};
	        bool hasGpu{false};
	        double gpuUtil01{0.0};
	        int vramUsedMb{0};
	        int vramTotalMb{0};
	        bool hasModelLoaded{false};
	        bool modelLoaded{false};
	        bool hasModelName{false};
	        std::string modelName;
            bool hasModelVersion{false};
            std::string modelVersion;
	    };
    std::vector<BackendSnapshot> GetBackendSnapshot() const;

	    // Health check configuration:
	    // - mode: "tcp" (default) or "http" or "script" or "off"
	    // - timeoutSec: per-check timeout
	    // - httpHost/httpPath: used only when mode == "http"
		    void ConfigureHealthCheck(const std::string& mode,
		                              double timeoutSec = 2.0,
		                              const std::string& httpHost = "127.0.0.1",
	                              const std::string& httpPath = "/health",
                                  const std::string& scriptCmd = "");
	    void StartHealthCheck(double intervalSec = 5.0);

	    // AI service check: fetch GPU/VRAM/model state via HTTP and update backend metrics/readiness.
    void ConfigureAiServiceCheck(double timeoutSec = 2.0,
                                 const std::string& httpHost = "127.0.0.1",
                                 const std::string& httpPath = "/ai/status");
    void StartAiServiceCheck(double intervalSec = 5.0);

    // Warmup management: when a new backend becomes online, trigger model preload
    // and keep it out of rotation until warmup succeeds.
    void ConfigureWarmup(bool enable,
                         const std::string& model,
                         double timeoutSec = 2.0,
                         const std::string& httpHost = "127.0.0.1",
                         const std::string& httpPath = "/ai/warmup");

	private:
	    void RebuildBalancer(); // Update balancer with healthy nodes
	    void RunHealthCheck();
	    void OnCheckResult(const proxy::network::InetAddress& addr, bool healthy);
	    void ScheduleNextCheck();
	    void RunAiServiceCheck();
	    void OnAiResult(bool ok, const proxy::network::InetAddress& addr, const AiServiceChecker::Result& r);
	    void ScheduleNextAiCheck();
	    bool IsEligibleLocked(const BackendInfo& b) const;
        void StartWarmupIfNeeded(const std::string& id, const proxy::network::InetAddress& addr);

	    proxy::network::EventLoop* loop_;
	    std::shared_ptr<Balancer> balancer_;
	    std::shared_ptr<HealthChecker> healthChecker_;
	    bool healthCheckEnabled_{true};
	    
	    mutable std::mutex mutex_;
	    std::map<std::string, BackendInfo> backends_; // Key: ip:port
    
	    double checkIntervalSec_;
	    int checkTimerFd_;
	    std::shared_ptr<proxy::network::Channel> checkTimerChannel_;

	    double aiIntervalSec_{0.0};
	    double aiTimeoutSec_{2.0};
	    std::string aiHttpHost_{"127.0.0.1"};
	    std::string aiHttpPath_{"/ai/status"};
	    int aiTimerFd_{-1};
	    std::shared_ptr<proxy::network::Channel> aiTimerChannel_;
	    std::shared_ptr<AiServiceChecker> aiChecker_;

        bool warmupEnabled_{false};
        double warmupTimeoutSec_{2.0};
        std::string warmupHttpHost_{"127.0.0.1"};
        std::string warmupHttpPath_{"/ai/warmup"};
        std::string warmupModel_;
        std::shared_ptr<WarmupChecker> warmupChecker_;

	    bool autoWeightAdjust_{false};
	    void RecomputeWeightLocked(BackendInfo& b);

        // Model affinity mapping: model name -> backend id ("ip:port"), best-effort.
        std::map<std::string, std::string> modelAffinity_;
        // Model-version affinity mapping: "model@version" (or "@version") -> backend id, best-effort.
        std::map<std::string, std::string> modelVersionAffinity_;
	};

} // namespace balancer
} // namespace proxy
