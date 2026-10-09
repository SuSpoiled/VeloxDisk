#include "engine.h"
#include "../cache/source.h"
#include "../common/util.h"
#include "../uimmi/uimmi.h"
#include <windows.h>
#include <cstdio>

extern "C" BOOL IsUserAnAdmin(void);
#include <thread>
#include <chrono>
#include <algorithm>
#include <memory>

namespace vd {

namespace {

std::string normUtf8(const std::string& p) {
    return fromWide(normalizePath(toWide(p).c_str()));
}

void enumerateFiles(const std::string& dir, std::vector<std::string>& out, size_t cap) {
    std::wstring wdir = toWide(dir);
    if (wdir.empty()) return;
    std::wstring pattern = wdir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fromWide(fd.cFileName);
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name != "." && name != "..") enumerateFiles(full, out, cap);
        } else {
            out.push_back(full);
            if (out.size() >= cap) { FindClose(h); return; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool extendFile(const std::string& path, uint64_t size) {
    std::wstring w = toWide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LONG hi = static_cast<LONG>(size >> 32);
    bool ok = SetFilePointer(h, static_cast<LONG>(size & 0xFFFFFFFF), &hi, FILE_BEGIN)
                   != INVALID_SET_FILE_POINTER && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

bool validTaskName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}

std::string escSep(const std::string& s) {
    std::string o;
    o.reserve(s.size() * 2);
    for (char c : s) {
        if (c == ';') o += "%3B";
        else if (c == '%') o += "%25";
        else o += c;
    }
    return o;
}

std::string unescSep(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {

        if (s.compare(i, 3, "%25") == 0) { o += '%'; i += 2; }
        else if (s.compare(i, 3, "%3B") == 0) { o += ';'; i += 2; }
        else o += s[i];
    }
    return o;
}

std::vector<uint8_t>& blockBuf(size_t bs) {
    static thread_local std::vector<uint8_t> buf;
    if (buf.size() < bs) buf.resize(bs);
    return buf;
}

}

struct FilePool {
    std::mutex mtx;
    std::map<std::string, std::unique_ptr<FileSource>> open;
};

TieredCache::SourceWriter makeWriter(Task* t) {
    auto pool = std::make_shared<FilePool>();
    return [t, pool](const BlockKey& key, const void* buf) -> bool {
        std::string path;
        {
            std::lock_guard<std::mutex> l(t->mapMtx);
            auto it = t->targetMap.find(key.targetId);
            if (it == t->targetMap.end()) return false;
            path = it->second;
        }

        const uint64_t bs = t->opts.blockSize;
        if (bs == 0 || key.blockIndex > (8ull << 40) / bs) return false;
        const uint64_t off = key.blockIndex * bs;
        const uint64_t end = off + bs;
        FileSource* src = nullptr;
        {
            std::lock_guard<std::mutex> l(pool->mtx);
            auto& p = pool->open[path];
            if (!p) p.reset(new FileSource());
            if (!p->isOpen() && !p->open(toWide(path), false)) return false;
            p->refreshSize();
            src = p.get();
        }
        if (!src->extendTo(end)) return false;
        bool ok = src->writeAt(off, buf, bs);
        if (!ok) {

            std::lock_guard<std::mutex> l(pool->mtx);
            auto it = pool->open.find(path);
            if (it != pool->open.end()) { it->second->close(); it->second.reset(); }
        }
        return ok;
    };
}

void invalidateTargetBlocks(Task* t, uint64_t tid, uint64_t bound) {
    const size_t bs = t->opts.blockSize;
    std::vector<BlockKey> victims;
    {
        std::lock_guard<std::mutex> ml(t->mapMtx);
        auto it = t->targetBlocks.find(tid);
        if (it != t->targetBlocks.end()) {
            for (uint64_t b : it->second)
                if (b * bs >= bound) victims.push_back(BlockKey{tid, b});
            for (const auto& k : victims) it->second.erase(k.blockIndex);
        }
    }
    TieredCache::SourceWriter w = makeWriter(t);
    for (const auto& k : victims) t->cache.invalidate(k, w);
}

void observeSize(Task* t, uint64_t tid, uint64_t curSz, const std::string& absPath) {
    uint64_t bound = 0;
    bool shrank = false;
    {
        std::lock_guard<std::mutex> l(t->mapMtx);
        if (t->targetMap.find(tid) == t->targetMap.end()) t->targetMap[tid] = absPath;
        auto it = t->targetSizes.find(tid);
        if (it != t->targetSizes.end() && curSz < (uint64_t)it->second) {
            shrank = true;
            bound = (curSz > t->opts.blockSize) ? curSz - t->opts.blockSize : 0;
        }
        t->targetSizes[tid] = (long long)curSz;
    }
    if (shrank) invalidateTargetBlocks(t, tid, bound);
}

bool Task::containsPath(const std::string& path) const {
    std::string p = normUtf8(path);

    for (const auto& oN : normObjects) {
        if (p == oN) return true;
        if (!oN.empty() && oN.back() != '\\' && p.size() > oN.size() &&
            p.compare(0, oN.size(), oN) == 0 && p[oN.size()] == '\\')
            return true;
    }
    return false;
}

Engine::~Engine() { stop(); }

uint64_t Engine::uptimeMs() const {
    if (m_startMs == 0) return 0;
    return (uint64_t)(nowMs() - (long long)m_startMs);
}

bool Engine::start(const std::string& configPath) {
    m_configPath = configPath;
    loadConfig();
    m_startMs = (uint64_t)nowMs();
    m_running = true;
    m_thread = std::thread([this] {
        while (m_running) {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    });
    return true;
}

void Engine::stop() {
    if (m_running) {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard<std::mutex> l(m_mtx);
        tasks = m_tasks;
        m_tasks.clear();
    }
    for (auto& t : tasks) {

        TieredCache::SourceWriter w = makeWriter(t.get());
        std::lock_guard<std::mutex> cl(t->cacheMtx);
        t->cache.stop(true, w);
    }
    std::map<std::string, RamDisk*> disks;
    {
        std::lock_guard<std::mutex> l(m_mtx);
        disks.swap(m_ramdisks);
    }
    for (auto& kv : disks) { kv.second->stop(); delete kv.second; }
}

bool Engine::addRamDisk(const std::string& name, size_t capacityBytes, size_t blockBytes) {

    if (!validTaskName(name) || capacityBytes == 0) return false;
    std::lock_guard<std::mutex> l(m_mtx);
    if (m_ramdisks.count(name)) return false;
    RamDisk* d = new RamDisk();
    if (!d->start(capacityBytes, blockBytes)) { delete d; return false; }
    m_ramdisks[name] = d;
    saveConfigLocked();
    return true;
}

bool Engine::removeRamDisk(const std::string& name) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_ramdisks.find(name);
    if (it == m_ramdisks.end()) return false;
    it->second->stop();
    delete it->second;
    m_ramdisks.erase(it);
    saveConfigLocked();
    return true;
}

bool Engine::ramDiskRead(const std::string& name, uint64_t offset, void* buf, size_t len) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_ramdisks.find(name);
    if (it == m_ramdisks.end()) return false;
    return it->second->readRange(offset, buf, len);
}

bool Engine::ramDiskWrite(const std::string& name, uint64_t offset, const void* buf, size_t len) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_ramdisks.find(name);
    if (it == m_ramdisks.end()) return false;
    return it->second->writeRange(offset, buf, len);
}

