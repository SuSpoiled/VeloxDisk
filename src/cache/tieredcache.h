#pragma once
#include "tier.h"
#include "key.h"
#include "../common/stats.h"
#include <string>
#include <functional>

namespace vd {

class RamTier;
class SsdTier;

class TieredCache {
public:
    enum class Strategy { ReadWrite, ReadOnly, WriteOnly };
    enum class WriteMode { Original, Smart, Idle, Buffer, Balanced };

    struct Options {

        size_t l1Bytes = 0;
        size_t l2Bytes = 0;
        size_t blockSize = 64 * 1024;
        bool numaAware = true;

        bool independentRwSpace = false;
        int l1WritePercent = 50;
        int l2WritePercent = 50;

        std::string l2Dir;
        std::string l2BaseName = "vd";

        Strategy strategy = Strategy::ReadWrite;
        bool deferWrite = true;
        int deferSeconds = 10;
        WriteMode writeMode = WriteMode::Smart;
        bool ignoreBusy = false;
        bool releaseAfterWrite = false;
        bool flushOnStandby = true;
        bool l1ToL2 = false;
        bool skipFlushOnShutdown = false;

        int l2BusyCollectIntervalS = 30;
        bool l2ResetOnBoot = false;
        bool l2SkipVerifyOnCrash = false;

        bool prefetchLast = false;
        bool prefetchAtBoot = false;
        bool lockPrefetchContent = false;
        bool prefetchFromL2 = false;
    };

    using SourceReader = std::function<bool(const BlockKey&, void* buf)>;
    using SourceWriter = std::function<bool(const BlockKey&, const void* buf)>;

    TieredCache() = default;
    ~TieredCache();

    bool start(const Options& opt, Stats* stats);
    void stop(bool flushAll, const SourceWriter& w);

    bool readBlock(const BlockKey& key, void* buf, const SourceReader& src);

    bool writeBlock(const BlockKey& key, const void* buf, const SourceWriter& fallback);

    bool trimBlock(const BlockKey& key);

    void invalidate(const BlockKey& key, const SourceWriter& w);

    bool flushKey(const BlockKey& key, const SourceWriter& w);

    void clearAll(const SourceWriter& w);

    size_t flushAll(bool emergency, const SourceWriter& w);

    void tick(bool systemIdle, const SourceWriter& w);

    void collectToL2(int maxBlocks);

    void handleBoot(const SourceReader& src, const SourceWriter& w);

    int verifyL2(const SourceReader& src);

    void savePrefetchSnapshot();
    void loadPrefetchSnapshot();

    size_t dirtyBlockCount() const;
    size_t l1UsedBytes() const;
    size_t l2UsedBytes() const;

private:
    SsdTier* l2ReadTier() const { return m_l2Read; }
    SsdTier* l2WriteTier() const { return m_opt.independentRwSpace ? m_l2Write : nullptr; }
    std::vector<ITier*> dirtyTiers() const;
    size_t flushDirtyCount(size_t n, bool emergency, const SourceWriter& w);
    void acceptWrite();
    void demoteToL2IfNeeded(const SourceWriter& w);

    void forEachTier(const std::function<void(ITier*)>& fn) const;
    std::string snapshotPath() const;

    Options m_opt;
    Stats* m_stats = nullptr;
    bool m_started = false;

    RamTier* m_l1Read = nullptr;
    RamTier* m_l1Write = nullptr;
    SsdTier* m_l2Read = nullptr;
    SsdTier* m_l2Write = nullptr;

    bool m_prefetchLocked = false;
    long long m_lastFlushMs = 0;
    long long m_lastCollectMs = 0;
};

}
