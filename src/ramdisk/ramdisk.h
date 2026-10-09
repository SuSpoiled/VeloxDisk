#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../cache/tier.h"
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vd {

class RamDisk : public ITier {
public:
    RamDisk() = default;
    ~RamDisk();
    RamDisk(const RamDisk&) = delete;
    RamDisk& operator=(const RamDisk&) = delete;

    bool start(size_t capacityBytes, size_t blockBytes);
    void stop();

    const char* name() const override { return "ramdisk"; }
    size_t blockBytes() const override { return m_blockBytes; }
    size_t capacityBlocks() const override { return m_capacityBlocks; }
    size_t freeBlocks() const override;
    size_t dirtyCount() const override { return 0; }

    BlockState getBlock(const BlockKey& key, void* outBuf) override;

    bool putBlock(const BlockKey& key, const void* buf, bool dirty) override;
    bool contains(const BlockKey& key) const override;
    void markDirty(const BlockKey& key) override {}
    void flushBlock(const BlockKey& key) override {}
    bool trimBlock(const BlockKey& key) override;
    BlockState stateOf(const BlockKey& key) const override {
        return contains(key) ? BlockState::ReadCached : BlockState::Free;
    }
    void removeBlock(const BlockKey& key) override;
    void forEachDirty(const std::function<void(const BlockKey&)>& fn) const override
    { (void)fn; }
    void clear() override;

    bool readRange(uint64_t offset, void* buf, size_t len) const;
    bool writeRange(uint64_t offset, const void* buf, size_t len);

    bool readPartial(uint64_t offset, void* buf, size_t len) const;

    size_t usedBytes() const;
    size_t committedBytes() const;

private:
    struct SlotMeta {
        BlockKey key{};
        bool used = false;
    };

    void stop_unlocked_locked();
    bool commitSlab(size_t slabIndex);
    void freeSlot(int slot);
    int acquireSlot();
    uint8_t* slotPtr(int slot) const;

    mutable std::mutex m_mtx;
    uint8_t* m_base = nullptr;
    size_t m_capacityBytes = 0;
    size_t m_capacityBlocks = 0;
    size_t m_blockBytes = 0;
    size_t m_slabBytes = 0;
    size_t m_slabCount = 0;
    std::vector<bool> m_slabCommitted;
    size_t m_committedBytes = 0;
    std::vector<SlotMeta> m_slots;
    std::unordered_map<BlockKey, int, BlockKeyHash> m_keyToSlot;
    std::vector<int> m_freeSlots;
};

}
