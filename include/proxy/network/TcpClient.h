#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/TcpConnection.h"
#include <mutex>

namespace proxy {
namespace network {

class Connector;
class EventLoop;

class TcpClient : proxy::common::noncopyable {
public:
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, const std::string& nameArg);
    ~TcpClient();

    void Connect();
    void Disconnect();
    void Stop();

    TcpConnectionPtr connection() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connection_;
    }

    bool retry() const { return retry_; }
    void EnableRetry() { retry_ = true; }

    const std::string& name() const { return name_; }

    void SetConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void SetMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void SetWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

private:
    void NewConnection(int sockfd);
    void RemoveConnection(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::shared_ptr<Connector> connector_;
    const std::string name_;
    
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    
    bool retry_;
    bool connect_;
    int nextConnId_;
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;
};

} // namespace network
} // namespace proxy
