#include "proxy/balancer/ScriptHealthChecker.h"
#include "proxy/common/Logger.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace proxy {
namespace balancer {

static void ReplaceAll(std::string* s, const std::string& from, const std::string& to) {
    if (!s || from.empty()) return;
    size_t pos = 0;
    while ((pos = s->find(from, pos)) != std::string::npos) {
        s->replace(pos, from.size(), to);
        pos += to.size();
    }
}

ScriptHealthChecker::ScriptHealthChecker(proxy::network::EventLoop* loop, double timeoutSec, std::string cmdTemplate)
    : HealthChecker(loop), timeoutSec_(timeoutSec), cmdTemplate_(std::move(cmdTemplate)) {
}

std::string ScriptHealthChecker::ExpandTemplate(const std::string& tpl, const proxy::network::InetAddress& addr) {
    std::string out = tpl;
    ReplaceAll(&out, "{ip}", addr.toIp());
    ReplaceAll(&out, "{port}", std::to_string(addr.toPort()));
    return out;
}

bool ScriptHealthChecker::RunOnceWithTimeout(const std::string& cmd, double timeoutSec) {
    if (cmd.empty()) return false;

    pid_t pid = -1;
    char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(cmd.c_str()), nullptr};
    const int sp = ::posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv, ::environ);
    if (sp != 0 || pid <= 0) return false;

    const auto start = std::chrono::steady_clock::now();
    int status = 0;
    while (true) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status) == 0;
            }
            return false;
        }
        if (r < 0) {
            return false;
        }
        const double elapsedSec =
            std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();
        if (timeoutSec > 0.0 && elapsedSec >= timeoutSec) {
            ::kill(pid, SIGKILL);
            // Reap to avoid zombies.
            (void)::waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ScriptHealthChecker::Check(const proxy::network::InetAddress& addr, CheckCallback cb) {
    const std::string tpl = cmdTemplate_;
    const double timeoutSec = timeoutSec_;
    auto* loop = loop_;

    std::thread([loop, addr, cb, tpl, timeoutSec]() {
        const std::string cmd = ExpandTemplate(tpl, addr);
        const bool ok = RunOnceWithTimeout(cmd, timeoutSec);
        if (loop) {
            loop->QueueInLoop([cb, ok, addr]() {
                if (cb) cb(ok, addr);
            });
        }
    }).detach();
}

} // namespace balancer
} // namespace proxy
