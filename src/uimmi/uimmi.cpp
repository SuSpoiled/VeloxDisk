#include "uimmi.h"
#include <windows.h>

namespace vd {

namespace {

struct RtlGetVersionFn {
    uint32_t dwOSVersionInfoSize;
    uint32_t dwMajorVersion;
    uint32_t dwMinorVersion;
    uint32_t dwBuildNumber;
    uint32_t dwPlatformId;
    char szCSDVersion[128];
};

typedef LONG (NTAPI* RtlGetVersionPtr)(void* lpVersionInformation);

bool getOsVersion(uint32_t& major, uint32_t& minor, uint32_t& build) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto fn = reinterpret_cast<RtlGetVersionPtr>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return false;
    RtlGetVersionFn vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    major = vi.dwMajorVersion;
    minor = vi.dwMinorVersion;
    build = vi.dwBuildNumber;
    return true;
}

}

Uimmi::Info Uimmi::detect() {
    Info i;
#if defined(_WIN64)
    i.isWin64 = true;
#endif
    if (getOsVersion(i.major, i.minor, i.build)) {

        i.supportedOs = i.isWin64 && i.major >= 10 && i.build >= 26100;
    }
    return i;
}

std::string Uimmi::Info::describe() const {
    std::string s;
    if (major == 10 && build >= 26100) {
        s += std::string(build == 26100 ? "Windows 11 24H2" : "Windows 11 25H2+")
             + " (build " + std::to_string(build) + ")";
    } else if (major == 10) {
        s += "Windows 10/11 (build " + std::to_string(build) + ")";
    } else {
        s += "Windows " + std::to_string(major) + "." + std::to_string(minor);
    }
    s += isWin64 ? " 64-bit" : " 32-bit";
    s += supportedOs ? " - UIMMI supported" : " - UIMMI not available";
    return s;
}

}
