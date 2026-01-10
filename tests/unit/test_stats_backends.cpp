#include "proxy/monitor/Stats.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    std::vector<proxy::monitor::Stats::BackendSnapshot> bs;
    {
        proxy::monitor::Stats::BackendSnapshot b;
        b.id = "127.0.0.1:1";
        b.healthy = true;
        b.online = true;
        b.weight = 2;
        b.baseWeight = 2;
        b.activeConnections = 3;
        b.ewmaResponseMs = 4.5;
        b.successes = 7;
        b.failures = 6;
        b.errorRate = 6.0 / 13.0;
        bs.push_back(b);
    }

    auto& s = proxy::monitor::Stats::Instance();
    s.SetBackendSnapshot(std::move(bs));
    s.RecordRequestLatencyMs(1.0);
    s.RecordRequestLatencyMs(2.0);

    std::string json = s.ToJson();
    assert(json.find("\"backends\"") != std::string::npos);
    assert(json.find("127.0.0.1:1") != std::string::npos);
    assert(json.find("\"ewma_response_ms\"") != std::string::npos);
    assert(json.find("\"base_weight\"") != std::string::npos);
    assert(json.find("\"successes\"") != std::string::npos);
    assert(json.find("\"error_rate\"") != std::string::npos);
    assert(json.find("\"backend_error_rate\"") != std::string::npos);
    assert(json.find("\"latency_ms\"") != std::string::npos);
    return 0;
}
