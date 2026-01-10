#include "proxy/monitor/AlertManager.h"
#include "proxy/common/Logger.h"
#include "proxy/monitor/Stats.h"
#include "proxy/network/Channel.h"
#include "proxy/network/TcpClient.h"
#include "proxy/network/InetAddress.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace proxy {
namespace monitor {

static long long ReadRssBytes() {
    std::ifstream f("/proc/self/status");
    if (!f.is_open()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line);
            std::string key;
            long long kb = 0;
            std::string unit;
            iss >> key >> kb >> unit;
            return kb * 1024;
        }
    }
    return 0;
}

static int ReadFdCount() {
    int count = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return 0;
    while (true) {
        dirent* e = ::readdir(d);
        if (!e) break;
        if (e->d_name[0] == '.') continue;
        count += 1;
    }
    ::closedir(d);
    return count;
}

static double ReadProcessCpuTimeSec() {
    std::ifstream f("/proc/self/stat");
    if (!f.is_open()) return 0.0;
    std::string line;
    std::getline(f, line);
    const size_t rparen = line.rfind(')');
    if (rparen == std::string::npos) return 0.0;
    const std::string rest = (rparen + 2 <= line.size()) ? line.substr(rparen + 2) : std::string();
    std::istringstream iss(rest);
    std::vector<std::string> fields;
    std::string tok;
    while (iss >> tok) fields.push_back(tok);
    if (fields.size() < 13) return 0.0;
    long long utime = 0;
    long long stime = 0;
    try {
        utime = std::stoll(fields[11]);
        stime = std::stoll(fields[12]);
    } catch (...) {
        return 0.0;
    }
    const long ticks = ::sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return 0.0;
    return static_cast<double>(utime + stime) / static_cast<double>(ticks);
}

AlertManager::AlertManager(proxy::network::EventLoop* loop, Config cfg)
    : loop_(loop), cfg_(std::move(cfg)) {
    lastWall_ = std::chrono::steady_clock::now();
    lastCpuTimeSec_ = ReadProcessCpuTimeSec();
    email_ = cfg_.email;
    if (!cfg_.webhookUrl.empty()) {
        (void)ParseWebhookUrl(cfg_.webhookUrl, &webhook_);
    }
    if (!cfg_.smsWebhookUrl.empty()) {
        (void)ParseWebhookUrl(cfg_.smsWebhookUrl, &smsWebhook_);
    }
}

AlertManager::~AlertManager() { Stop(); }

bool AlertManager::ParseWebhookUrl(const std::string& url, WebhookTarget* out) {
    if (!out) return false;
    // Minimal parser: http://host:port/path
    std::string u = url;
    if (u.rfind("http://", 0) == 0) u.erase(0, 7);
    if (u.empty()) return false;
    std::string hostport;
    std::string path = "/";
    size_t slash = u.find('/');
    if (slash == std::string::npos) hostport = u;
    else {
        hostport = u.substr(0, slash);
        path = u.substr(slash);
        if (path.empty()) path = "/";
    }
    size_t colon = hostport.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = hostport.substr(0, colon);
    std::string portStr = hostport.substr(colon + 1);
    if (host.empty() || portStr.empty()) return false;
    int port = 0;
    try {
        port = std::stoi(portStr);
    } catch (...) {
        return false;
    }
    if (port <= 0 || port > 65535) return false;
    out->host = host;
    out->port = static_cast<uint16_t>(port);
    out->path = path;
    return true;
}

