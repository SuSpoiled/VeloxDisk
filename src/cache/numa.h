#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vd {

struct MemRegion {
    void* base = nullptr;
    size_t size = 0;
    uint32_t node = 0;
};

class NumaAlloc {
public:

    static bool allocate(size_t bytes, bool numaAware, std::vector<MemRegion>& regions);

    static void freeAll(std::vector<MemRegion>& regions);
};

}
