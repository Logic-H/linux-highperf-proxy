#include "proxy/protocol/HttpServer.h"
#include "proxy/protocol/HttpContext.h"
#include "proxy/protocol/HttpRequest.h"
#include "proxy/protocol/HttpResponse.h"
#include "proxy/common/Logger.h"

namespace proxy {
namespace protocol {

HttpServer::HttpServer(proxy::network::EventLoop* loop,
                       const proxy::network::InetAddress& listenAddr,
                       const std::string& name,
                       proxy::network::TcpServer::Option option)
    : server_(loop, listenAddr, name, option) {
    server_.SetConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));
    server_.SetMessageCallback(
        std::bind(&HttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

void HttpServer::start() {
    LOG_INFO << "HttpServer[" << server_.name() << "] starts listening on " << server_.hostport();
    server_.Start();
}

void HttpServer::onConnection(const proxy::network::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        conn->SetContext(HttpContext());
    }
}

void HttpServer::onMessage(const proxy::network::TcpConnectionPtr& conn,
                           proxy::network::Buffer* buf,
                           std::chrono::system_clock::time_point receiveTime) {
    HttpContext* context = std::any_cast<HttpContext>(conn->GetMutableContext());

    // Support keep-alive / pipelining: drain all complete requests from buffer in one read.
    while (true) {
        if (!context->parseRequest(buf, receiveTime)) {
            conn->Send("HTTP/1.1 400 Bad Request\r\n\r\n");
            conn->Shutdown();
            return;
        }
        if (!context->gotAll()) {
            return;
        }
        onRequest(conn, context->request());
        context->reset();
        if (buf->ReadableBytes() == 0) {
            return;
        }
    }
}

void HttpServer::onRequest(const proxy::network::TcpConnectionPtr& conn, const HttpRequest& req) {
    const std::string& connection = req.getHeader("Connection");
    bool close = connection == "close" ||
                 (req.getVersion() == HttpRequest::kHttp10 && connection != "Keep-Alive");
    
    HttpResponse response(close);
    if (httpCallback_) {
        httpCallback_(req, &response);
    } else {
        response.setStatusCode(HttpResponse::k404NotFound);
        response.setStatusMessage("Not Found");
    }

    proxy::network::Buffer buf;
    response.appendToBuffer(&buf);
    conn->Send(buf.RetrieveAllAsString());
    
    if (response.closeConnection()) {
        conn->Shutdown();
    }
}

} // namespace protocol
} // namespace proxy