bool Engine::ramDiskReadPartial(const std::string& name, uint64_t offset, void* buf, size_t len) {
    std::lock_guard<std::mutex> l(m_mtx);
    auto it = m_ramdisks.find(name);
    if (it == m_ramdisks.end()) return false;
    return it->second->readPartial(offset, buf, len);
}

size_t Engine::ramDiskCount() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_ramdisks.size();
}

std::vector<std::string> Engine::ramDiskLines() const {
    std::lock_guard<std::mutex> l(m_mtx);
    std::vector<std::string> out;
    for (const auto& kv : m_ramdisks) {
        const RamDisk* d = kv.second;
        out.push_back(kv.first + " cap=" + fmtSize((long long)d->capacityBlocks() * d->blockBytes())
                     + " used=" + fmtSize((long long)d->usedBytes())
                     + " committed=" + fmtSize((long long)d->committedBytes())
                     + " block=" + fmtSize((long long)d->blockBytes())
                     + " free=" + std::to_string(d->freeBlocks()));
    }
    return out;
}

bool Engine::addTask(const std::string& name, const std::vector<std::string>& objects,
                     TieredCache::Options opts) {
    if (!validTaskName(name) || objects.empty() || objects.size() > 16) return false;

    if (!validTaskName(opts.l2BaseName)) return false;

    const size_t bs = opts.blockSize;
    if (bs < 4096 || bs > 512 * 1024 || (bs & (bs - 1)) != 0) return false;

    TieredCache::Options o = opts;
    const int wm = static_cast<int>(o.writeMode);
    if (wm < 0 || wm > 4) o.writeMode = TieredCache::WriteMode::Smart;
    std::lock_guard<std::mutex> l(m_mtx);
    if (findTaskLocked(name)) return false;
    auto t = std::make_shared<Task>();
    t->name = name;
    t->opts = o;

    for (const auto& obj : objects) {
        std::string abs = fromWide(makeAbsolute(toWide(obj).c_str()));
        t->objects.push_back(abs);
        t->normObjects.push_back(normUtf8(abs));
        std::wstring w = toWide(abs);
        DWORD attr = GetFileAttributesW(w.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            std::vector<std::string> files;
            enumerateFiles(abs, files, 50000);
            for (auto& f : files) {
                FileSource src;
                if (src.open(toWide(f), true)) {
                    std::lock_guard<std::mutex> ml(t->mapMtx);
                    t->targetMap[src.targetId()] = f;
                    src.close();
                }
            }
        } else {
            FileSource src;
            if (src.open(w, true)) {
                std::lock_guard<std::mutex> ml(t->mapMtx);
                t->targetMap[src.targetId()] = abs;
                src.close();
            }
        }
    }
    if (!t->cache.start(t->opts, &t->stats)) return false;
    t->cache.handleBoot(makeTaskSourceReader(t), makeWriter(t.get()));
    m_tasks.push_back(t);
    saveConfigLocked();
    return true;
}

