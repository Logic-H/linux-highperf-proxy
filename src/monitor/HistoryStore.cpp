#include "proxy/monitor/HistoryStore.h"
#include "proxy/common/Logger.h"
#include "proxy/monitor/Stats.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/timerfd.h>
#include <unistd.h>

namespace proxy {
namespace monitor {

static std::int64_t NowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

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

// (reserved for future string fields; currently unused)

HistoryStore::HistoryStore(proxy::network::EventLoop* loop, Config cfg) : loop_(loop), cfg_(std::move(cfg)) {
    lastWall_ = std::chrono::steady_clock::now();
    lastCpuTimeSec_ = ReadProcessCpuTimeSec();
}

HistoryStore::~HistoryStore() { Stop(); }

void HistoryStore::Start() {
    if (!cfg_.enabled || !loop_) return;
    loop_->RunInLoop([this]() {
        if (timerFd_ >= 0) return;
        ring_.clear();
        ringPos_ = 0;
        ringFilled_ = false;
        ring_.resize(std::max<size_t>(1, cfg_.maxPoints));

        lastTotal_ = Stats::Instance().GetTotalRequests();
        lastFails_ = Stats::Instance().GetBackendFailures();
        lastBytesIn_ = Stats::Instance().GetBytesIn();
        lastBytesOut_ = Stats::Instance().GetBytesOut();
        lastWall_ = std::chrono::steady_clock::now();
        lastCpuTimeSec_ = ReadProcessCpuTimeSec();

        timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timerFd_ < 0) {
            LOG_ERROR << "HistoryStore timerfd_create failed";
            return;
        }
        timerCh_ = std::make_shared<proxy::network::Channel>(loop_, timerFd_);
        timerCh_->SetReadCallback([this](std::chrono::system_clock::time_point) {
            uint64_t one;
            ::read(timerFd_, &one, sizeof one);
            OnTimer();
        });
        timerCh_->EnableReading();
        ArmTimer();
    });
}

void HistoryStore::Stop() {
    if (!loop_) return;
    loop_->RunInLoop([this]() {
        if (timerCh_) {
            timerCh_->DisableAll();
            timerCh_->Remove();
            timerCh_.reset();
        }
        if (timerFd_ >= 0) {
            ::close(timerFd_);
            timerFd_ = -1;
        }
        persist_.reset();
    });
}

void HistoryStore::ArmTimer() {
    if (timerFd_ < 0) return;
    int ms = cfg_.sampleMs;
    if (ms < 50) ms = 50;
    struct itimerspec howlong;
    std::memset(&howlong, 0, sizeof howlong);
    howlong.it_interval.tv_sec = ms / 1000;
    howlong.it_interval.tv_nsec = (ms % 1000) * 1000000L;
    howlong.it_value = howlong.it_interval;
    ::timerfd_settime(timerFd_, 0, &howlong, nullptr);
}

void HistoryStore::OnTimer() { SampleOnce(); }

void HistoryStore::SampleOnce() {
    Point p;
    p.tsMs = NowMs();
    auto& s = Stats::Instance();
    p.activeConnections = s.GetActiveConnections();
    p.totalRequests = s.GetTotalRequests();
    p.backendFailures = s.GetBackendFailures();
    p.bytesIn = s.GetBytesIn();
    p.bytesOut = s.GetBytesOut();

    const long dTotal = p.totalRequests - lastTotal_;
    const long dFails = p.backendFailures - lastFails_;
    (void)(p.bytesIn - lastBytesIn_);
    (void)(p.bytesOut - lastBytesOut_);
    lastTotal_ = p.totalRequests;
    lastFails_ = p.backendFailures;
    lastBytesIn_ = p.bytesIn;
    lastBytesOut_ = p.bytesOut;

    const double dt = std::max(1e-6, static_cast<double>(cfg_.sampleMs) / 1000.0);
    p.qps = static_cast<double>(std::max<long>(0, dTotal)) / dt;
    p.backendErrorRateInterval = (dTotal > 0) ? (static_cast<double>(dFails) / static_cast<double>(dTotal)) : 0.0;

    p.rssBytes = ReadRssBytes();
    p.fdCount = ReadFdCount();
    const double cpuNow = ReadProcessCpuTimeSec();
    const auto wallNow = std::chrono::steady_clock::now();
    const double wallSec = std::chrono::duration_cast<std::chrono::duration<double>>(wallNow - lastWall_).count();
    // Guard against extremely small wall interval (timer jitter, manual calls): would inflate cpu% massively.
    if (wallSec < 0.05) {
        p.cpuPctSingleCore = 0.0;
        lastCpuTimeSec_ = cpuNow;
        lastWall_ = wallNow;
    } else {
        double dCpu = cpuNow - lastCpuTimeSec_;
        if (dCpu < 0.0) dCpu = 0.0;
        double cpuPct = (dCpu / wallSec) * 100.0;
        if (cpuPct > 100000.0) cpuPct = 0.0;
        p.cpuPctSingleCore = cpuPct;
        lastCpuTimeSec_ = cpuNow;
        lastWall_ = wallNow;
    }

    // Latency percentiles: reuse Stats json cached to avoid adding new API (simple string search).
    // This is best-effort; if missing, keep zeros.
    {
        const std::string j = s.ToJsonCached(50.0);
        auto findNum = [&](const char* key) -> double {
            const std::string k = std::string("\"") + key + "\":";
            const size_t pos = j.find(k);
            if (pos == std::string::npos) return 0.0;
            size_t i = pos + k.size();
            while (i < j.size() && (j[i] == ' ')) ++i;
            size_t e = i;
            while (e < j.size() && (std::isdigit(static_cast<unsigned char>(j[e])) || j[e] == '.' || j[e] == '-')) ++e;
            try {
                return std::stod(j.substr(i, e - i));
            } catch (...) {
                return 0.0;
            }
        };
        p.p50Ms = findNum("p50_ms");
        p.p90Ms = findNum("p90_ms");
        p.p99Ms = findNum("p99_ms");
        p.avgMs = findNum("avg_ms");
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        ring_[ringPos_] = p;
        ringPos_ = (ringPos_ + 1) % ring_.size();
        if (ringPos_ == 0) ringFilled_ = true;
    }
    PersistPoint(p);
}

void HistoryStore::PersistPoint(const Point& p) {
    if (cfg_.persistPath.empty()) return;
    if (!persist_) {
        persist_ = std::make_unique<std::ofstream>(cfg_.persistPath, std::ios::app);
        if (!persist_ || !persist_->is_open()) {
            persist_.reset();
            return;
        }
    }
    (*persist_) << "{\"ts_ms\":" << p.tsMs << ",\"qps\":" << std::fixed << std::setprecision(2) << p.qps
                << ",\"active\":" << p.activeConnections << ",\"total\":" << p.totalRequests
                << ",\"backend_error_rate_interval\":" << std::fixed << std::setprecision(6) << p.backendErrorRateInterval
                << "}\n";
    persist_->flush();
}

std::vector<HistoryStore::Point> HistoryStore::QueryLastSeconds(int seconds) const {
    if (seconds <= 0) return {};
    std::lock_guard<std::mutex> lock(mu_);
    if (ring_.empty()) return {};

    const std::int64_t now = NowMs();
    const std::int64_t from = now - static_cast<std::int64_t>(seconds) * 1000;

    std::vector<Point> out;
    out.reserve(ring_.size());

    const size_t n = ring_.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = ringFilled_ ? ((ringPos_ + i) % n) : i;
        const Point& p = ring_[idx];
        if (!ringFilled_ && p.tsMs == 0) continue;
        if (p.tsMs >= from) out.push_back(p);
    }
    return out;
}

