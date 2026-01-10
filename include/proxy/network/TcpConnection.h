#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/Callbacks.h"
#include "proxy/network/Buffer.h"

#include <memory>
#include <string>
#include <atomic>
#include <chrono>
#include <any>
#include <cstdint>

struct ssl_ctx_st;
struct ssl_st;

namespace proxy {
namespace network {

class Channel;
class EventLoop;
class Socket;

class TcpConnection : proxy::common::noncopyable,
                      public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(EventLoop* loop,
                  const std::string& name,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr,
                  ssl_ctx_st* tlsCtx = nullptr);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddress() const { return localAddr_; }
    const InetAddress& peerAddress() const { return peerAddr_; }
    bool connected() const { return state_ == kConnected; }
    bool disconnected() const { return state_ == kDisconnected; }

    void SetContext(const std::any& context) { context_ = context; }
    const std::any& GetContext() const { return context_; }
    std::any* GetMutableContext() { return &context_; }

    // Thread safe
    void Send(const std::string& message);
    void Send(const void* data, size_t len);
    void Shutdown();
    void ForceClose();
    void StartRead();
    void StopRead();

    // For connection management (zombie cleanup)
    std::chrono::steady_clock::time_point LastActiveTime() const;

    void SetConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void SetMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void SetWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void SetHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark) { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }
    void SetCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }

    // Called when TcpServer accepts a new connection
    void ConnectEstablished();
    // Called when TcpServer has removed me from its map
    void ConnectDestroyed();

private:
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

    void HandleRead(std::chrono::system_clock::time_point receiveTime);
    void HandleWrite();
    void HandleClose();
    void HandleError();

    void SendInLoop(const std::string& message); // Changed to take string by value to simplify lifecycle
    void SendInLoop(const void* message, size_t len);
    void ShutdownInLoop();
    void ForceCloseInLoop();
    void StartReadInLoop();
    void StopReadInLoop();
    void Touch();
    bool tlsEnabled() const { return tlsCtx_ != nullptr; }
    bool tlsHandshakeComplete() const { return tlsState_ == 2; } // 0 unknown/plain, 1 handshake, 2 established
    bool tlsTryInitFromPeek();
    bool tlsDoHandshake();
    ssize_t tlsReadOnce(char* buf, size_t cap, int* savedErrno);
    ssize_t tlsWriteOnce(const void* data, size_t len, int* savedErrno);

    void SetState(StateE s) { state_ = s; }

    EventLoop* loop_;
    const std::string name_;
    std::atomic<StateE> state_;
    bool reading_;

    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;

    size_t highWaterMark_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
    
    std::any context_;

    std::atomic<std::int64_t> lastActiveNs_;

    // TLS termination (optional): auto-detect TLS handshake on first bytes.
    ssl_ctx_st* tlsCtx_{nullptr};
    ssl_st* ssl_{nullptr};
    int tlsState_{0}; // 0 unknown/plain, 1 handshake, 2 established
    bool tlsWantWrite_{false};
};

} // namespace network
} // namespace proxy
