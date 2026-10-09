#include "ssdtier.h"
#include "../common/util.h"
#include <cstring>
#include <algorithm>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vd {

namespace {

constexpr size_t kRecordSize = 32;
constexpr uint32_t kIndexMagic = 0x56446932u;

HANDLE openFile(const std::string& path, DWORD dwCreate) {

    std::wstring w = toWide(path);
    return CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       dwCreate, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool seekAbs(HANDLE h, uint64_t off) {
    LONG hi = static_cast<LONG>(off >> 32);
    return SetFilePointer(h, static_cast<LONG>(off & 0xFFFFFFFF), &hi, FILE_BEGIN)
           != INVALID_SET_FILE_POINTER;
}

OVERLAPPED atOff(uint64_t off) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    return ov;
}

bool writeFileAtomic(const std::string& path, const std::vector<unsigned char>& data) {
    std::string tmp = path + ".tmp";
    if (!writeFile(tmp, data)) return false;
    std::wstring wt = toWide(tmp);
    std::wstring wp = toWide(path);
    return MoveFileExW(wt.c_str(), wp.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}

bool SsdTier::start(const std::string& dir, const std::string& baseName,
                    size_t capacityBytes, size_t blockBytes) {
    std::lock_guard<std::mutex> l(m_mtx);
    m_blockBytes = (blockBytes >= 4096 && blockBytes <= 512 * 1024) ? blockBytes : 64 * 1024;
    m_capacityBlocks = capacityBytes / m_blockBytes;
    if (m_capacityBlocks == 0) return false;
    m_dir = dir;
    m_baseName = baseName;
    m_datPath = dir + "/" + baseName + ".dat";
    m_idxPath = dir + "/" + baseName + ".idx";
    m_jrnPath = dir + "/" + baseName + ".jrn";
    m_flagPath = dir + "/" + baseName + ".flag";
    if (!dir.empty()) makeDirs(dir);

    m_dataFile = static_cast<void*>(openFile(m_datPath, OPEN_ALWAYS));
    if (m_dataFile == reinterpret_cast<void*>(-1)) return false;
    LARGE_INTEGER li;
    uint64_t want = m_capacityBlocks * m_blockBytes;
    if (!GetFileSizeEx(static_cast<HANDLE>(m_dataFile), &li) ||
        static_cast<uint64_t>(li.QuadPart) < want) {
        if (!seekAbs(static_cast<HANDLE>(m_dataFile), want)) return false;
        if (!SetEndOfFile(static_cast<HANDLE>(m_dataFile))) return false;
    }

    m_jrnFile = static_cast<void*>(openFile(m_jrnPath, OPEN_ALWAYS));
    if (m_jrnFile == reinterpret_cast<void*>(-1)) {
        CloseHandle(static_cast<HANDLE>(m_dataFile));
        m_dataFile = nullptr;
        return false;
    }

    m_entries.clear();
    m_dirty.clear();
    m_evictable.clear();
    m_freeSlots.clear();

    std::vector<unsigned char> idx;
    if (readFile(m_idxPath, idx) && idx.size() >= 16) {
        uint32_t magic, ver;
        uint64_t count;
        std::memcpy(&magic, idx.data(), 4);
        std::memcpy(&ver, idx.data() + 4, 4);
        std::memcpy(&count, idx.data() + 8, 8);
        if (magic == kIndexMagic) {
            std::vector<bool> used(m_capacityBlocks, false);
            size_t off = 16;
            for (uint64_t i = 0; i < count && off + kRecordSize <= idx.size(); ++i, off += kRecordSize) {
                uint64_t tid, bidx;
                uint32_t slot, crc;
                uint8_t state;
                std::memcpy(&tid, idx.data() + off, 8);
                std::memcpy(&bidx, idx.data() + off + 8, 8);
                std::memcpy(&slot, idx.data() + off + 16, 4);
                state = idx[off + 20];
                std::memcpy(&crc, idx.data() + off + 21, 4);
                if (slot >= m_capacityBlocks) continue;

                if (used[slot]) continue;
                BlockKey key{tid, bidx};
                Entry e;
                e.slot = static_cast<int>(slot);
                e.state = (state == 2) ? BlockState::Dirty : BlockState::ReadCached;
                e.crc = crc;
                m_entries[key] = e;
                if (e.state == BlockState::Dirty) m_dirty.insert(key);
                else m_evictable.insert(key);
                used[slot] = true;
            }
            for (size_t s = 0; s < m_capacityBlocks; ++s)
                if (!used[s]) m_freeSlots.push_back(static_cast<int>(s));
        }
    }
    if (m_freeSlots.empty() && m_entries.empty()) {
        for (size_t s = 0; s < m_capacityBlocks; ++s)
            m_freeSlots.push_back(static_cast<int>(m_capacityBlocks - 1 - s));
    }

    std::string flag;
    m_wasClean = fileExists(m_flagPath) && readText(m_flagPath, flag)
               && flag.find("CLEAN") != std::string::npos;
    if (!m_wasClean) writeFile(m_flagPath, std::string("DIRTY\n"));
    return true;
}

void SsdTier::stop(bool clean) {
    std::lock_guard<std::mutex> l(m_mtx);
    if (!m_dataFile) return;
    saveIndex();

    const bool effectivelyClean = clean && m_dirty.empty();
    if (effectivelyClean)

        writeFile(m_jrnPath, std::vector<unsigned char>{});
    writeFile(m_flagPath, std::string(effectivelyClean ? "CLEAN\n" : "DIRTY\n"));
    if (m_jrnFile) {
        CloseHandle(static_cast<HANDLE>(m_jrnFile));
        m_jrnFile = nullptr;
    }
    CloseHandle(static_cast<HANDLE>(m_dataFile));
    m_dataFile = nullptr;
}

bool SsdTier::readSlot(int slot, void* buf) const {
    if (!m_dataFile) return false;

    OVERLAPPED ov = atOff(static_cast<uint64_t>(slot) * m_blockBytes);
    return ReadFileEx(static_cast<HANDLE>(m_dataFile), buf,
                      static_cast<DWORD>(m_blockBytes), &ov, nullptr) != 0;
}

bool SsdTier::writeSlot(int slot, const void* buf) {
    if (!m_dataFile) return false;
    OVERLAPPED ov = atOff(static_cast<uint64_t>(slot) * m_blockBytes);
    return WriteFileEx(static_cast<HANDLE>(m_dataFile), buf,
                       static_cast<DWORD>(m_blockBytes), &ov, nullptr) != 0;
}

int SsdTier::acquireSlot() {
    if (!m_freeSlots.empty()) {
        int s = m_freeSlots.back();
        m_freeSlots.pop_back();
        return s;
    }

    auto it = m_evictable.begin();
    if (it == m_evictable.end()) return -1;
    BlockKey key = *it;
    m_evictable.erase(it);
    auto en = m_entries.find(key);
    if (en == m_entries.end() || en->second.state != BlockState::ReadCached) return -1;
    int slot = en->second.slot;
    m_entries.erase(en);
    m_freeSlots.push_back(slot);
    return slot;
}

int SsdTier::freeSlotByKey(const BlockKey& key) {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return -1;
    int slot = it->second.slot;
    m_entries.erase(it);
    m_dirty.erase(key);
    m_evictable.erase(key);
    m_freeSlots.push_back(slot);
    return slot;
}

size_t SsdTier::freeBlocks() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_freeSlots.size();
}

size_t SsdTier::dirtyCount() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_dirty.size();
}

