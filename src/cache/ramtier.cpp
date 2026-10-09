#include "ramtier.h"
#include <windows.h>
#include <cstring>
#include <algorithm>

namespace vd {

namespace {
constexpr int kHybridScanWindow = 64;
}

bool RamTier::start(size_t capacityBytes, size_t blockBytes, bool numaAware) {
    std::lock_guard<std::mutex> l(m_mtx);
    stop_unlocked();
    m_blockBytes = (blockBytes >= 4096 && blockBytes <= 512 * 1024) ? blockBytes : 64 * 1024;
    m_capacityBlocks = capacityBytes / m_blockBytes;
    if (m_capacityBlocks == 0) return false;

    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && capacityBytes > ms.ullTotalPhys * 3 / 4)
        return false;

    std::vector<MemRegion> regions;
    if (!NumaAlloc::allocate(m_capacityBlocks * m_blockBytes, numaAware, regions)) return false;
    m_regions = std::move(regions);

    m_poolPerSlot.clear();
    m_poolPerSlot.reserve(m_capacityBlocks);
    size_t acc = 0;
    for (auto& r : m_regions) {
        size_t n = r.size / m_blockBytes;
        for (size_t i = 0; i < n; ++i)
            m_poolPerSlot.push_back(static_cast<uint8_t*>(r.base) + i * m_blockBytes);
        acc += n;
    }
    if (acc < m_capacityBlocks) { stop_unlocked(); return false; }

    m_slots.assign(m_capacityBlocks, SlotMeta{});
    m_keyToSlot.reserve(m_capacityBlocks);
    m_freeSlots.resize(m_capacityBlocks);
    for (size_t i = 0; i < m_capacityBlocks; ++i)
        m_freeSlots[i] = static_cast<int>(m_capacityBlocks - 1 - i);
    m_dirty.clear();
    m_lruHead = m_lruTail = -1;
    m_seqCounter = 0;
    return true;
}

void RamTier::stop() {
    std::lock_guard<std::mutex> l(m_mtx);
    stop_unlocked();
}

void RamTier::stop_unlocked() {
    m_slots.clear();
    m_keyToSlot.clear();
    m_freeSlots.clear();
    m_dirty.clear();
    NumaAlloc::freeAll(m_regions);
    m_regions.clear();
    m_poolPerSlot.clear();
    m_capacityBlocks = 0;
    m_lruHead = m_lruTail = -1;
}

size_t RamTier::freeBlocks() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_freeSlots.size();
}

size_t RamTier::dirtyCount() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_dirty.size();
}

void RamTier::lruUnlink(int slot) {
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    if (e.lruPrev >= 0) m_slots[static_cast<size_t>(e.lruPrev)].lruNext = e.lruNext;
    else m_lruHead = e.lruNext;
    if (e.lruNext >= 0) m_slots[static_cast<size_t>(e.lruNext)].lruPrev = e.lruPrev;
    else m_lruTail = e.lruPrev;
    e.lruPrev = e.lruNext = -1;
}

void RamTier::lruPushFront(int slot) {
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    e.lruPrev = -1;
    e.lruNext = m_lruHead;
    if (m_lruHead >= 0) m_slots[static_cast<size_t>(m_lruHead)].lruPrev = slot;
    m_lruHead = slot;
    if (m_lruTail < 0) m_lruTail = slot;
}

void RamTier::freeSlot(int slot) {
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    auto it = m_keyToSlot.find(e.key);
    if (it != m_keyToSlot.end() && it->second == slot) m_keyToSlot.erase(it);
    if (e.state == BlockState::ReadCached) lruUnlink(slot);
    e.used = false;
    e.state = BlockState::Free;
    e.hits = 0;
    e.seq = 0;
    e.key = BlockKey{};
    m_freeSlots.push_back(slot);
}

int RamTier::evictOne() {
    if (m_lruTail < 0) return -1;

    int victim = -1;
    int cur = m_lruTail;
    int scanned = 0;
    while (cur >= 0 && scanned < kHybridScanWindow) {
        const SlotMeta& e = m_slots[static_cast<size_t>(cur)];
        if (victim < 0) {
            victim = cur;
        } else {
            const SlotMeta& v = m_slots[static_cast<size_t>(victim)];
            if (e.hits < v.hits) victim = cur;
            else if (e.hits == v.hits && e.seq < v.seq) victim = cur;
        }
        cur = e.lruPrev;
        ++scanned;
    }
    if (victim >= 0) {
        lruUnlink(victim);
        SlotMeta& e = m_slots[static_cast<size_t>(victim)];
        auto it = m_keyToSlot.find(e.key);
        if (it != m_keyToSlot.end() && it->second == victim) m_keyToSlot.erase(it);
        e.used = false;
        e.state = BlockState::Free;
        e.key = BlockKey{};
    }
    return victim;
}

int RamTier::acquireSlot() {
    if (!m_freeSlots.empty()) {
        int s = m_freeSlots.back();
        m_freeSlots.pop_back();
        return s;
    }
    return evictOne();
}

BlockState RamTier::getBlock(const BlockKey& key, void* outBuf) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end() || !m_slots[static_cast<size_t>(it->second)].used)
        return BlockState::Free;
    int slot = it->second;
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    std::memcpy(outBuf, slotPtr(slot), m_blockBytes);
    if (e.state == BlockState::ReadCached) {
        lruUnlink(slot);
        lruPushFront(slot);
        ++e.hits;
    }
    return e.state;
}

