#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace vd {

struct BlockKey {
    uint64_t targetId = 0;
    uint64_t blockIndex = 0;

    bool operator==(const BlockKey& o) const {
        return targetId == o.targetId && blockIndex == o.blockIndex;
    }
    bool operator!=(const BlockKey& o) const { return !(*this == o); }
    bool operator<(const BlockKey& o) const {
        if (targetId != o.targetId) return targetId < o.targetId;
        return blockIndex < o.blockIndex;
    }
};

struct BlockKeyHash {
    size_t operator()(const BlockKey& k) const {
        uint64_t h = k.targetId * 0x9E3779B97F4A7C15ull;
        h ^= k.blockIndex + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

uint64_t makeFileTargetId(const std::wstring& normalizedPath, uint32_t volumeSerial);

}
