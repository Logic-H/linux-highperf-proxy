#include "proxy/common/SlabAllocator.h"
#include "proxy/common/Logger.h"
#include <iostream>
#include <vector>

using namespace proxy::common;

int main() {
    Logger::Instance().SetLevel(LogLevel::DEBUG);
    LOG_INFO << "Starting SlabAllocator test";

    // Create an allocator for 128-byte blocks
    SlabAllocator allocator(128);

    LOG_INFO << "Block size: " << allocator.GetBlockSize();

    std::vector<void*> ptrs;
    
    // Allocate 100 blocks
    for (int i = 0; i < 100; ++i) {
        void* p = allocator.Allocate(100); // Request less than block size
        if (p) {
            ptrs.push_back(p);
            // Write some data to verify memory is usable
            int* intPtr = static_cast<int*>(p);
            *intPtr = i;
        } else {
            LOG_ERROR << "Allocation failed at index " << i;
        }
    }

    LOG_INFO << "Allocated " << ptrs.size() << " blocks";

    // Deallocate all
    for (void* p : ptrs) {
        allocator.Deallocate(p);
    }
    ptrs.clear();

    LOG_INFO << "Deallocated all blocks";

    // Verify re-allocation works
    void* p = allocator.Allocate(128);
    if (p) {
        LOG_INFO << "Re-allocation successful";
        allocator.Deallocate(p);
    }

    LOG_INFO << "SlabAllocator test passed";
    return 0;
}
