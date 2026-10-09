#include "../engine/engine.h"
#include "../cache/source.h"
#include "../common/util.h"
#include "../uimmi/uimmi.h"
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <atomic>

using namespace vd;

extern "C" BOOL IsUserAnAdmin(void);

static const wchar_t* kPipeName = L"\\\\.\\pipe\\VeloxDiskCtrl";

namespace {

void printUsage() {
    std::printf(
        "VeloxDisk command line tool\n"
        "  vd.exe serve [--config <path>]     start resident engine + control server\n"
        "  vd.exe status                       list tasks and states\n"
        "  vd.exe stats [--task <name>]       statistics (JSON)\n"
        "  vd.exe task add <name> <objs> [k=v ...]\n"
        "        objs: semicolon-separated files or directories (max 16)\n"
        "        k=v: l1=512MB l2=1GB granularity=64KB l2dir=D:\\cache l2name=vd\n"
        "              strategy=2(ReadWrite|1=ReadOnly|3=WriteOnly) defer=10\n"
        "              mode=0..4 (Original|Smart|Idle|Buffer|Balanced)\n"
        "              independent=1 l1w=50 l2w=50 l1tol2=1 ignorebusy=1\n"
        "  vd.exe task remove <name>\n"
        "  vd.exe pause|resume|freeze|unfreeze <name>\n"
        "  vd.exe flush [name|all]            flush pending writes\n"
        "  vd.exe clear [name|all]            flush then wipe cache content\n"
        "  vd.exe l2verify <name>             verify L2 content against source\n"
        "  vd.exe ramdisk add <name> <capacity> [block]   create RAM disk\n"
        "  vd.exe ramdisk remove <name>       remove RAM disk\n"
        "  vd.exe ramdisk status              list RAM disks\n"
        "  vd.exe ramdisk read <name> <offset> <len>      hex dump (block-aligned offset, len 1..4096)\n"
        "  vd.exe ramdisk write <name> <offset> <hex>     write block-aligned data\n"
        "  vd.exe diag                        engine diagnostic report\n"
        "  vd.exe diag nfo [path]             export .nfo text report\n"
        "  vd.exe diag verify <name>          force L2 integrity check\n"
        "  vd.exe diag perf <name>            performance counters + throughput\n"
        "  vd.exe shutdown                    tell the resident engine to stop cleanly\n"
        "  vd.exe version\n"
        "  vd.exe ?\n");
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

bool sendCommand(const std::string& cmd, std::string& replyOut) {
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string line = cmd + "\n";
    DWORD sent = 0;
    if (!WriteFile(h, line.data(), (DWORD)line.size(), &sent, nullptr) || sent != line.size()) {
        CloseHandle(h);
        return false;
    }
    std::string reply;
    char buf[8192];
    DWORD got = 0;
    while (true) {
        if (!ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) break;
        buf[got] = 0;
        reply += buf;
        if (reply.find("<<<END") != std::string::npos) break;
    }
    CloseHandle(h);
    replyOut = reply;
    return true;
}

std::string bytesToHex(const void* data, size_t len) {
    static const char* h = "0123456789abcdef";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) { s.push_back(h[p[i] >> 4]); s.push_back(h[p[i] & 15]); }
    return s;
}

std::string quoteArg(const std::string& s) {
    if (s.find(' ') == std::string::npos) return s;
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    o += '"';
    return o;
}

bool hexToBytes(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2 != 0) return false;

