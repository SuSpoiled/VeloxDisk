#pragma once
#include "tier.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>

namespace vd {

class SsdTier : public ITier {
public:
    SsdTier() = default;
    ~SsdTier() { stop(false); }

    bool start(const std::string& dir, const std::string& baseName,
               size_t capacityBytes, size_t blockBytes);
    void stop(bool clean);

    SsdTier(const SsdTier&) = delete;
    SsdTier& operator=(const SsdTier&) = delete;

    const char* name() const override { return "ssd"; }
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

    bool wasCleanShutdown() const { return m_wasClean; }

    int verifyAgainstSource(const std::function<bool(const BlockKey&, uint32_t& crc)>& readSource);

    void saveIndex();
    void journalDirty(const BlockKey& key);

    int replayJournal();

private:
    struct Entry {
        int slot = -1;
        BlockState state = BlockState::Free;
        uint32_t crc = 0;
    };

    bool readSlot(int slot, void* buf) const;
    bool writeSlot(int slot, const void* buf);
    int acquireSlot();
    int freeSlotByKey(const BlockKey& key);

    void* m_dataFile = nullptr;
    void* m_jrnFile = nullptr;
    std::string m_dir, m_baseName;
    std::string m_datPath, m_idxPath, m_jrnPath, m_flagPath;
    std::unordered_map<BlockKey, Entry, BlockKeyHash> m_entries;
    std::unordered_set<BlockKey, BlockKeyHash> m_dirty;
    std::set<BlockKey> m_evictable;
    std::vector<int> m_freeSlots;
    size_t m_blockBytes = 0;
    size_t m_capacityBlocks = 0;
    bool m_wasClean = false;
    mutable std::mutex m_mtx;
};

}
