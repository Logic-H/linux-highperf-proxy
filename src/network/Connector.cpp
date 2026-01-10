#include "proxy/network/Connector.h"
#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

namespace proxy {
namespace network {

const int Connector::kMaxRetryDelayMs;
const int Connector::kInitRetryDelayMs;

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
    : loop_(loop),
      serverAddr_(serverAddr),
      connect_(false),
      state_(kDisconnected),
      retryDelayMs_(kInitRetryDelayMs) {
}

Connector::~Connector() {
    // assert(!channel_);
    CancelRetryTimer();
}

void Connector::Start() {
    connect_ = true;
    try {
        auto self = shared_from_this();
        loop_->RunInLoop([self]() { self->StartInLoop(); });
    } catch (const std::bad_weak_ptr&) {
        loop_->RunInLoop(std::bind(&Connector::StartInLoop, this));
    }
}

void Connector::StartInLoop() {
    if (connect_) {
        Connect();
    } else {
        LOG_DEBUG << "Connector::StartInLoop - stop";
    }
}

void Connector::Restart() {
    connect_ = true;
    try {
        auto self = shared_from_this();
        loop_->RunInLoop([self]() { self->StartInLoop(); });
    } catch (const std::bad_weak_ptr&) {
        loop_->RunInLoop(std::bind(&Connector::StartInLoop, this));
    }
}

void Connector::Stop() {
    connect_ = false;
    try {
        auto self = shared_from_this();
        loop_->QueueInLoop([self]() { self->StopInLoop(); });
    } catch (const std::bad_weak_ptr&) {
        loop_->QueueInLoop(std::bind(&Connector::StopInLoop, this));
    }
}

void Connector::StopInLoop() {
    CancelRetryTimer();
    if (state_ == kConnecting) {
        SetState(kDisconnected);
        int sockfd = RemoveAndResetChannel();
        Retry(sockfd); // actually clean up and maybe not retry if connect_ is false
    }
}

void Connector::Connect() {
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        LOG_FATAL << "Connector::Connect socket";
    }
    
    int ret = ::connect(sockfd, serverAddr_.getSockAddr(), sizeof(struct sockaddr_in));
    int savedErrno = (ret == 0) ? 0 : errno;
    
    switch (savedErrno) {
        case 0:
        case EINPROGRESS:
        case EINTR:
        case EISCONN:
            Connecting(sockfd);
            break;

        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
        case ENETUNREACH:
            Retry(sockfd);
            break;

        case EACCES:
        case EPERM:
        case EAFNOSUPPORT:
        case EALREADY:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            LOG_ERROR << "connect error in Connector::startInLoop " << savedErrno;
            ::close(sockfd);
            break;

        default:
            LOG_ERROR << "Unexpected error in Connector::startInLoop " << savedErrno;
            ::close(sockfd);
            break;
    }
}

void Connector::Connecting(int sockfd) {
    SetState(kConnecting);
    // assert(!channel_);
    channel_.reset(new Channel(loop_, sockfd));
    channel_->SetWriteCallback(std::bind(&Connector::HandleWrite, this));
    channel_->SetErrorCallback(std::bind(&Connector::HandleError, this));
    channel_->EnableWriting();
}

int Connector::RemoveAndResetChannel() {
    channel_->DisableAll();
    channel_->Remove();
    int sockfd = channel_->fd();
    // Can't reset channel_ here because we are inside Channel::HandleEvent
    try {
        auto self = shared_from_this();
        loop_->QueueInLoop([self]() { self->ResetChannel(); });
    } catch (const std::bad_weak_ptr&) {
        loop_->QueueInLoop(std::bind(&Connector::ResetChannel, this));
    }
    return sockfd;
}

void Connector::ResetChannel() {
    channel_.reset();
}

void Connector::HandleWrite() {
    LOG_DEBUG << "Connector::HandleWrite " << state_;

    if (state_ == kConnecting) {
        int sockfd = RemoveAndResetChannel();
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            err = errno;
        }
        
        if (err) {
            LOG_WARN << "Connector::HandleWrite - SO_ERROR = " << err << " " << strerror(err);
            Retry(sockfd);
        } else {
            SetState(kConnected);
            if (connect_) {
                if (newConnectionCallback_) {
                    newConnectionCallback_(sockfd);
                } else {
                    ::close(sockfd);
                }
            } else {
                ::close(sockfd);
            }
        }
    }
}

