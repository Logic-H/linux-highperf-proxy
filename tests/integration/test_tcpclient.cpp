#include "proxy/network/TcpClient.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/TcpServer.h"
#include "proxy/common/Logger.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <string>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstring>

using namespace proxy::network;
using namespace proxy::common;

static uint16_t pickFreePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    assert(port != 0);
    return port;
}

// --- Echo Server ---
class TestEchoServer {
public:
    TestEchoServer(EventLoop* loop, const InetAddress& addr)
        : server_(loop, addr, "TestServer") {
        server_.SetConnectionCallback([](const TcpConnectionPtr& conn){
            if (conn->connected()) {
                LOG_INFO << "Server: New connection from " << conn->peerAddress().toIpPort();
            }
        });
        server_.SetMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, std::chrono::system_clock::time_point){
            std::string msg = buf->RetrieveAllAsString();
            LOG_INFO << "Server received: " << msg;
            conn->Send(msg);
        });
    }
    void Start() { server_.Start(); }
private:
    TcpServer server_;
};

// --- Test Client ---
EventLoop* g_clientLoop;
TcpClient* g_client;

void onClientMessage(const TcpConnectionPtr& conn, Buffer* buf, std::chrono::system_clock::time_point) {
    std::string msg = buf->RetrieveAllAsString();
    LOG_INFO << "Client received: " << msg;
    if (msg == "Hello Proxy") {
        LOG_INFO << "Test PASS";
        g_client->Disconnect();
        g_clientLoop->Quit();
    }
}

void onClientConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO << "Client: Connected to " << conn->peerAddress().toIpPort();
        conn->Send("Hello Proxy");
    } else {
        LOG_INFO << "Client: Disconnected";
    }
}

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);
    const uint16_t port = pickFreePort();
    std::atomic<bool> serverReady{false};

    EventLoop serverLoop;
    TestEchoServer server(&serverLoop, InetAddress(port));
    std::thread serverThread([&]() {
        server.Start();
        serverReady.store(true);
        serverLoop.Loop();
    });

    // Wait for server to start (bounded).
    for (int i = 0; i < 50 && !serverReady.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(serverReady.load());

    // Run Client
    EventLoop loop;
    g_clientLoop = &loop;
    TcpClient client(&loop, InetAddress("127.0.0.1", port), "TestClient");
    g_client = &client;
    
    client.SetConnectionCallback(onClientConnection);
    client.SetMessageCallback(onClientMessage);
    client.Connect();
    
    loop.Loop();
    serverLoop.Quit();
    serverThread.join();
    return 0;
}
