#pragma once

#include "proxy/common/MemoryPool.h"
#include <string>
#include <vector>
#include <algorithm>
#include <sys/types.h> // for ssize_t

namespace proxy {
namespace network {

/// A buffer class using MemoryPool
class Buffer {
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(nullptr),
          capacity_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {
        buffer_ = static_cast<char*>(proxy::common::MemoryPool::Instance().Allocate(capacity_));
    }

    ~Buffer() {
        proxy::common::MemoryPool::Instance().Deallocate(buffer_, capacity_);
    }
    
    // Disable copy
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    
    // Enable move
    Buffer(Buffer&& other) noexcept
        : buffer_(other.buffer_),
          capacity_(other.capacity_),
          readerIndex_(other.readerIndex_),
          writerIndex_(other.writerIndex_) {
        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.readerIndex_ = 0;
        other.writerIndex_ = 0;
    }

    size_t ReadableBytes() const { return writerIndex_ - readerIndex_; }
    size_t WritableBytes() const { return capacity_ - writerIndex_; }
    size_t PrependableBytes() const { return readerIndex_; }

    const char* Peek() const { return buffer_ + readerIndex_; }

    void Retrieve(size_t len) {
        if (len < ReadableBytes()) {
            readerIndex_ += len;
        } else {
            RetrieveAll();
        }
    }

    void RetrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    std::string RetrieveAllAsString() {
        return RetrieveAsString(ReadableBytes());
    }

    std::string RetrieveAsString(size_t len) {
        std::string result(Peek(), len);
        Retrieve(len);
        return result;
    }

    void Append(const std::string& str) {
        Append(str.c_str(), str.size());
    }

    void Append(const char* data, size_t len) {
        EnsureWritableBytes(len);
        std::copy(data, data + len, BeginWrite());
        HasWritten(len);
    }

    char* BeginWrite() { return buffer_ + writerIndex_; }
    const char* BeginWrite() const { return buffer_ + writerIndex_; }

    void HasWritten(size_t len) { writerIndex_ += len; }

    void EnsureWritableBytes(size_t len) {
        if (WritableBytes() < len) {
            MakeSpace(len);
        }
    }

    ssize_t ReadFd(int fd, int* savedErrno);

private:
    char* Begin() { return buffer_; }
    const char* Begin() const { return buffer_; }

    void MakeSpace(size_t len) {
        if (WritableBytes() + PrependableBytes() < len + kCheapPrepend) {
            // Resize needed
            size_t newCapacity = capacity_ * 2;
            if (newCapacity < capacity_ + len) {
                 newCapacity = capacity_ + len + kCheapPrepend;
            }
            
            char* newBuffer = static_cast<char*>(proxy::common::MemoryPool::Instance().Allocate(newCapacity));
            
            // Copy data
            size_t readable = ReadableBytes();
            std::copy(Begin() + readerIndex_, Begin() + writerIndex_, newBuffer + kCheapPrepend);
            
            // Release old buffer
            proxy::common::MemoryPool::Instance().Deallocate(buffer_, capacity_);
            
            buffer_ = newBuffer;
            capacity_ = newCapacity;
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        } else {
            // Move readable data to the front
            size_t readable = ReadableBytes();
            std::copy(Begin() + readerIndex_,
                      Begin() + writerIndex_,
                      Begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    char* buffer_;
    size_t capacity_;
    size_t readerIndex_;
    size_t writerIndex_;
};

} // namespace network
} // namespace proxy