    if (s.size() > 2 * 8 * 1024 * 1024) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = val(s[i]), lo = val(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

int runControl(const std::vector<std::string>& argv) {

    if (argv.empty()) { printUsage(); return 2; }
    std::string request = quoteArg(argv[0]);
    for (size_t i = 1; i < argv.size(); ++i) {
        request += " ";
        request += quoteArg(argv[i]);
    }

    std::string reply;
    if (!sendCommand(request, reply)) {
        std::fprintf(stderr, "error: engine is not running (start it with: vd.exe serve)\n");
        return 3;
    }

    std::string first;
    size_t nl = reply.find('\n');
    if (nl != std::string::npos) { first = reply.substr(0, nl); reply = reply.substr(nl + 1); }
    if (first.rfind("OK", 0) == 0) {
        std::printf("%s", reply.c_str());
        return 0;
    }
    std::fprintf(stderr, "error: %s\n", first.c_str());

    if (first.rfind("ERR usage:", 0) == 0) return 2;
    return 1;
}

bool isKnownOpt(const std::string& k) {
    static const char* keys[] = {"l1", "l2", "granularity", "block", "l2dir", "l2name",
        "strategy", "defer", "mode", "independent", "l1w", "l2w", "l1tol2",
        "ignorebusy", "flushstandby", "skipflush", "l2busyint", "l2reset",
        "l2skipverify", "prefetch", "prefetchboot", "lockprefetch", "prefetchl2",
        "deferwrite"};
    for (const char* p : keys) if (k == p) return true;
    return false;
}

}

namespace {

void handlePipeLine(Engine& eng, std::string line, std::string& out) {

    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto fail = [&](const std::string& msg) { out = "ERR " + msg + "\n<<<END\n"; };
    auto ok = [&](const std::string& payload) { out = "OK\n" + payload + "<<<END\n"; };

    std::vector<std::string> args;
    {
        std::string cur;
        bool inTok = false, inQ = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (inQ) {
                if (c == '\\' && i + 1 < line.size()) {
                    char e = line[++i];
                    if (e == '"' || e == '\\') cur += e;
                    else { cur += c; cur += e; }
                } else if (c == '"') inQ = false;
                else cur += c;
            } else if (c == '"') {
                inQ = true; inTok = true;
            } else if (c == ' ') {
                if (inTok) { args.push_back(cur); cur.clear(); inTok = false; }
            } else { cur += c; inTok = true; }
        }
        if (inTok) args.push_back(cur);
    }
    if (args.empty()) return fail("empty command");
    const std::string& cmd = args[0];

