#include "proxy/common/SlabAllocator.h"
#include "proxy/common/Logger.h"
#include <cstdlib>
#include <cassert>
#include <algorithm>
#include <sys/mman.h>

namespace proxy {
namespace common {

static constexpr size_t kDefaultChunkSize = 64 * 1024;
static constexpr size_t kHugePageSize = 2 * 1024 * 1024;

static size_t RoundUp(size_t n, size_t align) {
    if (align == 0) return n;
    const size_t r = n % align;
    return r == 0 ? n : (n + (align - r));
}

SlabAllocator::SlabAllocator(size_t blockSize, size_t initialChunks)
    : SlabAllocator(blockSize, initialChunks, Options()) {
}

SlabAllocator::SlabAllocator(size_t blockSize, size_t initialChunks, const Options& opt)
    : blockSize_(blockSize),
      chunkSizeBytes_(opt.chunkSizeBytes ? opt.chunkSizeBytes : kDefaultChunkSize),
      opt_(opt),
      freeList_(nullptr) {
    
    // Ensure block size is at least sizeof(Slot) to store free list pointers
    if (blockSize_ < sizeof(Slot)) {
        blockSize_ = sizeof(Slot);
    }

    // Ensure chunk size fits at least one slot.
    if (chunkSizeBytes_ < blockSize_) {
        chunkSizeBytes_ = blockSize_;
    }
    if (opt_.adviseHugepage) {
        // Best-effort: make chunk size 2MB-aligned and at least 2MB.
        chunkSizeBytes_ = std::max(chunkSizeBytes_, kHugePageSize);
        chunkSizeBytes_ = RoundUp(chunkSizeBytes_, kHugePageSize);
    }
    
    // Calculate how many slots fit in a chunk
    slotsPerChunk_ = std::max<size_t>(1, chunkSizeBytes_ / blockSize_);
    
    for (size_t i = 0; i < initialChunks; ++i) {
        AllocateChunk();
    }
}

SlabAllocator::~SlabAllocator() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (void* chunk : chunks_) {
        std::free(chunk);
    }
    chunks_.clear();
}

void* SlabAllocator::Allocate(size_t size) {
    if (size > blockSize_) {
        LOG_FATAL << "SlabAllocator::Allocate requested size " << size << " > blockSize " << blockSize_;
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!freeList_) {
        AllocateChunk();
    }

    assert(freeList_);
    Slot* slot = freeList_;
    freeList_ = slot->next;
    freeSlots_.fetch_sub(1, std::memory_order_relaxed);
    inUseSlots_.fetch_add(1, std::memory_order_relaxed);
    allocCalls_.fetch_add(1, std::memory_order_relaxed);
    return static_cast<void*>(slot);
}

void SlabAllocator::Deallocate(void* ptr) {
    if (!ptr) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!Owns(ptr)) {
        // Safety net: should not happen for MemoryPool-managed allocations.
        LOG_WARN << "SlabAllocator::Deallocate ptr not owned by slab, falling back to free";
        std::free(ptr);
        return;
    }
    Slot* slot = static_cast<Slot*>(ptr);
    slot->next = freeList_;
    freeList_ = slot;
    freeSlots_.fetch_add(1, std::memory_order_relaxed);
    inUseSlots_.fetch_sub(1, std::memory_order_relaxed);
    freeCalls_.fetch_add(1, std::memory_order_relaxed);
}

void SlabAllocator::AllocateChunk() {
    void* chunk = nullptr;
    if (opt_.adviseHugepage) {
        // 2MB alignment helps THP/hugepage backing (best-effort).
        if (::posix_memalign(&chunk, kHugePageSize, chunkSizeBytes_) != 0) {
            chunk = nullptr;
        }
    }
    if (!chunk) {
        chunk = ::malloc(chunkSizeBytes_);
    }
    if (!chunk) {
        LOG_FATAL << "SlabAllocator::AllocateChunk failed";
    }
#ifdef MADV_HUGEPAGE
    if (opt_.adviseHugepage && chunkSizeBytes_ >= kHugePageSize) {
        // Best-effort; ignore errors.
        (void)::madvise(chunk, chunkSizeBytes_, MADV_HUGEPAGE);
    }
#endif
    chunks_.push_back(chunk);
    chunkCount_.fetch_add(1, std::memory_order_relaxed);
    totalSlots_.fetch_add(slotsPerChunk_, std::memory_order_relaxed);
    freeSlots_.fetch_add(slotsPerChunk_, std::memory_order_relaxed);

    // Slice chunk into slots and add to free list
    char* start = static_cast<char*>(chunk);
    for (size_t i = 0; i < slotsPerChunk_; ++i) {
        Slot* slot = reinterpret_cast<Slot*>(start + i * blockSize_);
        slot->next = freeList_;
        freeList_ = slot;
    }
    
    LOG_DEBUG << "SlabAllocator allocated new chunk, total slots: " << slotsPerChunk_ * chunks_.size();
}

bool SlabAllocator::Owns(const void* ptr) const {
    for (const void* chunk : chunks_) {
        const auto begin = reinterpret_cast<const char*>(chunk);
        const auto end = begin + chunkSizeBytes_;
        const auto p = reinterpret_cast<const char*>(ptr);
        if (p >= begin && p < end) return true;
    }
    return false;
}

SlabAllocator::Stats SlabAllocator::GetStats() const {
    Stats s;
    s.blockSize = blockSize_;
    s.chunkSizeBytes = chunkSizeBytes_;
    s.chunkCount = chunkCount_.load(std::memory_order_relaxed);
    s.slotsPerChunk = slotsPerChunk_;
    s.totalSlots = totalSlots_.load(std::memory_order_relaxed);
    s.freeSlots = freeSlots_.load(std::memory_order_relaxed);
    s.inUseSlots = inUseSlots_.load(std::memory_order_relaxed);
    s.reservedBytes = s.chunkCount * chunkSizeBytes_;
    s.allocCalls = allocCalls_.load(std::memory_order_relaxed);
    s.freeCalls = freeCalls_.load(std::memory_order_relaxed);
    return s;
}

} // namespace common
} // namespace proxy
