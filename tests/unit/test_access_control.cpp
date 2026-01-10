#include "proxy/monitor/AccessControl.h"
#include "proxy/common/Logger.h"

#include <cassert>

using proxy::monitor::AccessControl;

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    // Allowlist: only 127.0.0.0/8
    {
        AccessControl::Config cfg;
        cfg.ipMode = AccessControl::IpMode::kAllowList;
        cfg.cidrs = {"127.0.0.0/8"};
        cfg.requireToken = false;
        AccessControl ac(cfg);
        assert(ac.Allow("127.0.0.1", ""));
        assert(!ac.Allow("10.0.0.1", ""));
    }

    // Denylist: block 10.0.0.0/8
    {
        AccessControl::Config cfg;
        cfg.ipMode = AccessControl::IpMode::kDenyList;
        cfg.cidrs = {"10.0.0.0/8"};
        AccessControl ac(cfg);
        assert(!ac.Allow("10.1.2.3", ""));
        assert(ac.Allow("127.0.0.1", ""));
    }

    // Token required
    {
        AccessControl::Config cfg;
        cfg.ipMode = AccessControl::IpMode::kOff;
        cfg.requireToken = true;
        cfg.validTokens = {"t1", "t2"};
        AccessControl ac(cfg);
        assert(!ac.Allow("127.0.0.1", ""));
        assert(!ac.Allow("127.0.0.1", "bad"));
        assert(ac.Allow("127.0.0.1", "t2"));
    }

    // API Key required
    {
        AccessControl::Config cfg;
        cfg.ipMode = AccessControl::IpMode::kOff;
        cfg.requireApiKey = true;
        cfg.validApiKeys = {"k1", "k2"};
        AccessControl ac(cfg);
        assert(!ac.Allow("127.0.0.1", "", ""));
        assert(!ac.Allow("127.0.0.1", "", "bad"));
        assert(ac.Allow("127.0.0.1", "", "k1"));
    }

    // Token + API Key required simultaneously
    {
        AccessControl::Config cfg;
        cfg.ipMode = AccessControl::IpMode::kOff;
        cfg.requireToken = true;
        cfg.validTokens = {"t"};
        cfg.requireApiKey = true;
        cfg.validApiKeys = {"k"};
        AccessControl ac(cfg);
        assert(!ac.Allow("127.0.0.1", "t", ""));
        assert(!ac.Allow("127.0.0.1", "", "k"));
        assert(ac.Allow("127.0.0.1", "t", "k"));
    }

    return 0;
}
