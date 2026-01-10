#include "proxy/balancer/BackendManager.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

using namespace proxy;

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::INFO);

    network::EventLoop loop;
    balancer::BackendManager manager(&loop, "roundrobin");
    manager.AddBackend("127.0.0.1", 9101, 1);
    manager.AddBackend("127.0.0.1", 9102, 1);

    // Mark 9101 as failed by passive signal and ensure selection avoids it.
    manager.ReportBackendFailure(network::InetAddress("127.0.0.1", 9101));

    for (int i = 0; i < 10; ++i) {
        const std::string sel = manager.SelectBackend("k").toIpPort();
        if (sel == "127.0.0.1:9101") {
            LOG_ERROR << "Passive failover: FAIL (selected failed backend)";
            return 1;
        }
    }

    LOG_INFO << "Passive failover: PASS";
    return 0;
}

