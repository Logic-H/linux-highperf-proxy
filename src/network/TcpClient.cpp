#include "proxy/network/TcpClient.h"
#include "proxy/network/Connector.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"

#include <stdio.h>

namespace proxy {
namespace network {

namespace detail {
void removeConnection(EventLoop* loop, const TcpConnectionPtr& conn) {
    loop->QueueInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
}
} // namespace detail

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, const std::string& nameArg)
    : loop_(loop),
      connector_(new Connector(loop, serverAddr)),
      name_(nameArg),
      retry_(false),
      connect_(true),
      nextConnId_(1) {
    connector_->SetNewConnectionCallback(
        std::bind(&TcpClient::NewConnection, this, std::placeholders::_1));
    LOG_INFO << "TcpClient::TcpClient[" << name_ << "] - connector " << connector_.get();
}

TcpClient::~TcpClient() {
    LOG_INFO << "TcpClient::~TcpClient[" << name_ << "] - connector " << connector_.get();
    TcpConnectionPtr conn;
    bool unique = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unique = (connection_.use_count() == 1);
        conn = connection_;
    }
    if (conn) {
        // CloseCallback will be called
        // If we are the last one holding conn, we must destroy it
        CloseCallback cb = std::bind(&detail::removeConnection, loop_, std::placeholders::_1);
        loop_->RunInLoop(std::bind(&TcpConnection::SetCloseCallback, conn, cb));
        if (unique) {
            conn->Shutdown(); // Graceful close
        }
    } else {
        connector_->Stop();
    }
}

void TcpClient::Connect() {
    LOG_INFO << "TcpClient::Connect[" << name_ << "] - connecting to " 
             << connector_->serverAddress().toIpPort();
    connect_ = true;
    connector_->Start();
}

void TcpClient::Disconnect() {
    connect_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            connection_->Shutdown();
        }
    }
}

void TcpClient::Stop() {
    connect_ = false;
    connector_->Stop();
}

void TcpClient::NewConnection(int sockfd) {
    // Check loop
    InetAddress peerAddr = connector_->serverAddress(); // TODO: getpeername
    InetAddress localAddr(0); // TODO: getsockname
    
    char buf[32];
    snprintf(buf, sizeof buf, ":%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    TcpConnectionPtr conn(new TcpConnection(loop_,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr,
                                            nullptr));
    
    conn->SetConnectionCallback(connectionCallback_);
    conn->SetMessageCallback(messageCallback_);
    conn->SetWriteCompleteCallback(writeCompleteCallback_);
    conn->SetCloseCallback(
        std::bind(&TcpClient::RemoveConnection, this, std::placeholders::_1));
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }
    conn->ConnectEstablished();
}

void TcpClient::RemoveConnection(const TcpConnectionPtr& conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
    }

    loop_->QueueInLoop(std::bind(&TcpConnection::ConnectDestroyed, conn));
    if (retry_ && connect_) {
        LOG_INFO << "TcpClient::connect[" << name_ << "] - Reconnecting to "
                 << connector_->serverAddress().toIpPort();
        connector_->Restart();
    }
}

} // namespace network
} // namespace proxy
