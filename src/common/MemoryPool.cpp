#include "proxy/common/MemoryPool.h"
#include "proxy/common/Logger.h"
#include "proxy/common/Config.h"

namespace proxy {
namespace common {

MemoryPool& MemoryPool::Instance() {
    static MemoryPool instance;
    return instance;
}

MemoryPool::MemoryPool() {
    auto& conf = proxy::common::Config::Instance();
    const bool huge = conf.GetInt("memory", "hugepage", 0) != 0;
    int chunkKb = conf.GetInt("memory", "slab_chunk_kb", huge ? 2048 : 64);
    if (chunkKb <= 0) chunkKb = huge ? 2048 : 64;
    SlabAllocator::Options opt;
    opt.chunkSizeBytes = static_cast<size_t>(chunkKb) * 1024;
    opt.adviseHugepage = huge;

    // Define block sizes: 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB, 8KB, 16KB, 32KB, 64KB
    size_t sizes[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    
    for (size_t s : sizes) {
        allocators_.push_back(std::make_unique<SlabAllocator>(s, 1, opt));
        sizeMap_[s] = allocators_.size() - 1;
    }
    maxSlabSize_ = sizes[sizeof(sizes) / sizeof(sizes[0]) - 1];

    if (huge) {
        LOG_INFO << "MemoryPool hugepage mode enabled (best-effort): slab_chunk_kb=" << chunkKb;
    }

    // Buddy allocator for large blocks (LRU reclaim on idle arenas).
    const int buddyEnable = conf.GetInt("memory", "buddy_enable", 1);
    if (buddyEnable != 0) {
        BuddyAllocator::Options bo;
        int minKb = conf.GetInt("memory", "buddy_min_kb", 128);
        int arenaKb = conf.GetInt("memory", "buddy_arena_kb", 8192);
        int keepArenas = conf.GetInt("memory", "buddy_keep_arenas", 1);
        int maxArenas = conf.GetInt("memory", "buddy_max_arenas", 8);
        if (minKb <= 0) minKb = 128;
        if (arenaKb <= 0) arenaKb = 8192;
        if (keepArenas < 0) keepArenas = 0;
        if (maxArenas <= 0) maxArenas = 1;
        bo.minBlockBytes = static_cast<size_t>(minKb) * 1024;
        bo.arenaSizeBytes = static_cast<size_t>(arenaKb) * 1024;
        bo.keepArenas = static_cast<size_t>(keepArenas);
        bo.maxArenas = static_cast<size_t>(maxArenas);
        buddyArenaSizeBytes_ = bo.arenaSizeBytes;
        buddy_ = std::make_unique<BuddyAllocator>(bo);
        LOG_INFO << "MemoryPool buddy enabled: min_kb=" << minKb << " arena_kb=" << arenaKb
                 << " keep_arenas=" << keepArenas << " max_arenas=" << maxArenas;
    }
}

void* MemoryPool::Allocate(size_t size) {
    auto it = sizeMap_.lower_bound(size);
    if (it != sizeMap_.end()) {
        return allocators_[it->second]->Allocate(size);
    }

    if (buddy_ && size > maxSlabSize_ && size <= buddyArenaSizeBytes_) {
        void* p = buddy_->Allocate(size);
        if (p) return p;
    }

    // Fallback for very large allocations or OOM in buddy.
    LOG_WARN << "MemoryPool: Large allocation " << size << " bytes, using malloc directly";
    mallocAllocs_.fetch_add(1, std::memory_order_relaxed);
    mallocInUseBytes_.fetch_add(size, std::memory_order_relaxed);
    return ::malloc(size);
}

void MemoryPool::Deallocate(void* ptr, size_t size) {
    if (!ptr) return;
    
    auto it = sizeMap_.lower_bound(size);
    if (it != sizeMap_.end()) {
        allocators_[it->second]->Deallocate(ptr);
        return;
    }

    if (buddy_ && size > maxSlabSize_ && size <= buddyArenaSizeBytes_) {
        buddy_->Deallocate(ptr, size);
        return;
    }

    mallocFrees_.fetch_add(1, std::memory_order_relaxed);
    mallocInUseBytes_.fetch_sub(size, std::memory_order_relaxed);
    ::free(ptr);
}

MemoryPool::Stats MemoryPool::GetStats() const {
    Stats s;
    s.slabs.reserve(allocators_.size());

    for (const auto& allocator : allocators_) {
        auto st = allocator->GetStats();
        s.slabReservedBytes += st.reservedBytes;
        s.slabInUseBytes += st.inUseSlots * st.blockSize;
        s.slabFreeBytes += st.freeSlots * st.blockSize;
        s.slabs.push_back(st);
    }
    s.mallocInUseBytes = mallocInUseBytes_.load(std::memory_order_relaxed);
    s.mallocAllocs = mallocAllocs_.load(std::memory_order_relaxed);
    s.mallocFrees = mallocFrees_.load(std::memory_order_relaxed);
    if (buddy_) {
        s.buddy = buddy_->GetStats();
        s.buddyReservedBytes = s.buddy.reservedBytes;
        s.buddyInUseBytes = s.buddy.inUseBytes;
        s.buddyFreeBytes = s.buddy.freeBytes;
        s.buddyArenas = s.buddy.arenasTotal;
        s.buddyIdleArenas = s.buddy.arenasIdle;
        s.buddyArenaReclaims = s.buddy.arenaReclaims;
    }
    s.totalInUseBytes = s.slabInUseBytes + s.buddyInUseBytes + s.mallocInUseBytes;
    return s;
}

} // namespace common
} // namespace proxy
