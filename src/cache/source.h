#pragma once
#include <cstdint>
#include <string>

namespace vd {

class FileSource {
public:
    FileSource() = default;
    ~FileSource();

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    bool open(const std::wstring& path, bool readOnly);
    void close();
    bool isOpen() const { return m_handle != reinterpret_cast<void*>(-1); }

    uint64_t size() const { return m_size; }
    bool refreshSize();

    bool extendTo(uint64_t size);

    uint64_t readAt(uint64_t offset, void* buf, uint64_t len);

    bool writeAt(uint64_t offset, const void* buf, uint64_t len);

    uint64_t targetId() const { return m_targetId; }

private:
    void* m_handle = reinterpret_cast<void*>(-1);
    uint64_t m_size = 0;
    uint32_t m_volumeSerial = 0;
    bool m_readOnly = true;
    uint64_t m_targetId = 0;
};

std::wstring normalizePath(const wchar_t* path);

std::wstring makeAbsolute(const wchar_t* path);

}
