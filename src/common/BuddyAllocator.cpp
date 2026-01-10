#include "proxy/common/BuddyAllocator.h"
#include "proxy/common/Logger.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace proxy {
namespace common {

bool BuddyAllocator::IsPowerOfTwo(size_t x) {
    return x != 0 && ((x & (x - 1)) == 0);
}

size_t BuddyAllocator::NextPowerOfTwo(size_t x) {
    if (x <= 1) return 1;
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

size_t BuddyAllocator::RoundUpPow2(size_t x, size_t minPow2) {
    size_t y = NextPowerOfTwo(x);
    if (y < minPow2) y = minPow2;
    return y;
}

BuddyAllocator::BuddyAllocator(const Options& opt) : opt_(opt) {
    minBlockBytes_ = opt_.minBlockBytes;
    arenaSizeBytes_ = opt_.arenaSizeBytes;
    if (minBlockBytes_ < 1024) minBlockBytes_ = 1024;
    if (!IsPowerOfTwo(minBlockBytes_)) minBlockBytes_ = NextPowerOfTwo(minBlockBytes_);
    if (arenaSizeBytes_ < minBlockBytes_) arenaSizeBytes_ = minBlockBytes_;
    if (!IsPowerOfTwo(arenaSizeBytes_)) arenaSizeBytes_ = NextPowerOfTwo(arenaSizeBytes_);
    if (opt_.maxArenas == 0) opt_.maxArenas = 1;
}

BuddyAllocator::~BuddyAllocator() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& a : arenas_) {
        if (a.base) std::free(a.base);
        a.base = nullptr;
    }
    arenas_.clear();
    allocs_.clear();
}

uint32_t BuddyAllocator::MaxOrder() const {
    // order 0 => minBlockBytes
    uint32_t o = 0;
    size_t s = minBlockBytes_;
    while (s < arenaSizeBytes_) {
        s <<= 1;
        o++;
    }
    return o;
}

size_t BuddyAllocator::OrderToSize(uint32_t order) const {
    return minBlockBytes_ << order;
}

uint32_t BuddyAllocator::SizeToOrder(size_t size) const {
    size_t s = minBlockBytes_;
    uint32_t o = 0;
    while (s < size) {
        s <<= 1;
        o++;
    }
    return o;
}

std::optional<size_t> BuddyAllocator::FindArenaForPtrLocked(const void* p) const {
    const auto pc = reinterpret_cast<const char*>(p);
    for (size_t i = 0; i < arenas_.size(); ++i) {
        const auto* base = reinterpret_cast<const char*>(arenas_[i].base);
        if (!base) continue;
        if (pc >= base && pc < base + static_cast<std::ptrdiff_t>(arenas_[i].bytes)) return i;
    }
    return std::nullopt;
}

BuddyAllocator::Arena* BuddyAllocator::CreateArenaLocked() {
    if (arenas_.size() >= opt_.maxArenas) return nullptr;
    void* mem = nullptr;
    // Best-effort alignment to arena size.
    const size_t align = std::max<size_t>(4096, arenaSizeBytes_);
    if (::posix_memalign(&mem, align, arenaSizeBytes_) != 0) mem = nullptr;
    if (!mem) mem = std::malloc(arenaSizeBytes_);
    if (!mem) return nullptr;

    Arena a;
    a.base = mem;
    a.bytes = arenaSizeBytes_;
    a.lastTouch = ++tick_;
    a.inUseBytes = 0;
    a.inUseBlocks = 0;
    a.freeSets.assign(static_cast<size_t>(MaxOrder()) + 1, {});
    a.freeSets.back().insert(0); // whole arena free at max order
    arenas_.push_back(std::move(a));
    arenaCreates_.fetch_add(1, std::memory_order_relaxed);
    return &arenas_.back();
}

void BuddyAllocator::MaybeReclaimIdleLocked() {
    if (opt_.keepArenas > opt_.maxArenas) opt_.keepArenas = opt_.maxArenas;
    // Count idle arenas: fully free and no outstanding blocks.
    size_t idle = 0;
    for (const auto& a : arenas_) {
        if (!a.base) continue;
        if (a.inUseBlocks == 0 && a.freeSets.size() == static_cast<size_t>(MaxOrder()) + 1 && a.freeSets.back().count(0) == 1) {
            idle++;
        }
    }
    while (idle > opt_.keepArenas) {
        ReclaimOneIdleLocked();
        idle--;
    }
}

void BuddyAllocator::ReclaimOneIdleLocked() {
    size_t best = static_cast<size_t>(-1);
    uint64_t bestTick = 0;
    for (size_t i = 0; i < arenas_.size(); ++i) {
        const auto& a = arenas_[i];
        if (!a.base) continue;
        if (a.inUseBlocks != 0) continue;
        if (a.freeSets.empty()) continue;
        if (a.freeSets.back().count(0) != 1) continue;
        if (best == static_cast<size_t>(-1) || a.lastTouch < bestTick) {
            best = i;
            bestTick = a.lastTouch;
        }
    }
    if (best == static_cast<size_t>(-1)) return;
    std::free(arenas_[best].base);
    arenas_[best].base = nullptr;
    arenas_[best].bytes = 0;
    arenas_[best].freeSets.clear();
    arenas_[best].inUseBytes = 0;
    arenas_[best].inUseBlocks = 0;
    arenas_[best].lastTouch = ++tick_;
    arenaReclaims_.fetch_add(1, std::memory_order_relaxed);
}

