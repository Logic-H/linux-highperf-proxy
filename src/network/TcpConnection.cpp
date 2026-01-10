#include "proxy/network/TcpConnection.h"
#include "proxy/network/Socket.h"
#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"
#include "proxy/monitor/Stats.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

namespace proxy {
namespace network {

static std::int64_t ToSteadyNs(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

static std::chrono::steady_clock::time_point FromSteadyNs(std::int64_t ns) {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

TcpConnection::TcpConnection(EventLoop* loop,
                             const std::string& nameArg,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr,
                             ssl_ctx_st* tlsCtx)
    : loop_(loop),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024),
      tlsCtx_(tlsCtx) {
    
    channel_->SetReadCallback(
        std::bind(&TcpConnection::HandleRead, this, std::placeholders::_1));
    channel_->SetWriteCallback(
        std::bind(&TcpConnection::HandleWrite, this));
    channel_->SetCloseCallback(
        std::bind(&TcpConnection::HandleClose, this));
    channel_->SetErrorCallback(
        std::bind(&TcpConnection::HandleError, this));
    
    LOG_DEBUG << "TcpConnection::ctor[" <<  name_ << "] at " << this << " fd=" << sockfd;
    socket_->SetKeepAlive(true);
    lastActiveNs_.store(ToSteadyNs(std::chrono::steady_clock::now()), std::memory_order_relaxed);
}

TcpConnection::~TcpConnection() {
    LOG_DEBUG << "TcpConnection::dtor[" <<  name_ << "] at " << this << " fd=" << channel_->fd() << " state=" << state_;
    if (ssl_) {
        SSL_free(reinterpret_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
}

void TcpConnection::ConnectEstablished() {
    // assert(state_ == kConnecting);
    SetState(kConnected);
    Touch();
    channel_->EnableReading();

    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::ConnectDestroyed() {
    if (state_ == kConnected) {
        SetState(kDisconnected);
        channel_->DisableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }
    channel_->Remove();
}

bool TcpConnection::tlsTryInitFromPeek() {
    if (!tlsEnabled()) return false;
    if (tlsState_ != 0) return false;

    unsigned char b = 0;
    const int n = ::recv(channel_->fd(), &b, 1, MSG_PEEK);
    if (n <= 0) return false;

    // TLS record type 0x16 indicates Handshake. If not, treat as plaintext.
    if (b != 0x16) {
        tlsState_ = 0; // plaintext
        tlsCtx_ = nullptr;
        return false;
    }

    SSL* s = SSL_new(reinterpret_cast<SSL_CTX*>(tlsCtx_));
    if (!s) {
        tlsCtx_ = nullptr;
        return false;
    }
    SSL_set_fd(s, channel_->fd());
    SSL_set_accept_state(s);
    ssl_ = reinterpret_cast<ssl_st*>(s);
    tlsState_ = 1;
    tlsWantWrite_ = false;
    return true;
}

bool TcpConnection::tlsDoHandshake() {
    if (!ssl_ || tlsState_ != 1) return false;
    SSL* s = reinterpret_cast<SSL*>(ssl_);
    const int r = SSL_accept(s);
    if (r == 1) {
        tlsState_ = 2;
        tlsWantWrite_ = false;
        return true;
    }
    const int e = SSL_get_error(s, r);
    if (e == SSL_ERROR_WANT_READ) {
        tlsWantWrite_ = false;
        return false;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        tlsWantWrite_ = true;
        if (!channel_->IsWriting()) channel_->EnableWriting();
        return false;
    }
    // Fatal handshake error.
    unsigned long le = ERR_get_error();
    if (le != 0) {
        char buf[256];
        ERR_error_string_n(le, buf, sizeof(buf));
        LOG_WARN << "TLS handshake failed: " << buf;
    } else {
        LOG_WARN << "TLS handshake failed error=" << e;
    }
    return false;
}

ssize_t TcpConnection::tlsReadOnce(char* buf, size_t cap, int* savedErrno) {
    if (!ssl_ || cap == 0) return 0;
    SSL* s = reinterpret_cast<SSL*>(ssl_);
    const int r = SSL_read(s, buf, static_cast<int>(cap));
    if (r > 0) return r;
    const int e = SSL_get_error(s, r);
    if (e == SSL_ERROR_WANT_READ) return -2;
    if (e == SSL_ERROR_WANT_WRITE) {
        tlsWantWrite_ = true;
        if (!channel_->IsWriting()) channel_->EnableWriting();
        return -2;
    }
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
    if (savedErrno) *savedErrno = EIO;
    return -1;
}

ssize_t TcpConnection::tlsWriteOnce(const void* data, size_t len, int* savedErrno) {
    if (!ssl_ || len == 0) return 0;
    SSL* s = reinterpret_cast<SSL*>(ssl_);
    const int r = SSL_write(s, data, static_cast<int>(len));
    if (r > 0) return r;
    const int e = SSL_get_error(s, r);
    if (e == SSL_ERROR_WANT_WRITE) return -2;
    if (e == SSL_ERROR_WANT_READ) return -2;
    if (savedErrno) *savedErrno = EIO;
    return -1;
}

void TcpConnection::HandleRead(std::chrono::system_clock::time_point receiveTime) {
    if (tlsEnabled() && tlsState_ == 0) {
        (void)tlsTryInitFromPeek();
    }
    if (ssl_ && tlsState_ == 1) {
        (void)tlsDoHandshake();
        if (tlsState_ != 2) return;
    }

    int savedErrno = 0;
    ssize_t n = 0;
    if (ssl_ && tlsState_ == 2) {
        char tmp[64 * 1024];
        n = tlsReadOnce(tmp, sizeof(tmp), &savedErrno);
        if (n > 0) {
            inputBuffer_.Append(tmp, static_cast<size_t>(n));
        } else if (n == -2) {
            return;
        }
    } else {
        n = inputBuffer_.ReadFd(channel_->fd(), &savedErrno);
    }
    if (n > 0) {
        proxy::monitor::Stats::Instance().AddBytesIn(n);
        Touch();
        if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
        }
    } else if (n == 0) {
        HandleClose();
    } else {
        errno = savedErrno;
        LOG_ERROR << "TcpConnection::HandleRead";
        HandleError();
    }
}

void TcpConnection::HandleWrite() {
    if (ssl_ && tlsState_ == 1 && tlsWantWrite_) {
        (void)tlsDoHandshake();
        if (tlsState_ != 2) return;
    }

    if (channel_->IsWriting()) {
        ssize_t n = 0;
        int savedErrno = 0;
        if (ssl_ && tlsState_ == 2) {
            n = tlsWriteOnce(outputBuffer_.Peek(), outputBuffer_.ReadableBytes(), &savedErrno);
            if (n == -2) return;
        } else {
            n = ::write(channel_->fd(), outputBuffer_.Peek(), outputBuffer_.ReadableBytes());
        }
        if (n > 0) {
            proxy::monitor::Stats::Instance().AddBytesOut(n);
            Touch();
            outputBuffer_.Retrieve(n);
            if (outputBuffer_.ReadableBytes() == 0) {
                channel_->DisableWriting();
                if (writeCompleteCallback_) {
                    loop_->QueueInLoop(
                        std::bind(writeCompleteCallback_, shared_from_this()));
                }
                if (state_ == kDisconnecting) {
                    ShutdownInLoop();
                }
            }
        } else {
            LOG_ERROR << "TcpConnection::HandleWrite";
        }
    } else {
        LOG_INFO << "Connection fd = " << channel_->fd() << " is down, no more writing";
    }
}

void TcpConnection::HandleClose() {
    LOG_DEBUG << "fd = " << channel_->fd() << " state = " << state_;
    SetState(kDisconnected);
    channel_->DisableAll();

    TcpConnectionPtr guardThis(shared_from_this());
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

void TcpConnection::HandleError() {
    int err = 0;
    int optval;
    socklen_t optlen = static_cast<socklen_t>(sizeof optval);
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        err = errno;
    } else {
        err = optval;
    }
    LOG_ERROR << "TcpConnection::HandleError name:" << name_ << " - SO_ERROR:" << err;
}

void TcpConnection::Send(const std::string& message) {
    if (state_ == kConnected) {
        if (loop_->IsInLoopThread()) {
            SendInLoop(message.data(), message.size());
        } else {
            // Copy data to lambda
            loop_->RunInLoop([ptr = shared_from_this(), msg = message]() {
                ptr->SendInLoop(msg.data(), msg.size());
            });
        }
    }
}

void TcpConnection::Send(const void* data, size_t len) {
    if (state_ == kConnected) {
        if (loop_->IsInLoopThread()) {
            SendInLoop(data, len);
        } else {
            std::string msg(static_cast<const char*>(data), len);
            loop_->RunInLoop([ptr = shared_from_this(), msg = std::move(msg)]() {
                ptr->SendInLoop(msg.data(), msg.size());
            });
        }
    }
}

void TcpConnection::SendInLoop(const void* data, size_t len) {
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (state_ == kDisconnected) {
        LOG_WARN << "disconnected, give up writing";
        return;
    }

    if (tlsEnabled() && tlsState_ == 0) {
        (void)tlsTryInitFromPeek();
    }
    if (ssl_ && tlsState_ == 1) {
        (void)tlsDoHandshake();
    }

    // if nothing in output queue, try write directly
    if (!channel_->IsWriting() && outputBuffer_.ReadableBytes() == 0) {
        int savedErrno = 0;
        if (ssl_ && tlsState_ == 2) {
            const ssize_t r = tlsWriteOnce(data, len, &savedErrno);
            if (r == -2) {
                nwrote = 0;
            } else {
                nwrote = r;
            }
        } else {
            nwrote = ::write(channel_->fd(), data, len);
        }
        if (nwrote >= 0) {
            if (nwrote > 0) {
                proxy::monitor::Stats::Instance().AddBytesOut(nwrote);
            }
            Touch();
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_) {
                loop_->QueueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                LOG_ERROR << "TcpConnection::SendInLoop";
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;
                }
            }
        }
    }

    // append remaining to buffer
    if (!faultError && remaining > 0) {
        size_t oldLen = outputBuffer_.ReadableBytes();
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_) {
            loop_->QueueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.Append(static_cast<const char*>(data) + nwrote, remaining);
        if (!channel_->IsWriting()) {
            channel_->EnableWriting();
        }
    }
}

void TcpConnection::Shutdown() {
    if (state_ == kConnected) {
        SetState(kDisconnecting);
        loop_->RunInLoop(std::bind(&TcpConnection::ShutdownInLoop, this));
    }
}

void TcpConnection::ShutdownInLoop() {
    if (!channel_->IsWriting()) {
        socket_->ShutdownWrite();
    }
}

void TcpConnection::ForceClose() {
    if (state_ == kConnected || state_ == kDisconnecting || state_ == kConnecting) {
        loop_->RunInLoop([conn = shared_from_this()]() {
            conn->ForceCloseInLoop();
        });
    }
}

void TcpConnection::ForceCloseInLoop() {
    if (state_ == kConnected || state_ == kDisconnecting || state_ == kConnecting) {
        HandleClose();
    }
}

void TcpConnection::StartRead() {
    auto self = shared_from_this();
    loop_->RunInLoop([self]() { self->StartReadInLoop(); });
}

void TcpConnection::StopRead() {
    auto self = shared_from_this();
    loop_->RunInLoop([self]() { self->StopReadInLoop(); });
}

void TcpConnection::StartReadInLoop() {
    if (!reading_) {
        reading_ = true;
        channel_->EnableReading();
    }
}

void TcpConnection::StopReadInLoop() {
    if (reading_) {
        reading_ = false;
        channel_->DisableReading();
    }
}

void TcpConnection::Touch() {
    lastActiveNs_.store(ToSteadyNs(std::chrono::steady_clock::now()), std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point TcpConnection::LastActiveTime() const {
    return FromSteadyNs(lastActiveNs_.load(std::memory_order_relaxed));
}

} // namespace network
} // namespace proxy
