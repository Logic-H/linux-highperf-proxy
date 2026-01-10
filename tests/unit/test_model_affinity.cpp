#include "proxy/balancer/BackendManager.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"

#include <iostream>
#include <string>

using namespace proxy;

static int Fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
}

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::ERROR);

    network::EventLoop loop;
    balancer::BackendManager manager(&loop, "roundrobin");
    manager.AddBackend("127.0.0.1", 9101, 1);
    manager.AddBackend("127.0.0.1", 9102, 1);

    // Pre-mark model "m1" loaded on backend 9101.
    if (!manager.SetBackendLoadedModel("127.0.0.1:9101", "m1", true)) {
        return Fail("SetBackendLoadedModel failed for 127.0.0.1:9101");
    }

    {
        const auto a1 = manager.SelectBackendForModel("k#ip#model:m1", "m1").toIpPort();
        const auto a2 = manager.SelectBackendForModel("k#ip#model:m1", "m1").toIpPort();
        if (a1 != "127.0.0.1:9101" || a2 != "127.0.0.1:9101") {
            return Fail("model affinity (preloaded) should always select 127.0.0.1:9101");
        }
    }

    // Model "m2": first request selects by strategy, subsequent requests should stick.
    std::string first;
    {
        first = manager.SelectBackendForModel("k#ip#model:m2", "m2").toIpPort();
        if (first != "127.0.0.1:9101" && first != "127.0.0.1:9102") {
            return Fail("unexpected backend for model m2: " + first);
        }
        const auto second = manager.SelectBackendForModel("k#ip#model:m2", "m2").toIpPort();
        if (second != first) {
            return Fail("model affinity (sticky) expected same backend, first=" + first + " second=" + second);
        }
    }

    // If the sticky backend is offline, selection must fall back to an eligible backend.
    if (!manager.SetBackendOnline(first, false)) {
        return Fail("SetBackendOnline failed for " + first);
    }
    {
        const auto now = manager.SelectBackendForModel("k#ip#model:m2", "m2").toIpPort();
        if (now == first || now == "0.0.0.0:0") {
            return Fail("expected fallback backend after offline; got " + now);
        }
    }

    std::cout << "PASS\n";
    return 0;
}

