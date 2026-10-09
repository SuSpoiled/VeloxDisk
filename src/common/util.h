#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace vd {

std::wstring toWide(const std::string& utf8);
std::string fromWide(const std::wstring& w);

bool readFile(const std::string& path, std::vector<unsigned char>& out);
bool writeFile(const std::string& path, const unsigned char* data, size_t len);
bool writeFile(const std::string& path, const std::vector<unsigned char>& data);
bool writeFile(const std::string& path, const std::string& text);
bool readText(const std::string& path, std::string& out);

bool fileExists(const std::string& path);
bool makeDirs(const std::string& path);
bool copyFileIfAbsent(const std::string& oldPath, const std::string& newPath);

std::uint32_t crc32(const unsigned char* data, size_t len);
std::uint32_t crc32(const std::vector<unsigned char>& v);

std::string fmtSize(long long bytes);

long long nowMs();

void splitPath(const std::string& path, std::string& dir, std::string& base);

}
