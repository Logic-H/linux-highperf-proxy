#include "proxy/network/Poller.h"
#include "proxy/network/SelectPoller.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <cstdlib>

using proxy::common::Logger;
using proxy::network::Poller;
using proxy::network::SelectPoller;

int main() {
    Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    ::setenv("PROXY_USE_SELECT", "1", 1);
    Poller* poller = Poller::NewDefaultPoller(nullptr);
    assert(dynamic_cast<SelectPoller*>(poller) != nullptr);
    delete poller;

    ::unsetenv("PROXY_USE_SELECT");
    return 0;
}

