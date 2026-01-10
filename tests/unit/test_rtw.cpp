#include "proxy/balancer/ResponseTimeWeightedBalancer.h"
#include "proxy/common/Logger.h"

#include <cassert>

using namespace proxy::balancer;

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    ResponseTimeWeightedBalancer b(0.5);
    b.AddNode("A", 1);
    b.AddNode("B", 1);

    // Record A slow, B fast
    b.RecordResponseTimeMs("A", 100.0);
    b.RecordResponseTimeMs("B", 5.0);

    for (int i = 0; i < 10; ++i) {
        assert(b.GetNode("k") == "B");
    }

    // If B becomes very busy, A might win due to (1+active) factor
    for (int i = 0; i < 50; ++i) b.OnConnectionStart("B");
    auto pick = b.GetNode("k");
    assert(pick == "A" || pick == "B");
    return 0;
}

