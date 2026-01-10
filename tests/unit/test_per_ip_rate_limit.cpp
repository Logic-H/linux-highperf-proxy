#include "proxy/monitor/PerKeyRateLimiter.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <chrono>
#include <thread>

using proxy::monitor::PerKeyRateLimiter;

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    // Basic allow/deny
    {
        PerKeyRateLimiter::Config cfg;
        cfg.qps = 1.0;
        cfg.burst = 1.0;
        cfg.cleanupEvery = 1;
        cfg.maxEntries = 100;
        cfg.idleSec = 60.0;
        PerKeyRateLimiter lim(cfg);

        assert(lim.Allow("127.0.0.1"));
        assert(!lim.Allow("127.0.0.1")); // immediate second should be limited
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(lim.Allow("127.0.0.1"));
    }

    // Max entries eviction
    {
        PerKeyRateLimiter::Config cfg;
        cfg.qps = 1000.0;
        cfg.burst = 1000.0;
        cfg.cleanupEvery = 1;
        cfg.maxEntries = 2;
        cfg.idleSec = 3600.0;
        PerKeyRateLimiter lim(cfg);

        assert(lim.Allow("A"));
        assert(lim.Allow("B"));
        assert(lim.Size() == 2);
        assert(lim.Allow("C"));
        assert(lim.Size() <= 2);
    }

    return 0;
}

