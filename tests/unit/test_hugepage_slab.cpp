#include "proxy/common/SlabAllocator.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <vector>

using proxy::common::Logger;
using proxy::common::SlabAllocator;

int main() {
    Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    SlabAllocator::Options opt;
    opt.adviseHugepage = true;
    opt.chunkSizeBytes = 2 * 1024 * 1024;

    SlabAllocator slab(256, 1, opt);

    std::vector<void*> ptrs;
    for (int i = 0; i < 1000; ++i) {
        void* p = slab.Allocate(128);
        assert(p != nullptr);
        ptrs.push_back(p);
    }
    for (void* p : ptrs) slab.Deallocate(p);

    auto st = slab.GetStats();
    assert(st.blockSize >= 256);
    assert(st.chunkSizeBytes >= 2 * 1024 * 1024);
    assert(st.allocCalls >= 1000);
    assert(st.freeCalls >= 1000);
    return 0;
}

