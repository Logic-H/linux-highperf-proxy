#include "proxy/network/TcpServer.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <cstring>
#include <string>
#include <thread>

using namespace proxy::network;
using namespace proxy::common;

static int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    return fd;
}

static std::string recvSome(int fd, int timeoutMs = 2000) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;
    int pret = ::poll(&pfd, 1, timeoutMs);
    assert(pret == 1);
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    assert(n > 0);
    return std::string(buf, buf + n);
}

class EchoServer {
public:
    EchoServer(EventLoop* loop, const InetAddress& addr, const std::string& name)
        : server_(loop, addr, name), loop_(loop) {
        server_.SetConnectionCallback(
            std::bind(&EchoServer::OnConnection, this, std::placeholders::_1));
        server_.SetMessageCallback(
            std::bind(&EchoServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }

    void Start() {
        server_.Start();
    }

private:
    void OnConnection(const TcpConnectionPtr& conn) {
        LOG_INFO << "EchoServer - " << conn->peerAddress().toIpPort() << " -> "
                 << conn->localAddress().toIpPort() << " is "
                 << (conn->connected() ? "UP" : "DOWN");
    }

    void OnMessage(const TcpConnectionPtr& conn, Buffer* buf, std::chrono::system_clock::time_point time) {
        std::string msg = buf->RetrieveAllAsString();
        LOG_INFO << conn->name() << " echo " << msg.size() << " bytes, "
                 << "data received at " << std::chrono::system_clock::to_time_t(time);
        
        // Echo back
        conn->Send(msg);
        
        // If received "quit", stop server
        if (msg == "quit\r\n" || msg == "quit\n") {
            LOG_INFO << "Quit command received. Stopping server...";
            loop_->Quit();
        }
    }

    TcpServer server_;
    EventLoop* loop_;
};

int main() {
    Logger::Instance().SetLevel(LogLevel::DEBUG);
    LOG_INFO << "Starting EchoServer test";

    EventLoop loop;
    constexpr uint16_t port = 9981;
    InetAddress addr(port);
    EchoServer server(&loop, addr, "EchoServer");

    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int fd = connectTo(port);

        const char* msg = "hello\n";
        ssize_t n1 = ::send(fd, msg, std::strlen(msg), 0);
        assert(n1 == (ssize_t)std::strlen(msg));
        std::string echoed = recvSome(fd);
        assert(echoed.find("hello") != std::string::npos);

        const char* quit = "quit\n";
        ssize_t n2 = ::send(fd, quit, std::strlen(quit), 0);
        assert(n2 == (ssize_t)std::strlen(quit));

        ::close(fd);
    });

    loop.Loop();
    client.join();

    LOG_INFO << "EchoServer test finished";
    return 0;
}
