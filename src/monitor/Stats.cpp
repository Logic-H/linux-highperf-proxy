#include "proxy/monitor/Stats.h"
#include "proxy/common/Config.h"
#include "proxy/common/MemoryPool.h"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <map>

namespace proxy {
namespace monitor {

Stats& Stats::Instance() {
    static Stats instance;
    return instance;
}

Stats::Stats() {
    startTime_ = std::chrono::system_clock::now();
}

void Stats::AddDdosDrops(long long n) {
    if (n <= 0) return;
    ddosDrops_.fetch_add(n, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    cachedJson_.clear();
}

void Stats::SetBackendSnapshot(std::vector<BackendSnapshot> backends) {
    std::lock_guard<std::mutex> lock(mutex_);
    backends_ = std::move(backends);
    cachedJson_.clear();
}

void Stats::RecordRequestLatencyMs(double ms) {
    if (ms < 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr size_t kWindow = 256;
    if (recentLatMs_.empty()) {
        recentLatMs_.assign(kWindow, 0.0);
        latRingPos_ = 0;
    }
    recentLatMs_[latRingPos_ % recentLatMs_.size()] = ms;
    latRingPos_ += 1;
}

static void BoundedInc(std::unordered_map<std::string, unsigned long long>& m,
                       const std::string& key,
                       size_t maxKeys,
                       const std::string& overflowKey) {
    if (key.empty()) return;
    auto it = m.find(key);
    if (it != m.end()) {
        it->second += 1;
        return;
    }
    if (m.size() >= maxKeys) {
        m[overflowKey] += 1;
        return;
    }
    m.emplace(key, 1ULL);
}

void Stats::RecordRequestMethod(const std::string& method) {
    std::lock_guard<std::mutex> lock(mutex_);
    BoundedInc(methodCounts_, method, kMaxBusinessKeys, "OTHER");
    cachedJson_.clear();
}

void Stats::RecordRequestPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    BoundedInc(pathCounts_, path, kMaxBusinessKeys, "OTHER");
    cachedJson_.clear();
}

void Stats::RecordModelName(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    BoundedInc(modelCounts_, model, kMaxBusinessKeys, "OTHER");
    cachedJson_.clear();
}

static long long ReadRssBytes() {
    std::ifstream f("/proc/self/status");
    if (!f.is_open()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            // VmRSS:   12345 kB
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

    // comm can contain spaces, it is wrapped in parentheses. Find the last ')'.
    const size_t rparen = line.rfind(')');
    if (rparen == std::string::npos) return 0.0;
    const std::string rest = (rparen + 2 <= line.size()) ? line.substr(rparen + 2) : std::string();

    // rest starts from field 3 (state).
    std::istringstream iss(rest);
    std::vector<std::string> fields;
    std::string tok;
    while (iss >> tok) fields.push_back(tok);
    // Need at least up to stime (field15): indices 12 in fields (0-based).
    if (fields.size() < 13) return 0.0;

    // utime field14 -> index 11; stime field15 -> index 12
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

static bool ReadTcpSnmp(long long* outSegs, long long* retransSegs) {
    if (outSegs) *outSegs = 0;
    if (retransSegs) *retransSegs = 0;
    std::ifstream f("/proc/net/snmp");
    if (!f.is_open()) return false;
    std::string line1, line2;
    while (std::getline(f, line1)) {
        if (line1.rfind("Tcp:", 0) != 0) continue;
        if (!std::getline(f, line2)) break;
        if (line2.rfind("Tcp:", 0) != 0) continue;

        std::istringstream h(line1.substr(4));
        std::istringstream v(line2.substr(4));
        std::vector<std::string> keys;
        std::vector<long long> vals;
        std::string k;
        while (h >> k) keys.push_back(k);
        long long x = 0;
        while (v >> x) vals.push_back(x);
        if (keys.size() != vals.size() || keys.empty()) return false;

        std::map<std::string, long long> m;
        for (size_t i = 0; i < keys.size(); ++i) m[keys[i]] = vals[i];
        if (outSegs && m.count("OutSegs")) *outSegs = m["OutSegs"];
        if (retransSegs && m.count("RetransSegs")) *retransSegs = m["RetransSegs"];
        return true;
    }
    return false;
}

std::string Stats::ToJson() {
    auto now = std::chrono::system_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
    
    std::stringstream ss;
    ss << "{\n";
    ss << "  \"uptime_sec\": " << uptime << ",\n";
    ss << "  \"total_requests\": " << totalRequests_.load() << ",\n";
    ss << "  \"active_connections\": " << activeConnections_.load() << ",\n";
    ss << "  \"backend_failures\": " << backendFailures_.load() << ",\n";
    ss << "  \"bytes_in\": " << bytesIn_.load() << ",\n";
    ss << "  \"bytes_out\": " << bytesOut_.load() << ",\n";
    ss << "  \"udp_rx_drops\": " << udpRxDrops_.load() << ",\n";
    ss << "  \"ddos_drops\": " << ddosDrops_.load() << ",\n";

    // I/O model info (configured vs runtime selection).
    std::string configuredIoModel = proxy::common::Config::Instance().GetString("global", "io_model", "epoll");
    std::string runtimeIoModel = "epoll";
    if (::getenv("PROXY_USE_URING")) runtimeIoModel = "uring";
    else if (::getenv("PROXY_USE_SELECT")) runtimeIoModel = "select";
    else if (::getenv("PROXY_USE_POLL")) runtimeIoModel = "poll";
    ss << "  \"io\": {\n";
    ss << "    \"configured_model\": \"" << configuredIoModel << "\",\n";
    ss << "    \"runtime_model\": \"" << runtimeIoModel << "\",\n";
    ss << "    \"supported_models\": [\"select\", \"poll\", \"epoll\"";
#if PROXY_WITH_URING
    ss << ", \"uring\"";
#endif
    ss << "]\n";
    ss << "  },\n";
    
    double qps = (uptime > 0) ? (double)totalRequests_.load() / uptime : 0.0;
    ss << "  \"avg_qps\": " << std::fixed << std::setprecision(2) << qps << ",\n";
    const long totalReq = totalRequests_.load();
    const long backendFails = backendFailures_.load();
    const double backendErrRate = (totalReq > 0) ? (static_cast<double>(backendFails) / static_cast<double>(totalReq)) : 0.0;
    ss << "  \"backend_error_rate\": " << std::fixed << std::setprecision(6) << backendErrRate << ",\n";

    const auto mem = proxy::common::MemoryPool::Instance().GetStats();
    ss << "  \"memory\": {\n";
    ss << "    \"total_in_use_bytes\": " << mem.totalInUseBytes << ",\n";
    ss << "    \"slab_in_use_bytes\": " << mem.slabInUseBytes << ",\n";
    ss << "    \"slab_reserved_bytes\": " << mem.slabReservedBytes << ",\n";
    ss << "    \"buddy_in_use_bytes\": " << mem.buddyInUseBytes << ",\n";
    ss << "    \"buddy_reserved_bytes\": " << mem.buddyReservedBytes << ",\n";
    ss << "    \"buddy_arenas\": " << mem.buddyArenas << ",\n";
    ss << "    \"buddy_idle_arenas\": " << mem.buddyIdleArenas << ",\n";
    ss << "    \"buddy_arena_reclaims\": " << mem.buddyArenaReclaims << ",\n";
    ss << "    \"malloc_in_use_bytes\": " << mem.mallocInUseBytes << ",\n";
    ss << "    \"malloc_allocs\": " << mem.mallocAllocs << ",\n";
    ss << "    \"malloc_frees\": " << mem.mallocFrees << "\n";
    ss << "  }\n";

    // Request latency window (best-effort).
    {
        std::vector<double> lat;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lat = recentLatMs_;
        }
        lat.erase(std::remove_if(lat.begin(), lat.end(), [](double v) { return v <= 0.0; }), lat.end());
        if (!lat.empty()) {
            std::sort(lat.begin(), lat.end());
            auto pct = [&](double p) {
                size_t idx = static_cast<size_t>(p * (lat.size() - 1));
                return lat[idx];
            };
            ss << ",\n  \"latency_ms\": {\n";
            ss << "    \"p50_ms\": " << std::fixed << std::setprecision(3) << pct(0.50) << ",\n";
            ss << "    \"p90_ms\": " << std::fixed << std::setprecision(3) << pct(0.90) << ",\n";
            ss << "    \"p99_ms\": " << std::fixed << std::setprecision(3) << pct(0.99) << ",\n";
            double avg = 0.0;
            for (double v : lat) avg += v;
            avg /= static_cast<double>(lat.size());
            ss << "    \"avg_ms\": " << std::fixed << std::setprecision(3) << avg << "\n";
            ss << "  }";
        }
    }

    const long long rss = ReadRssBytes();
    const int fds = ReadFdCount();
    const double cpuTimeSec = ReadProcessCpuTimeSec();
    double cpuPct = 0.0;
    if (uptime > 0) cpuPct = (cpuTimeSec / static_cast<double>(uptime)) * 100.0;

    ss << ",\n  \"process\": {\n";
    ss << "    \"rss_bytes\": " << rss << ",\n";
    ss << "    \"fd_count\": " << fds << ",\n";
    ss << "    \"cpu_time_sec\": " << std::fixed << std::setprecision(3) << cpuTimeSec << ",\n";
    ss << "    \"cpu_pct_single_core_avg\": " << std::fixed << std::setprecision(2) << cpuPct << "\n";
    ss << "  }\n";

    // Network-layer: best-effort TCP retransmission ratio (approx packet loss).
    {
        long long outSegs = 0;
        long long retransSegs = 0;
        const bool ok = ReadTcpSnmp(&outSegs, &retransSegs);
        const double rate = (outSegs > 0) ? (static_cast<double>(retransSegs) / static_cast<double>(outSegs)) : 0.0;
        ss << ",\n  \"tcp\": {\n";
        ss << "    \"snmp_ok\": " << (ok ? "true" : "false") << ",\n";
        ss << "    \"out_segs\": " << outSegs << ",\n";
        ss << "    \"retrans_segs\": " << retransSegs << ",\n";
        ss << "    \"retrans_rate\": " << std::fixed << std::setprecision(6) << rate << "\n";
        ss << "  }\n";
    }

    // Business-layer: request type distribution + model invocation frequency (top-N).
    {
        std::vector<std::pair<std::string, unsigned long long>> methods;
        std::vector<std::pair<std::string, unsigned long long>> paths;
        std::vector<std::pair<std::string, unsigned long long>> models;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            methods.assign(methodCounts_.begin(), methodCounts_.end());
            paths.assign(pathCounts_.begin(), pathCounts_.end());
            models.assign(modelCounts_.begin(), modelCounts_.end());
        }

        auto sortDesc = [](auto& v) {
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
        };
        sortDesc(methods);
        sortDesc(paths);
        sortDesc(models);

        auto dumpTopArray = [&](const char* name,
                                const std::vector<std::pair<std::string, unsigned long long>>& v,
                                size_t topN) {
            ss << ",\n  \"" << name << "\": [\n";
            size_t n = std::min(topN, v.size());
            for (size_t i = 0; i < n; ++i) {
                ss << "    {\"key\": \"" << v[i].first << "\", \"count\": " << v[i].second << "}"
                   << (i + 1 < n ? "," : "") << "\n";
            }
            ss << "  ]\n";
        };

        dumpTopArray("top_methods", methods, 10);
        dumpTopArray("top_paths", paths, 10);
        dumpTopArray("top_models", models, 10);
    }

    // Backend snapshot (service-layer metrics)
    {
        std::vector<BackendSnapshot> bs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bs = backends_;
        }
        ss << ",\n  \"backends\": [\n";
        for (size_t i = 0; i < bs.size(); ++i) {
            const auto& b = bs[i];
            ss << "    {\n";
            ss << "      \"id\": \"" << b.id << "\",\n";
            ss << "      \"healthy\": " << (b.healthy ? "true" : "false") << ",\n";
            ss << "      \"online\": " << (b.online ? "true" : "false") << ",\n";
            ss << "      \"weight\": " << b.weight << ",\n";
            ss << "      \"base_weight\": " << b.baseWeight << ",\n";
            ss << "      \"active_connections\": " << b.activeConnections << ",\n";
            ss << "      \"queue_len\": " << b.queueLen << ",\n";
            ss << "      \"queue_len_external\": " << (b.hasQueueLen ? "true" : "false") << ",\n";
            ss << "      \"ewma_response_ms\": " << std::fixed << std::setprecision(3) << b.ewmaResponseMs << ",\n";
            ss << "      \"gpu_present\": " << (b.hasGpu ? "true" : "false") << ",\n";
            ss << "      \"gpu_util\": " << std::fixed << std::setprecision(3) << b.gpuUtil01 << ",\n";
            ss << "      \"vram_used_mb\": " << b.vramUsedMb << ",\n";
            ss << "      \"vram_total_mb\": " << b.vramTotalMb << ",\n";
            ss << "      \"model_loaded_present\": " << (b.hasModelLoaded ? "true" : "false") << ",\n";
            ss << "      \"model_loaded\": " << (b.modelLoaded ? "true" : "false") << ",\n";
            ss << "      \"model_name_present\": " << (b.hasModelName ? "true" : "false") << ",\n";
            ss << "      \"model_name\": \"" << b.modelName << "\",\n";
            ss << "      \"model_version_present\": " << (b.hasModelVersion ? "true" : "false") << ",\n";
            ss << "      \"model_version\": \"" << b.modelVersion << "\",\n";
            ss << "      \"successes\": " << b.successes << ",\n";
            ss << "      \"failures\": " << b.failures << ",\n";
            ss << "      \"error_rate\": " << std::fixed << std::setprecision(6) << b.errorRate << "\n";
            ss << "    }" << (i + 1 < bs.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
    }

    ss << "}";
    return ss.str();
}

std::string Stats::ToJsonCached(double maxAgeMs) {
    if (maxAgeMs < 0.0) maxAgeMs = 0.0;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cachedJson_.empty()) {
            const auto age = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - cachedAt_).count();
            if (age <= maxAgeMs) {
                return cachedJson_;
            }
        }
    }

    std::string fresh = ToJson();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cachedJson_ = fresh;
        cachedAt_ = now;
    }
    return fresh;
}

} // namespace monitor
} // namespace proxy
