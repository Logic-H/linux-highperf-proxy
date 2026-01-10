#include "proxy/common/MemoryPool.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <cstddef>

using proxy::common::Logger;
using proxy::common::MemoryPool;

int main() {
    Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    auto& pool = MemoryPool::Instance();

    const auto before = pool.GetStats();

    // Slab path
    void* p = pool.Allocate(1);
    const auto afterAlloc = pool.GetStats();
    assert(afterAlloc.totalInUseBytes >= before.totalInUseBytes + 64);

    pool.Deallocate(p, 1);
    const auto afterFree = pool.GetStats();
    assert(afterFree.totalInUseBytes == before.totalInUseBytes);

    // buddy path (large but within buddy arena)
    const size_t big = 200000;
    void* p2 = pool.Allocate(big);
    const auto afterBuddy = pool.GetStats();
    assert(afterBuddy.totalInUseBytes >= before.totalInUseBytes + big);
    assert(afterBuddy.mallocInUseBytes == before.mallocInUseBytes);

    pool.Deallocate(p2, big);
    const auto afterBuddyFree = pool.GetStats();
    assert(afterBuddyFree.totalInUseBytes == before.totalInUseBytes);
    assert(afterBuddyFree.mallocInUseBytes == before.mallocInUseBytes);

    // malloc fallback path (beyond buddy arena default 8MB)
    const size_t huge = 16 * 1024 * 1024;
    void* p3 = pool.Allocate(huge);
    const auto afterMalloc = pool.GetStats();
    assert(afterMalloc.mallocInUseBytes == before.mallocInUseBytes + huge);

    pool.Deallocate(p3, huge);
    const auto afterMallocFree = pool.GetStats();
    assert(afterMallocFree.mallocInUseBytes == before.mallocInUseBytes);

    return 0;
}
