#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <chrono>
#include <vector>
#include <unordered_map>

namespace proxy {
namespace monitor {

class Stats {
public:
    static Stats& Instance();

    void IncTotalRequests() { totalRequests_.fetch_add(1, std::memory_order_relaxed); }
    long GetTotalRequests() const { return totalRequests_.load(std::memory_order_relaxed); }

    void IncActiveConnections() { activeConnections_.fetch_add(1, std::memory_order_relaxed); }
    void DecActiveConnections() { activeConnections_.fetch_sub(1, std::memory_order_relaxed); }
    long GetActiveConnections() const { return activeConnections_.load(std::memory_order_relaxed); }

    void IncBackendFailures() { backendFailures_.fetch_add(1, std::memory_order_relaxed); }
    long GetBackendFailures() const { return backendFailures_.load(std::memory_order_relaxed); }

    void AddBytesIn(long long n) { bytesIn_.fetch_add(n, std::memory_order_relaxed); }
    void AddBytesOut(long long n) { bytesOut_.fetch_add(n, std::memory_order_relaxed); }
    long long GetBytesIn() const { return bytesIn_.load(std::memory_order_relaxed); }
    long long GetBytesOut() const { return bytesOut_.load(std::memory_order_relaxed); }

    void AddUdpRxDrops(long long n) { udpRxDrops_.fetch_add(n, std::memory_order_relaxed); }
    long long GetUdpRxDrops() const { return udpRxDrops_.load(std::memory_order_relaxed); }

    void AddDdosDrops(long long n);
    long long GetDdosDrops() const { return ddosDrops_.load(std::memory_order_relaxed); }

    struct BackendSnapshot {
        std::string id;
        bool healthy{false};
        bool online{false};
        int weight{0};
        int baseWeight{0};
        int activeConnections{0};
        double ewmaResponseMs{0.0};
        long long failures{0};
        long long successes{0};
        double errorRate{0.0};
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
    void SetBackendSnapshot(std::vector<BackendSnapshot> backends);

    // Records per-request latency in milliseconds (keeps a sliding window).
    void RecordRequestLatencyMs(double ms);

    // Business-layer metrics (best-effort, bounded maps).
    void RecordRequestMethod(const std::string& method);
    void RecordRequestPath(const std::string& path);
    void RecordModelName(const std::string& model);

    std::string ToJson();
    std::string ToJsonCached(double maxAgeMs = 100.0);

private:
    Stats();
    
    std::atomic<long> totalRequests_{0};
    std::atomic<long> activeConnections_{0};
    std::atomic<long> backendFailures_{0};
    std::atomic<long long> bytesIn_{0};
    std::atomic<long long> bytesOut_{0};
    std::atomic<long long> udpRxDrops_{0};
    std::atomic<long long> ddosDrops_{0};
    std::chrono::system_clock::time_point startTime_;

    mutable std::mutex mutex_;
    std::vector<BackendSnapshot> backends_;
    std::vector<double> recentLatMs_;
    size_t latRingPos_{0};

    static constexpr size_t kMaxBusinessKeys = 1024;
    std::unordered_map<std::string, unsigned long long> methodCounts_;
    std::unordered_map<std::string, unsigned long long> pathCounts_;
    std::unordered_map<std::string, unsigned long long> modelCounts_;

    std::string cachedJson_;
    std::chrono::steady_clock::time_point cachedAt_{};
};

} // namespace monitor
} // namespace proxy
