#include "proxy/balancer/LeastConnectionsBalancer.h"
#include "proxy/common/Logger.h"

#include <cassert>

using namespace proxy::balancer;

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    LeastConnectionsBalancer b;
    b.AddNode("A", 1);
    b.AddNode("B", 1);

    // Initially either is fine.
    auto n1 = b.GetNode("k");
    assert(n1 == "A" || n1 == "B");

    // Simulate A is busy
    b.OnConnectionStart("A");
    b.OnConnectionStart("A");

    // Should prefer B now
    for (int i = 0; i < 10; ++i) {
        assert(b.GetNode("k") == "B");
    }

    b.OnConnectionEnd("A");
    b.OnConnectionEnd("A");

    // Back to tie
    auto n2 = b.GetNode("k");
    assert(n2 == "A" || n2 == "B");
    return 0;
}