bool Engine::removeTask(const std::string& name) {
    std::shared_ptr<Task> t;
    {
        std::lock_guard<std::mutex> l(m_mtx);
        for (size_t i = 0; i < m_tasks.size(); ++i)
            if (m_tasks[i]->name == name) { t = m_tasks[i]; m_tasks.erase(m_tasks.begin() + i); break; }
    }
    if (!t) return false;

    TieredCache::SourceWriter w = makeWriter(t.get());
    {
        std::lock_guard<std::mutex> cl(t->cacheMtx);
        t->cache.stop(true, w);
    }
    std::lock_guard<std::mutex> l(m_mtx);
    saveConfigLocked();
    return true;
}

Task* Engine::findTaskLocked(const std::string& name) const {
    for (const auto& t : m_tasks)
        if (t->name == name) return t.get();
    return nullptr;
}

std::shared_ptr<Task> Engine::findTask(const std::string& name) const {
    std::lock_guard<std::mutex> l(m_mtx);
    for (const auto& t : m_tasks)
        if (t->name == name) return t;
    return nullptr;
}

std::vector<std::shared_ptr<Task>> Engine::tasks() const {
    std::vector<std::shared_ptr<Task>> v;
    std::lock_guard<std::mutex> l(m_mtx);
    v = m_tasks;
    return v;
}

size_t Engine::taskCount() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return m_tasks.size();
}

bool Engine::readFile(const std::string& path, uint64_t offset, void* buf, size_t len,
                      const std::string& taskName) {
    std::shared_ptr<Task> t = findTask(taskName);
    if (!t) return false;
    const std::wstring w = toWide(path);
    FileSource src;
    if (!src.open(w, true)) return false;
    uint64_t tid = src.targetId();
    const size_t bs = t->opts.blockSize;
    const uint64_t fileSz = src.size();

    const std::string absPath = fromWide(makeAbsolute(w.c_str()));

    if (!t->containsPath(absPath)) return false;

    std::lock_guard<std::mutex> cl(t->cacheMtx);
    observeSize(t.get(), tid, fileSz, absPath);

    const bool bypass = t->paused.load();
    uint8_t* out = static_cast<uint8_t*>(buf);

    std::vector<uint8_t>& b = blockBuf(bs);
    bool srcShort = false;
    TieredCache::SourceReader r = [&](const BlockKey& k, void* p) -> bool {
        uint64_t got = src.readAt(k.blockIndex * bs, p, bs);
        if (got < bs) { srcShort = true; return false; }
        return true;
    };

    std::vector<uint64_t> seen;
    auto commitSeen = [&]() {
        if (seen.empty()) return;
        std::lock_guard<std::mutex> l(t->mapMtx);
        auto& s = t->targetBlocks[tid];
        for (uint64_t v : seen) s.insert(v);
        seen.clear();
    };
    uint64_t pos = offset;
    size_t remaining = len;
    while (remaining > 0) {
        uint64_t blockIdx = pos / bs;
        uint64_t blockOff = blockIdx * bs;
        size_t take = std::min<size_t>(remaining, bs - (pos - blockOff));
        if (bypass || blockOff + bs > fileSz) {

            uint64_t got = src.readAt(pos, out + (pos - offset), take);
            if (got != take) { commitSeen(); src.close(); return false; }
        } else {
            BlockKey key{tid, blockIdx};
            // A full block-aligned read can go straight into the caller buffer,
            // saving one copy of the block through the scratch buffer.
            void* dst = (take == bs) ? static_cast<void*>(out + (pos - offset))
                                     : static_cast<void*>(b.data());
            t->cache.readBlock(key, dst, r);
            if (srcShort) {

                uint64_t got = src.readAt(pos, out + (pos - offset), take);
                if (got != take) { commitSeen(); src.close(); return false; }
            } else {

                if (take != bs)
                    std::memcpy(out + (pos - offset), b.data() + (pos - blockOff), take);
                seen.push_back(blockIdx);
            }
        }
        pos += take;
        remaining -= take;
    }
    commitSeen();
    src.close();
    return true;
}

