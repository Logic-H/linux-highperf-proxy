#include "proxy/monitor/Stats.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <string>

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    auto& s = proxy::monitor::Stats::Instance();
    s.AddBytesIn(123);
    s.AddBytesOut(456);
    s.AddUdpRxDrops(7);
    s.RecordRequestMethod("GET");
    s.RecordRequestPath("/infer");
    s.RecordModelName("llama2");

    std::string json = s.ToJson();
    assert(json.find("\"bytes_in\"") != std::string::npos);
    assert(json.find("\"bytes_out\"") != std::string::npos);
    assert(json.find("\"udp_rx_drops\"") != std::string::npos);
    assert(json.find("\"ddos_drops\"") != std::string::npos);
    assert(json.find("\"process\"") != std::string::npos);
    assert(json.find("\"rss_bytes\"") != std::string::npos);
    assert(json.find("\"fd_count\"") != std::string::npos);
    assert(json.find("\"tcp\"") != std::string::npos);
    assert(json.find("\"retrans_rate\"") != std::string::npos);
    assert(json.find("\"top_methods\"") != std::string::npos);
    assert(json.find("\"top_paths\"") != std::string::npos);
    assert(json.find("\"top_models\"") != std::string::npos);
    return 0;
}