BlockState SsdTier::getBlock(const BlockKey& key, void* outBuf) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return BlockState::Free;
    if (!readSlot(it->second.slot, outBuf)) return BlockState::Free;
    return it->second.state;
}

bool SsdTier::putBlock(const BlockKey& key, const void* buf, bool dirty) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        uint32_t c = crc32(static_cast<const unsigned char*>(buf), m_blockBytes);
        if (!writeSlot(it->second.slot, buf)) return false;
        it->second.crc = c;
        if (dirty && it->second.state != BlockState::Dirty) {
            it->second.state = BlockState::Dirty;
            m_dirty.insert(key);
            m_evictable.erase(key);
            journalDirty(key);
        }
        return true;
    }
    int slot = acquireSlot();
    if (slot < 0) return false;
    uint32_t c = crc32(static_cast<const unsigned char*>(buf), m_blockBytes);
    if (!writeSlot(slot, buf)) {

        m_freeSlots.push_back(slot);
        return false;
    }
    Entry e;
    e.slot = slot;
    e.state = dirty ? BlockState::Dirty : BlockState::ReadCached;
    e.crc = c;
    m_entries[key] = e;
    if (dirty) {
        m_dirty.insert(key);
        journalDirty(key);
    } else {
        m_evictable.insert(key);
    }
    return true;
}

bool SsdTier::contains(const BlockKey& key) const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_entries.find(key) != m_entries.end();
}

void SsdTier::markDirty(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->second.state == BlockState::Dirty) return;
    it->second.state = BlockState::Dirty;
    m_dirty.insert(key);
    m_evictable.erase(key);
    journalDirty(key);
}

void SsdTier::flushBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->second.state != BlockState::Dirty) return;
    it->second.state = BlockState::ReadCached;
    m_dirty.erase(key);
    m_evictable.insert(key);
}

bool SsdTier::trimBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return false;

    freeSlotByKey(key);
    return true;
}

BlockState SsdTier::stateOf(const BlockKey& key) const {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_entries.find(key);
    return it == m_entries.end() ? BlockState::Free : it->second.state;
}

void SsdTier::removeBlock(const BlockKey& key) {
    std::lock_guard<std::mutex> l(m_mtx);
    freeSlotByKey(key);
}

void SsdTier::forEachDirty(const std::function<void(const BlockKey&)>& fn) const {
    std::lock_guard<std::mutex> l(m_mtx);
    for (const BlockKey& k : m_dirty) {
        auto it = m_entries.find(k);
        if (it != m_entries.end() && it->second.state == BlockState::Dirty) fn(k);
    }
}

