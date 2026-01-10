#include "proxy/monitor/TokenBucket.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <chrono>

using proxy::common::Logger;
using proxy::monitor::TokenBucket;

static void testBurstAndRefill() {
    TokenBucket bucket(/*qps*/ 10.0, /*burst*/ 5.0);

    auto t0 = TokenBucket::Clock::now();

    // Burst: allow 5 immediately.
    for (int i = 0; i < 5; ++i) {
        assert(bucket.AllowAt(t0, 1.0));
    }
    assert(!bucket.AllowAt(t0, 1.0));

    // After 100ms at 10 qps -> +1 token.
    auto t1 = t0 + std::chrono::milliseconds(100);
    assert(bucket.AllowAt(t1, 1.0));
    assert(!bucket.AllowAt(t1, 1.0));

    // After 500ms more -> +5 tokens (capped by capacity 5).
    auto t2 = t1 + std::chrono::milliseconds(500);
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (bucket.AllowAt(t2, 1.0)) ++allowed;
    }
    assert(allowed == 5);
}

static void testNonPositiveCostAllowed() {
    TokenBucket bucket(/*qps*/ 1.0, /*burst*/ 1.0);
    assert(bucket.AllowAt(TokenBucket::Clock::now(), 0.0));
    assert(bucket.AllowAt(TokenBucket::Clock::now(), -1.0));
}

int main() {
    Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);
    testBurstAndRefill();
    testNonPositiveCostAllowed();
    return 0;
}

