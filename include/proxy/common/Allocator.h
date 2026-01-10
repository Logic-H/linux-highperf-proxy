#pragma once

#include "proxy/common/noncopyable.h"
#include <cstddef>

namespace proxy {
namespace common {

class Allocator : noncopyable {
public:
    virtual ~Allocator() = default;

    virtual void* Allocate(size_t size) = 0;
    virtual void Deallocate(void* ptr) = 0;
    virtual size_t GetBlockSize() const = 0;
};

} // namespace common
} // namespace proxy