    if (cmd == "status" || cmd == "tasks") {
        std::string s;
        auto tasks = eng.tasks();
        s += std::to_string(tasks.size()) + " task(s)\n";
        for (auto& t : tasks) {
            s += t->name;
            s += t->paused.load() ? "  [paused]" : t->frozen.load() ? "  [frozen]" : "  [active]";
            s += "  objects=" + std::to_string(t->objects.size()) + "\n";
        }
        return ok(s);
    }
    if (cmd == "stats") {
        std::string name;
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] == "--task" && i + 1 < args.size()) name = args[i + 1];
        std::string s = name.empty() ? eng.statsJsonAll() : eng.statsJson(name);
        return ok(s);
    }
    if (cmd == "task" && args.size() >= 3 && args[1] == "add") {
        const std::string& name = args[2];

        std::string objs;
        std::vector<std::pair<std::string, std::string>> kv;
        for (size_t i = 3; i < args.size(); ++i) {
            size_t eq = args[i].find('=');
            if (eq != std::string::npos && isKnownOpt(args[i].substr(0, eq))) {
                kv.emplace_back(trim(args[i].substr(0, eq)), trim(args[i].substr(eq + 1)));
            } else {
                // 用 ';' 连接多个对象参数: 服务端按 ';' 切分。
                // (原来用空格连接, 导致两个未加引号的路径被合并成一个非法对象)
                if (!objs.empty()) objs += ";";
                objs += args[i];
            }
        }
        TieredCache::Options opts;
        for (auto& p : kv) {
            const std::string& k = p.first;
            const std::string& v = p.second;
            if (k == "l1") opts.l1Bytes = (size_t)Config::parseSize(v, 0);
            else if (k == "l2") opts.l2Bytes = (size_t)Config::parseSize(v, 0);
            else if (k == "granularity" || k == "block") opts.blockSize = (size_t)Config::parseSize(v, 65536);
            else if (k == "l2dir") opts.l2Dir = v;
            else if (k == "l2name") opts.l2BaseName = v;
            else if (k == "strategy") {
                int st = atoi(v.c_str());
                if (st == 1) opts.strategy = TieredCache::Strategy::ReadOnly;
                else if (st == 2) opts.strategy = TieredCache::Strategy::ReadWrite;
                else if (st == 3) opts.strategy = TieredCache::Strategy::WriteOnly;
                else return fail("usage: strategy must be 1..3");
            }
            else if (k == "defer") opts.deferSeconds = atoi(v.c_str());
            else if (k == "mode") {

                int m = atoi(v.c_str());
                if (m < 0 || m > 4) return fail("usage: mode must be 0..4");
                opts.writeMode = (TieredCache::WriteMode)m;
            }
            else if (k == "independent") opts.independentRwSpace = (atoi(v.c_str()) != 0);
            else if (k == "l1w") opts.l1WritePercent = atoi(v.c_str());
            else if (k == "l2w") opts.l2WritePercent = atoi(v.c_str());
            else if (k == "l1tol2") opts.l1ToL2 = (atoi(v.c_str()) != 0);
            else if (k == "ignorebusy") opts.ignoreBusy = (atoi(v.c_str()) != 0);
            else if (k == "flushstandby") opts.flushOnStandby = (atoi(v.c_str()) != 0);
            else if (k == "skipflush") opts.skipFlushOnShutdown = (atoi(v.c_str()) != 0);
            else if (k == "l2busyint") opts.l2BusyCollectIntervalS = atoi(v.c_str());
            else if (k == "l2reset") opts.l2ResetOnBoot = (atoi(v.c_str()) != 0);
            else if (k == "l2skipverify") opts.l2SkipVerifyOnCrash = (atoi(v.c_str()) != 0);
            else if (k == "prefetch") opts.prefetchLast = (atoi(v.c_str()) != 0);
            else if (k == "prefetchboot") opts.prefetchAtBoot = (atoi(v.c_str()) != 0);
            else if (k == "lockprefetch") opts.lockPrefetchContent = (atoi(v.c_str()) != 0);
            else if (k == "prefetchl2") opts.prefetchFromL2 = (atoi(v.c_str()) != 0);
            else if (k == "deferwrite") opts.deferWrite = (atoi(v.c_str()) != 0);
            else return fail("unknown option: " + k);
        }
        std::vector<std::string> objsList;
        std::string cur;
        for (size_t i = 0; i < objs.size(); ++i) {
            if (objs[i] == ';') { if (!cur.empty()) objsList.push_back(cur); cur.clear(); }
            else cur += objs[i];
        }
        if (!cur.empty()) objsList.push_back(cur);
        if (eng.addTask(name, objsList, opts)) return ok("task added\n");
        return fail("failed to add task (bad name, objects, capacity, or options)");
    }
    if (cmd == "task" && args.size() >= 3 && args[1] == "remove") {
        return eng.removeTask(args[2]) ? ok("task removed\n") : fail("task not found");
    }
    if (cmd == "pause") {
        return args.size() == 2 && eng.pauseTask(args[1]) ? ok("paused\n") : fail("task not found");
    }
    if (cmd == "resume") {
        return args.size() == 2 && eng.resumeTask(args[1]) ? ok("resumed\n") : fail("task not found");
    }
    if (cmd == "freeze") {
        return args.size() == 2 && eng.freezeTask(args[1]) ? ok("frozen\n") : fail("task not found");
    }
    if (cmd == "unfreeze") {
        return args.size() == 2 && eng.unfreezeTask(args[1]) ? ok("unfrozen\n") : fail("task not found");
    }
    if (cmd == "flush") {
        if (args.size() == 1 || args[1] == "all") {
            bool any = false;
            for (auto& t : eng.tasks()) any |= eng.flushTask(t->name);
            return ok(any ? "flushed all\n" : "no tasks\n");
        }
        return eng.flushTask(args[1]) ? ok("flushed\n") : fail("task not found");
    }
    if (cmd == "clear") {
        if (args.size() == 1 || args[1] == "all") {
            bool any = false;
            for (auto& t : eng.tasks()) any |= eng.clearTask(t->name);
            return ok(any ? "cleared all\n" : "no tasks\n");
        }
        return eng.clearTask(args[1]) ? ok("cleared\n") : fail("task not found");
    }
    if (cmd == "l2verify" && args.size() == 2) {
        int bad = eng.verifyL2Task(args[1]);
        if (bad < 0) return fail("task not found or no L2 tier");
        return ok("L2 verified\n");
    }
    if (cmd == "diag") {
        if (args.size() >= 2 && args[1] == "verify") {
            if (args.size() != 3) return fail("usage: diag verify <task>");
            if (!eng.findTask(args[2])) return fail("task not found");
            int bad = eng.verifyL2Task(args[2]);
            if (bad < 0) return fail("task has no L2 tier");
            return ok(std::string("L2 verified: ") + std::to_string(bad) +
                      " bad block(s) discarded\n");
        }
        if (args.size() >= 2 && args[1] == "perf") {
            if (args.size() != 3) return fail("usage: diag perf <task>");
            if (!eng.findTask(args[2])) return fail("task not found");
            std::string out = eng.perfReport(args[2]);
            if (out.empty()) return fail("task not found");
            return ok(out);
        }
        if (args.size() >= 2 && args[1] == "nfo") {
            std::string path;
            if (args.size() >= 3) path = args[2];
            if (path.empty()) {
                wchar_t ld[32768] = {0};
                DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", ld, 32768);
                path = fromWide(std::wstring(ld, n)) + "\\VeloxDisk\\diag.nfo";
            }
            std::string dir, base;
            splitPath(path, dir, base);
            if (!dir.empty() && !fileExists(dir) && !makeDirs(dir))
                return fail("cannot create directory: " + dir);
            std::string text = eng.nfoText();
            if (!writeFile(path, text)) return fail("cannot write: " + path);
            return ok("NFO written: " + path + "\n");
        }
        return ok(eng.diagReport());
    }
    if (cmd == "ramdisk") {
        if (args.size() >= 2 && args[1] == "add") {
            if (args.size() < 4) return fail("usage: ramdisk add <name> <capacity> [block]");
            long long cap = Config::parseSize(args[3], 0);
            long long blk = args.size() >= 5 ? Config::parseSize(args[4], 65536) : 65536;
            if (cap <= 0) return fail("bad capacity");
            if (eng.addRamDisk(args[2], (size_t)cap, (size_t)blk))
                return ok("RAM disk added\n");
            return fail("cannot add RAM disk (name exists or allocation failed)");
        }
        if (args.size() >= 3 && args[1] == "remove") {
            return eng.removeRamDisk(args[2]) ? ok("RAM disk removed\n") : fail("RAM disk not found");
        }
        if (args.size() >= 3 && args[1] == "read") {
            if (args.size() != 5) return fail("usage: ramdisk read <name> <offset> <len>");
            uint64_t off = strtoull(args[3].c_str(), nullptr, 10);
            long long len = strtoll(args[4].c_str(), nullptr, 10);
            if (len <= 0 || len > 4096) return fail("len must be 1..4096");
            std::vector<uint8_t> buf((size_t)len);

            if (!eng.ramDiskReadPartial(args[2], off, buf.data(), buf.size()))
                return fail("read failed (unwritten area, bad offset, or len exceeds block)");
            return ok(bytesToHex(buf.data(), buf.size()) + "\n");
        }
        if (args.size() >= 4 && args[1] == "write") {
            if (args.size() != 5) return fail("usage: ramdisk write <name> <offset> <hex>");
            uint64_t off = strtoull(args[3].c_str(), nullptr, 10);
            std::vector<uint8_t> data;
            if (!hexToBytes(args[4], data)) return fail("bad hex");
            if (data.empty()) return fail("empty data");
            if (!eng.ramDiskWrite(args[2], off, data.data(), data.size()))
                return fail("write failed (disk full or bad alignment)");
            return ok("written " + std::to_string(data.size()) + " bytes\n");
        }
        std::string out;
        for (const auto& line : eng.ramDiskLines()) out += line + "\n";
        if (out.empty()) out = "(no RAM disks)\n";
        return ok(out);
    }
    if (cmd == "version") return ok("VeloxDisk 1.0.0\n");
    return fail("unknown command: " + cmd);
}