bool Engine::writeFile(const std::string& path, uint64_t offset, const void* buf, size_t len,
                       const std::string& taskName) {
    std::shared_ptr<Task> t = findTask(taskName);
    if (!t) return false;
    const std::wstring wp = toWide(path);
    FileSource src;
    if (!src.open(wp, false)) return false;
    uint64_t tid = src.targetId();
    const size_t bs = t->opts.blockSize;

    const std::string absPath = fromWide(makeAbsolute(wp.c_str()));

    if (!t->containsPath(absPath)) return false;

    std::lock_guard<std::mutex> cl(t->cacheMtx);
    observeSize(t.get(), tid, src.size(), absPath);
    const bool bypass = t->paused.load() || t->frozen.load();
    const uint8_t* in = static_cast<const uint8_t*>(buf);
    uint64_t pos = offset;
    size_t remaining = len;
    TieredCache::SourceWriter w = makeWriter(t.get());
    std::vector<uint8_t>& b = blockBuf(bs);
    while (remaining > 0) {
        uint64_t blockIdx = pos / bs;
        uint64_t blockOff = blockIdx * bs;
        size_t take = std::min<size_t>(remaining, bs - (pos - blockOff));
        uint64_t newEnd = pos + take;
        uint64_t curSize = src.size();
        if (newEnd > curSize) {

            if (!extendFile(path, newEnd)) { src.close(); return false; }
            curSize = newEnd;
            src.refreshSize();
        }
        if (blockOff + bs > curSize) {

            if (!src.writeAt(pos, in + (pos - offset), take)) { src.close(); return false; }
        } else if (bypass) {

            t->cache.flushKey(BlockKey{tid, blockIdx}, w);
            if (!src.writeAt(pos, in + (pos - offset), take)) { src.close(); return false; }
            t->cache.invalidate(BlockKey{tid, blockIdx}, w);
        } else {

            uint64_t got = src.readAt(blockOff, b.data(), bs);
            if (got < bs) {

                if (!src.writeAt(pos, in + (pos - offset), take)) { src.close(); return false; }
            } else {
                size_t srcOff = (pos - blockOff);
                std::memcpy(b.data() + srcOff, in + (pos - offset), take);
                BlockKey key{tid, blockIdx};
                if (!t->cache.writeBlock(key, b.data(), w)) {

                    if (!src.writeAt(pos, in + (pos - offset), take)) { src.close(); return false; }
                }
            }
        }
        pos += take;
        remaining -= take;
    }
    {
        std::lock_guard<std::mutex> l(t->mapMtx);
        t->targetSizes[tid] = (long long)src.size();
    }
    src.close();
    return true;
}

bool Engine::trimRange(const std::string& path, uint64_t offset, size_t len,
                       const std::string& taskName) {
    std::shared_ptr<Task> t = findTask(taskName);
    if (!t) return false;
    FileSource probe;
    if (!probe.open(toWide(path), true)) return false;
    uint64_t tid = probe.targetId();
    probe.close();
    const std::string absPath = fromWide(makeAbsolute(toWide(path).c_str()));
    if (!t->containsPath(absPath)) return false;
    const size_t bs = t->opts.blockSize;

    if (bs == 0 || len > (uint64_t)-1 - offset) return false;
    const uint64_t end = offset + len;
    std::lock_guard<std::mutex> cl(t->cacheMtx);
    bool any = false;
    std::vector<uint64_t> trimmed;
    for (uint64_t blockIdx = offset / bs; blockIdx * bs < end; ++blockIdx) {
        BlockKey key{tid, blockIdx};
        if (t->cache.trimBlock(key)) {
            any = true;
            trimmed.push_back(blockIdx);
        }
    }
    if (!trimmed.empty()) {
        std::lock_guard<std::mutex> l(t->mapMtx);
        auto& s = t->targetBlocks[tid];
        for (uint64_t v : trimmed) s.erase(v);
    }
    return any;
}

bool Engine::freezeTask(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t || t->frozen.load()) return false;
    t->frozen = true;
    TieredCache::SourceWriter w = makeWriter(t.get());
    std::lock_guard<std::mutex> cl(t->cacheMtx);
    t->cache.flushAll(false, w);
    return true;
}

bool Engine::unfreezeTask(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t) return false;
    t->frozen = false;
    return true;
}

bool Engine::pauseTask(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t) return false;
    t->paused = true;
    return true;
}

bool Engine::resumeTask(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t) return false;
    t->paused = false;
    return true;
}

bool Engine::flushTask(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t) return false;
    TieredCache::SourceWriter w = makeWriter(t.get());
    std::lock_guard<std::mutex> cl(t->cacheMtx);
    t->cache.flushAll(false, w);
    return true;
}

bool Engine::clearTask(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t) return false;
    TieredCache::SourceWriter w = makeWriter(t.get());
    {
        std::lock_guard<std::mutex> cl(t->cacheMtx);
        t->cache.clearAll(w);
    }
    std::lock_guard<std::mutex> l(t->mapMtx);
    t->targetBlocks.clear();
    return true;
}

void Engine::tick() {
    bool idle = systemIdle();
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard<std::mutex> l(m_mtx);
        tasks = m_tasks;
    }
    for (auto& t : tasks) {
        if (t->paused.load() || t->frozen.load()) continue;
        TieredCache::SourceWriter w = makeWriter(t.get());

        std::lock_guard<std::mutex> cl(t->cacheMtx);
        t->cache.tick(idle, w);
    }
}

