#pragma once
#include "../cache/tieredcache.h"
#include "../common/config.h"
#include "../common/stats.h"
#include "../ramdisk/ramdisk.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace vd {

class Task {
public:
    std::string name;
    std::vector<std::string> objects;

    std::vector<std::string> normObjects;
    TieredCache::Options opts;
    TieredCache cache;
    Stats stats;

    std::atomic<bool> frozen{false};
    std::atomic<bool> paused{false};

    bool containsPath(const std::string& path) const;

    mutable std::mutex mapMtx;
    std::map<uint64_t, std::string> targetMap;

    std::map<uint64_t, long long> targetSizes;
    std::map<uint64_t, std::set<uint64_t>> targetBlocks;

    mutable std::mutex cacheMtx;
};

class Engine {
public:
    Engine() = default;
    ~Engine();

    bool start(const std::string& configPath);
    void stop();

    bool addTask(const std::string& name, const std::vector<std::string>& objects,
                 TieredCache::Options opts);
    bool removeTask(const std::string& name);

    std::shared_ptr<Task> findTask(const std::string& name) const;
    Task* findTaskLocked(const std::string& name) const;
    std::vector<std::shared_ptr<Task>> tasks() const;
    size_t taskCount() const;

    bool readFile(const std::string& path, uint64_t offset, void* buf, size_t len,
                  const std::string& taskName);
    bool writeFile(const std::string& path, uint64_t offset, const void* buf, size_t len,
                   const std::string& taskName);
    bool trimRange(const std::string& path, uint64_t offset, size_t len,
                   const std::string& taskName);

    bool freezeTask(const std::string& name);
    bool unfreezeTask(const std::string& name);
    bool pauseTask(const std::string& name);
    bool resumeTask(const std::string& name);
    bool flushTask(const std::string& name);
    bool clearTask(const std::string& name);

    void tick();
    std::string statsJson(const std::string& taskName) const;
    std::string statsJsonAll() const;
    bool saveConfig() const;
    bool saveConfigLocked() const;
    bool loadConfig();

    static bool systemIdle();
    uint64_t uptimeMs() const;

    bool addRamDisk(const std::string& name, size_t capacityBytes, size_t blockBytes);
    bool removeRamDisk(const std::string& name);
    bool ramDiskRead(const std::string& name, uint64_t offset, void* buf, size_t len);
    bool ramDiskWrite(const std::string& name, uint64_t offset, const void* buf, size_t len);
    bool ramDiskReadPartial(const std::string& name, uint64_t offset, void* buf, size_t len);
    size_t ramDiskCount() const;
    std::vector<std::string> ramDiskLines() const;

    std::string diagReport() const;
    std::string nfoText() const;
    std::string perfReport(const std::string& taskName) const;
    int verifyL2Task(const std::string& taskName);
    std::vector<std::pair<std::string, const RamDisk*>> ramDisks() const;

private:
    bool loadTaskConfig(Task& t, Config& cfg, const std::string& section);
    void writeTaskConfig(Config& cfg, const Task& t) const;
    std::string taskSection(const std::string& name) const;

    std::string m_configPath;
    std::vector<std::shared_ptr<Task>> m_tasks;
    std::map<std::string, RamDisk*> m_ramdisks;
    uint64_t m_startMs = 0;
    mutable std::mutex m_mtx;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

TieredCache::SourceReader makeTaskSourceReader(const std::shared_ptr<Task>& t);

}