bool RamTier::putBlock(const BlockKey& key, const void* buf, bool dirty) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it != m_keyToSlot.end() && m_slots[static_cast<size_t>(it->second)].used) {
        int slot = it->second;
        SlotMeta& e = m_slots[static_cast<size_t>(slot)];
        std::memcpy(slotPtr(slot), buf, m_blockBytes);
        if (dirty && e.state != BlockState::Dirty) {
            if (e.state == BlockState::ReadCached) lruUnlink(slot);
            e.state = BlockState::Dirty;
            m_dirty.insert(slot);
            ++e.hits;
        } else if (e.state == BlockState::Dirty) {

        } else if (e.state == BlockState::ReadCached) {
            lruUnlink(slot);
            lruPushFront(slot);
            ++e.hits;
        }
        return true;
    }
    int slot = acquireSlot();
    if (slot < 0) return false;
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    e.used = true;
    e.key = key;
    e.hits = 1;
    e.seq = ++m_seqCounter;
    std::memcpy(slotPtr(slot), buf, m_blockBytes);
    m_keyToSlot[key] = slot;
    if (dirty) {
        e.state = BlockState::Dirty;
        m_dirty.insert(slot);
    } else {
        e.state = BlockState::ReadCached;
        lruPushFront(slot);
    }
    return true;
}

bool RamTier::contains(const BlockKey& key) const {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    return it != m_keyToSlot.end() && m_slots[static_cast<size_t>(it->second)].used;
}

void RamTier::markDirty(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end()) return;
    int slot = it->second;
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    if (!e.used) return;
    if (e.state == BlockState::ReadCached) lruUnlink(slot);
    e.state = BlockState::Dirty;
    m_dirty.insert(slot);
}

void RamTier::flushBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end()) return;
    int slot = it->second;
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    if (!e.used || e.state != BlockState::Dirty) return;
    m_dirty.erase(slot);
    e.state = BlockState::ReadCached;
    e.seq = ++m_seqCounter;
    lruPushFront(slot);
}

bool RamTier::trimBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end()) return false;
    int slot = it->second;
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    if (!e.used) return false;

    m_dirty.erase(slot);
    freeSlot(slot);
    return true;
}

BlockState RamTier::stateOf(const BlockKey& key) const {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end()) return BlockState::Free;
    const SlotMeta& e = m_slots[static_cast<size_t>(it->second)];
    return e.used ? e.state : BlockState::Free;
}

void RamTier::removeBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end()) return;
    int slot = it->second;
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    if (!e.used) return;
    m_dirty.erase(slot);
    freeSlot(slot);
}

void RamTier::forEachDirty(const std::function<void(const BlockKey&)>& fn) const {
    std::lock_guard<std::mutex> l(m_mtx);
    for (int slot : m_dirty) {
        const SlotMeta& e = m_slots[static_cast<size_t>(slot)];
        if (e.used && e.state == BlockState::Dirty) fn(e.key);
    }
}

void RamTier::clear() {
    std::lock_guard<std::mutex> l(m_mtx);

    size_t cap = m_capacityBlocks;
    m_keyToSlot.clear();
    m_dirty.clear();
    m_freeSlots.clear();
    m_freeSlots.resize(cap);
    m_slots.assign(cap, SlotMeta{});
    for (size_t i = 0; i < cap; ++i)
        m_freeSlots[i] = static_cast<int>(cap - 1 - i);
    m_lruHead = m_lruTail = -1;
}

void RamTier::forEachReadCached(
    const std::function<void(const BlockKey&, const uint8_t*, size_t)>& fn) const {
    std::lock_guard<std::mutex> l(m_mtx);
    for (size_t s = 0; s < m_slots.size(); ++s) {
        const SlotMeta& e = m_slots[s];
        if (e.used && e.state == BlockState::ReadCached)
            fn(e.key, slotPtr(static_cast<int>(s)), m_blockBytes);
    }
}

int RamTier::collectReadTop(int n,
                            const std::function<bool(const BlockKey&, const uint8_t*, size_t)>& writeOut) {
    std::lock_guard<std::mutex> l(m_mtx);
    struct C { BlockKey key; uint32_t hits; uint64_t seq; };
    std::vector<C> cands;
    cands.reserve(1024);
    for (size_t s = 0; s < m_slots.size(); ++s) {
        const SlotMeta& e = m_slots[s];
        if (e.used && e.state == BlockState::ReadCached)
            cands.push_back(C{e.key, e.hits, e.seq});
    }

    const size_t lim = std::min<size_t>(cands.size(), n > 0 ? static_cast<size_t>(n) : 0);
    std::partial_sort(cands.begin(), cands.begin() + lim, cands.end(),
                      [](const C& a, const C& b) {
        if (a.hits != b.hits) return a.hits > b.hits;
        return a.seq > b.seq;
    });
    int done = 0;
    for (size_t i = 0; i < lim; ++i) {
        const C& c = cands[i];
        auto it = m_keyToSlot.find(c.key);
        if (it == m_keyToSlot.end() || !m_slots[static_cast<size_t>(it->second)].used) continue;
        int slot = it->second;
        if (writeOut(c.key, slotPtr(slot), m_blockBytes)) {
            freeSlot(slot);
            ++done;
        }
    }
    return done;
}

}
