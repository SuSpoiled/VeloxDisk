#include "ramdisk.h"
#include <cstring>
#include <algorithm>

namespace vd {

namespace {
constexpr size_t kMinSlab = 1024 * 1024;
}

RamDisk::~RamDisk() { stop(); }

bool RamDisk::start(size_t capacityBytes, size_t blockBytes) {
    std::lock_guard<std::mutex> l(m_mtx);
    stop_unlocked_locked();
    m_blockBytes = (blockBytes >= 4096 && blockBytes <= 512 * 1024) ? blockBytes : 64 * 1024;
    m_capacityBlocks = capacityBytes / m_blockBytes;
    if (m_capacityBlocks == 0) return false;

    if (static_cast<unsigned long long>(m_capacityBlocks) * 32 > (1ull << 30)) return false;

    m_slabBytes = std::max(kMinSlab, m_blockBytes);

    m_slabBytes = ((m_slabBytes + m_blockBytes - 1) / m_blockBytes) * m_blockBytes;
    const size_t blocksPerSlab = m_slabBytes / m_blockBytes;
    m_slabCount = (m_capacityBlocks + blocksPerSlab - 1) / blocksPerSlab;

    m_capacityBytes = m_capacityBlocks * m_blockBytes;
    m_base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, m_capacityBytes, MEM_RESERVE, PAGE_READWRITE));
    if (!m_base) {
        m_slabCount = 0;
        return false;
    }
    m_slabCommitted.assign(m_slabCount, false);
    m_committedBytes = 0;

    m_slots.assign(m_capacityBlocks, SlotMeta{});
    m_keyToSlot.reserve(m_capacityBlocks);
    m_freeSlots.resize(m_capacityBlocks);
    for (size_t i = 0; i < m_capacityBlocks; ++i)
        m_freeSlots[i] = static_cast<int>(m_capacityBlocks - 1 - i);
    return true;
}

void RamDisk::stop() {
    std::lock_guard<std::mutex> l(m_mtx);
    stop_unlocked_locked();
}

void RamDisk::stop_unlocked_locked() {
    if (m_base) {

        VirtualFree(m_base, 0, MEM_RELEASE);
        m_base = nullptr;
    }
    m_slots.clear();
    m_keyToSlot.clear();
    m_freeSlots.clear();
    m_slabCommitted.clear();
    m_capacityBytes = m_capacityBlocks = m_blockBytes = m_slabBytes = m_slabCount = 0;
    m_committedBytes = 0;
}

size_t RamDisk::freeBlocks() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_freeSlots.size();
}

size_t RamDisk::usedBytes() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return (m_capacityBlocks - m_freeSlots.size()) * m_blockBytes;
}

size_t RamDisk::committedBytes() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_committedBytes;
}

bool RamDisk::commitSlab(size_t slabIndex) {
    if (slabIndex >= m_slabCount || m_slabCommitted[slabIndex]) return true;
    size_t off = slabIndex * m_slabBytes;
    size_t size = std::min(m_slabBytes, m_capacityBytes - off);
    uint8_t* p = static_cast<uint8_t*>(
        VirtualAlloc(m_base + off, size, MEM_COMMIT, PAGE_READWRITE));
    if (p != m_base + off) return false;
    m_slabCommitted[slabIndex] = true;
    m_committedBytes += size;
    return true;
}

void RamDisk::freeSlot(int slot) {
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    auto it = m_keyToSlot.find(e.key);
    if (it != m_keyToSlot.end() && it->second == slot) m_keyToSlot.erase(it);
    e.used = false;
    e.key = BlockKey{};
    m_freeSlots.push_back(slot);
}

int RamDisk::acquireSlot() {
    if (m_freeSlots.empty()) return -1;
    int s = m_freeSlots.back();
    m_freeSlots.pop_back();
    return s;
}

uint8_t* RamDisk::slotPtr(int slot) const {
    return m_base + static_cast<size_t>(slot) * m_blockBytes;
}

BlockState RamDisk::getBlock(const BlockKey& key, void* outBuf) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end() || !m_slots[static_cast<size_t>(it->second)].used)
        return BlockState::Free;
    int slot = it->second;
    std::memcpy(outBuf, slotPtr(slot), m_blockBytes);
    return BlockState::ReadCached;
}

