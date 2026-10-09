#include "log.h"
#include <ctime>

namespace vd {

const char* Logger::name(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "     ";
    }
}

std::string Logger::now() {
    auto t = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    std::time_t s = static_cast<std::time_t>(ms / 1000);
    char buf[32];
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &s);
#else
    localtime_r(&s, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    char full[48];
    std::snprintf(full, sizeof(full), "%s.%03d", buf, static_cast<int>(ms % 1000));
    return full;
}

void Logger::log(LogLevel lv, const std::string& msg) {
    std::string line = now() + " [" + name(lv) + "] " + msg;
    std::printf("%s\n", line.c_str());
}

} // namespace vd