std::string HistoryStore::PointsLastSecondsJson(int seconds) const {
    auto pts = QueryLastSeconds(seconds);
    std::ostringstream ss;
    ss << "{";
    ss << "\"now_ms\":" << NowMs() << ",";
    const long cpuCores = ::sysconf(_SC_NPROCESSORS_ONLN);
    ss << "\"cpu_cores\":" << (cpuCores > 0 ? cpuCores : 1) << ",";
    ss << "\"points\":[";
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& p = pts[i];
        if (i) ss << ",";
        ss << "{";
        ss << "\"ts_ms\":" << p.tsMs << ",";
        ss << "\"active_connections\":" << p.activeConnections << ",";
        ss << "\"total_requests\":" << p.totalRequests << ",";
        ss << "\"qps\":" << std::fixed << std::setprecision(2) << p.qps << ",";
        ss << "\"backend_error_rate_interval\":" << std::fixed << std::setprecision(6) << p.backendErrorRateInterval << ",";
        ss << "\"p99_ms\":" << std::fixed << std::setprecision(3) << p.p99Ms << ",";
        ss << "\"bytes_in\":" << p.bytesIn << ",";
        ss << "\"bytes_out\":" << p.bytesOut << ",";
        ss << "\"rss_bytes\":" << p.rssBytes << ",";
        ss << "\"fd_count\":" << p.fdCount << ",";
        ss << "\"cpu_pct_single_core\":" << std::fixed << std::setprecision(2) << p.cpuPctSingleCore;
        ss << "}";
    }
    ss << "]}";
    return ss.str();
}

std::string HistoryStore::SummaryLastSecondsJson(int seconds) const {
    auto pts = QueryLastSeconds(seconds);
    if (pts.empty()) return "{\"error\":\"no data\"}";

    auto mmavg = [&](auto getter) {
        double mn = getter(pts[0]);
        double mx = mn;
        double sum = 0.0;
        for (const auto& p : pts) {
            const double v = getter(p);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            sum += v;
        }
        return std::tuple<double, double, double>(mn, mx, sum / static_cast<double>(pts.size()));
    };

    auto [qmn, qmx, qavg] = mmavg([](const Point& p) { return p.qps; });
    auto [emn, emx, eavg] = mmavg([](const Point& p) { return p.backendErrorRateInterval; });
    auto [pmn, pmx, pavg] = mmavg([](const Point& p) { return p.p99Ms; });
    auto [rmn, rmx, ravg] = mmavg([](const Point& p) { return static_cast<double>(p.rssBytes); });

    std::ostringstream ss;
    ss << "{";
    ss << "\"seconds\":" << seconds << ",";
    ss << "\"points\":" << pts.size() << ",";
    ss << "\"qps\":{\"min\":" << std::fixed << std::setprecision(2) << qmn << ",\"max\":" << qmx << ",\"avg\":" << qavg << "},";
    ss << "\"backend_error_rate_interval\":{\"min\":" << std::fixed << std::setprecision(6) << emn << ",\"max\":" << emx
       << ",\"avg\":" << eavg << "},";
    ss << "\"p99_ms\":{\"min\":" << std::fixed << std::setprecision(3) << pmn << ",\"max\":" << pmx << ",\"avg\":" << pavg << "},";
    ss << "\"rss_bytes\":{\"min\":" << std::fixed << std::setprecision(0) << rmn << ",\"max\":" << rmx << ",\"avg\":" << ravg << "}";
    ss << "}";
    return ss.str();
}

} // namespace monitor
} // namespace proxy