void* BuddyAllocator::AllocateFromArenaLocked(size_t arenaIndex, size_t sizeRounded, uint32_t wantOrder) {
    Arena& a = arenas_[arenaIndex];
    const uint32_t maxO = MaxOrder();
    for (uint32_t o = wantOrder; o <= maxO; ++o) {
        auto& set = a.freeSets[static_cast<size_t>(o)];
        if (set.empty()) continue;
        // Take one block.
        const size_t offset = *set.begin();
        set.erase(set.begin());

        // Split down to wantOrder.
        size_t curOff = offset;
        uint32_t curO = o;
        while (curO > wantOrder) {
            curO--;
            const size_t half = OrderToSize(curO);
            const size_t buddyOff = curOff + half;
            a.freeSets[static_cast<size_t>(curO)].insert(buddyOff);
        }

        void* p = reinterpret_cast<char*>(a.base) + static_cast<std::ptrdiff_t>(curOff);
        AllocationMeta meta;
        meta.arenaIndex = arenaIndex;
        meta.order = wantOrder;
        meta.sizeRounded = sizeRounded;
        allocs_[p] = meta;
        a.inUseBlocks += 1;
        a.inUseBytes += sizeRounded;
        a.lastTouch = ++tick_;
        return p;
    }
    return nullptr;
}

void BuddyAllocator::FreeToArenaLocked(size_t arenaIndex, size_t offset, uint32_t order, size_t sizeRounded) {
    Arena& a = arenas_[arenaIndex];
    const uint32_t maxO = MaxOrder();
    uint32_t o = order;
    size_t off = offset;
    while (o < maxO) {
        const size_t blockSize = OrderToSize(o);
        const size_t buddyOff = off ^ blockSize;
        auto& set = a.freeSets[static_cast<size_t>(o)];
        auto it = set.find(buddyOff);
        if (it == set.end()) break;
        // merge with buddy
        set.erase(it);
        off = std::min(off, buddyOff);
        o++;
    }
    a.freeSets[static_cast<size_t>(o)].insert(off);
    if (a.inUseBlocks > 0) a.inUseBlocks -= 1;
    if (a.inUseBytes >= sizeRounded) a.inUseBytes -= sizeRounded;
    a.lastTouch = ++tick_;
}

void* BuddyAllocator::Allocate(size_t size) {
    if (size == 0) size = 1;
    allocCalls_.fetch_add(1, std::memory_order_relaxed);

    // We only handle sizes up to arena size.
    const size_t rounded = RoundUpPow2(size, minBlockBytes_);
    if (rounded > arenaSizeBytes_) {
        mallocFallbackAllocs_.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(size);
    }
    const uint32_t wantOrder = SizeToOrder(rounded);

    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < arenas_.size(); ++i) {
        if (!arenas_[i].base) continue;
        void* p = AllocateFromArenaLocked(i, rounded, wantOrder);
        if (p) return p;
    }

    // Need a new arena.
    Arena* a = CreateArenaLocked();
    if (!a) {
        mallocFallbackAllocs_.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(size);
    }
    const size_t idx = arenas_.size() - 1;
    void* p = AllocateFromArenaLocked(idx, rounded, wantOrder);
    if (!p) {
        // should not happen; fallback
        mallocFallbackAllocs_.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(size);
    }
    return p;
}

void BuddyAllocator::Deallocate(void* ptr, size_t size) {
    if (!ptr) return;
    freeCalls_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mu_);
    auto it = allocs_.find(ptr);
    if (it == allocs_.end()) {
        // Fallback: not from buddy (or metadata lost). Use free.
        mallocFallbackFrees_.fetch_add(1, std::memory_order_relaxed);
        std::free(ptr);
        return;
    }
    const AllocationMeta meta = it->second;
    allocs_.erase(it);
    if (meta.arenaIndex >= arenas_.size() || !arenas_[meta.arenaIndex].base) {
        mallocFallbackFrees_.fetch_add(1, std::memory_order_relaxed);
        std::free(ptr);
        return;
    }
    const auto base = reinterpret_cast<const char*>(arenas_[meta.arenaIndex].base);
    const auto pc = reinterpret_cast<const char*>(ptr);
    const size_t off = static_cast<size_t>(pc - base);
    FreeToArenaLocked(meta.arenaIndex, off, meta.order, meta.sizeRounded);
    (void)size; // size is advisory; meta.sizeRounded is authoritative.

    // LRU reclaim for fully-free arenas.
    MaybeReclaimIdleLocked();
}

BuddyAllocator::Stats BuddyAllocator::GetStats() const {
    Stats s;
    s.minBlockBytes = minBlockBytes_;
    s.arenaSizeBytes = arenaSizeBytes_;
    s.allocCalls = allocCalls_.load(std::memory_order_relaxed);
    s.freeCalls = freeCalls_.load(std::memory_order_relaxed);
    s.arenaCreates = arenaCreates_.load(std::memory_order_relaxed);
    s.arenaReclaims = arenaReclaims_.load(std::memory_order_relaxed);
    s.mallocFallbackAllocs = mallocFallbackAllocs_.load(std::memory_order_relaxed);
    s.mallocFallbackFrees = mallocFallbackFrees_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mu_);
    s.arenasTotal = 0;
    s.arenasIdle = 0;
    s.reservedBytes = 0;
    s.inUseBytes = 0;
    for (const auto& a : arenas_) {
        if (!a.base) continue;
        s.arenasTotal += 1;
        s.reservedBytes += a.bytes;
        s.inUseBytes += a.inUseBytes;
        if (a.inUseBlocks == 0 && !a.freeSets.empty() && a.freeSets.back().count(0) == 1) s.arenasIdle += 1;
    }
    s.freeBytes = (s.reservedBytes >= s.inUseBytes) ? (s.reservedBytes - s.inUseBytes) : 0;
    return s;
}

} // namespace common
} // namespace proxy
