#include "util.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <chrono>
#include <unordered_map>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace vd {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string fromWide(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(toWide(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n < 0 ? 0 : n));
    if (out.empty()) return n <= 0;
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));

    return static_cast<long long>(f.gcount()) == n;
}

bool writeFile(const std::string& path, const unsigned char* data, size_t len) {
    std::ofstream f(toWide(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (len) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return static_cast<bool>(f);
}

bool writeFile(const std::string& path, const std::vector<unsigned char>& data) {
    return writeFile(path, data.data(), data.size());
}

bool writeFile(const std::string& path, const std::string& text) {
    return writeFile(path, reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream f(toWide(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool fileExists(const std::string& path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(toWide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool makeDirs(const std::string& path) {
    if (path.empty()) return true;
#ifdef _WIN32
    std::string p = path;
    if (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    size_t i = p.find_first_of("\\/");
    std::string acc;
    while (i != std::string::npos) {
        acc = p.substr(0, i);
        ::_wmkdir(toWide(acc).c_str());
        i = p.find_first_of("\\/", i + 1);
    }
    ::_wmkdir(toWide(p).c_str());
    return true;
#else
    std::string acc;
    for (char c : path) { acc.push_back(c); if (c == '/') { ::mkdir(acc.c_str(), 0755); } }
    ::mkdir(path.c_str(), 0755);
    return true;
#endif
}

bool copyFileIfAbsent(const std::string& oldPath, const std::string& newPath) {
    if (fileExists(newPath) || !fileExists(oldPath)) return false;
    std::vector<unsigned char> data;
    if (!readFile(oldPath, data)) return false;
    std::string dir, base;
    splitPath(newPath, dir, base);
    if (!dir.empty() && dir != "." && !makeDirs(dir)) return false;
    return writeFile(newPath, data);
}

std::uint32_t crc32(const unsigned char* data, size_t len) {

    static const std::uint32_t (*tbl)[256] = [] {
        static std::uint32_t t[4][256];
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }

        for (int k = 1; k < 4; ++k)
            for (std::uint32_t i = 0; i < 256; ++i)
                t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    const unsigned char* p = data;
    while (len >= 4) {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        c ^= v;
        c = tbl[3][c & 0xFF] ^ tbl[2][(c >> 8) & 0xFF]
          ^ tbl[1][(c >> 16) & 0xFF] ^ tbl[0][(c >> 24) & 0xFF];
        p += 4;
        len -= 4;
    }
    while (len-- > 0)
        c = tbl[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(const std::vector<unsigned char>& v) { return crc32(v.data(), v.size()); }

std::string fmtSize(long long bytes) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    char buf[32];
    if (i == 0) std::snprintf(buf, sizeof(buf), "%lld B", bytes);
    else std::snprintf(buf, sizeof(buf), "%.2f %s", v, u[i]);
    return buf;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void splitPath(const std::string& path, std::string& dir, std::string& base) {
    size_t i = path.find_last_of("/\\");
    if (i == std::string::npos) { dir = "."; base = path; }
    else { dir = path.substr(0, i); base = path.substr(i + 1); }
}

}
