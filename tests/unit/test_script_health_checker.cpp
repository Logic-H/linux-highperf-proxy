#include "proxy/balancer/ScriptHealthChecker.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using proxy::balancer::ScriptHealthChecker;
using proxy::common::Logger;
using proxy::network::EventLoop;
using proxy::network::InetAddress;

static std::string makeTempScript(const std::string& body) {
    char path[] = "/tmp/proxy_script_check_XXXXXX";
    int fd = ::mkstemp(path);
    assert(fd >= 0);
    FILE* f = ::fdopen(fd, "w");
    assert(f != nullptr);
    ::fputs("#!/bin/sh\n", f);
    ::fputs(body.c_str(), f);
    ::fclose(f);
    ::chmod(path, 0700);
    return std::string(path);
}

int main() {
    Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    EventLoop loop;
    InetAddress addr("127.0.0.1", 12345);

    // exit 0 => healthy
    {
        std::string script = makeTempScript("exit 0\n");
        ScriptHealthChecker checker(&loop, 1.0, script + " {ip} {port}");
        std::atomic<bool> got{false};
        std::atomic<bool> ok{false};
        checker.Check(addr, [&](bool healthy, const InetAddress&) {
            ok.store(healthy);
            got.store(true);
            loop.Quit();
        });
        loop.Loop();
        assert(got.load());
        assert(ok.load());
        ::unlink(script.c_str());
    }

    // non-zero => unhealthy
    {
        std::string script = makeTempScript("exit 7\n");
        ScriptHealthChecker checker(&loop, 1.0, script + " {ip} {port}");
        std::atomic<bool> got{false};
        std::atomic<bool> ok{true};
        checker.Check(addr, [&](bool healthy, const InetAddress&) {
            ok.store(healthy);
            got.store(true);
            loop.Quit();
        });
        loop.Loop();
        assert(got.load());
        assert(!ok.load());
        ::unlink(script.c_str());
    }

    // timeout => unhealthy
    {
        std::string script = makeTempScript("sleep 2\nexit 0\n");
        ScriptHealthChecker checker(&loop, 0.2, script + " {ip} {port}");
        std::atomic<bool> got{false};
        std::atomic<bool> ok{true};
        auto start = std::chrono::steady_clock::now();
        checker.Check(addr, [&](bool healthy, const InetAddress&) {
            ok.store(healthy);
            got.store(true);
            loop.Quit();
        });
        loop.Loop();
        const double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();
        assert(got.load());
        assert(!ok.load());
        assert(elapsed < 1.5);
        ::unlink(script.c_str());
    }

    return 0;
}