void AlertManager::Start() {
    if (!cfg_.enabled) return;
    if (!cfg_.webhookUrl.empty() && (webhook_.host.empty() || webhook_.port == 0)) {
        if (!ParseWebhookUrl(cfg_.webhookUrl, &webhook_)) {
            LOG_ERROR << "AlertManager invalid webhook_url: " << cfg_.webhookUrl;
        }
    }
    if (!cfg_.smsWebhookUrl.empty() && (smsWebhook_.host.empty() || smsWebhook_.port == 0)) {
        if (!ParseWebhookUrl(cfg_.smsWebhookUrl, &smsWebhook_)) {
            LOG_ERROR << "AlertManager invalid sms_webhook_url: " << cfg_.smsWebhookUrl;
        }
    }
    email_ = cfg_.email;
    // Seed interval-based anomaly metrics to avoid a large first delta.
    lastTotalRequests_ = Stats::Instance().GetTotalRequests();
    lastBackendFailures_ = Stats::Instance().GetBackendFailures();

    loop_->RunInLoop([this]() {
        if (timerFd_ >= 0) return;
        timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timerFd_ < 0) {
            LOG_ERROR << "AlertManager timerfd_create failed";
            return;
        }
        timerChannel_ = std::make_shared<proxy::network::Channel>(loop_, timerFd_);
        timerChannel_->SetReadCallback([this](std::chrono::system_clock::time_point) {
            uint64_t one;
            ::read(timerFd_, &one, sizeof one);
            OnTimer();
        });
        timerChannel_->EnableReading();

        if (cfg_.mergeWindowSec > 0.0 && flushTimerFd_ < 0) {
            flushTimerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (flushTimerFd_ < 0) {
                LOG_ERROR << "AlertManager flush timerfd_create failed";
            } else {
                flushTimerChannel_ = std::make_shared<proxy::network::Channel>(loop_, flushTimerFd_);
                flushTimerChannel_->SetReadCallback([this](std::chrono::system_clock::time_point) {
                    uint64_t one;
                    ::read(flushTimerFd_, &one, sizeof one);
                    flushScheduled_ = false;
                    FlushPending();
                });
                flushTimerChannel_->EnableReading();
            }
        }
        ScheduleNext();
    });
}

void AlertManager::Stop() {
    cfg_.enabled = false;
    if (!loop_) return;
    loop_->QueueInLoop([this]() {
        if (timerChannel_) {
            timerChannel_->DisableAll();
            timerChannel_->Remove();
            timerChannel_.reset();
        }
        if (timerFd_ >= 0) {
            ::close(timerFd_);
            timerFd_ = -1;
        }
        if (flushTimerChannel_) {
            flushTimerChannel_->DisableAll();
            flushTimerChannel_->Remove();
            flushTimerChannel_.reset();
        }
        if (flushTimerFd_ >= 0) {
            ::close(flushTimerFd_);
            flushTimerFd_ = -1;
        }
        pending_.clear();
        flushScheduled_ = false;
        inFlight_.clear();
    });
}

void AlertManager::ScheduleNext() {
    if (timerFd_ < 0) return;
    double sec = cfg_.intervalSec;
    if (sec <= 0.0) sec = 1.0;
    struct itimerspec howlong;
    std::memset(&howlong, 0, sizeof howlong);
    const long s = static_cast<long>(sec);
    const long ns = static_cast<long>((sec - s) * 1e9);
    howlong.it_value.tv_sec = (s > 0) ? s : 0;
    howlong.it_value.tv_nsec = (ns > 0) ? ns : 1000000;
    ::timerfd_settime(timerFd_, 0, &howlong, nullptr);
}

void AlertManager::OnTimer() {
    EvaluateAndSend();
    ScheduleNext();
}

void AlertManager::ScheduleFlush() {
    if (cfg_.mergeWindowSec <= 0.0) {
        FlushPending();
        return;
    }
    if (flushScheduled_) return;
    if (flushTimerFd_ < 0) {
        FlushPending();
        return;
    }
    flushScheduled_ = true;
    double sec = cfg_.mergeWindowSec;
    if (sec < 0.0) sec = 0.0;
    struct itimerspec howlong;
    std::memset(&howlong, 0, sizeof howlong);
    const long s = static_cast<long>(sec);
    const long ns = static_cast<long>((sec - s) * 1e9);
    howlong.it_value.tv_sec = (s > 0) ? s : 0;
    howlong.it_value.tv_nsec = (ns > 0) ? ns : 1000000;
    ::timerfd_settime(flushTimerFd_, 0, &howlong, nullptr);
}