bool Engine::systemIdle() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return true;
    auto q = [](const FILETIME& ft) -> long long {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return (long long)u.QuadPart;
    };
    long long i = q(idle), k = q(kernel), u = q(user);

    long long busy = (k - i) + u;
    long long total = k + u;
    if (total <= 0) return true;

    return 100LL * busy < 5LL * total;
}

std::string Engine::statsJson(const std::string& taskName) const {
    std::shared_ptr<Task> t = findTask(taskName);
    if (!t) return "{}";
    return t->stats.toJson();
}

std::string Engine::statsJsonAll() const {
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard<std::mutex> l(m_mtx);
        tasks = m_tasks;
    }
    std::string out = "{\"tasks\": {";

    for (size_t i = 0; i < tasks.size(); ++i) {
        out += "\"";
        out += tasks[i]->name;
        out += "\": " + tasks[i]->stats.toJson();
        if (i + 1 < tasks.size()) out += ", ";
    }
    out += "}}\n";
    return out;
}

std::string Engine::taskSection(const std::string& name) const {
    return "task." + name;
}

void Engine::writeTaskConfig(Config& cfg, const Task& t) const {
    std::string sec = taskSection(t.name);
    std::string objs;
    for (size_t i = 0; i < t.objects.size(); ++i) {
        if (i) objs += ";";
        objs += escSep(t.objects[i]);
    }
    cfg.set(sec, "objects", objs);
    cfg.set(sec, "strategy",
            t.opts.strategy == TieredCache::Strategy::ReadWrite ? "2"
            : t.opts.strategy == TieredCache::Strategy::ReadOnly ? "1" : "3");
    cfg.set(sec, "l1Bytes", std::to_string(t.opts.l1Bytes));
    cfg.set(sec, "l2Bytes", std::to_string(t.opts.l2Bytes));
    cfg.set(sec, "blockSize", std::to_string(t.opts.blockSize));
    cfg.set(sec, "numaAware", t.opts.numaAware ? "1" : "0");
    cfg.set(sec, "independentRwSpace", t.opts.independentRwSpace ? "1" : "0");
    cfg.set(sec, "l1WritePercent", std::to_string(t.opts.l1WritePercent));
    cfg.set(sec, "l2WritePercent", std::to_string(t.opts.l2WritePercent));
    cfg.set(sec, "l2Dir", t.opts.l2Dir);
    cfg.set(sec, "l2BaseName", t.opts.l2BaseName);
    cfg.set(sec, "deferWrite", t.opts.deferWrite ? "1" : "0");
    cfg.set(sec, "deferSeconds", std::to_string(t.opts.deferSeconds));
    cfg.set(sec, "writeMode", std::to_string((int)t.opts.writeMode));
    cfg.set(sec, "ignoreBusy", t.opts.ignoreBusy ? "1" : "0");
    cfg.set(sec, "releaseAfterWrite", t.opts.releaseAfterWrite ? "1" : "0");
    cfg.set(sec, "flushOnStandby", t.opts.flushOnStandby ? "1" : "0");
    cfg.set(sec, "l1ToL2", t.opts.l1ToL2 ? "1" : "0");
    cfg.set(sec, "skipFlushOnShutdown", t.opts.skipFlushOnShutdown ? "1" : "0");
    cfg.set(sec, "l2BusyCollectIntervalS", std::to_string(t.opts.l2BusyCollectIntervalS));
    cfg.set(sec, "l2ResetOnBoot", t.opts.l2ResetOnBoot ? "1" : "0");
    cfg.set(sec, "l2SkipVerifyOnCrash", t.opts.l2SkipVerifyOnCrash ? "1" : "0");
    cfg.set(sec, "prefetchLast", t.opts.prefetchLast ? "1" : "0");
    cfg.set(sec, "prefetchAtBoot", t.opts.prefetchAtBoot ? "1" : "0");
    cfg.set(sec, "lockPrefetchContent", t.opts.lockPrefetchContent ? "1" : "0");
    cfg.set(sec, "prefetchFromL2", t.opts.prefetchFromL2 ? "1" : "0");

    std::string tm;
    {
        std::lock_guard<std::mutex> l(t.mapMtx);
        for (const auto& kv : t.targetMap) {
            if (!tm.empty()) tm += ";";
            tm += std::to_string(kv.first) + "=" + escSep(kv.second);
        }
    }
    cfg.set(sec, "targetMap", tm);
}

