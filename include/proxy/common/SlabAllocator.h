#pragma once

#include "proxy/common/Allocator.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <cstddef>

namespace proxy {
namespace common {

class SlabAllocator : public Allocator {
public:
    struct Options {
        size_t chunkSizeBytes{64 * 1024};
        bool adviseHugepage{false}; // best-effort MADV_HUGEPAGE + 2MB alignment/size
    };

    struct Stats {
        size_t blockSize{0};
        size_t chunkSizeBytes{0};
        size_t chunkCount{0};
        size_t slotsPerChunk{0};
        size_t totalSlots{0};
        size_t freeSlots{0};
        size_t inUseSlots{0};
        size_t reservedBytes{0};
        size_t allocCalls{0};
        size_t freeCalls{0};
    };

    explicit SlabAllocator(size_t blockSize, size_t initialChunks = 1);
    SlabAllocator(size_t blockSize, size_t initialChunks, const Options& opt);
    ~SlabAllocator() override;

    void* Allocate(size_t size) override;
    void Deallocate(void* ptr) override;
    size_t GetBlockSize() const override { return blockSize_; }
    Stats GetStats() const;

private:
    struct Slot {
        Slot* next;
    };

    void AllocateChunk();
    bool Owns(const void* ptr) const;

    size_t blockSize_;
    size_t chunkSizeBytes_{0};
    size_t slotsPerChunk_;
    Options opt_;
    
    std::mutex mutex_;
    Slot* freeList_;
    std::vector<void*> chunks_;

    std::atomic<size_t> chunkCount_{0};
    std::atomic<size_t> totalSlots_{0};
    std::atomic<size_t> freeSlots_{0};
    std::atomic<size_t> inUseSlots_{0};
    std::atomic<size_t> allocCalls_{0};
    std::atomic<size_t> freeCalls_{0};
};

} // namespace common
} // namespace proxy
