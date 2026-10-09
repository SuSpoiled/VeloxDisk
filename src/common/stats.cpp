#include "stats.h"
#include <sstream>
#include <iomanip>

namespace vd {

void Stats::reset() {
    readTotal = 0; bytesReadCache = 0; readL2Bytes = 0; bytesReadSource = 0;
    writeRequestedTotal = 0; writeL1L2Total = 0; writeL2Bytes = 0;
    writeDiskTotal = 0; emergencyWriteBytes = 0; normalWriteBytes = 0;
    dirtyBlocks = 0; trimmedBlocks = 0;
    prefetchRequestedBytes = 0; prefetchActualBytes = 0;
    readHits = readHitsSsd = readMisses = 0;
    bytesFlushed = 0;
    evictionsRam = evictionsSsd = 0;
    promotions = demotions = 0;
    invalidations = journalReplays = 0;
    errors = 0;
}

std::string Stats::toJson() const {
    long long rt = readTotal.load();
    long long rc = bytesReadCache.load();
    double hitRate = rt > 0 ? 100.0 * static_cast<double>(rc) / static_cast<double>(rt) : 0.0;
    double l2Rate = rt > 0 ? 100.0 * static_cast<double>(readL2Bytes.load()) / static_cast<double>(rt) : 0.0;
    double diskRatio = writeRequestedTotal.load() > 0
        ? 100.0 * static_cast<double>(writeDiskTotal.load()) / static_cast<double>(writeRequestedTotal.load())
        : 0.0;
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    o << "{\n"
      << "  \"readTotal\": "               << rt                    << ",\n"
      << "  \"readCache\": "               << rc                    << ",\n"
      << "  \"readL2\": "                  << readL2Bytes.load()    << ",\n"
      << "  \"cacheHitRatePercent\": "     << hitRate               << ",\n"
      << "  \"l2HitRatePercent\": "        << l2Rate                << ",\n"
      << "  \"writeRequestedTotal\": "     << writeRequestedTotal.load() << ",\n"
      << "  \"writeL1L2Total\": "          << writeL1L2Total.load()    << ",\n"
      << "  \"writeL2\": "                 << writeL2Bytes.load()      << ",\n"
      << "  \"writeDiskTotal\": "          << writeDiskTotal.load()    << ",\n"
      << "  \"writeDiskRatioPercent\": "   << diskRatio              << ",\n"
      << "  \"emergencyWrite\": "          << emergencyWriteBytes.load() << ",\n"
      << "  \"normalWrite\": "             << normalWriteBytes.load()    << ",\n"
      << "  \"dirtyBlocks\": "             << dirtyBlocks.load()        << ",\n"
      << "  \"trimmedBlocks\": "           << trimmedBlocks.load()      << ",\n"
      << "  \"prefetchActual\": "          << prefetchActualBytes.load()    << ",\n"
      << "  \"prefetchRequested\": "       << prefetchRequestedBytes.load() << ",\n"
      << "  \"readHitsRam\": "             << readHits.load()         << ",\n"
      << "  \"readHitsSsd\": "             << readHitsSsd.load()      << ",\n"
      << "  \"readMisses\": "              << readMisses.load()       << ",\n"
      << "  \"bytesFlushed\": "            << bytesFlushed.load()     << ",\n"
      << "  \"evictionsRam\": "            << evictionsRam.load()     << ",\n"
      << "  \"evictionsSsd\": "            << evictionsSsd.load()     << ",\n"
      << "  \"promotions\": "              << promotions.load()       << ",\n"
      << "  \"demotions\": "               << demotions.load()        << ",\n"
      << "  \"invalidations\": "           << invalidations.load()    << ",\n"
      << "  \"journalReplays\": "          << journalReplays.load()   << ",\n"
      << "  \"errors\": "                  << errors.load()           << "\n"
      << "}\n";
    return o.str();
}

}
