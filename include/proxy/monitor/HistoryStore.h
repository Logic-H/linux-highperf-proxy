#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace proxy {
namespace monitor {

class HistoryStore : proxy::common::noncopyable {
public:
    struct Config {
        bool enabled{false};
        int sampleMs{1000};
        size_t maxPoints{3600}; // 1 hour @ 1s
        std::string persistPath; // optional (JSONL)
    };

    struct Point {
        std::int64_t tsMs{0};
        long activeConnections{0};
        long totalRequests{0};
        long backendFailures{0};
        long long bytesIn{0};
        long long bytesOut{0};

        // Derived per-interval metrics (best-effort).
        double qps{0.0};
        double backendErrorRateInterval{0.0};

        // Process (best-effort).
        long long rssBytes{0};
        int fdCount{0};
        double cpuPctSingleCore{0.0}; // over last interval

        // Latency percentiles from Stats sliding window (best-effort; 0 if unavailable).
        double p50Ms{0.0};
        double p90Ms{0.0};
        double p99Ms{0.0};
        double avgMs{0.0};
    };

    explicit HistoryStore(proxy::network::EventLoop* loop, Config cfg);
    ~HistoryStore();

    void Start();
    void Stop();

    std::vector<Point> QueryLastSeconds(int seconds) const;
    std::string SummaryLastSecondsJson(int seconds) const;
    std::string PointsLastSecondsJson(int seconds) const;

private:
    void ArmTimer();
    void OnTimer();
    void SampleOnce();
    void PersistPoint(const Point& p);

    proxy::network::EventLoop* loop_{nullptr};
    Config cfg_{};

    int timerFd_{-1};
    std::shared_ptr<proxy::network::Channel> timerCh_;

    mutable std::mutex mu_;
    std::vector<Point> ring_;
    size_t ringPos_{0};
    bool ringFilled_{false};

    long lastTotal_{0};
    long lastFails_{0};
    long long lastBytesIn_{0};
    long long lastBytesOut_{0};
    double lastCpuTimeSec_{0.0};
    std::chrono::steady_clock::time_point lastWall_{};

    // Lazy-opened append file.
    std::unique_ptr<std::ofstream> persist_;
};

} // namespace monitor
} // namespace proxy
