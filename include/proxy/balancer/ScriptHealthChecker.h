#pragma once

#include "proxy/balancer/HealthChecker.h"

#include <string>

namespace proxy {
namespace balancer {

// Run a user-defined script/command to decide backend health.
// - The command is executed via "/bin/sh -c <cmd>".
// - Placeholders:
//   - {ip}: backend IPv4 string
//   - {port}: backend port number
// - Exit code 0 => healthy; otherwise unhealthy.
// - Timeout enforced (best-effort): process is SIGKILL'd after timeout.
class ScriptHealthChecker : public HealthChecker {
public:
    ScriptHealthChecker(proxy::network::EventLoop* loop, double timeoutSec, std::string cmdTemplate);
    ~ScriptHealthChecker() override = default;

    void Check(const proxy::network::InetAddress& addr, CheckCallback cb) override;

private:
    static std::string ExpandTemplate(const std::string& tpl, const proxy::network::InetAddress& addr);
    static bool RunOnceWithTimeout(const std::string& cmd, double timeoutSec);

    double timeoutSec_{2.0};
    std::string cmdTemplate_;
};

} // namespace balancer
} // namespace proxy

