#pragma once

#include <atomic>
#include <string>
#include <cstdint>

namespace vd {

struct Stats {
    std::atomic<long long> readTotal{0};
    std::atomic<long long> bytesReadCache{0};
    std::atomic<long long> readL2Bytes{0};
    std::atomic<long long> bytesReadSource{0};
    std::atomic<long long> writeRequestedTotal{0};
    std::atomic<long long> writeL1L2Total{0};
    std::atomic<long long> writeL2Bytes{0};
    std::atomic<long long> writeDiskTotal{0};
    std::atomic<long long> emergencyWriteBytes{0};
    std::atomic<long long> normalWriteBytes{0};
    std::atomic<long long> dirtyBlocks{0};
    std::atomic<long long> trimmedBlocks{0};
    std::atomic<long long> prefetchRequestedBytes{0};
    std::atomic<long long> prefetchActualBytes{0};
    std::atomic<long long> readHits{0};
    std::atomic<long long> readHitsSsd{0};
    std::atomic<long long> readMisses{0};
    std::atomic<long long> bytesFlushed{0};
    std::atomic<long long> evictionsRam{0};
    std::atomic<long long> evictionsSsd{0};
    std::atomic<long long> promotions{0};
    std::atomic<long long> demotions{0};
    std::atomic<long long> invalidations{0};
    std::atomic<long long> journalReplays{0};
    std::atomic<long long> errors{0};

    void reset();

    std::string toJson() const;
};

}
