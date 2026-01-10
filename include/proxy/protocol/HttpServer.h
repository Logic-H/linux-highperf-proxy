#pragma once

#include "proxy/network/TcpServer.h"
#include "proxy/common/noncopyable.h"
#include <functional>

namespace proxy {
namespace protocol {

class HttpRequest;
class HttpResponse;

class HttpServer : proxy::common::noncopyable {
public:
    using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;

    HttpServer(proxy::network::EventLoop* loop,
               const proxy::network::InetAddress& listenAddr,
               const std::string& name,
               proxy::network::TcpServer::Option option = proxy::network::TcpServer::kNoReusePort);

    proxy::network::EventLoop* getLoop() const { return server_.getLoop(); }

    void setHttpCallback(const HttpCallback& cb) {
        httpCallback_ = cb;
    }

    void setThreadNum(int numThreads) {
        server_.SetThreadNum(numThreads);
    }

    void start();

private:
    void onConnection(const proxy::network::TcpConnectionPtr& conn);
    void onMessage(const proxy::network::TcpConnectionPtr& conn,
                   proxy::network::Buffer* buf,
                   std::chrono::system_clock::time_point receiveTime);
    void onRequest(const proxy::network::TcpConnectionPtr&, const HttpRequest&);

    proxy::network::TcpServer server_;
    HttpCallback httpCallback_;
};

} // namespace protocol
} // namespace proxy
