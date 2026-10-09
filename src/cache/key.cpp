#include "key.h"
#include <cwchar>

namespace vd {

uint64_t makeFileTargetId(const std::wstring& path, uint32_t volumeSerial) {

    uint64_t h = 1469598103934665603ull;
    for (wchar_t wc : path) {
        uint16_t c = static_cast<uint16_t>(::towlower(static_cast<wchar_t>(wc)));
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
        h ^= static_cast<uint64_t>(c) << 16;
        h *= 1099511628211ull;
    }

    h ^= static_cast<uint64_t>(volumeSerial) * 0x9E3779B97F4A7C15ull;
    h *= 1099511628211ull;
    h ^= h >> 32;
    return h;
}

}
