#pragma once

#include "proxy/common/SlabAllocator.h"
#include "proxy/common/BuddyAllocator.h"
#include <vector>
#include <memory>
#include <map>
#include <atomic>

namespace proxy {
namespace common {

class MemoryPool : noncopyable {
public:
    struct Stats {
        size_t slabReservedBytes{0};
        size_t slabInUseBytes{0};
        size_t slabFreeBytes{0};
        size_t buddyReservedBytes{0};
        size_t buddyInUseBytes{0};
        size_t buddyFreeBytes{0};
        size_t buddyArenas{0};
        size_t buddyIdleArenas{0};
        size_t buddyArenaReclaims{0};
        size_t mallocInUseBytes{0};
        size_t totalInUseBytes{0};
        std::vector<SlabAllocator::Stats> slabs;
        BuddyAllocator::Stats buddy;
        size_t mallocAllocs{0};
        size_t mallocFrees{0};
    };

    static MemoryPool& Instance();

    void* Allocate(size_t size);
    void Deallocate(void* ptr, size_t size); // Size is required for Slab matching
    Stats GetStats() const;

private:
    MemoryPool();
    ~MemoryPool() = default;

    std::vector<std::unique_ptr<SlabAllocator>> allocators_;
    // Map minimal block size to allocator index
    std::map<size_t, size_t> sizeMap_;
    size_t maxSlabSize_{0};
    std::unique_ptr<BuddyAllocator> buddy_;
    size_t buddyArenaSizeBytes_{0};

    std::atomic<size_t> mallocInUseBytes_{0};
    std::atomic<size_t> mallocAllocs_{0};
    std::atomic<size_t> mallocFrees_{0};
};

} // namespace common
} // namespace proxy
