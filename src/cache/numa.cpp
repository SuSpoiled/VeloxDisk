#include "numa.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memoryapi.h>

namespace vd {

namespace {

constexpr size_t kRegionGranularity = 64 * 1024;

int highestNode() {
    ULONG highest = 0;
    if (!::GetNumaHighestNodeNumber(&highest)) highest = 0;
    return static_cast<int>(highest) + 1;
}

using VirtualAlloc2Fn = PVOID (*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                  MEM_EXTENDED_PARAMETER*, ULONG);

VirtualAlloc2Fn loadVirtualAlloc2() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return nullptr;
    return reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(k32, "VirtualAlloc2"));
}

VirtualAlloc2Fn getVirtualAlloc2() {
    static VirtualAlloc2Fn fn = loadVirtualAlloc2();
    return fn;
}

}

bool NumaAlloc::allocate(size_t bytes, bool numaAware, std::vector<MemRegion>& regions) {
    regions.clear();
    if (bytes == 0) return false;
    bytes -= bytes % kRegionGranularity;
    if (bytes == 0) return false;

    const int nodes = numaAware ? highestNode() : 1;

    size_t basePerNode = bytes / static_cast<size_t>(nodes);
    size_t remainder = bytes % static_cast<size_t>(nodes);

    for (int i = 0; i < nodes; ++i) {
        size_t chunk = basePerNode + (i == 0 ? remainder : 0);
        chunk -= chunk % kRegionGranularity;
        if (chunk == 0) continue;

        void* p = nullptr;
        if (numaAware) {

            VirtualAlloc2Fn va2 = getVirtualAlloc2();
            if (va2) {
                MEM_EXTENDED_PARAMETER ext{};
                ext.Type = MemExtendedParameterNumaNode;
                ext.ULong = static_cast<DWORD>(i);
                p = va2(nullptr, nullptr, chunk, MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE, &ext, 1);
            }
            if (!p) p = VirtualAlloc(nullptr, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        } else {
            p = VirtualAlloc(nullptr, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        if (!p) {
            freeAll(regions);
            return false;
        }
        regions.push_back(MemRegion{p, chunk, numaAware ? static_cast<uint32_t>(i) : 0});
    }
    return !regions.empty();
}

void NumaAlloc::freeAll(std::vector<MemRegion>& regions) {
    for (auto& r : regions) {
        if (r.base) VirtualFree(r.base, 0, MEM_RELEASE);
        r.base = nullptr;
        r.size = 0;
    }
    regions.clear();
}

}