bool Engine::loadTaskConfig(Task& t, Config& cfg, const std::string& sec) {
    std::string objs = cfg.getStr(sec, "objects");
    if (objs.empty()) return false;
    std::string item;
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i] == ';') {
            if (!item.empty() && t.objects.size() < 16) t.objects.push_back(unescSep(item));
            item.clear();
        } else item += objs[i];
    }
    if (!item.empty() && t.objects.size() < 16) t.objects.push_back(unescSep(item));
    for (const auto& o : t.objects) t.normObjects.push_back(normUtf8(o));

    long long strat = cfg.getInt(sec, "strategy", 2);
    t.opts.strategy = strat == 1 ? TieredCache::Strategy::ReadOnly
                   : strat == 3 ? TieredCache::Strategy::WriteOnly
                                : TieredCache::Strategy::ReadWrite;
    t.opts.l1Bytes = static_cast<size_t>(cfg.getInt(sec, "l1Bytes", 0));
    t.opts.l2Bytes = static_cast<size_t>(cfg.getInt(sec, "l2Bytes", 0));

    {
        size_t bsz = static_cast<size_t>(cfg.getInt(sec, "blockSize", 65536));
        if (bsz < 4096 || bsz > 512 * 1024 || (bsz & (bsz - 1)) != 0) bsz = 64 * 1024;
        t.opts.blockSize = bsz;
    }
    t.opts.numaAware = cfg.getBool(sec, "numaAware", true);
    t.opts.independentRwSpace = cfg.getBool(sec, "independentRwSpace", false);
    t.opts.l1WritePercent = (int)cfg.getInt(sec, "l1WritePercent", 50);
    t.opts.l2WritePercent = (int)cfg.getInt(sec, "l2WritePercent", 50);
    t.opts.l2Dir = cfg.getStr(sec, "l2Dir");

    t.opts.l2BaseName = cfg.getStr(sec, "l2BaseName", t.name);
    if (!validTaskName(t.opts.l2BaseName)) t.opts.l2BaseName = t.name;
    t.opts.deferWrite = cfg.getBool(sec, "deferWrite", true);
    t.opts.deferSeconds = (int)cfg.getInt(sec, "deferSeconds", 10);

    {
        int wm = (int)cfg.getInt(sec, "writeMode", 1);
        if (wm < 0 || wm > 4) wm = 1;
        t.opts.writeMode = static_cast<TieredCache::WriteMode>(wm);
    }
    t.opts.ignoreBusy = cfg.getBool(sec, "ignoreBusy", false);
    t.opts.releaseAfterWrite = cfg.getBool(sec, "releaseAfterWrite", false);
    t.opts.flushOnStandby = cfg.getBool(sec, "flushOnStandby", true);
    t.opts.l1ToL2 = cfg.getBool(sec, "l1ToL2", false);
    t.opts.skipFlushOnShutdown = cfg.getBool(sec, "skipFlushOnShutdown", false);
    t.opts.l2BusyCollectIntervalS = (int)cfg.getInt(sec, "l2BusyCollectIntervalS", 30);
    t.opts.l2ResetOnBoot = cfg.getBool(sec, "l2ResetOnBoot", false);
    t.opts.l2SkipVerifyOnCrash = cfg.getBool(sec, "l2SkipVerifyOnCrash", false);
    t.opts.prefetchLast = cfg.getBool(sec, "prefetchLast", false);
    t.opts.prefetchAtBoot = cfg.getBool(sec, "prefetchAtBoot", false);
    t.opts.lockPrefetchContent = cfg.getBool(sec, "lockPrefetchContent", false);
    t.opts.prefetchFromL2 = cfg.getBool(sec, "prefetchFromL2", false);

    std::string tm = cfg.getStr(sec, "targetMap");
    size_t off = 0;
    while (off < tm.size()) {
        size_t sc = tm.find(';', off);
        std::string part = tm.substr(off, sc == std::string::npos ? std::string::npos : sc - off);
        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            try {
                uint64_t id = std::stoull(part.substr(0, eq));
                t.targetMap[id] = unescSep(part.substr(eq + 1));
            } catch (...) {}
        }
        if (sc == std::string::npos) break;
        off = sc + 1;
    }
    return true;
}

bool Engine::saveConfigLocked() const {
    if (m_configPath.empty()) return false;
    Config cfg;
    for (const auto& t : m_tasks) writeTaskConfig(cfg, *t);
    for (const auto& kv : m_ramdisks) {
        std::string sec = "ramdisk." + kv.first;
        cfg.set(sec, "capacity", std::to_string((long long)kv.second->capacityBlocks() * kv.second->blockBytes()));
        cfg.set(sec, "block", std::to_string((long long)kv.second->blockBytes()));
    }
    return cfg.save(m_configPath);
}

bool Engine::saveConfig() const {
    std::lock_guard<std::mutex> l(m_mtx);
    return saveConfigLocked();
}

