#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/EventLoop.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace proxy {
namespace network {
class TcpClient;
}
namespace monitor {

class AlertManager : proxy::common::noncopyable {
public:
    struct EmailConfig {
        std::string smtpHost;
        uint16_t smtpPort{25};
        std::string mailFrom;
        std::string mailTo;
        std::string subjectPrefix{"Proxy Alert"};
    };

    struct Thresholds {
        long maxActiveConnections{-1};     // <0 disables
        double maxCpuPct{-1.0};            // <0 disables
        long long maxRssBytes{-1};         // <0 disables
        int maxFdCount{-1};               // <0 disables
        double maxBackendErrorRate{-1.0};  // <0 disables (0..1)
    };

    struct AnomalyConfig {
        bool enabled{false};
        double zThreshold{3.0};   // larger => less sensitive
        double alpha{0.2};        // EWMA smoothing factor (0..1)
        int minSamples{10};       // warmup samples before alerting
    };

    struct Config {
        bool enabled{false};
        double intervalSec{1.0};
        double cooldownSec{30.0}; // per-metric suppression
        double mergeWindowSec{0.2}; // coalesce alerts into one send (storm control)
        std::string webhookUrl;   // e.g. http://127.0.0.1:9000/alert
        std::string smsWebhookUrl; // e.g. http://127.0.0.1:9001/sms
        EmailConfig email;
        Thresholds thresholds;
        AnomalyConfig anomaly;
    };

    explicit AlertManager(proxy::network::EventLoop* loop, Config cfg);
    ~AlertManager();

    void Start();
    void Stop();

private:
    struct WebhookTarget {
        std::string host;
        uint16_t port{0};
        std::string path{"/"};
    };

    struct AlertItem {
        std::string type; // "threshold" | "anomaly"
        std::string metric;
        std::string value;
        std::string threshold;
    };

    static bool ParseWebhookUrl(const std::string& url, WebhookTarget* out);
    void ScheduleNext();
    void OnTimer();
    void EvaluateAndSend();
    void ScheduleFlush();
    void FlushPending();
    void SendWebhook(const WebhookTarget& t, const std::string& name, const std::string& body);
    void SendEmail(const std::string& body);

    proxy::network::EventLoop* loop_;
    Config cfg_;
    WebhookTarget webhook_;
    WebhookTarget smsWebhook_;
    EmailConfig email_;

    int timerFd_{-1};
    std::shared_ptr<proxy::network::Channel> timerChannel_;

    std::map<std::string, std::chrono::steady_clock::time_point> lastSent_;
    std::map<std::string, AlertItem> pending_;
    bool flushScheduled_{false};
    int flushTimerFd_{-1};
    std::shared_ptr<proxy::network::Channel> flushTimerChannel_;

    double lastCpuTimeSec_{0.0};
    std::chrono::steady_clock::time_point lastWall_{};

    struct EwmaState {
        int n{0};
        double mean{0.0};
        double var{0.0};
    };
    std::map<std::string, EwmaState> ewma_;
    long lastTotalRequests_{0};
    long lastBackendFailures_{0};

    std::vector<std::shared_ptr<proxy::network::TcpClient>> inFlight_;
};

} // namespace monitor
} // namespace proxy
