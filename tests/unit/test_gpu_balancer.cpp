#include "proxy/balancer/GpuAwareBalancer.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <string>

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    proxy::balancer::GpuAwareBalancer b;
    b.AddNode("A", 1);
    b.AddNode("B", 1);

    // No metrics: fall back to RR, must return some node.
    std::string n0 = b.GetNode("k0");
    assert(!n0.empty());

    b.RecordGpuUtil("A", 0.9, 1000, 10000);
    b.RecordGpuUtil("B", 0.1, 5000, 10000);
    for (int i = 0; i < 50; ++i) {
        std::string n = b.GetNode("key" + std::to_string(i));
        assert(n == "B");
    }

    b.RecordGpuUtil("B", 0.95, 9000, 10000);
    b.RecordGpuUtil("A", 0.2, 1000, 10000);
    for (int i = 0; i < 50; ++i) {
        std::string n = b.GetNode("key2" + std::to_string(i));
        assert(n == "A");
    }
    return 0;
}

