#pragma once
#include "tier.h"
#include "numa.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>

namespace vd {

class RamTier : public ITier {
public:
    RamTier() = default;
    bool start(size_t capacityBytes, size_t blockBytes, bool numaAware);
    void stop();

    RamTier(const RamTier&) = delete;
    RamTier& operator=(const RamTier&) = delete;

    const char* name() const override { return "ram"; }
    size_t blockBytes() const override { return m_blockBytes; }
    size_t capacityBlocks() const override { return m_capacityBlocks; }
    size_t freeBlocks() const override;
    size_t dirtyCount() const override;

    BlockState getBlock(const BlockKey& key, void* outBuf) override;
    bool putBlock(const BlockKey& key, const void* buf, bool dirty) override;
    bool contains(const BlockKey& key) const override;
    void markDirty(const BlockKey& key) override;
    void flushBlock(const BlockKey& key) override;
    bool trimBlock(const BlockKey& key) override;
    BlockState stateOf(const BlockKey& key) const override;
    void removeBlock(const BlockKey& key) override;
    void forEachDirty(const std::function<void(const BlockKey&)>& fn) const override;
    void clear() override;

    int collectReadTop(int n, const std::function<bool(const BlockKey&, const uint8_t*, size_t)>& writeOut);

    void forEachReadCached(const std::function<void(const BlockKey&, const uint8_t*, size_t)>& fn) const;

private:
    struct SlotMeta {
        BlockKey key;
        bool used = false;
        BlockState state = BlockState::Free;
        uint32_t hits = 0;
        uint64_t seq = 0;
        int lruPrev = -1;
        int lruNext = -1;
    };

    uint8_t* slotPtr(int slot) const { return m_poolPerSlot[static_cast<size_t>(slot)]; }
    void lruUnlink(int slot);
    void lruPushFront(int slot);
    int evictOne();
    void freeSlot(int slot);
    int acquireSlot();
    void stop_unlocked();

    mutable std::mutex m_mtx;
    std::vector<MemRegion> m_regions;
    std::vector<uint8_t*> m_poolPerSlot;
    std::vector<SlotMeta> m_slots;
    std::unordered_map<BlockKey, int, BlockKeyHash> m_keyToSlot;
    std::vector<int> m_freeSlots;
    std::unordered_set<int> m_dirty;
    int m_lruHead = -1;
    int m_lruTail = -1;
    size_t m_blockBytes = 0;
    size_t m_capacityBlocks = 0;
    uint64_t m_seqCounter = 0;
};

}