void Connector::HandleError() {
    LOG_ERROR << "Connector::HandleError";
    if (state_ == kConnecting) {
        int sockfd = RemoveAndResetChannel();
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len);
        LOG_WARN << "SO_ERROR = " << err << " " << strerror(err);
        Retry(sockfd);
    }
}

void Connector::Retry(int sockfd) {
    ::close(sockfd);
    SetState(kDisconnected);
    if (connect_) {
        LOG_INFO << "Connector::Retry - Retry connecting to " << serverAddr_.toIpPort()
                 << " in " << retryDelayMs_ << " milliseconds. ";

        ScheduleRetryTimer();
        retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
    } else {
        LOG_DEBUG << "do not connect";
    }
}

void Connector::CancelRetryTimer() {
    Channel* ch = nullptr;
    if (retryTimerChannel_) ch = retryTimerChannel_.release();
    int fd = retryTimerFd_;
    retryTimerFd_ = -1;
    if (!ch && fd < 0) return;

    auto cleanup = [ch, fd]() {
        if (ch) {
            ch->DisableAll();
            ch->Remove();
            delete ch;
        }
        if (fd >= 0) {
            ::close(fd);
        }
    };

    if (loop_->IsInLoopThread()) {
        cleanup();
    } else {
        loop_->QueueInLoop(cleanup);
    }
}

void Connector::ScheduleRetryTimer() {
    CancelRetryTimer();

    retryTimerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (retryTimerFd_ < 0) {
        LOG_ERROR << "Connector::ScheduleRetryTimer timerfd_create failed errno=" << errno;
        loop_->RunInLoop(std::bind(&Connector::StartInLoop, this));
        return;
    }

    struct itimerspec howlong;
    std::memset(&howlong, 0, sizeof howlong);
    howlong.it_value.tv_sec = retryDelayMs_ / 1000;
    howlong.it_value.tv_nsec = (retryDelayMs_ % 1000) * 1000 * 1000;
    if (::timerfd_settime(retryTimerFd_, 0, &howlong, nullptr) != 0) {
        LOG_ERROR << "Connector::ScheduleRetryTimer timerfd_settime failed errno=" << errno;
        CancelRetryTimer();
        loop_->RunInLoop(std::bind(&Connector::StartInLoop, this));
        return;
    }

    retryTimerChannel_.reset(new Channel(loop_, retryTimerFd_));
    std::weak_ptr<Connector> weakSelf;
    bool hasWeak = true;
    try {
        weakSelf = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        hasWeak = false;
    }
    if (hasWeak) {
        retryTimerChannel_->SetReadCallback([weakSelf](std::chrono::system_clock::time_point) {
            auto self = weakSelf.lock();
            if (!self) return;
        uint64_t one;
        ::read(self->retryTimerFd_, &one, sizeof one);
        // Avoid deleting the Channel while executing its callback (UAF).
        if (self->retryTimerChannel_) {
            self->retryTimerChannel_->DisableAll();
            self->retryTimerChannel_->Remove();
            Channel* ch = self->retryTimerChannel_.release();
            self->loop_->QueueInLoop([ch]() { delete ch; });
        }
        if (self->retryTimerFd_ >= 0) {
            ::close(self->retryTimerFd_);
            self->retryTimerFd_ = -1;
        }
        self->StartInLoop();
        });
    } else {
        retryTimerChannel_->SetReadCallback([this](std::chrono::system_clock::time_point) {
            uint64_t one;
            ::read(retryTimerFd_, &one, sizeof one);
            if (retryTimerChannel_) {
                retryTimerChannel_->DisableAll();
                retryTimerChannel_->Remove();
                Channel* ch = retryTimerChannel_.release();
                loop_->QueueInLoop([ch]() { delete ch; });
            }
            if (retryTimerFd_ >= 0) {
                ::close(retryTimerFd_);
                retryTimerFd_ = -1;
            }
            StartInLoop();
        });
    }
    retryTimerChannel_->EnableReading();
}

} // namespace network
} // namespace proxy
