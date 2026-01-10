#include "proxy/network/Buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

namespace proxy {
namespace network {

ssize_t Buffer::ReadFd(int fd, int* savedErrno) {
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = WritableBytes();
    vec[0].iov_base = Begin() + writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;
    // when there is enough space in this buffer, don't read into extrabuf.
    // when extrabuf is used, we read 128k-1 bytes at most.
    const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writerIndex_ += n;
    } else {
        writerIndex_ = capacity_;
        Append(extrabuf, n - writable);
    }
    return n;
}

} // namespace network
} // namespace proxy
