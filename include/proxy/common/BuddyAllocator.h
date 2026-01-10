#pragma once

#include "proxy/common/Allocator.h"
#include "proxy/common/noncopyable.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace proxy {
namespace common {

// Buddy allocator for large blocks.
// - Manages arenas of size `arenaSizeBytes` (power-of-two).
// - Supports splitting/merging blocks in powers-of-two down to `minBlockBytes`.
// - Optional LRU reclaim: fully-free arenas beyond keepArenas are freed (least-recently-used first).
class BuddyAllocator : noncopyable {
public:
    struct Options {
        size_t minBlockBytes{128 * 1024};   // power-of-two recommended
        size_t arenaSizeBytes{8 * 1024 * 1024}; // power-of-two recommended
        size_t keepArenas{1};              // keep at least this many fully-free arenas
        size_t maxArenas{8};               // hard cap, best-effort (may fallback to malloc if exceeded)
    };

    struct Stats {
        size_t minBlockBytes{0};
        size_t arenaSizeBytes{0};
        size_t arenasTotal{0};
        size_t arenasIdle{0};
        size_t reservedBytes{0};
        size_t inUseBytes{0};
        size_t freeBytes{0};
        size_t allocCalls{0};
        size_t freeCalls{0};
        size_t arenaCreates{0};
        size_t arenaReclaims{0};
        size_t mallocFallbackAllocs{0};
        size_t mallocFallbackFrees{0};
    };

    explicit BuddyAllocator(const Options& opt);
    ~BuddyAllocator();

    void* Allocate(size_t size);
    void Deallocate(void* ptr, size_t size);

    Stats GetStats() const;

private:
    struct AllocationMeta {
        size_t arenaIndex{0};
        uint32_t order{0};
        size_t sizeRounded{0};
    };

    struct Arena {
        void* base{nullptr};
        size_t bytes{0};
        uint64_t lastTouch{0};
        size_t inUseBytes{0};
        size_t inUseBlocks{0};
        // freeSets[order] holds offsets from base (in bytes) of free blocks at that order.
        std::vector<std::unordered_set<size_t>> freeSets;
    };

    static bool IsPowerOfTwo(size_t x);
    static size_t NextPowerOfTwo(size_t x);
    static size_t RoundUpPow2(size_t x, size_t minPow2);

    uint32_t MaxOrder() const;
    size_t OrderToSize(uint32_t order) const;
    uint32_t SizeToOrder(size_t size) const;

    std::optional<size_t> FindArenaForPtrLocked(const void* p) const;
    Arena* CreateArenaLocked();
    void MaybeReclaimIdleLocked();
    void ReclaimOneIdleLocked();

    void* AllocateFromArenaLocked(size_t arenaIndex, size_t sizeRounded, uint32_t wantOrder);
    void FreeToArenaLocked(size_t arenaIndex, size_t offset, uint32_t order, size_t sizeRounded);

    Options opt_;
    size_t minBlockBytes_{0};
    size_t arenaSizeBytes_{0};

    mutable std::mutex mu_;
    uint64_t tick_{0};
    std::vector<Arena> arenas_;
    std::unordered_map<void*, AllocationMeta> allocs_;

    std::atomic<size_t> allocCalls_{0};
    std::atomic<size_t> freeCalls_{0};
    std::atomic<size_t> arenaCreates_{0};
    std::atomic<size_t> arenaReclaims_{0};
    std::atomic<size_t> mallocFallbackAllocs_{0};
    std::atomic<size_t> mallocFallbackFrees_{0};
};

} // namespace common
} // namespace proxy