void AlertManager::FlushPending() {
    if (pending_.empty()) return;

    const bool hasWebhook = (!webhook_.host.empty() && webhook_.port != 0);
    const bool hasSmsWebhook = (!smsWebhook_.host.empty() && smsWebhook_.port != 0);
    const bool hasEmail = (!email_.smtpHost.empty() && !email_.mailFrom.empty() && !email_.mailTo.empty() && email_.smtpPort != 0);
    if (!hasWebhook && !hasSmsWebhook && !hasEmail) {
        pending_.clear();
        flushScheduled_ = false;
        return;
    }

    std::vector<AlertItem> alerts;
    alerts.reserve(pending_.size());
    for (const auto& kv : pending_) alerts.push_back(kv.second);

    std::string outerType = "mixed";
    if (!alerts.empty()) {
        outerType = alerts.front().type;
        for (const auto& a : alerts) {
            if (a.type != outerType) {
                outerType = "mixed";
                break;
            }
        }
    }

    std::ostringstream body;
    body << "{";
    body << "\"type\":\"" << outerType << "\",";
    body << "\"alerts\":[";
    for (size_t i = 0; i < alerts.size(); ++i) {
        const auto& a = alerts[i];
        body << "{\"type\":\"" << a.type << "\",\"metric\":\"" << a.metric << "\",\"value\":\"" << a.value << "\",\"threshold\":\""
             << a.threshold << "\"}" << (i + 1 < alerts.size() ? "," : "");
    }
    body << "]";
    body << "}";

    const std::string payload = body.str();
    if (hasWebhook) SendWebhook(webhook_, "AlertWebhook", payload);
    if (hasSmsWebhook) SendWebhook(smsWebhook_, "AlertSms", payload);
    if (hasEmail) SendEmail(payload);

    const auto now = std::chrono::steady_clock::now();
    for (const auto& kv : pending_) lastSent_[kv.first] = now;
    pending_.clear();
    flushScheduled_ = false;
}