void pipeServerLoop(Engine& eng, std::atomic<bool>& running) {

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    bool haveSd = ConvertStringSecurityDescriptorToSecurityDescriptorW(
                      L"D:(A;;GA;;;SY)(A;;GA;;;BA)", 1, &sd, nullptr) && sd != nullptr;
    if (haveSd) sa.lpSecurityDescriptor = sd;
    while (running) {

        HANDLE h = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    1, 4096, 65536, 0, haveSd ? &sa : nullptr);
        if (h == INVALID_HANDLE_VALUE) break;
        BOOL connected = ConnectNamedPipe(h, nullptr) != 0;

        if (connected || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::string pending;
            char buf[8192];
            DWORD got = 0;
            while (running && ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                buf[got] = 0;
                pending += buf;

                if (pending.size() > 65536) break;
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    if (line.empty()) continue;
                    if (line == "shutdown") {
                        std::string out = "OK\nengine stopping\n<<<END\n";
                        DWORD w2 = 0;
                        WriteFile(h, out.data(), (DWORD)out.size(), &w2, nullptr);
                        running.store(false);
                        break;
                    }
                    std::string out;
                    handlePipeLine(eng, line, out);
                    DWORD w2 = 0;
                    WriteFile(h, out.data(), (DWORD)out.size(), &w2, nullptr);
                }
            }
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    if (sd) LocalFree(sd);
}

