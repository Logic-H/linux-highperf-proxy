#include "proxy/common/BuddyAllocator.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <cstddef>
#include <vector>

using proxy::common::BuddyAllocator;

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    BuddyAllocator::Options opt;
    opt.minBlockBytes = 64;
    opt.arenaSizeBytes = 1024;
    opt.keepArenas = 0;
    opt.maxArenas = 2;
    BuddyAllocator b(opt);

    // Allocate small sizes -> rounded to power-of-two >= 64.
    void* p1 = b.Allocate(1);
    void* p2 = b.Allocate(60);
    void* p3 = b.Allocate(65); // -> 128
    assert(p1 && p2 && p3);
    assert(p1 != p2 && p1 != p3 && p2 != p3);

    auto st1 = b.GetStats();
    assert(st1.arenasTotal >= 1);
    assert(st1.inUseBytes >= 64 + 64 + 128);

    // Free and ensure merge can recreate a larger block.
    b.Deallocate(p1, 1);
    b.Deallocate(p2, 60);
    b.Deallocate(p3, 65);
    auto st2 = b.GetStats();
    assert(st2.inUseBytes == 0);

    // After freeing everything, keepArenas=0 => arena may be reclaimed.
    // Allocate again to ensure allocator still works.
    void* p4 = b.Allocate(512);
    assert(p4);
    b.Deallocate(p4, 512);

    // LRU reclaim should keep idle arenas <= keepArenas (0).
    auto st3 = b.GetStats();
    assert(st3.arenasIdle <= st3.arenasTotal);
    assert(st3.arenasIdle == 0);
    return 0;
}