void AlertManager::EvaluateAndSend() {
    if (!cfg_.enabled) return;
    const bool hasWebhook = (!webhook_.host.empty() && webhook_.port != 0);
    const bool hasSmsWebhook = (!smsWebhook_.host.empty() && smsWebhook_.port != 0);
    const bool hasEmail = (!email_.smtpHost.empty() && !email_.mailFrom.empty() && !email_.mailTo.empty() && email_.smtpPort != 0);
    if (!hasWebhook && !hasSmsWebhook && !hasEmail) return;

    std::vector<AlertItem> alerts;
    alerts.reserve(8);

    const auto now = std::chrono::steady_clock::now();
    auto allowMetric = [&](const std::string& key) {
        auto it = lastSent_.find(key);
        if (it == lastSent_.end()) return true;
        const double age = std::chrono::duration_cast<std::chrono::duration<double>>(now - it->second).count();
        return age >= std::max(0.0, cfg_.cooldownSec);
    };
    auto keyOf = [&](const std::string& type, const std::string& metric) { return type + ":" + metric; };
    auto& s = Stats::Instance();

    if (cfg_.thresholds.maxActiveConnections >= 0) {
        long v = s.GetActiveConnections();
        if (v > cfg_.thresholds.maxActiveConnections && allowMetric(keyOf("threshold", "active_connections"))) {
            alerts.push_back({"threshold", "active_connections", std::to_string(v), std::to_string(cfg_.thresholds.maxActiveConnections)});
        }
    }

    // CPU pct sampled over last interval (single core %).
    if (cfg_.thresholds.maxCpuPct >= 0.0) {
        const double cpuNow = ReadProcessCpuTimeSec();
        const auto wallNow = std::chrono::steady_clock::now();
        const double wallSec = std::chrono::duration_cast<std::chrono::duration<double>>(wallNow - lastWall_).count();
        double cpuPct = 0.0;
        if (wallSec > 0.0) cpuPct = ((cpuNow - lastCpuTimeSec_) / wallSec) * 100.0;
        lastCpuTimeSec_ = cpuNow;
        lastWall_ = wallNow;
        if (cpuPct > cfg_.thresholds.maxCpuPct && allowMetric(keyOf("threshold", "cpu_pct"))) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) << cpuPct;
            std::ostringstream thr;
            thr << std::fixed << std::setprecision(2) << cfg_.thresholds.maxCpuPct;
            alerts.push_back({"threshold", "cpu_pct", oss.str(), thr.str()});
        }
    }

    if (cfg_.thresholds.maxRssBytes >= 0) {
        long long rss = ReadRssBytes();
        if (rss > cfg_.thresholds.maxRssBytes && allowMetric(keyOf("threshold", "rss_bytes"))) {
            alerts.push_back({"threshold", "rss_bytes", std::to_string(rss), std::to_string(cfg_.thresholds.maxRssBytes)});
        }
    }

    if (cfg_.thresholds.maxFdCount >= 0) {
        int fds = ReadFdCount();
        if (fds > cfg_.thresholds.maxFdCount && allowMetric(keyOf("threshold", "fd_count"))) {
            alerts.push_back({"threshold", "fd_count", std::to_string(fds), std::to_string(cfg_.thresholds.maxFdCount)});
        }
    }

    if (cfg_.thresholds.maxBackendErrorRate >= 0.0) {
        const long total = s.GetTotalRequests();
        const long fails = s.GetBackendFailures();
        double rate = 0.0;
        if (total > 0) rate = static_cast<double>(fails) / static_cast<double>(total);
        if (rate > cfg_.thresholds.maxBackendErrorRate && allowMetric(keyOf("threshold", "backend_error_rate"))) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6) << rate;
            std::ostringstream thr;
            thr << std::fixed << std::setprecision(6) << cfg_.thresholds.maxBackendErrorRate;
            alerts.push_back({"threshold", "backend_error_rate", oss.str(), thr.str()});
        }
    }

    // Anomaly detection based on historical (EWMA) interval metrics.
    if (cfg_.anomaly.enabled) {
        const long total = s.GetTotalRequests();
        const long fails = s.GetBackendFailures();
        const long dTotal = total - lastTotalRequests_;
        const long dFails = fails - lastBackendFailures_;
        lastTotalRequests_ = total;
        lastBackendFailures_ = fails;

        double intervalErrRate = 0.0;
        if (dTotal > 0) intervalErrRate = static_cast<double>(dFails) / static_cast<double>(dTotal);
        const std::string metric = "backend_error_rate_interval";
        auto& ew = ewma_[metric];

        double baseline = ew.mean;
        double z = 0.0;
        bool isAnom = false;
        if (ew.n == 0) {
            ew.n = 1;
            ew.mean = intervalErrRate;
            ew.var = 0.0;
        } else {
            baseline = ew.mean;
            const double var = ew.var;
            const double diff = intervalErrRate - baseline;
            const double denom = std::sqrt(std::max(1e-12, var));
            z = diff / denom;
            const double a = std::min(1.0, std::max(0.0, cfg_.anomaly.alpha));
            ew.mean = baseline + a * diff;
            ew.var = (1.0 - a) * (var + a * diff * diff);
            ew.n += 1;
            if (ew.n >= std::max(1, cfg_.anomaly.minSamples) && std::abs(z) >= std::max(0.0, cfg_.anomaly.zThreshold)) {
                isAnom = true;
            }
        }

        if (isAnom && allowMetric(keyOf("anomaly", metric))) {
            std::ostringstream v;
            v << std::fixed << std::setprecision(6) << intervalErrRate;
            std::ostringstream thr;
            thr << "z>=" << std::fixed << std::setprecision(2) << cfg_.anomaly.zThreshold << " baseline=" << std::fixed
                << std::setprecision(6) << baseline << " z=" << std::fixed << std::setprecision(2) << z;
            alerts.push_back({"anomaly", metric, v.str(), thr.str()});
        }
    }

    if (alerts.empty()) return;

    for (const auto& a : alerts) {
        pending_[keyOf(a.type, a.metric)] = a;
    }
    if (cfg_.mergeWindowSec <= 0.0) {
        FlushPending();
    } else {
        ScheduleFlush();
    }
}