void SsdTier::clear() {
    std::lock_guard<std::mutex> l(m_mtx);
    m_entries.clear();
    m_dirty.clear();
    m_evictable.clear();
    m_freeSlots.resize(m_capacityBlocks);
    for (size_t i = 0; i < m_capacityBlocks; ++i)
        m_freeSlots[i] = static_cast<int>(m_capacityBlocks - 1 - i);
}

void SsdTier::saveIndex() {
    std::vector<unsigned char> out;
    out.reserve(16 + m_entries.size() * kRecordSize);
    uint32_t magic = kIndexMagic, ver = 1;
    uint64_t count = m_entries.size();
    auto push = [&](const void* p, size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        out.insert(out.end(), c, c + n);
    };
    push(&magic, 4);
    push(&ver, 4);
    push(&count, 8);
    for (const auto& kv : m_entries) {

        unsigned char rec[kRecordSize] = {0};
        std::memcpy(rec, &kv.first.targetId, 8);
        std::memcpy(rec + 8, &kv.first.blockIndex, 8);
        uint32_t slot = static_cast<uint32_t>(kv.second.slot);
        rec[20] = static_cast<uint8_t>(kv.second.state == BlockState::Dirty ? 2 : 1);
        std::memcpy(rec + 16, &slot, 4);
        std::memcpy(rec + 21, &kv.second.crc, 4);
        out.insert(out.end(), rec, rec + kRecordSize);
    }
    writeFileAtomic(m_idxPath, out);
}

void SsdTier::journalDirty(const BlockKey& key) {

    auto it = m_entries.find(key);
    uint32_t slot = (it != m_entries.end()) ? static_cast<uint32_t>(it->second.slot) : 0;
    uint32_t crc = (it != m_entries.end()) ? it->second.crc : 0;
    unsigned char rec[kRecordSize] = {0};
    std::memcpy(rec, &key.targetId, 8);
    std::memcpy(rec + 8, &key.blockIndex, 8);
    std::memcpy(rec + 16, &slot, 4);
    rec[20] = 2;
    std::memcpy(rec + 21, &crc, 4);
    if (!m_jrnFile) return;
    HANDLE h = static_cast<HANDLE>(m_jrnFile);
    if (SetFilePointer(h, 0, nullptr, FILE_END) == INVALID_SET_FILE_POINTER) return;
    DWORD w = 0;
    WriteFile(h, rec, sizeof(rec), &w, nullptr);
}

int SsdTier::replayJournal() {
    std::lock_guard<std::mutex> l(m_mtx);
    std::vector<unsigned char> jrn;
    if (!readFile(m_jrnPath, jrn) || jrn.empty()) return 0;
    int recovered = 0;
    std::vector<unsigned char> buf(m_blockBytes);

    std::unordered_map<int, BlockKey> slotOwner;
    slotOwner.reserve(m_entries.size());
    for (const auto& kv : m_entries) slotOwner[kv.second.slot] = kv.first;
    std::unordered_set<int> freeSet(m_freeSlots.begin(), m_freeSlots.end());
    for (size_t off = 0; off + kRecordSize <= jrn.size(); off += kRecordSize) {
        uint64_t tid, bidx;
        uint32_t slot, crc;
        std::memcpy(&tid, jrn.data() + off, 8);
        std::memcpy(&bidx, jrn.data() + off + 8, 8);
        std::memcpy(&slot, jrn.data() + off + 16, 4);
        std::memcpy(&crc, jrn.data() + off + 21, 4);
        if (slot >= m_capacityBlocks) continue;
        BlockKey key{tid, bidx};
        if (!readSlot(static_cast<int>(slot), buf.data())) continue;
        if (crc32(buf) != crc) continue;

        auto own = slotOwner.find(static_cast<int>(slot));
        if (own != slotOwner.end() && own->second != key) continue;

        auto ex = m_entries.find(key);
        if (ex != m_entries.end() && ex->second.slot != static_cast<int>(slot))
            freeSet.insert(ex->second.slot);
        Entry e;
        e.slot = static_cast<int>(slot);
        e.state = BlockState::Dirty;
        e.crc = crc;
        m_entries[key] = e;
        m_dirty.insert(key);
        slotOwner[static_cast<int>(slot)] = key;
        freeSet.erase(static_cast<int>(slot));
        ++recovered;
    }
    m_freeSlots.assign(freeSet.begin(), freeSet.end());
    writeFile(m_jrnPath, std::vector<unsigned char>{});
    saveIndex();
    return recovered;
}

int SsdTier::verifyAgainstSource(
    const std::function<bool(const BlockKey&, uint32_t& crc)>& readSource) {
    std::lock_guard<std::mutex> l(m_mtx);
    int bad = 0;
    std::vector<BlockKey> victims;
    for (const auto& kv : m_entries) {
        if (kv.second.state != BlockState::ReadCached) continue;
        uint32_t srcCrc = 0;
        if (!readSource(kv.first, srcCrc) || srcCrc != kv.second.crc)
            victims.push_back(kv.first);
    }
    for (auto& k : victims) {
        if (freeSlotByKey(k) >= 0) ++bad;
    }
    if (bad > 0) saveIndex();
    return bad;
}

}
