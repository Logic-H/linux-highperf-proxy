#include "proxy/balancer/BackendManager.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/Acceptor.h"
#include "proxy/network/Channel.h"
#include "proxy/common/Logger.h"
#include <unistd.h>
#include <iostream>
#include <sys/timerfd.h>
#include <cstring>

using namespace proxy;

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::INFO);
    LOG_INFO << "=== Testing BackendManager ===";

    network::EventLoop loop;
    
    // Setup healthy server on 9990
    network::InetAddress addr1(9990);
    network::Acceptor acc1(&loop, addr1, true);
    acc1.SetNewConnectionCallback([](int fd, const network::InetAddress&){ ::close(fd); });
    acc1.Listen();

    balancer::BackendManager manager(&loop, "roundrobin");
    manager.AddBackend("127.0.0.1", 9990, 1); // Healthy
    manager.AddBackend("127.0.0.1", 9991, 1); // Unhealthy (Closed)

    // Start checking (interval 0.5s to be fast)
    manager.StartHealthCheck(0.5);

    // Initial state: Both might be assumed healthy or checked.
    // We assume true initially in code, so RR will return both until check fails.
    
    // Loop for 2 seconds to let checks run
    // Use a timer to quit loop
    int timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    struct itimerspec ts;
    std::memset(&ts, 0, sizeof ts);
    ts.it_value.tv_sec = 2;
    ts.it_value.tv_nsec = 0;
    ::timerfd_settime(timerFd, 0, &ts, NULL);
    
    auto timerCh = std::make_shared<network::Channel>(&loop, timerFd);
    timerCh->SetReadCallback([&](std::chrono::system_clock::time_point){
        LOG_INFO << "Test Phase 1 Check:";
        // After 2s, 9990 should be UP, 9991 should be DOWN.
        // GetNode should only return 9990.
        std::string n1 = manager.SelectBackend("").toIpPort();
        std::string n2 = manager.SelectBackend("").toIpPort();
        std::string n3 = manager.SelectBackend("").toIpPort();
        
        LOG_INFO << "Nodes selected: " << n1 << ", " << n2 << ", " << n3;
        
        if (n1 == "127.0.0.1:9990" && n2 == "127.0.0.1:9990" && n3 == "127.0.0.1:9990") {
             LOG_INFO << "BackendManager Health Logic: PASS";
        } else {
             LOG_ERROR << "BackendManager Health Logic: FAIL (Expected only 9990)";
        }
        
        loop.Quit();
    });
    timerCh->EnableReading();

    loop.Loop();
    return 0;
}