void AlertManager::SendWebhook(const WebhookTarget& t, const std::string& name, const std::string& body) {
    using proxy::network::InetAddress;
    using proxy::network::TcpClient;

    auto client = std::make_shared<TcpClient>(loop_, InetAddress(t.host, t.port), name);
    inFlight_.push_back(client);

    std::weak_ptr<TcpClient> weakClient = client;
    auto removeInFlight = [this, weakClient]() {
        if (auto cc = weakClient.lock()) {
            auto it = std::find(inFlight_.begin(), inFlight_.end(), cc);
            if (it != inFlight_.end()) inFlight_.erase(it);
        }
    };

    client->SetConnectionCallback([this, t, body](const proxy::network::TcpConnectionPtr& c) {
        if (!c->connected()) {
            return;
        }
        std::ostringstream req;
        req << "POST " << t.path << " HTTP/1.1\r\n";
        req << "Host: " << t.host << ":" << t.port << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
        req << "Connection: close\r\n";
        req << "\r\n";
        req << body;
        c->Send(req.str());
        c->Shutdown();
    });
    client->SetWriteCompleteCallback([removeInFlight](const proxy::network::TcpConnectionPtr&) { removeInFlight(); });
    client->SetMessageCallback([removeInFlight](const proxy::network::TcpConnectionPtr&, proxy::network::Buffer* b,
                                                std::chrono::system_clock::time_point) {
        b->RetrieveAll();
        removeInFlight();
    });
    client->Connect();
}

void AlertManager::SendEmail(const std::string& body) {
    using proxy::network::InetAddress;
    using proxy::network::TcpClient;

    auto client = std::make_shared<TcpClient>(loop_, InetAddress(email_.smtpHost, email_.smtpPort), "AlertEmail");
    inFlight_.push_back(client);

    std::weak_ptr<TcpClient> weakClient = client;
    auto removeInFlight = [this, weakClient]() {
        if (auto cc = weakClient.lock()) {
            auto it = std::find(inFlight_.begin(), inFlight_.end(), cc);
            if (it != inFlight_.end()) inFlight_.erase(it);
        }
    };

    client->SetConnectionCallback([this, body](const proxy::network::TcpConnectionPtr& c) {
        if (!c->connected()) return;
        std::ostringstream mail;
        mail << "HELO proxy\r\n";
        mail << "MAIL FROM:<" << email_.mailFrom << ">\r\n";
        mail << "RCPT TO:<" << email_.mailTo << ">\r\n";
        mail << "DATA\r\n";
        mail << "Subject: " << email_.subjectPrefix << "\r\n";
        mail << "From: " << email_.mailFrom << "\r\n";
        mail << "To: " << email_.mailTo << "\r\n";
        mail << "\r\n";
        mail << body << "\r\n";
        mail << ".\r\n";
        mail << "QUIT\r\n";
        c->Send(mail.str());
        c->Shutdown();
    });
    client->SetWriteCompleteCallback([removeInFlight](const proxy::network::TcpConnectionPtr&) { removeInFlight(); });
    client->SetMessageCallback([removeInFlight](const proxy::network::TcpConnectionPtr&, proxy::network::Buffer* b,
                                                std::chrono::system_clock::time_point) {
        b->RetrieveAll();
        removeInFlight();
    });
    client->Connect();
}

} // namespace monitor
} // namespace proxy