std::string defaultConfigPath() {
    wchar_t buf[4096] = {0};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 4096);
    char mb[4096] = {0};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, mb, 4096, nullptr, nullptr);
    std::string p = std::string(mb) + "\\VeloxDisk\\config.ini";
    // 旧版本目录回退（新到旧）: CacheBoost 时代 → OpenCache 时代
    copyFileIfAbsent(std::string(mb) + "\\CacheBoost\\config.ini", p);
    copyFileIfAbsent(std::string(mb) + "\\OpenCache\\config.ini", p);
    return p;
}

}

static Engine* s_ctrlEng = nullptr;
static std::atomic<bool>* s_ctrlRunning = nullptr;
static BOOL WINAPI s_ctrlHandler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT ||
        sig == CTRL_LOGOFF_EVENT || sig == CTRL_SHUTDOWN_EVENT) {
        if (s_ctrlRunning) s_ctrlRunning->store(false);
        if (s_ctrlEng) s_ctrlEng->stop();
        ExitProcess(0);
    }
    return FALSE;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    if (!args.empty() && (args[0] == "serve" || args[0] == "--serve")) {
        std::string cfg = defaultConfigPath();
        for (size_t i = 1; i + 1 < args.size(); ++i)
            if (args[i] == "--config") cfg = args[i + 1];
        size_t ds = cfg.find_last_of("\\/");
        if (ds != std::string::npos) makeDirs(cfg.substr(0, ds));

        HANDLE mtx = CreateMutexW(nullptr, FALSE, L"Local\\VeloxDisk.Serve");

        if (mtx == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (mtx) CloseHandle(mtx);
            std::fprintf(stderr, "error: another VeloxDisk serve is already running\n");
            return 1;
        }
        bool admin = IsUserAnAdmin();

        Engine eng;
        if (!eng.start(cfg)) {
            std::fprintf(stderr, "engine failed to start\n");
            return 1;
        }
        std::fprintf(stderr, "VeloxDisk engine running (config: %s, admin=%s)\n",
                     cfg.c_str(), admin ? "yes" : "NO - some operations may fail");
        std::atomic<bool> running{true};
        std::thread server([&] { pipeServerLoop(eng, running); });

        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
            std::fprintf(stderr, "[crash] EXCEPTION 0x%08X at %p\n",
                         ep ? ep->ExceptionRecord->ExceptionCode : 0,
                         ep ? ep->ExceptionRecord->ExceptionAddress : nullptr);
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 3);
            return EXCEPTION_EXECUTE_HANDLER;
        });

        s_ctrlEng = &eng;
        s_ctrlRunning = &running;
        SetConsoleCtrlHandler(&s_ctrlHandler, TRUE);
        while (running.load()) Sleep(500);
        if (server.joinable()) server.join();
        eng.stop();
        return 0;
    }

    if (!args.empty() && (args[0] == "?" || args[0] == "help")) { printUsage(); return 0; }
    if (!args.empty() && (args[0] == "version" || args[0] == "ver")) {
        std::printf("VeloxDisk 1.0.0 (user-mode build)\n");
        return 0;
    }
    if (!IsUserAnAdmin()) {
        std::fprintf(stderr, "error: VeloxDisk must run as administrator\n");
        return 4;
    }
    return runControl(args);
}
