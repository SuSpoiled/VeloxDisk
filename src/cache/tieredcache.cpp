#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "tieredcache.h"
#include "ramtier.h"
#include "ssdtier.h"
#include "../common/util.h"
#include "../common/log.h"
#include <windows.h>
#include <vector>
#include <algorithm>

namespace vd {

TieredCache::~TieredCache() {
    stop(false, nullptr);
}

namespace {
size_t pct(size_t total, int percent) {
    if (percent <= 0) return 0;
    if (percent >= 100) return total;
    return total * static_cast<size_t>(percent) / 100;
}

size_t tierUsedBytes(ITier* t, size_t bs) {
    return t ? (t->capacityBlocks() - t->freeBlocks()) * bs : 0;
}

std::vector<uint8_t>& blkBuf(size_t bs) {
    static thread_local std::vector<uint8_t> b;
    if (b.size() < bs) b.resize(bs);
    return b;
}
}

bool TieredCache::start(const Options& opt, Stats* stats) {
    if (m_started) return false;
    m_opt = opt;
    m_stats = stats;
    m_opt.blockSize = (m_opt.blockSize >= 4096 && m_opt.blockSize <= 512 * 1024)
                          ? m_opt.blockSize : 64 * 1024;
    if (m_opt.l1Bytes == 0 && m_opt.l2Bytes == 0) return false;
    if (m_opt.l2Bytes > 0 && m_opt.l2Dir.empty()) return false;

    const bool reads = m_opt.strategy != Strategy::WriteOnly;
    const bool writes = m_opt.strategy != Strategy::ReadOnly;

    if (m_opt.l1Bytes > 0) {
        if (m_opt.independentRwSpace) {
            size_t rb = reads ? pct(m_opt.l1Bytes, 100 - m_opt.l1WritePercent) : 0;
            size_t wb = writes ? pct(m_opt.l1Bytes, m_opt.l1WritePercent) : 0;
            if (reads && rb >= m_opt.blockSize) {
                m_l1Read = new RamTier();
                if (!m_l1Read->start(rb, m_opt.blockSize, m_opt.numaAware)) {
                    delete m_l1Read; m_l1Read = nullptr;
                }
            }
            if (writes && wb >= m_opt.blockSize) {
                m_l1Write = new RamTier();
                if (!m_l1Write->start(wb, m_opt.blockSize, m_opt.numaAware)) {
                    delete m_l1Write; m_l1Write = nullptr;
                }
            }
        } else {

            m_l1Read = new RamTier();
            if (!m_l1Read->start(m_opt.l1Bytes, m_opt.blockSize, m_opt.numaAware)) {
                delete m_l1Read; m_l1Read = nullptr;
            }
        }
    }

    if (m_opt.l2Bytes > 0) {
        if (m_opt.independentRwSpace) {
            size_t rb = reads ? pct(m_opt.l2Bytes, 100 - m_opt.l2WritePercent) : 0;
            size_t wb = writes ? pct(m_opt.l2Bytes, m_opt.l2WritePercent) : 0;
            if (reads && rb >= m_opt.blockSize) {
                m_l2Read = new SsdTier();
                if (!m_l2Read->start(m_opt.l2Dir, m_opt.l2BaseName + ".rd",
                                     rb, m_opt.blockSize)) {
                    delete m_l2Read; m_l2Read = nullptr;
                }
            }
            if (writes && wb >= m_opt.blockSize) {
                m_l2Write = new SsdTier();
                if (!m_l2Write->start(m_opt.l2Dir, m_opt.l2BaseName + ".wr",
                                     wb, m_opt.blockSize)) {
                    delete m_l2Write; m_l2Write = nullptr;
                }
            }
        } else {
            m_l2Read = new SsdTier();
            if (!m_l2Read->start(m_opt.l2Dir, m_opt.l2BaseName + ".sh",
                                 m_opt.l2Bytes, m_opt.blockSize)) {
                delete m_l2Read; m_l2Read = nullptr;
            }
        }
    }

    if (!m_l1Read && !m_l2Read) {

        if (m_l1Write) { delete m_l1Write; m_l1Write = nullptr; }
        if (m_l2Write) { delete m_l2Write; m_l2Write = nullptr; }
        return false;
    }

    m_prefetchLocked = false;
    if (m_opt.prefetchLast) {
        if (m_opt.prefetchAtBoot)
            loadPrefetchSnapshot();
        else if (m_opt.lockPrefetchContent && fileExists(snapshotPath()))
            m_prefetchLocked = true;
    }

    m_lastFlushMs = nowMs();
    m_lastCollectMs = 0;
    m_started = true;
    return true;
}

std::string TieredCache::snapshotPath() const {
    return m_opt.l2Dir + "/prefetch." + m_opt.l2BaseName + ".bin";
}

std::vector<ITier*> TieredCache::dirtyTiers() const {
    std::vector<ITier*> v;
    if (m_l1Write) v.push_back(m_l1Write);
    else if (!m_opt.independentRwSpace && m_l1Read) v.push_back(m_l1Read);
    if (m_l2Write) v.push_back(m_l2Write);
    else if (!m_opt.independentRwSpace && m_l2Read && m_opt.strategy != Strategy::ReadOnly)
        v.push_back(m_l2Read);
    return v;
}

void TieredCache::acceptWrite() {
    if (m_stats) {
        m_stats->writeL1L2Total += static_cast<long long>(m_opt.blockSize);
        m_stats->dirtyBlocks++;
    }
}

void TieredCache::forEachTier(const std::function<void(ITier*)>& fn) const {
    fn(m_l1Write);
    fn(m_opt.independentRwSpace ? nullptr : m_l1Read);
    fn(m_l2Write);
    fn(m_opt.independentRwSpace ? nullptr : m_l2Read);
}

size_t TieredCache::flushDirtyCount(size_t n, bool emergency, const SourceWriter& w) {
    size_t total = 0;
    const size_t bs = m_opt.blockSize;
    std::vector<uint8_t>& buf = blkBuf(bs);
    for (ITier* t : dirtyTiers()) {
        if (total >= n) break;
        const bool isL1 = (t == static_cast<ITier*>(m_l1Write)) ||
                          (!m_opt.independentRwSpace && t == static_cast<ITier*>(m_l1Read));
        bool tierFlushed = false;
        std::vector<BlockKey> dirty;
        dirty.reserve(t->dirtyCount());
        t->forEachDirty([&](const BlockKey& k) { dirty.push_back(k); });
        for (const auto& k : dirty) {
            if (total >= n) break;
            if (t->getBlock(k, buf.data()) != BlockState::Dirty) continue;
            if (!w || !w(k, buf.data())) {
                if (m_stats) m_stats->errors++;
                continue;
            }
            t->flushBlock(k);
            if (m_stats) {
                m_stats->bytesFlushed += static_cast<long long>(bs);
                m_stats->writeDiskTotal += static_cast<long long>(bs);
                if (emergency) m_stats->emergencyWriteBytes += static_cast<long long>(bs);
                else m_stats->normalWriteBytes += static_cast<long long>(bs);
                m_stats->dirtyBlocks--;
            }

            if (m_opt.releaseAfterWrite && isL1)
                t->removeBlock(k);
            ++total;
            tierFlushed = true;
        }
        if (tierFlushed) {
            if (auto* s = dynamic_cast<SsdTier*>(t)) s->saveIndex();
        }
    }
    if (total > 0) m_lastFlushMs = nowMs();
    return total;
}

size_t TieredCache::flushAll(bool emergency, const SourceWriter& w) {
    size_t dirty = dirtyBlockCount();
    if (dirty == 0) return 0;

    return flushDirtyCount(dirty, emergency, w);
}

bool TieredCache::readBlock(const BlockKey& key, void* buf, const SourceReader& src) {
    if (!m_started) return false;
    const long long bs = static_cast<long long>(m_opt.blockSize);
    const bool reads = m_opt.strategy != Strategy::WriteOnly;
    if (m_stats) m_stats->readTotal += bs;

    if (reads && m_l1Read) {
        BlockState st = m_l1Read->getBlock(key, buf);
        if (st != BlockState::Free) {
            if (m_stats) { m_stats->readHits++; m_stats->bytesReadCache += bs; }
            return true;
        }
    }

    SsdTier* l2r = l2ReadTier();
    if (reads && l2r) {
        BlockState st = l2r->getBlock(key, buf);
        if (st != BlockState::Free) {
            if (m_stats) {
                m_stats->readHitsSsd++;
                m_stats->readL2Bytes += bs;
                m_stats->bytesReadCache += bs;
                m_stats->promotions++;
            }
            if (m_l1Read) m_l1Read->putBlock(key, buf, false);
            return true;
        }
    }

    if (m_stats) m_stats->readMisses++;
    if (src) {
        if (src(key, buf)) {
            if (m_stats) m_stats->bytesReadSource += bs;
            if (reads && m_l1Read) m_l1Read->putBlock(key, buf, false);
            return false;
        }
    }
    return false;
}

bool TieredCache::writeBlock(const BlockKey& key, const void* buf, const SourceWriter& fallback) {
    if (!m_started) return false;
    const long long bs = static_cast<long long>(m_opt.blockSize);
    if (m_stats) m_stats->writeRequestedTotal += bs;
    if (!m_opt.deferWrite || m_opt.deferSeconds <= 0 || m_opt.strategy == Strategy::ReadOnly)
        return false;
    ITier* wt = m_l1Write ? static_cast<ITier*>(m_l1Write)
                          : static_cast<ITier*>(m_l1Read);
    if (!wt) return false;
    if (wt->putBlock(key, buf, true)) { acceptWrite(); return true; }

    demoteToL2IfNeeded(fallback);
    flushAll(true, fallback);
    if (wt->putBlock(key, buf, true)) { acceptWrite(); return true; }
    return false;
}

void TieredCache::demoteToL2IfNeeded(const SourceWriter& w) {
    if (!m_opt.l1ToL2 || !m_l1Write || !m_l2Write) return;
    std::vector<BlockKey> dirty;
    dirty.reserve(m_l1Write->dirtyCount());
    m_l1Write->forEachDirty([&](const BlockKey& k) { dirty.push_back(k); });
    size_t limit = dirty.size() / 2;
    std::vector<uint8_t>& buf = blkBuf(m_opt.blockSize);
    for (size_t i = 0; i < limit; ++i) {
        const BlockKey& k = dirty[i];
        if (m_l1Write->getBlock(k, buf.data()) != BlockState::Dirty) continue;
        if (!m_l2Write->putBlock(k, buf.data(), true)) {
            flushAll(true, w);
            if (!m_l2Write->putBlock(k, buf.data(), true)) continue;
        }
        m_l1Write->removeBlock(k);
        if (m_stats) m_stats->demotions++;
    }
}

bool TieredCache::trimBlock(const BlockKey& key) {
    bool done = false;
    int dirtyRemoved = 0;
    auto trimIn = [&](ITier* t) {
        if (!t) return;

        if (t->stateOf(key) == BlockState::Dirty) dirtyRemoved++;
        if (t->trimBlock(key)) done = true;
    };
    trimIn(m_l1Write ? static_cast<ITier*>(m_l1Write)
                     : (m_opt.independentRwSpace ? nullptr : static_cast<ITier*>(m_l1Read)));
    trimIn(m_l2Write ? static_cast<ITier*>(m_l2Write)
                     : (m_opt.independentRwSpace ? nullptr : static_cast<ITier*>(m_l2Read)));
    if (done && m_stats) {
        m_stats->trimmedBlocks++;
        if (dirtyRemoved > 0) m_stats->dirtyBlocks -= dirtyRemoved;
    }
    return done;
}

void TieredCache::invalidate(const BlockKey& key, const SourceWriter& w) {
    bool any = false;
    SsdTier* sIdx[2] = {nullptr, nullptr};
    int nSIdx = 0;
    const size_t bs = m_opt.blockSize;
    std::vector<uint8_t>& buf = blkBuf(bs);
    forEachTier([&](ITier* t) {
        if (!t || !t->contains(key)) return;

        if (t->stateOf(key) == BlockState::Dirty && w &&
            t->getBlock(key, buf.data()) == BlockState::Dirty && w(key, buf.data())) {
            t->flushBlock(key);
            if (m_stats) {
                m_stats->bytesFlushed += static_cast<long long>(bs);
                m_stats->writeDiskTotal += static_cast<long long>(bs);
                m_stats->normalWriteBytes += static_cast<long long>(bs);
                m_stats->dirtyBlocks--;
            }
        }
        t->removeBlock(key);
        any = true;
        if (auto* s = dynamic_cast<SsdTier*>(t)) {
            if (nSIdx < 2) sIdx[nSIdx++] = s;
        }
    });
    if (any) {
        if (m_stats) m_stats->invalidations++;
        for (int i = 0; i < nSIdx; ++i) sIdx[i]->saveIndex();
    }
}

bool TieredCache::flushKey(const BlockKey& key, const SourceWriter& w) {
    bool flushed = false;
    SsdTier* sIdx[2] = {nullptr, nullptr};
    int nSIdx = 0;
    const size_t bs = m_opt.blockSize;
    std::vector<uint8_t>& buf = blkBuf(bs);
    forEachTier([&](ITier* t) {
        if (!t || !t->contains(key)) return;
        if (t->stateOf(key) == BlockState::Dirty && w &&
            t->getBlock(key, buf.data()) == BlockState::Dirty && w(key, buf.data())) {
            t->flushBlock(key);
            if (m_stats) {
                m_stats->bytesFlushed += static_cast<long long>(bs);
                m_stats->writeDiskTotal += static_cast<long long>(bs);
                m_stats->normalWriteBytes += static_cast<long long>(bs);
                m_stats->dirtyBlocks--;
            }
            flushed = true;
            if (auto* s = dynamic_cast<SsdTier*>(t)) {
                if (nSIdx < 2) sIdx[nSIdx++] = s;
            }
        }
    });
    for (int i = 0; i < nSIdx; ++i) sIdx[i]->saveIndex();
    return flushed;
}

void TieredCache::clearAll(const SourceWriter& w) {
    flushAll(false, w);
    forEachTier([](ITier* t) { if (t) t->clear(); });
    if (auto* s = l2ReadTier()) s->saveIndex();
    if (auto* s = l2WriteTier()) s->saveIndex();
}

size_t TieredCache::dirtyBlockCount() const {
    size_t n = 0;
    forEachTier([&](ITier* t) { if (t) n += t->dirtyCount(); });
    return n;
}

size_t TieredCache::l1UsedBytes() const {
    return tierUsedBytes(m_l1Read, m_opt.blockSize) + tierUsedBytes(m_l1Write, m_opt.blockSize);
}

size_t TieredCache::l2UsedBytes() const {
    return tierUsedBytes(m_l2Read, m_opt.blockSize) + tierUsedBytes(m_l2Write, m_opt.blockSize);
}

void TieredCache::tick(bool systemIdle, const SourceWriter& w) {
    if (!m_started || !m_opt.deferWrite || m_opt.deferSeconds <= 0) return;
    const long long now = nowMs();
    const bool effIdle = systemIdle || m_opt.ignoreBusy;
    const size_t dirty = dirtyBlockCount();
    size_t cap = 0;
    forEachTier([&](ITier* t) { if (t) cap += t->capacityBlocks(); });
    const double ratio = cap ? 100.0 * static_cast<double>(dirty) / static_cast<double>(cap) : 0.0;

    const bool dueOriginal = (now - m_lastFlushMs) >= static_cast<long long>(m_opt.deferSeconds) * 1000;

    switch (m_opt.writeMode) {
        case WriteMode::Original:
            if (dirty && dueOriginal) flushAll(false, w);
            break;
        case WriteMode::Smart:
            if (dirty && ratio >= 90.0) flushDirtyCount(dirty / 4 + 1, false, w);
            else if (dirty && dueOriginal) flushAll(false, w);
            break;
        case WriteMode::Idle:
            if (dirty && (dueOriginal || effIdle)) flushAll(false, w);
            break;
        case WriteMode::Buffer:
            if (dirty && ratio >= 40.0) flushDirtyCount(dirty * 3 / 5 + 1, false, w);
            else if (dirty && dueOriginal) flushAll(false, w);
            break;
        case WriteMode::Balanced:
            if (dirty) flushDirtyCount(std::max<size_t>(1, dirty / 10), false, w);
            break;
    }

    if (m_opt.flushOnStandby && systemIdle && dirty &&
        (now - m_lastFlushMs) >= static_cast<long long>(m_opt.deferSeconds) * 1000)
        flushAll(false, w);

    SsdTier* l2r = l2ReadTier();
    if (l2r && m_l1Read && m_opt.strategy != Strategy::WriteOnly) {
        const bool busy = !systemIdle;
        const bool shouldCollect =
            effIdle ||
            (busy && m_opt.l2BusyCollectIntervalS > 0 &&
             now - m_lastCollectMs >= static_cast<long long>(m_opt.l2BusyCollectIntervalS) * 1000);
        if (shouldCollect) {
            collectToL2(16);
            m_lastCollectMs = now;
        }
    }
}

void TieredCache::collectToL2(int maxBlocks) {
    RamTier* l1r = m_l1Read;
    SsdTier* l2r = l2ReadTier();
    if (!l1r || !l2r) return;
    l1r->collectReadTop(maxBlocks, [&](const BlockKey& key, const uint8_t* data, size_t len) -> bool {
        if (l2r->contains(key)) return false;
        if (!l2r->putBlock(key, data, false)) return false;
        if (m_stats) {
            m_stats->writeL2Bytes += static_cast<long long>(len);
            m_stats->demotions++;
        }
        return true;
    });
}

void TieredCache::handleBoot(const SourceReader& src, const SourceWriter& w) {
    SsdTier* l2r = l2ReadTier();
    SsdTier* l2w = l2WriteTier();
    if (!l2r && !l2w) return;

    bool wasClean = true;
    if (l2r && !l2r->wasCleanShutdown()) wasClean = false;
    if (l2w && !l2w->wasCleanShutdown()) wasClean = false;

    if (!wasClean) {
        int rec = 0;
        if (l2r) rec += l2r->replayJournal();
        if (l2w) rec += l2w->replayJournal();
        if (m_stats && rec > 0) m_stats->journalReplays += rec;
    }
    if (wasClean) return;

    if (m_opt.l2ResetOnBoot || m_opt.l2SkipVerifyOnCrash) {

        if (w) flushAll(false, w);
        if (l2r) l2r->clear();
        if (l2w) l2w->clear();
        return;
    }

    auto verify = [&](SsdTier* tier) {
        if (!tier || !src) return;
        std::vector<uint8_t>& buf = blkBuf(m_opt.blockSize);
        int bad = tier->verifyAgainstSource([&](const BlockKey& k, uint32_t& crc) -> bool {
            if (!src(k, buf.data())) return false;
            crc = crc32(buf);
            return true;
        });
        if (m_stats && bad > 0) m_stats->invalidations += bad;
    };
    verify(l2r);
    verify(l2w);
}

int TieredCache::verifyL2(const SourceReader& src) {
    SsdTier* l2r = l2ReadTier();
    if (!m_started || !l2r || !src) return -1;
    std::vector<uint8_t>& buf = blkBuf(m_opt.blockSize);
    int bad = l2r->verifyAgainstSource([&](const BlockKey& k, uint32_t& crc) -> bool {
        if (!src(k, buf.data())) return false;
        crc = crc32(buf);
        return true;
    });
    if (m_stats && bad > 0) m_stats->invalidations += bad;
    return bad;
}

void TieredCache::savePrefetchSnapshot() {
    if (!m_opt.prefetchLast || !m_l1Read || m_opt.l2Dir.empty() || m_prefetchLocked)
        return;

    const std::wstring wpath = toWide(snapshotPath());
    HANDLE fh = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        Logger::instance().warn("prefetch snapshot write failed: " + snapshotPath());
        return;
    }
    auto wAt = [&](uint64_t off, const void* p, size_t n) -> bool {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        if (SetFilePointerEx(fh, li, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return false;
        DWORD w = 0;
        return WriteFile(fh, p, static_cast<DWORD>(n), &w, nullptr) && w == static_cast<DWORD>(n);
    };
    uint32_t magic = 0x50524631u;
    uint64_t count = 0;
    bool ok = wAt(0, &magic, 4) && wAt(4, &count, 8);
    uint64_t off = 12;
    m_l1Read->forEachReadCached([&](const BlockKey& key, const uint8_t* data, size_t len) {
        if (!ok) return;
        uint64_t tid = key.targetId, bidx = key.blockIndex;
        if (!wAt(off, &tid, 8) || !wAt(off + 8, &bidx, 8) || !wAt(off + 16, data, len)) {
            ok = false;
            return;
        }
        off += 16 + len;
        ++count;
    });
    if (ok && !wAt(4, &count, 8)) ok = false;
    CloseHandle(fh);
    if (!ok) {
        DeleteFileW(wpath.c_str());
        Logger::instance().warn("prefetch snapshot write failed: " + snapshotPath());
    }
}

void TieredCache::loadPrefetchSnapshot() {
    if (!m_opt.prefetchLast || !m_l1Read || m_opt.l2Dir.empty()) return;

    HANDLE fh = CreateFileW(toWide(snapshotPath()).c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;
    uint8_t hdr[12];
    DWORD got = 0;
    if (!ReadFile(fh, hdr, sizeof(hdr), &got, nullptr) || got != sizeof(hdr)) {
        CloseHandle(fh);
        return;
    }
    uint32_t magic = 0;
    uint64_t count = 0;
    std::memcpy(&magic, hdr, 4);
    std::memcpy(&count, hdr + 4, 8);
    if (magic != 0x50524631u) { CloseHandle(fh); return; }
    const size_t bs = m_opt.blockSize;
    std::vector<uint8_t>& b = blkBuf(bs);
    uint8_t keyBuf[16];
    long long requested = 0, actual = 0;
    SsdTier* l2r = l2ReadTier();
    for (uint64_t i = 0; i < count; ++i) {

        if (!ReadFile(fh, keyBuf, sizeof(keyBuf), &got, nullptr) || got != sizeof(keyBuf)) break;
        if (!ReadFile(fh, b.data(), static_cast<DWORD>(bs), &got, nullptr) || got != bs) break;
        uint64_t tid, bidx;
        std::memcpy(&tid, keyBuf, 8);
        std::memcpy(&bidx, keyBuf + 8, 8);
        BlockKey key{tid, bidx};
        requested += static_cast<long long>(bs);
        if (m_opt.prefetchFromL2 && l2r && l2r->contains(key)) {
            actual += static_cast<long long>(bs);
        } else if (m_l1Read->putBlock(key, b.data(), false)) {
            actual += static_cast<long long>(bs);
        }
    }
    CloseHandle(fh);
    if (m_stats) {
        m_stats->prefetchRequestedBytes = requested;
        m_stats->prefetchActualBytes = actual;
    }
    if (m_opt.lockPrefetchContent) m_prefetchLocked = true;
}

void TieredCache::stop(bool flush, const SourceWriter& w) {
    if (!m_started) return;
    if (flush && !m_opt.skipFlushOnShutdown && w) flushAll(false, w);
    savePrefetchSnapshot();
    if (m_l1Read) { m_l1Read->stop(); delete m_l1Read; m_l1Read = nullptr; }
    if (m_l1Write) { m_l1Write->stop(); delete m_l1Write; m_l1Write = nullptr; }
    if (m_l2Read) { m_l2Read->stop(true); delete m_l2Read; m_l2Read = nullptr; }
    if (m_l2Write) { m_l2Write->stop(true); delete m_l2Write; m_l2Write = nullptr; }
    m_started = false;
}

}
