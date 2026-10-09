#pragma once
// 轻量 logger: 控制台输出(仅保留在用的 warn 级别入口)
#include <string>
#include <chrono>
#include <cstdio>

namespace vd {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 9 };

class Logger {
public:
    static Logger& instance() { static Logger l; return l; }

    void warn(const std::string& m) { log(LogLevel::Warn, m); }

private:
    Logger() = default;
    void log(LogLevel lv, const std::string& msg);
    static const char* name(LogLevel lv);
    static std::string now();
};

} // namespace vd
