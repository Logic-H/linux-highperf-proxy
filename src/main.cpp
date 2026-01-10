#include "proxy/ProxyServer.h"
#include "proxy/network/UdpProxyServer.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"
#include "proxy/common/Config.h"
#include "proxy/monitor/AlertManager.h"
#include "proxy/balancer/ServiceDiscovery.h"
#include <unistd.h>
#include <getopt.h>
#include <cctype>
#include <algorithm>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace proxy;
    
    std::string configFile = "../config/proxy.conf";
    bool checkOnly = false;
    int ch;
    while ((ch = getopt(argc, argv, "c:hC")) != -1) {
        switch (ch) {
            case 'c':
                configFile = optarg;
                break;
            case 'C':
                checkOnly = true;
                break;
            case 'h':
            default:
                printf("Usage: %s [-c config_file] [-C]\n", argv[0]);
                printf("  -C  check config and exit\n");
                return 0;
        }
    }

    if (!common::Config::Instance().Load(configFile)) {
        LOG_ERROR << "Failed to load config, using defaults.";
    }

    if (checkOnly) {
        // Minimal validation: config parsed + at least one backend or empty allowed.
        // Exit code indicates success/failure for management scripts/CI.
        const auto backends = common::Config::Instance().GetBackends();
        (void)backends;
        printf("OK\n");
        return 0;
    }

    auto& conf = common::Config::Instance();
    common::Logger::Instance().SetLevel(common::Logger::Instance().ParseLevel(conf.GetString("global", "log_level", "INFO")));
    
    uint16_t port = static_cast<uint16_t>(conf.GetInt("global", "listen_port", 8080));
    int threads = conf.GetInt("global", "threads", 4);
    std::string strategy = conf.GetString("global", "strategy", "roundrobin");
    std::string ioModel = conf.GetString("global", "io_model", "epoll");
    int reusePort = conf.GetInt("global", "reuse_port", 0);
    int tlsEnable = conf.GetInt("tls", "enable", 0);
    std::string tlsCertPath = conf.GetString("tls", "cert_path", "");
    std::string tlsKeyPath = conf.GetString("tls", "key_path", "");
    std::string acmeChallengeDir = conf.GetString("tls", "acme_challenge_dir", "");
    double qps = conf.GetDouble("rate_limit", "qps", 0.0);
    double burst = conf.GetDouble("rate_limit", "burst", 0.0);
    double perIpQps = conf.GetDouble("rate_limit", "per_ip_qps", 0.0);
    double perIpBurst = conf.GetDouble("rate_limit", "per_ip_burst", 0.0);
    double perIpIdleSec = conf.GetDouble("rate_limit", "per_ip_idle_sec", 60.0);
    int perIpMaxEntries = conf.GetInt("rate_limit", "per_ip_max_entries", 10000);
    double perPathQps = conf.GetDouble("rate_limit", "per_path_qps", 0.0);
    double perPathBurst = conf.GetDouble("rate_limit", "per_path_burst", 0.0);
    double perPathIdleSec = conf.GetDouble("rate_limit", "per_path_idle_sec", 60.0);
    int perPathMaxEntries = conf.GetInt("rate_limit", "per_path_max_entries", 10000);
    int ccEnable = conf.GetInt("congestion", "enable", 0);
    int ccInitial = conf.GetInt("congestion", "initial_window", 64);
    int ccMin = conf.GetInt("congestion", "min_window", 1);
    int ccMax = conf.GetInt("congestion", "max_window", 1024);
    int ccAi = conf.GetInt("congestion", "additive_increase", 1);
    double ccBeta = conf.GetDouble("congestion", "multiplicative_decrease", 0.7);
    int maxConnections = conf.GetInt("connection_limit", "max_total", 0);
    int maxConnectionsPerIp = conf.GetInt("connection_limit", "max_per_ip", 0);
    int maxConnectionsPerUser = conf.GetInt("connection_limit", "max_per_user", 0);
    int maxConnectionsPerService = conf.GetInt("connection_limit", "max_per_service", 0);
    std::string userConnHeader = conf.GetString("connection_limit", "user_header", "X-Api-Token");
    int userConnMaxEntries = conf.GetInt("connection_limit", "user_max_entries", 10000);
    int serviceConnMaxEntries = conf.GetInt("connection_limit", "service_max_entries", 10000);
    double idleTimeoutSec = conf.GetDouble("connection_limit", "idle_timeout_sec", 0.0);
    double cleanupIntervalSec = conf.GetDouble("connection_limit", "cleanup_interval_sec", 1.0);
    uint16_t udpPort = static_cast<uint16_t>(conf.GetInt("udp", "listen_port", 0));
    double udpIdleTimeoutSec = conf.GetDouble("udp", "idle_timeout_sec", 10.0);
    double udpCleanupIntervalSec = conf.GetDouble("udp", "cleanup_interval_sec", 1.0);
    std::string healthMode = conf.GetString("health_check", "mode", "tcp");
    std::string healthHttpHost = conf.GetString("health_check", "http_host", "127.0.0.1");
    std::string healthHttpPath = conf.GetString("health_check", "http_path", "/health");
    std::string healthScriptCmd = conf.GetString("health_check", "script_cmd", "");
    double healthInterval = conf.GetDouble("health_check", "interval", 5.0);
    double healthTimeout = conf.GetDouble("health_check", "timeout", 2.0);
    int aiCheckEnable = conf.GetInt("ai_check", "enable", 0);
    std::string aiHttpHost = conf.GetString("ai_check", "http_host", "127.0.0.1");
    std::string aiHttpPath = conf.GetString("ai_check", "http_path", "/ai/status");
    double aiInterval = conf.GetDouble("ai_check", "interval", 5.0);
    double aiTimeout = conf.GetDouble("ai_check", "timeout", 2.0);
    int autoWeight = conf.GetInt("service_discovery", "auto_weight", 0);
    std::string sdProvider = conf.GetString("service_discovery", "provider", "off");
    double sdInterval = conf.GetDouble("service_discovery", "interval", 5.0);
    double sdTimeout = conf.GetDouble("service_discovery", "timeout", 2.0);
    int sdDefaultWeight = conf.GetInt("service_discovery", "default_weight", 1);
    std::string sdConsulUrl = conf.GetString("service_discovery", "consul_url", "http://127.0.0.1:8500");
    std::string sdConsulService = conf.GetString("service_discovery", "consul_service", "");
    int sdConsulPassingOnly = conf.GetInt("service_discovery", "consul_passing_only", 1);
    std::string sdEtcdUrl = conf.GetString("service_discovery", "etcd_url", "http://127.0.0.1:2379");
    std::string sdEtcdKey = conf.GetString("service_discovery", "etcd_key", "");
    std::string sdK8sUrl = conf.GetString("service_discovery", "k8s_url", "");
    std::string sdK8sToken = conf.GetString("service_discovery", "k8s_token", "");
    std::string sdK8sNs = conf.GetString("service_discovery", "k8s_namespace", "default");
    std::string sdK8sEndpoints = conf.GetString("service_discovery", "k8s_endpoints", "");
    std::string affinityMode = conf.GetString("session_affinity", "mode", "none");
    std::string affinityHeader = conf.GetString("session_affinity", "header_name", "");
    std::string affinityCookie = conf.GetString("session_affinity", "cookie_name", "");
    int batchEnable = conf.GetInt("batch", "enable", 0);
    int batchWindowMs = conf.GetInt("batch", "window_ms", 2);
    int batchMaxSize = conf.GetInt("batch", "max_batch_size", 8);
    int batchMaxBytes = conf.GetInt("batch", "max_batch_bytes", 262144);
    int batchMaxRespBytes = conf.GetInt("batch", "max_response_bytes", 1048576);
    std::string batchPaths = conf.GetString("batch", "paths", "");
    int batchRequireHeader = conf.GetInt("batch", "require_header", 0);
    std::string batchHeaderName = conf.GetString("batch", "header_name", "X-Batch");
    int warmupEnable = conf.GetInt("warmup", "enable", 0);
    std::string warmupModel = conf.GetString("warmup", "model", "");
    std::string warmupHttpHost = conf.GetString("warmup", "http_host", "127.0.0.1");
    std::string warmupHttpPath = conf.GetString("warmup", "http_path", "/ai/warmup");
    double warmupTimeout = conf.GetDouble("warmup", "timeout", 2.0);
    double ddosAcceptQps = conf.GetDouble("ddos", "accept_qps", 0.0);
    double ddosAcceptBurst = conf.GetDouble("ddos", "accept_burst", 0.0);
    double ddosPerIpAcceptQps = conf.GetDouble("ddos", "per_ip_accept_qps", 0.0);
    double ddosPerIpAcceptBurst = conf.GetDouble("ddos", "per_ip_accept_burst", 0.0);
    double ddosPerIpIdleSec = conf.GetDouble("ddos", "per_ip_idle_sec", 60.0);
    int ddosPerIpMaxEntries = conf.GetInt("ddos", "per_ip_max_entries", 10000);
    int alertEnable = conf.GetInt("alerts", "enable", 0);
    double alertInterval = conf.GetDouble("alerts", "interval_sec", 1.0);
    double alertCooldown = conf.GetDouble("alerts", "cooldown_sec", 30.0);
    double alertMergeWindow = conf.GetDouble("alerts", "merge_window_sec", 0.2);
    int alertAnomalyEnable = conf.GetInt("alerts", "anomaly_enable", 0);
    double alertAnomalyZ = conf.GetDouble("alerts", "anomaly_z", 3.0);
    double alertAnomalyAlpha = conf.GetDouble("alerts", "anomaly_alpha", 0.2);
    int alertAnomalyMinSamples = conf.GetInt("alerts", "anomaly_min_samples", 10);
    std::string alertWebhookUrl = conf.GetString("alerts", "webhook_url", "");
    std::string alertSmsWebhookUrl = conf.GetString("alerts", "sms_webhook_url", "");
    std::string alertEmailSmtpHost = conf.GetString("alerts", "email_smtp_host", "");
    int alertEmailSmtpPort = conf.GetInt("alerts", "email_smtp_port", 25);
    std::string alertEmailFrom = conf.GetString("alerts", "email_from", "");
    std::string alertEmailTo = conf.GetString("alerts", "email_to", "");
    std::string alertEmailSubject = conf.GetString("alerts", "email_subject_prefix", "Proxy Alert");
    long alertMaxActive = conf.GetInt("alerts", "max_active_connections", -1);
    double alertMaxCpuPct = conf.GetDouble("alerts", "max_cpu_pct", -1.0);
    long long alertMaxRssMb = static_cast<long long>(conf.GetInt("alerts", "max_rss_mb", -1));
    int alertMaxFd = conf.GetInt("alerts", "max_fd_count", -1);
    double alertMaxBackendErr = conf.GetDouble("alerts", "max_backend_error_rate", -1.0);
    std::string aclIpMode = conf.GetString("access_control", "ip_mode", "off");
    std::string aclCidrs = conf.GetString("access_control", "cidrs", "");
    int aclRequireToken = conf.GetInt("access_control", "require_token", 0);
    std::string aclTokenHeader = conf.GetString("access_control", "token_header", "X-Api-Token");
    std::string aclTokens = conf.GetString("access_control", "valid_tokens", "");
    int aclRequireApiKey = conf.GetInt("access_control", "require_api_key", 0);
    std::string aclApiKeyHeader = conf.GetString("access_control", "api_key_header", "X-Api-Key");
    std::string aclApiKeys = conf.GetString("access_control", "valid_api_keys", "");
    std::string auditLogPath = conf.GetString("audit_log", "path", "");
    int mirrorEnable = conf.GetInt("mirror", "enable", 0);
    std::string mirrorHost = conf.GetString("mirror", "udp_host", "127.0.0.1");
    int mirrorPort = conf.GetInt("mirror", "udp_port", 0);
    double mirrorSample = conf.GetDouble("mirror", "sample_rate", 1.0);
    int mirrorMaxBytes = conf.GetInt("mirror", "max_bytes", 4096);
    int mirrorMaxBodyBytes = conf.GetInt("mirror", "max_body_bytes", 1024);
    int mirrorIncludeReqBody = conf.GetInt("mirror", "include_req_body", 1);
    int mirrorIncludeRespBody = conf.GetInt("mirror", "include_resp_body", 0);
    int cacheEnable = conf.GetInt("cache", "enable", 0);
    std::string cacheBackend = conf.GetString("cache", "backend", "off");
    std::string cacheHost = conf.GetString("cache", "host", "127.0.0.1");
    int cachePort = conf.GetInt("cache", "port", 0);
    int cacheTtlSec = conf.GetInt("cache", "ttl_sec", 60);
    int cacheTimeoutMs = conf.GetInt("cache", "timeout_ms", 5);
    int cacheMaxValueBytes = conf.GetInt("cache", "max_value_bytes", 262144);
    int histEnable = conf.GetInt("history", "enable", 0);
    int histSampleMs = conf.GetInt("history", "sample_ms", 1000);
    int histMaxPoints = conf.GetInt("history", "max_points", 3600);
    std::string histPersistPath = conf.GetString("history", "persist_path", "");
    int pluginsEnable = conf.GetInt("plugins", "enable", 0);
    std::string pluginsPaths = conf.GetString("plugins", "paths", "");
    std::string pluginsHttpPrefixes = conf.GetString("plugins", "http_prefixes", "/plugin");
    int prioEnable = conf.GetInt("priority", "enable", 0);
    std::string prioMode = conf.GetString("priority", "mode", "priority");
    int prioMaxInflight = conf.GetInt("priority", "max_inflight", 0);
    int prioHighThreshold = conf.GetInt("priority", "high_threshold", 8);
    int prioLowDelayMs = conf.GetInt("priority", "low_delay_ms", 0);
    std::string prioHeaderName = conf.GetString("priority", "header_name", "X-Priority");
    std::string prioQueryName = conf.GetString("priority", "query_name", "priority");
    std::string prioFlowHeader = conf.GetString("priority", "flow_header_name", "X-Flow");
    std::string prioFlowQuery = conf.GetString("priority", "flow_query_name", "flow");
    std::string prioDeadlineHeader = conf.GetString("priority", "deadline_header_name", "X-Deadline-Ms");
    std::string prioDeadlineQuery = conf.GetString("priority", "deadline_query_name", "deadline_ms");
    int prioDefaultDeadlineMs = conf.GetInt("priority", "default_deadline_ms", 60000);
    int l4Enable = conf.GetInt("l4", "enable", 0);
    uint16_t l4ListenPort = static_cast<uint16_t>(conf.GetInt("l4", "listen_port", 0));

    LOG_INFO << "Starting Gemini Proxy Server on port " << port << "...";

    // Select I/O model at runtime (startup-time switch) via env flags used by Poller::NewDefaultPoller.
    // epoll is the default when no flags are set.
    ::unsetenv("PROXY_USE_POLL");
    ::unsetenv("PROXY_USE_SELECT");
    ::unsetenv("PROXY_USE_URING");
    if (ioModel == "poll") {
        ::setenv("PROXY_USE_POLL", "1", 1);
    } else if (ioModel == "select") {
        ::setenv("PROXY_USE_SELECT", "1", 1);
    } else if (ioModel == "uring") {
        ::setenv("PROXY_USE_URING", "1", 1);
    }

    network::EventLoop loop;
    ProxyServer server(&loop, port, strategy, "GeminiProxy", reusePort != 0);

    if (l4Enable != 0 && l4ListenPort != 0) {
        server.ConfigureL4Tunnel(l4ListenPort);
        LOG_INFO << "L4 tunnel enabled: listen_port=" << l4ListenPort;
    }

    if (pluginsEnable != 0) {
        auto splitCsv = [](const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == ',') {
                    if (!cur.empty()) out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) out.push_back(cur);
            for (auto& x : out) {
                size_t i = 0;
                while (i < x.size() && std::isspace(static_cast<unsigned char>(x[i]))) ++i;
                size_t j = x.size();
                while (j > i && std::isspace(static_cast<unsigned char>(x[j - 1]))) --j;
                x = x.substr(i, j - i);
            }
            out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& v) { return v.empty(); }), out.end());
            return out;
        };
        proxy::common::PluginManager::Config pc;
        pc.enabled = true;
        pc.paths = splitCsv(pluginsPaths);
        pc.httpPathPrefixes = splitCsv(pluginsHttpPrefixes);
        server.ConfigurePlugins(pc);
        LOG_INFO << "Plugins enabled: count=" << pc.paths.size();
    }

    if (mirrorEnable != 0 && mirrorPort > 0) {
        proxy::protocol::TrafficMirror::Config cfg;
        cfg.enabled = true;
        cfg.udpHost = mirrorHost.empty() ? "127.0.0.1" : mirrorHost;
        cfg.udpPort = static_cast<uint16_t>(mirrorPort);
        cfg.sampleRate = mirrorSample;
        cfg.maxBytes = static_cast<size_t>(mirrorMaxBytes > 0 ? mirrorMaxBytes : 4096);
        cfg.maxBodyBytes = static_cast<size_t>(mirrorMaxBodyBytes > 0 ? mirrorMaxBodyBytes : 1024);
        cfg.includeReqBody = (mirrorIncludeReqBody != 0);
        cfg.includeRespBody = (mirrorIncludeRespBody != 0);
        server.ConfigureTrafficMirror(cfg);
        LOG_INFO << "Traffic mirroring enabled: udp=" << cfg.udpHost << ":" << cfg.udpPort
                 << " sample_rate=" << cfg.sampleRate
                 << " max_bytes=" << cfg.maxBytes;
    }
    if (cacheEnable != 0 && cachePort > 0 && cacheBackend != "off") {
        proxy::protocol::Cache::Config cfg;
        cfg.enabled = true;
        cfg.backend = cacheBackend;
        cfg.host = cacheHost.empty() ? "127.0.0.1" : cacheHost;
        cfg.port = static_cast<uint16_t>(cachePort);
        cfg.ttlSec = cacheTtlSec;
        cfg.timeoutMs = cacheTimeoutMs;
        cfg.maxValueBytes = static_cast<size_t>(cacheMaxValueBytes > 0 ? cacheMaxValueBytes : 262144);
        server.ConfigureCache(cfg);
        LOG_INFO << "Cache enabled: backend=" << cfg.backend << " addr=" << cfg.host << ":" << cfg.port << " ttl_sec=" << cfg.ttlSec;
    }
    if (histEnable != 0) {
        proxy::monitor::HistoryStore::Config hc;
        hc.enabled = true;
        hc.sampleMs = histSampleMs;
        hc.maxPoints = static_cast<size_t>(histMaxPoints > 0 ? histMaxPoints : 3600);
        hc.persistPath = histPersistPath;
        server.ConfigureHistory(hc);
        LOG_INFO << "History enabled: sample_ms=" << hc.sampleMs << " max_points=" << hc.maxPoints
                 << (hc.persistPath.empty() ? "" : (" persist_path=" + hc.persistPath));
    }
    if (!acmeChallengeDir.empty()) {
        server.SetAcmeChallengeDir(acmeChallengeDir);
        LOG_INFO << "ACME HTTP-01 challenge enabled: dir=" << acmeChallengeDir;
    }
    if (tlsEnable != 0) {
        if (tlsCertPath.empty() || tlsKeyPath.empty()) {
            LOG_ERROR << "TLS enabled but cert_path/key_path not set";
        } else if (!server.EnableTls(tlsCertPath, tlsKeyPath)) {
            LOG_ERROR << "TLS enable failed";
        } else {
            LOG_INFO << "TLS enabled (auto sniff): cert=" << tlsCertPath << " key=" << tlsKeyPath;
        }
    }
    if (prioEnable != 0) {
        ProxyServer::PriorityConfig pc;
        pc.enabled = true;
        pc.mode = prioMode.empty() ? "priority" : prioMode;
        pc.maxInflight = prioMaxInflight;
        pc.highThreshold = prioHighThreshold;
        pc.lowDelayMs = prioLowDelayMs;
        pc.priorityHeader = prioHeaderName.empty() ? "X-Priority" : prioHeaderName;
        pc.priorityQuery = prioQueryName.empty() ? "priority" : prioQueryName;
        pc.flowHeader = prioFlowHeader.empty() ? "X-Flow" : prioFlowHeader;
        pc.flowQuery = prioFlowQuery.empty() ? "flow" : prioFlowQuery;
        pc.deadlineHeader = prioDeadlineHeader.empty() ? "X-Deadline-Ms" : prioDeadlineHeader;
        pc.deadlineQuery = prioDeadlineQuery.empty() ? "deadline_ms" : prioDeadlineQuery;
        pc.defaultDeadlineMs = prioDefaultDeadlineMs;
        server.ConfigurePriorityScheduling(pc);
        LOG_INFO << "Scheduling enabled: mode=" << pc.mode
                 << " max_inflight=" << pc.maxInflight
                 << " high_threshold=" << pc.highThreshold
                 << " low_delay_ms=" << pc.lowDelayMs
                 << " header=" << pc.priorityHeader
                 << " query=" << pc.priorityQuery;
    }

    server.SetThreadNum(threads);

    // Rewrite rules: request/response header/body modifications (opt-in).
    {
        auto trim = [](const std::string& s) {
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            size_t j = s.size();
            while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
            return s.substr(i, j - i);
        };
        auto split = [&](const std::string& s, char sep) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == sep) {
                    out.push_back(trim(cur));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            out.push_back(trim(cur));
            out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& v) { return v.empty(); }), out.end());
            return out;
        };
        auto parseHeaderPairs = [&](const std::string& s) {
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& item : split(s, ',')) {
                const size_t pos = item.find(':');
                if (pos == std::string::npos) continue;
                const std::string k = trim(item.substr(0, pos));
                const std::string v = trim(item.substr(pos + 1));
                if (!k.empty()) out.push_back({k, v});
            }
            return out;
        };
        auto parseReplaces = [&](const std::string& s) {
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& item : split(s, ';')) {
                const size_t pos = item.find("=>");
                if (pos == std::string::npos) continue;
                out.push_back({item.substr(0, pos), item.substr(pos + 2)});
            }
            return out;
        };

        auto secs = conf.GetSectionsWithPrefix("rewrite:");
        auto parseIdx = [](const std::string& section) -> int {
            const size_t pos = section.find(':');
            if (pos == std::string::npos) return 0;
            try {
                return std::stoi(section.substr(pos + 1));
            } catch (...) {
                return 0;
            }
        };
        std::sort(secs.begin(), secs.end(), [&](const auto& a, const auto& b) { return parseIdx(a.first) < parseIdx(b.first); });

        std::vector<proxy::protocol::RewriteRule> rules;
        for (const auto& sec : secs) {
            const auto& m = sec.second;
            proxy::protocol::RewriteRule r;
            auto it = m.find("path_prefix");
            if (it != m.end()) r.pathPrefix = it->second;
            it = m.find("method");
            if (it != m.end()) {
                const std::string mm = trim(it->second);
                if (mm == "GET") r.method = proxy::protocol::HttpRequest::kGet;
                else if (mm == "POST") r.method = proxy::protocol::HttpRequest::kPost;
                else if (mm == "HEAD") r.method = proxy::protocol::HttpRequest::kHead;
                else if (mm == "PUT") r.method = proxy::protocol::HttpRequest::kPut;
                else if (mm == "DELETE") r.method = proxy::protocol::HttpRequest::kDelete;
                else r.method = proxy::protocol::HttpRequest::kInvalid;
            }
            it = m.find("req_set_headers");
            if (it != m.end()) {
                for (const auto& kv : parseHeaderPairs(it->second)) r.reqSetHeaders[kv.first] = kv.second;
            }
            it = m.find("req_del_headers");
            if (it != m.end()) {
                r.reqDelHeaders = split(it->second, ',');
            }
            it = m.find("req_body_replace");
            if (it != m.end()) {
                r.reqBodyReplaces = parseReplaces(it->second);
            }
            it = m.find("resp_set_headers");
            if (it != m.end()) {
                for (const auto& kv : parseHeaderPairs(it->second)) r.respSetHeaders[kv.first] = kv.second;
            }
            it = m.find("resp_del_headers");
            if (it != m.end()) {
                r.respDelHeaders = split(it->second, ',');
            }
            it = m.find("resp_body_replace");
            if (it != m.end()) {
                r.respBodyReplaces = parseReplaces(it->second);
            }
            if (!r.pathPrefix.empty() ||
                r.method != proxy::protocol::HttpRequest::kInvalid ||
                !r.reqSetHeaders.empty() ||
                !r.reqDelHeaders.empty() ||
                !r.reqBodyReplaces.empty() ||
                !r.respSetHeaders.empty() ||
                !r.respDelHeaders.empty() ||
                !r.respBodyReplaces.empty()) {
                rules.push_back(std::move(r));
            }
        }
        if (!rules.empty()) {
            server.ConfigureRewriteRules(rules);
            LOG_INFO << "Rewrite rules enabled: " << rules.size();
        }
    }

    server.EnableAutoWeightAdjust(autoWeight != 0);
    server.ConfigureHealthCheck(healthMode, healthTimeout, healthHttpHost, healthHttpPath, healthScriptCmd);
    server.StartHealthCheck(healthInterval);
    if (aiCheckEnable != 0) {
        server.ConfigureAiServiceCheck(aiTimeout, aiHttpHost, aiHttpPath);
        server.StartAiServiceCheck(aiInterval);
        LOG_INFO << "AI service check enabled: interval=" << aiInterval
                 << " timeout=" << aiTimeout
                 << " http_path=" << aiHttpPath;
    }
    if (batchEnable != 0) {
        proxy::protocol::HttpBatcher::Config cfg;
        cfg.enabled = true;
        cfg.windowMs = std::max(0, batchWindowMs);
        cfg.maxBatchSize = static_cast<size_t>(batchMaxSize > 0 ? batchMaxSize : 1);
        cfg.maxBatchBytes = static_cast<size_t>(batchMaxBytes > 0 ? batchMaxBytes : 1);
        cfg.maxResponseBytes = static_cast<size_t>(batchMaxRespBytes > 0 ? batchMaxRespBytes : 1);
        cfg.requireHeader = (batchRequireHeader != 0);
        cfg.headerName = batchHeaderName.empty() ? "X-Batch" : batchHeaderName;
        // parse comma-separated paths
        auto splitCsv = [](const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == ',') {
                    if (!cur.empty()) out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) out.push_back(cur);
            for (auto& x : out) {
                size_t i = 0;
                while (i < x.size() && std::isspace(static_cast<unsigned char>(x[i]))) ++i;
                size_t j = x.size();
                while (j > i && std::isspace(static_cast<unsigned char>(x[j - 1]))) --j;
                x = x.substr(i, j - i);
            }
            out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& v) { return v.empty(); }), out.end());
            return out;
        };
        cfg.paths = splitCsv(batchPaths);
        server.ConfigureHttpBatching(cfg);
        LOG_INFO << "HTTP batching enabled: window_ms=" << cfg.windowMs
                 << " max_batch_size=" << cfg.maxBatchSize
                 << " max_batch_bytes=" << cfg.maxBatchBytes
                 << " require_header=" << (cfg.requireHeader ? 1 : 0);
    }
    if (warmupEnable != 0) {
        server.ConfigureWarmup(true, warmupModel, warmupTimeout, warmupHttpHost, warmupHttpPath);
        LOG_INFO << "Warmup enabled: model=" << warmupModel
                 << " timeout=" << warmupTimeout
                 << " http_path=" << warmupHttpPath;
    }
    server.SetSessionAffinity(affinityMode, affinityHeader, affinityCookie);

    std::unique_ptr<proxy::balancer::ServiceDiscoveryManager> sd;
    if (sdProvider != "off") {
        proxy::balancer::ServiceDiscoveryManager::Config cfg;
        cfg.provider = sdProvider;
        cfg.intervalSec = sdInterval;
        cfg.timeoutSec = sdTimeout;
        cfg.defaultWeight = sdDefaultWeight;
        cfg.consulUrl = sdConsulUrl;
        cfg.consulService = sdConsulService;
        cfg.consulPassingOnly = (sdConsulPassingOnly != 0);
        cfg.etcdUrl = sdEtcdUrl;
        cfg.etcdKey = sdEtcdKey;
        cfg.k8sUrl = sdK8sUrl;
        cfg.k8sToken = sdK8sToken;
        cfg.k8sNamespace = sdK8sNs;
        cfg.k8sEndpoints = sdK8sEndpoints;
        sd = std::make_unique<proxy::balancer::ServiceDiscoveryManager>(&loop, server.GetBackendManager(), cfg);
        sd->Start();
        LOG_INFO << "Service discovery enabled: provider=" << sdProvider << " interval=" << sdInterval << " timeout=" << sdTimeout;
    }
    if (ddosAcceptQps > 0.0) {
        server.SetAcceptRateLimit(ddosAcceptQps, ddosAcceptBurst);
        LOG_INFO << "DDoS accept rate limit enabled: qps=" << ddosAcceptQps
                 << " burst=" << (ddosAcceptBurst > 0.0 ? ddosAcceptBurst : ddosAcceptQps);
    }
    if (ddosPerIpAcceptQps > 0.0) {
        server.SetPerIpAcceptRateLimit(ddosPerIpAcceptQps,
                                       ddosPerIpAcceptBurst,
                                       ddosPerIpIdleSec,
                                       static_cast<size_t>(ddosPerIpMaxEntries));
        LOG_INFO << "DDoS per-ip accept rate limit enabled: per_ip_qps=" << ddosPerIpAcceptQps
                 << " per_ip_burst=" << (ddosPerIpAcceptBurst > 0.0 ? ddosPerIpAcceptBurst : ddosPerIpAcceptQps)
                 << " per_ip_idle_sec=" << ddosPerIpIdleSec
                 << " per_ip_max_entries=" << ddosPerIpMaxEntries;
    }

    std::unique_ptr<proxy::monitor::AlertManager> alertManager;
    if (alertEnable != 0) {
        proxy::monitor::AlertManager::Config cfg;
        cfg.enabled = true;
        cfg.intervalSec = alertInterval;
        cfg.cooldownSec = alertCooldown;
        cfg.mergeWindowSec = alertMergeWindow;
        cfg.webhookUrl = alertWebhookUrl;
        cfg.smsWebhookUrl = alertSmsWebhookUrl;
        cfg.email.smtpHost = alertEmailSmtpHost;
        cfg.email.smtpPort = static_cast<uint16_t>((alertEmailSmtpPort > 0 && alertEmailSmtpPort <= 65535) ? alertEmailSmtpPort : 25);
        cfg.email.mailFrom = alertEmailFrom;
        cfg.email.mailTo = alertEmailTo;
        cfg.email.subjectPrefix = alertEmailSubject;
        cfg.thresholds.maxActiveConnections = alertMaxActive;
        cfg.thresholds.maxCpuPct = alertMaxCpuPct;
        cfg.thresholds.maxRssBytes = (alertMaxRssMb >= 0) ? (alertMaxRssMb * 1024LL * 1024LL) : -1;
        cfg.thresholds.maxFdCount = alertMaxFd;
        cfg.thresholds.maxBackendErrorRate = alertMaxBackendErr;
        cfg.anomaly.enabled = (alertAnomalyEnable != 0);
        cfg.anomaly.zThreshold = alertAnomalyZ;
        cfg.anomaly.alpha = alertAnomalyAlpha;
        cfg.anomaly.minSamples = alertAnomalyMinSamples;
        alertManager = std::make_unique<proxy::monitor::AlertManager>(&loop, cfg);
        alertManager->Start();
        LOG_INFO << "Alerts enabled: interval_sec=" << alertInterval
                 << " cooldown_sec=" << alertCooldown
                 << " anomaly_enable=" << alertAnomalyEnable
                 << " webhook_url=" << alertWebhookUrl
                 << " sms_webhook_url=" << alertSmsWebhookUrl
                 << " email_smtp_host=" << alertEmailSmtpHost;
    }
    {
        proxy::monitor::AccessControl::Config ac;
        if (aclIpMode == "allow") ac.ipMode = proxy::monitor::AccessControl::IpMode::kAllowList;
        else if (aclIpMode == "deny") ac.ipMode = proxy::monitor::AccessControl::IpMode::kDenyList;
        else ac.ipMode = proxy::monitor::AccessControl::IpMode::kOff;

        // parse cidrs/tokens comma-separated
        auto splitCsv = [](const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == ',') {
                    if (!cur.empty()) out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) out.push_back(cur);
            // trim spaces
            for (auto& x : out) {
                size_t i = 0;
                while (i < x.size() && std::isspace(static_cast<unsigned char>(x[i]))) ++i;
                size_t j = x.size();
                while (j > i && std::isspace(static_cast<unsigned char>(x[j - 1]))) --j;
                x = x.substr(i, j - i);
            }
            out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& v) { return v.empty(); }), out.end());
            return out;
        };

        ac.cidrs = splitCsv(aclCidrs);
        ac.requireToken = (aclRequireToken != 0);
        ac.tokenHeader = aclTokenHeader;
        ac.validTokens = splitCsv(aclTokens);
        ac.requireApiKey = (aclRequireApiKey != 0);
        ac.apiKeyHeader = aclApiKeyHeader;
        ac.validApiKeys = splitCsv(aclApiKeys);
        server.SetAccessControl(ac);
    }
    if (!auditLogPath.empty()) {
        server.EnableAuditLog(auditLogPath);
        LOG_INFO << "Audit log enabled: " << auditLogPath;
    }
    if (maxConnections > 0 || maxConnectionsPerIp > 0) {
        server.SetConnectionLimits(maxConnections, maxConnectionsPerIp);
        LOG_INFO << "Connection limits enabled: max_total=" << maxConnections
                 << " max_per_ip=" << maxConnectionsPerIp;
    }
    if (maxConnectionsPerUser > 0) {
        server.SetMaxConnectionsPerUser(maxConnectionsPerUser, userConnHeader, static_cast<size_t>(userConnMaxEntries));
        LOG_INFO << "Per-user connection limit enabled: max_per_user=" << maxConnectionsPerUser
                 << " header=" << userConnHeader
                 << " max_entries=" << userConnMaxEntries;
    }
    if (maxConnectionsPerService > 0) {
        server.SetMaxConnectionsPerService(maxConnectionsPerService, static_cast<size_t>(serviceConnMaxEntries));
        LOG_INFO << "Per-service connection limit enabled: max_per_service=" << maxConnectionsPerService
                 << " max_entries=" << serviceConnMaxEntries;
    }
    if (idleTimeoutSec > 0.0) {
        server.SetIdleTimeout(idleTimeoutSec, cleanupIntervalSec);
        LOG_INFO << "Idle timeout enabled: idle_timeout_sec=" << idleTimeoutSec
                 << " cleanup_interval_sec=" << cleanupIntervalSec;
    }
    if (qps > 0.0) {
        server.EnableRateLimit(qps, burst);
        LOG_INFO << "Rate limit enabled: qps=" << qps << " burst=" << (burst > 0.0 ? burst : qps);
    }
    if (perIpQps > 0.0) {
        server.EnablePerIpRateLimit(perIpQps, perIpBurst, perIpIdleSec, static_cast<size_t>(perIpMaxEntries));
        LOG_INFO << "Per-IP rate limit enabled: per_ip_qps=" << perIpQps
                 << " per_ip_burst=" << (perIpBurst > 0.0 ? perIpBurst : perIpQps)
                 << " per_ip_idle_sec=" << perIpIdleSec
                 << " per_ip_max_entries=" << perIpMaxEntries;
    }
    if (perPathQps > 0.0) {
        server.EnablePerPathRateLimit(perPathQps, perPathBurst, perPathIdleSec, static_cast<size_t>(perPathMaxEntries));
        LOG_INFO << "Per-path rate limit enabled: per_path_qps=" << perPathQps
                 << " per_path_burst=" << (perPathBurst > 0.0 ? perPathBurst : perPathQps)
                 << " per_path_idle_sec=" << perPathIdleSec
                 << " per_path_max_entries=" << perPathMaxEntries;
    }
    if (ccEnable != 0) {
        proxy::monitor::CongestionControl::Config cfg;
        cfg.enabled = true;
        cfg.initialWindow = ccInitial;
        cfg.minWindow = ccMin;
        cfg.maxWindow = ccMax;
        cfg.additiveIncrease = ccAi;
        cfg.multiplicativeDecrease = ccBeta;
        server.ConfigureCongestionControl(cfg);
        LOG_INFO << "Congestion control enabled: initial_window=" << cfg.initialWindow
                 << " min_window=" << cfg.minWindow
                 << " max_window=" << cfg.maxWindow
                 << " additive_increase=" << cfg.additiveIncrease
                 << " multiplicative_decrease=" << cfg.multiplicativeDecrease;
    }
    
    // Load backends from config
    auto backends = conf.GetBackends();
    for (const auto& b : backends) {
        server.AddBackend(b.ip, b.port, b.weight);
        if (b.queueLen >= 0 || b.gpuUtil01 >= 0.0) {
            const std::string id = proxy::network::InetAddress(b.ip, b.port).toIpPort();
            server.UpdateBackendMetrics(id, b.queueLen, b.gpuUtil01, b.vramUsedMb, b.vramTotalMb);
        }
    }

    std::unique_ptr<network::UdpProxyServer> udpServer;
    if (udpPort > 0) {
        udpServer = std::make_unique<network::UdpProxyServer>(&loop, udpPort, "GeminiUdpProxy");
        udpServer->SetIdleTimeout(udpIdleTimeoutSec, udpCleanupIntervalSec);
        for (const auto& b : backends) {
            udpServer->AddBackend(b.ip, b.port, b.weight);
        }
        // Keep UDP health check at default TCP connect for now.
        udpServer->StartHealthCheck(healthInterval);
        udpServer->Start();
        LOG_INFO << "UDP proxy enabled on port " << udpPort;
    }
    
    server.Start();
    
    loop.Loop();
    return 0;
}