bool RamDisk::putBlock(const BlockKey& key, const void* buf, bool dirty) {
    (void)dirty;
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it != m_keyToSlot.end() && m_slots[static_cast<size_t>(it->second)].used) {
        std::memcpy(slotPtr(it->second), buf, m_blockBytes);
        return true;
    }
    int slot = acquireSlot();
    if (slot < 0) return false;
    const size_t slabIndex = static_cast<size_t>(slot) * m_blockBytes / m_slabBytes;
    if (!commitSlab(slabIndex)) {
        m_freeSlots.push_back(slot);
        return false;
    }
    SlotMeta& e = m_slots[static_cast<size_t>(slot)];
    e.used = true;
    e.key = key;
    std::memcpy(slotPtr(slot), buf, m_blockBytes);
    m_keyToSlot[key] = slot;
    return true;
}

bool RamDisk::contains(const BlockKey& key) const {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    return it != m_keyToSlot.end() && m_slots[static_cast<size_t>(it->second)].used;
}

bool RamDisk::trimBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end()) return false;
    int slot = it->second;
    if (!m_slots[static_cast<size_t>(slot)].used) return false;
    freeSlot(slot);
    return true;
}

void RamDisk::removeBlock(const BlockKey& key) { trimBlock(key); }

void RamDisk::clear() {
    std::lock_guard<std::mutex> l(m_mtx);

    for (size_t i = 0; i < m_slabCount; ++i) {
        if (!m_slabCommitted[i]) continue;
        size_t off = i * m_slabBytes;
        size_t size = std::min(m_slabBytes, m_capacityBytes - off);
        VirtualFree(m_base + off, 0, MEM_DECOMMIT);
        m_slabCommitted[i] = false;
    }
    m_committedBytes = 0;
    const size_t cap = m_capacityBlocks;
    m_keyToSlot.clear();
    m_freeSlots.clear();
    m_slots.assign(cap, SlotMeta{});
    m_freeSlots.resize(cap);
    for (size_t i = 0; i < cap; ++i)
        m_freeSlots[i] = static_cast<int>(cap - 1 - i);
}

bool RamDisk::readRange(uint64_t offset, void* buf, size_t len) const {
    if (offset % m_blockBytes != 0 || len % m_blockBytes != 0 || len == 0) return false;

    if (offset >= m_capacityBytes || len > m_capacityBytes - offset) return false;
    std::lock_guard<std::mutex> l(m_mtx);
    uint8_t* out = static_cast<uint8_t*>(buf);
    for (uint64_t off = 0; off + m_blockBytes <= len; off += m_blockBytes) {
        BlockKey key{0, (offset + off) / m_blockBytes};
        auto it = m_keyToSlot.find(key);
        if (it == m_keyToSlot.end() || !m_slots[static_cast<size_t>(it->second)].used)
            return false;
        std::memcpy(out + off, slotPtr(it->second), m_blockBytes);
    }
    return true;
}

bool RamDisk::writeRange(uint64_t offset, const void* buf, size_t len) {
    if (offset % m_blockBytes != 0 || len % m_blockBytes != 0 || len == 0) return false;
    if (offset >= m_capacityBytes || len > m_capacityBytes - offset) return false;
    const uint8_t* in = static_cast<const uint8_t*>(buf);
    const size_t blocks = len / m_blockBytes;

    {
        std::lock_guard<std::mutex> l(m_mtx);
        size_t need = 0;
        for (size_t i = 0; i < blocks; ++i) {
            BlockKey key{0, (offset + i * m_blockBytes) / m_blockBytes};
            auto it = m_keyToSlot.find(key);
            if (it == m_keyToSlot.end() || !m_slots[static_cast<size_t>(it->second)].used)
                ++need;
        }
        if (need > m_freeSlots.size()) return false;
    }
    for (size_t i = 0; i < blocks; ++i) {
        BlockKey key{0, (offset + i * m_blockBytes) / m_blockBytes};
        if (!putBlock(key, in + i * m_blockBytes, false)) return false;
    }
    return true;
}

bool RamDisk::readPartial(uint64_t offset, void* buf, size_t len) const {
    if (len == 0 || len > m_blockBytes || offset % m_blockBytes != 0) return false;
    if (offset >= m_capacityBytes || len > m_capacityBytes - offset) return false;
    std::lock_guard<std::mutex> l(m_mtx);
    BlockKey key{0, offset / m_blockBytes};
    auto it = m_keyToSlot.find(key);
    if (it == m_keyToSlot.end() || !m_slots[static_cast<size_t>(it->second)].used)
        return false;
    std::memcpy(buf, slotPtr(it->second), len);
    return true;
}

}
