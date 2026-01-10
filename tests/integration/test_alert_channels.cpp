#include "proxy/common/Logger.h"
#include "proxy/monitor/AlertManager.h"
#include "proxy/monitor/Stats.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using proxy::network::EventLoop;

namespace {

static uint16_t bindEphemeralPort(int* listenFdOut) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(fd, 16) == 0);

    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    assert(port != 0);
    *listenFdOut = fd;
    return port;
}

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}

static std::string recvAllWithTimeout(int fd, int timeoutMs = 2000) {
    std::string out;
    while (pollReadable(fd, timeoutMs)) {
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) out.append(buf, buf + n);
        else break;
        if (out.size() > 256 * 1024) break;
    }
    return out;
}

} // namespace

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int webhookListen = -1;
    int smsListen = -1;
    int smtpListen = -1;
    const uint16_t webhookPort = bindEphemeralPort(&webhookListen);
    const uint16_t smsPort = bindEphemeralPort(&smsListen);
    const uint16_t smtpPort = bindEphemeralPort(&smtpListen);

    std::atomic<bool> gotWebhook{false};
    std::atomic<bool> gotSms{false};
    std::atomic<bool> gotEmail{false};
    std::string webhookReq;
    std::string smsReq;
    std::string smtpReq;

    EventLoop loop;

    auto maybeQuit = [&]() {
        if (gotWebhook.load() && gotSms.load() && gotEmail.load()) {
            loop.QueueInLoop([&]() { loop.Quit(); });
        }
    };

    std::thread webhookServer([&]() {
        if (!pollReadable(webhookListen, 2500)) {
            ::close(webhookListen);
            loop.QueueInLoop([&]() { loop.Quit(); });
            return;
        }
        int cfd = ::accept(webhookListen, nullptr, nullptr);
        if (cfd >= 0) {
            webhookReq = recvAllWithTimeout(cfd, 2000);
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
            ::send(cfd, resp, std::strlen(resp), 0);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            gotWebhook.store(true);
        }
        ::close(webhookListen);
        maybeQuit();
    });

    std::thread smsServer([&]() {
        if (!pollReadable(smsListen, 2500)) {
            ::close(smsListen);
            loop.QueueInLoop([&]() { loop.Quit(); });
            return;
        }
        int cfd = ::accept(smsListen, nullptr, nullptr);
        if (cfd >= 0) {
            smsReq = recvAllWithTimeout(cfd, 2000);
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
            ::send(cfd, resp, std::strlen(resp), 0);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            gotSms.store(true);
        }
        ::close(smsListen);
        maybeQuit();
    });

    std::thread smtpServer([&]() {
        if (!pollReadable(smtpListen, 2500)) {
            ::close(smtpListen);
            loop.QueueInLoop([&]() { loop.Quit(); });
            return;
        }
        int cfd = ::accept(smtpListen, nullptr, nullptr);
        if (cfd >= 0) {
            // Optional greeting (client doesn't require it, but keeps the flow realistic).
            const char* greet = "220 local smtp\r\n";
            ::send(cfd, greet, std::strlen(greet), 0);
            smtpReq = recvAllWithTimeout(cfd, 2000);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            gotEmail.store(true);
        }
        ::close(smtpListen);
        maybeQuit();
    });

    auto& stats = proxy::monitor::Stats::Instance();
    stats.IncActiveConnections(); // trigger threshold

    proxy::monitor::AlertManager::Config cfg;
    cfg.enabled = true;
    cfg.intervalSec = 0.05;
    cfg.cooldownSec = 0.0;
    cfg.webhookUrl = std::string("http://127.0.0.1:") + std::to_string(webhookPort) + "/alert";
    cfg.smsWebhookUrl = std::string("http://127.0.0.1:") + std::to_string(smsPort) + "/sms";
    cfg.email.smtpHost = "127.0.0.1";
    cfg.email.smtpPort = smtpPort;
    cfg.email.mailFrom = "proxy@example.com";
    cfg.email.mailTo = "ops@example.com";
    cfg.email.subjectPrefix = "Proxy Alert";
    cfg.thresholds.maxActiveConnections = 0;

    proxy::monitor::AlertManager am(&loop, cfg);
    am.Start();

    std::thread killer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        loop.QueueInLoop([&]() { loop.Quit(); });
    });
    loop.Loop();

    killer.join();
    webhookServer.join();
    smsServer.join();
    smtpServer.join();
    stats.DecActiveConnections();

    assert(gotWebhook.load());
    assert(gotSms.load());
    assert(gotEmail.load());

    assert(webhookReq.find("POST /alert HTTP/1.1") != std::string::npos);
    assert(smsReq.find("POST /sms HTTP/1.1") != std::string::npos);
    assert(webhookReq.find("\"type\":\"threshold\"") != std::string::npos);
    assert(smsReq.find("\"metric\":\"active_connections\"") != std::string::npos);

    assert(smtpReq.find("MAIL FROM:<proxy@example.com>") != std::string::npos);
    assert(smtpReq.find("RCPT TO:<ops@example.com>") != std::string::npos);
    assert(smtpReq.find("Subject: Proxy Alert") != std::string::npos);
    assert(smtpReq.find("\"type\":\"threshold\"") != std::string::npos);

    return 0;
}

