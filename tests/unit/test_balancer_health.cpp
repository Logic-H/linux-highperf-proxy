#include "proxy/balancer/RoundRobinBalancer.h"
#include "proxy/balancer/TcpHealthChecker.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/Acceptor.h"
#include "proxy/common/Logger.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>

using namespace proxy;

void TestRoundRobin() {
    LOG_INFO << "=== Testing RoundRobinBalancer ===";
    balancer::RoundRobinBalancer rr;
    rr.AddNode("A");
    rr.AddNode("B");
    rr.AddNode("C");

    std::vector<std::string> results;
    for(int i=0; i<6; ++i) {
        results.push_back(rr.GetNode(""));
    }

    // Basic RR check
    if (results[0] == "A" && results[1] == "B" && results[2] == "C" &&
        results[3] == "A" && results[4] == "B" && results[5] == "C") {
        LOG_INFO << "RoundRobin basic: PASS";
    } else {
        LOG_ERROR << "RoundRobin basic: FAIL";
        for(const auto& s : results) std::cout << s << " ";
        std::cout << std::endl;
    }

    // Weighted test (Simulated by adding multiple times)
    balancer::RoundRobinBalancer wrr;
    wrr.AddNode("A", 2); // Weight 2
    wrr.AddNode("B", 1); // Weight 1

    results.clear();
    for(int i=0; i<6; ++i) {
        results.push_back(wrr.GetNode(""));
    }
    // A A B A A B (Sequence depends on implementation, likely A A B or A B A)
    // Current impl pushes back: A, A, B. So A, A, B, A, A, B.
    if (results[0] == "A" && results[1] == "A" && results[2] == "B") {
        LOG_INFO << "Weighted RoundRobin: PASS";
    } else {
        LOG_WARN << "Weighted RoundRobin: Sequence might differ but acceptable if A:B ratio is 2:1";
        int countA = 0;
        for(const auto& s : results) if(s == "A") countA++;
        if (countA == 4) LOG_INFO << "Weighted RoundRobin ratio: PASS";
        else LOG_ERROR << "Weighted RoundRobin ratio: FAIL (A count=" << countA << ")";
    }
}

void TestHealthChecker() {
    LOG_INFO << "=== Testing TcpHealthChecker ===";
    network::EventLoop loop;
    
    // 1. Setup a dummy server on port 9999
    network::InetAddress listenAddr(9999);
    network::Acceptor acceptor(&loop, listenAddr, true);
    acceptor.SetNewConnectionCallback([](int sockfd, const network::InetAddress&){
        ::close(sockfd); // Accept and close immediately
    });
    acceptor.Listen();

    auto checker = std::make_shared<balancer::TcpHealthChecker>(&loop, 1.0); // 1s timeout
    
    std::atomic<int> callbacks(0);

    // Test 1: Connect to localhost:9999 (Should Success)
    checker->Check(network::InetAddress("127.0.0.1", 9999), 
        [&](bool healthy, const network::InetAddress& addr) {
            if (healthy) LOG_INFO << "Check " << addr.toIpPort() << ": Healthy (EXPECTED)";
            else LOG_ERROR << "Check " << addr.toIpPort() << ": Unhealthy (UNEXPECTED)";
            callbacks++;
            if (callbacks == 2) loop.Quit();
    });

    // Test 2: Connect to localhost:9998 (Should Fail/Timeout)
    // Ensure port 9998 is closed
    checker->Check(network::InetAddress("127.0.0.1", 9998), 
        [&](bool healthy, const network::InetAddress& addr) {
            if (!healthy) LOG_INFO << "Check " << addr.toIpPort() << ": Unhealthy (EXPECTED)";
            else LOG_ERROR << "Check " << addr.toIpPort() << ": Healthy (UNEXPECTED)";
            callbacks++;
            if (callbacks == 2) loop.Quit();
    });

    loop.Loop();
}

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::INFO);
    TestRoundRobin();
    TestHealthChecker();
    return 0;
}
