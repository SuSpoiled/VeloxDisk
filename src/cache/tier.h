#pragma once
#include "key.h"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace vd {

enum class BlockState : uint8_t {
    Free = 0,
    ReadCached,
    Dirty
};

class ITier {
public:
    virtual ~ITier() = default;

    virtual const char* name() const = 0;
    virtual size_t blockBytes() const = 0;
    virtual size_t capacityBlocks() const = 0;
    virtual size_t freeBlocks() const = 0;
    virtual size_t dirtyCount() const = 0;

    virtual BlockState getBlock(const BlockKey& key, void* outBuf) = 0;

    virtual bool putBlock(const BlockKey& key, const void* buf, bool dirty) = 0;

    virtual bool contains(const BlockKey& key) const = 0;
    virtual void markDirty(const BlockKey& key) = 0;
    virtual void flushBlock(const BlockKey& key) = 0;

    virtual bool trimBlock(const BlockKey& key) = 0;
    virtual BlockState stateOf(const BlockKey& key) const = 0;
    virtual void removeBlock(const BlockKey& key) = 0;

    virtual void forEachDirty(const std::function<void(const BlockKey&)>& fn) const = 0;

    virtual void clear() = 0;
};

}