bool Engine::loadConfig() {
    if (m_configPath.empty() || !fileExists(m_configPath)) return false;
    Config cfg;
    if (!cfg.load(m_configPath)) return false;
    {
        std::lock_guard<std::mutex> l(m_mtx);
        const auto& all = cfg.all();
        for (const auto& sec : all) {
            const std::string& secName = sec.first;
            if (secName.rfind("task.", 0) == 0) {
                std::string name = secName.substr(5);
                if (!validTaskName(name)) continue;
                auto task = std::make_shared<Task>();
                task->name = name;
                if (!loadTaskConfig(*task, cfg, secName)) continue;
                if (!task->cache.start(task->opts, &task->stats)) continue;
                task->cache.handleBoot(makeTaskSourceReader(task), makeWriter(task.get()));
                m_tasks.push_back(task);
            } else if (secName.rfind("ramdisk.", 0) == 0) {
                std::string name = secName.substr(8);
                if (!validTaskName(name)) continue;
                long long cap = cfg.getInt(secName, "capacity", 0);
                long long blk = cfg.getInt(secName, "block", 65536);
                if (cap <= 0) continue;
                RamDisk* d = new RamDisk();
                if (!d->start(static_cast<size_t>(cap), static_cast<size_t>(blk))) {
                    delete d;
                    continue;
                }
                m_ramdisks[name] = d;
            }
        }
    }
    return true;
}

TieredCache::SourceReader makeTaskSourceReader(const std::shared_ptr<Task>& t) {

    auto pool = std::make_shared<FilePool>();
    return [t, pool](const BlockKey& key, void* buf) -> bool {
        std::string path;
        {
            std::lock_guard<std::mutex> l(t->mapMtx);
            auto it = t->targetMap.find(key.targetId);
            if (it == t->targetMap.end()) return false;
            path = it->second;
        }
        const uint64_t bs = t->opts.blockSize;
        FileSource* src = nullptr;
        {
            std::lock_guard<std::mutex> l(pool->mtx);
            auto& p = pool->open[path];
            if (!p) p.reset(new FileSource());
            if (!p->isOpen() && !p->open(toWide(path), true)) return false;
            p->refreshSize();
            src = p.get();
        }
        uint64_t got = src->readAt(key.blockIndex * bs, buf, bs);
        if (got < bs) {

            std::lock_guard<std::mutex> l(pool->mtx);
            auto it = pool->open.find(path);
            if (it != pool->open.end()) { it->second->close(); it->second.reset(); }
        }
        return got == bs;
    };
}

std::string Engine::diagReport() const {
    Uimmi::Info u = Uimmi::detect();
    std::string out;
    out += "VeloxDisk 1.0.0 (user-mode build)\n";
    out += "uptime=" + std::to_string((long long)(uptimeMs() / 1000)) + "s\n";
    out += "admin=" + std::string(IsUserAnAdmin() ? "yes" : "no") + "\n";
    out += "os=" + u.describe() + "\n";
    out += "systemIdle=" + std::string(systemIdle() ? "yes" : "no") + "\n";
    const std::vector<std::shared_ptr<Task>> snap = tasks();
    out += "tasks=" + std::to_string(snap.size()) + "\n";
    for (const auto& t : snap) {
        const TieredCache::Options& o = t->opts;
        std::string objs;
        for (size_t i = 0; i < t->objects.size(); ++i) { if (i) objs += "; "; objs += t->objects[i]; }
        out += "  [" + t->name + "] state="
             + std::string(t->paused ? "paused" : (t->frozen ? "frozen" : "active")) + "\n";
        out += "    objects=" + objs + "\n";
        out += "    l1=" + fmtSize((long long)o.l1Bytes) + " l2=" + fmtSize((long long)o.l2Bytes)
             + " block=" + fmtSize((long long)o.blockSize)
             + " strategy=" + std::string(o.strategy == TieredCache::Strategy::ReadWrite ? "ReadWrite"
                    : o.strategy == TieredCache::Strategy::ReadOnly ? "ReadOnly" : "WriteOnly")
             + " defer=" + std::to_string(o.deferSeconds) + "s\n";
        out += "    l1Used=" + fmtSize((long long)t->cache.l1UsedBytes())
             + " l2Used=" + fmtSize((long long)t->cache.l2UsedBytes())
             + " dirtyBlocks=" + std::to_string((long long)t->stats.dirtyBlocks) + "\n";
        out += "    reads: total=" + fmtSize(t->stats.readTotal)
             + " cached=" + fmtSize(t->stats.bytesReadCache)
             + " l2=" + fmtSize(t->stats.readL2Bytes)
             + " hitsRam=" + std::to_string((long long)t->stats.readHits)
             + " hitsSsd=" + std::to_string((long long)t->stats.readHitsSsd)
             + " misses=" + std::to_string((long long)t->stats.readMisses) + "\n";
        out += "    writes: requested=" + fmtSize(t->stats.writeRequestedTotal)
             + " toCache=" + fmtSize(t->stats.writeL1L2Total)
             + " toDisk=" + fmtSize(t->stats.writeDiskTotal)
             + " flushed=" + fmtSize(t->stats.bytesFlushed) + "\n";
    }
    std::vector<std::string> rds = ramDiskLines();
    out += "ramdisks=" + std::to_string(rds.size()) + "\n";
    for (const auto& line : rds) out += "  " + line + "\n";
    return out;
}

