#include "proxy/balancer/LeastQueueBalancer.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <string>

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    proxy::balancer::LeastQueueBalancer b;
    b.AddNode("A", 1);
    b.AddNode("B", 1);

    // Without metrics, should still return some node.
    std::string n0 = b.GetNode("k0");
    assert(!n0.empty());

    b.RecordQueueLength("A", 10);
    b.RecordQueueLength("B", 2);
    for (int i = 0; i < 50; ++i) {
        std::string n = b.GetNode("key" + std::to_string(i));
        assert(n == "B");
    }

    b.RecordQueueLength("B", 20);
    for (int i = 0; i < 50; ++i) {
        std::string n = b.GetNode("key2" + std::to_string(i));
        assert(n == "A");
    }

    b.RemoveNode("A");
    std::string n3 = b.GetNode("k3");
    assert(n3 == "B");
    return 0;
}

