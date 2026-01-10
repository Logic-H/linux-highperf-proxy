#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/InetAddress.h"

#include <functional>
#include <memory>
#include <atomic>

namespace proxy {
namespace network {

class Channel;
class EventLoop;

class Connector : public std::enable_shared_from_this<Connector>,
                  proxy::common::noncopyable {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    ~Connector();

    void SetNewConnectionCallback(const NewConnectionCallback& cb) {
        newConnectionCallback_ = cb;
    }

    void Start();
    void Restart();
    void Stop();

    const InetAddress& serverAddress() const { return serverAddr_; }

private:
    enum States { kDisconnected, kConnecting, kConnected };
    static const int kMaxRetryDelayMs = 30 * 1000;
    static const int kInitRetryDelayMs = 500;

    void SetState(States s) { state_ = s; }
    void StartInLoop();
    void StopInLoop();
    void Connect();
    void Connecting(int sockfd);
    void HandleWrite();
    void HandleError();
    void Retry(int sockfd);
    int RemoveAndResetChannel();
    void ResetChannel();
    void CancelRetryTimer();
    void ScheduleRetryTimer();

    EventLoop* loop_;
    InetAddress serverAddr_;
    bool connect_; // atomic
    States state_;
    std::unique_ptr<Channel> channel_;
    NewConnectionCallback newConnectionCallback_;
    int retryDelayMs_;

    int retryTimerFd_{-1};
    std::unique_ptr<Channel> retryTimerChannel_;
};

} // namespace network
} // namespace proxy