std::string Engine::nfoText() const {
    Uimmi::Info u = Uimmi::detect();
    std::string text;
    text += "; VeloxDisk NFO export\n";
    text += "; generated=" + std::to_string((long long)nowMs()) + "\n\n";
    text += "[VeloxDisk]\n";
    text += "version=1.0.0\n";
    text += "build=user-mode\n";
    text += "uptimeMs=" + std::to_string((long long)uptimeMs()) + "\n";
    text += "admin=" + std::string(IsUserAnAdmin() ? "1" : "0") + "\n";
    text += "os=" + u.describe() + "\n\n";
    const std::vector<std::shared_ptr<Task>> snap = tasks();
    for (const auto& t : snap) {
        const TieredCache::Options& o = t->opts;
        std::string objs;
        for (size_t i = 0; i < t->objects.size(); ++i) { if (i) objs += ";"; objs += t->objects[i]; }
        text += "[Task " + t->name + "]\n";
        text += "state=" + std::string(t->paused ? "paused" : (t->frozen ? "frozen" : "active")) + "\n";
        text += "objects=" + objs + "\n";
        text += "l1Bytes=" + std::to_string((long long)o.l1Bytes) + "\n";
        text += "l2Bytes=" + std::to_string((long long)o.l2Bytes) + "\n";
        text += "blockSize=" + std::to_string((long long)o.blockSize) + "\n";
        text += "strategy=" + std::string(o.strategy == TieredCache::Strategy::ReadWrite ? "2"
                : o.strategy == TieredCache::Strategy::ReadOnly ? "1" : "3") + "\n";
        text += "l1Used=" + fmtSize((long long)t->cache.l1UsedBytes()) + "\n";
        text += "l2Used=" + fmtSize((long long)t->cache.l2UsedBytes()) + "\n";
        text += "dirtyBlocks=" + std::to_string((long long)t->stats.dirtyBlocks) + "\n";
        text += "readTotal=" + fmtSize(t->stats.readTotal) + "\n";
        text += "bytesReadCache=" + fmtSize(t->stats.bytesReadCache) + "\n";
        text += "bytesReadSource=" + fmtSize(t->stats.bytesReadSource) + "\n";
        text += "writeRequestedTotal=" + fmtSize(t->stats.writeRequestedTotal) + "\n";
        text += "writeL1L2Total=" + fmtSize(t->stats.writeL1L2Total) + "\n";
        text += "writeDiskTotal=" + fmtSize(t->stats.writeDiskTotal) + "\n";
        text += "bytesFlushed=" + fmtSize(t->stats.bytesFlushed) + "\n";
        text += "readHits=" + std::to_string((long long)t->stats.readHits) + "\n";
        text += "readHitsSsd=" + std::to_string((long long)t->stats.readHitsSsd) + "\n";
        text += "readMisses=" + std::to_string((long long)t->stats.readMisses) + "\n";
        text += "journalReplays=" + std::to_string((long long)t->stats.journalReplays) + "\n";
        text += "errors=" + std::to_string((long long)t->stats.errors) + "\n";
        text += "\n[Stats " + t->name + "]\n" + t->stats.toJson() + "\n\n";
    }
    size_t i = 0;
    for (const auto& line : ramDiskLines()) {
        text += "[RamDisk " + std::to_string(++i) + "]\n" + line + "\n\n";
    }
    return text;
}

std::string Engine::perfReport(const std::string& taskName) const {
    std::shared_ptr<Task> t = findTask(taskName);
    if (!t) return "";
    double upSec = (double)uptimeMs() / 1000.0;
    if (upSec < 1.0) upSec = 1.0;
    std::string out = statsJson(taskName);
    char line[512];
    std::snprintf(line, sizeof(line),
                  "perf uptime=%.1fs readCacheMBps=%.3f writeCacheMBps=%.3f readHitRate=%.2f%%\n",
                  upSec,
                  (double)t->stats.bytesReadCache / upSec / 1048576.0,
                  (double)t->stats.writeL1L2Total / upSec / 1048576.0,
                  t->stats.readTotal > 0
                      ? 100.0 * (double)t->stats.bytesReadCache / (double)t->stats.readTotal
                      : 0.0);
    out += "\n";
    out += line;
    return out;
}

int Engine::verifyL2Task(const std::string& name) {
    std::shared_ptr<Task> t = findTask(name);
    if (!t) return -1;

    std::lock_guard<std::mutex> cl(t->cacheMtx);
    return t->cache.verifyL2(makeTaskSourceReader(t));
}

std::vector<std::pair<std::string, const RamDisk*>> Engine::ramDisks() const {
    std::lock_guard<std::mutex> l(m_mtx);
    std::vector<std::pair<std::string, const RamDisk*>> v;
    v.reserve(m_ramdisks.size());
    for (const auto& kv : m_ramdisks) v.push_back(kv);
    return v;
}

}
