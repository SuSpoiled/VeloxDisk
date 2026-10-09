#include "source.h"
#include "key.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mutex>
#include <map>
#include <cwchar>

namespace vd {

namespace {
HANDLE h(void* p) { return static_cast<HANDLE>(p); }

struct VolCache { std::mutex mtx; std::map<wchar_t, DWORD> byRoot; };
DWORD volumeSerial(const wchar_t* path, DWORD def) {
    if (wcslen(path) < 2 || path[1] != L':') return def;
    static VolCache vc;
    std::lock_guard<std::mutex> l(vc.mtx);
    auto it = vc.byRoot.find(path[0]);
    if (it != vc.byRoot.end()) return it->second;
    wchar_t root[4] = { path[0], L':', L'\\', 0 };
    DWORD serial = def;
    wchar_t volName[MAX_PATH];
    if (GetVolumeInformationW(root, volName, MAX_PATH, &serial, nullptr, nullptr, nullptr, 0))
        vc.byRoot[path[0]] = serial;
    return serial;
}

OVERLAPPED atOff(uint64_t off) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    return ov;
}
}

std::wstring normalizePath(const wchar_t* path) {
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(path, MAX_PATH, full, nullptr);
    std::wstring out = (n > 0 && n < MAX_PATH) ? std::wstring(full) : std::wstring(path);
    while (out.size() > 1 && out.back() == L'\\') out.pop_back();

    for (auto& c : out) c = static_cast<wchar_t>(::towlower(static_cast<wchar_t>(c)));
    return out;
}

std::wstring makeAbsolute(const wchar_t* path) {
    if (!path || !*path) return std::wstring();
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(path, MAX_PATH, full, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(full) : std::wstring(path);
}

FileSource::~FileSource() { close(); }

void FileSource::close() {
    if (isOpen()) {
        CloseHandle(h(m_handle));
        m_handle = reinterpret_cast<void*>(-1);
    }
}

bool FileSource::open(const std::wstring& path, bool readOnly) {
    close();
    m_readOnly = readOnly;

    DWORD access = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);

    m_handle = static_cast<void*>(
        CreateFileW(path.c_str(), access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    0, nullptr));
    if (m_handle == reinterpret_cast<void*>(-1)) return false;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h(m_handle), &li)) {
        close();
        return false;
    }
    m_size = static_cast<uint64_t>(li.QuadPart);

    m_volumeSerial = volumeSerial(path.c_str(), 0);
    m_targetId = makeFileTargetId(normalizePath(path.c_str()), m_volumeSerial);
    return true;
}

bool FileSource::extendTo(uint64_t size) {
    if (!isOpen() || m_readOnly || size <= m_size) return true;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    if (SetFilePointerEx(h(m_handle), li, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return false;
    if (!SetEndOfFile(h(m_handle))) return false;
    return refreshSize();
}

bool FileSource::refreshSize() {
    if (!isOpen()) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h(m_handle), &li)) return false;
    m_size = static_cast<uint64_t>(li.QuadPart);
    return true;
}

uint64_t FileSource::readAt(uint64_t offset, void* buf, uint64_t len) {
    if (!isOpen() || len == 0) return 0;
    if (offset >= m_size) return 0;

    if (len > m_size - offset) len = m_size - offset;
    DWORD got = 0;
    if (!ReadFile(h(m_handle), buf, static_cast<DWORD>(len), &got, &atOff(offset))) return 0;
    return got;
}

bool FileSource::writeAt(uint64_t offset, const void* buf, uint64_t len) {
    if (!isOpen() || m_readOnly || len == 0) return false;

    if (offset >= m_size || len > m_size - offset) return false;
    DWORD got = 0;
    if (!WriteFile(h(m_handle), buf, static_cast<DWORD>(len), &got, &atOff(offset))) return false;
    return got == (DWORD)len;
}

}
