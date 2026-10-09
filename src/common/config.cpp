#include "config.h"
#include "util.h"
#include "log.h"
#include <cctype>
#include <algorithm>

namespace vd {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool Config::load(const std::string& path) {
    std::string text;
    if (!readText(path, text)) { Logger::instance().warn("config: cannot read " + path); return false; }
    std::string section;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            std::string t = trim(line); line.clear();
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            if (t.front() == '[' && t.back() == ']') { section = trim(t.substr(1, t.size() - 2)); continue; }
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(t.substr(0, eq));
            std::string v = trim(t.substr(eq + 1));
            set(section, k, v);
        } else {
            line.push_back(c);
        }
    }
    std::string t = trim(line);
    if (!t.empty() && t[0] != ';' && t[0] != '#') {
        if (t.front() == '[' && t.back() == ']') section = trim(t.substr(1, t.size() - 2));
        else {
            size_t eq = t.find('=');
            if (eq != std::string::npos) set(section, trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
        }
    }
    return true;
}

bool Config::save(const std::string& path) const {
    std::string out;
    for (const auto& sec : sectionOrder_) {
        auto it = data_.find(sec);
        if (it == data_.end()) continue;
        out += "[" + sec + "]\n";
        for (const auto& kv : it->second) out += kv.first + " = " + kv.second + "\n";
        out += "\n";
    }
    for (const auto& sec : data_) {
        if (std::find(sectionOrder_.begin(), sectionOrder_.end(), sec.first) == sectionOrder_.end()) {
            out += "[" + sec.first + "]\n";
            for (const auto& kv : sec.second) out += kv.first + " = " + kv.second + "\n";
            out += "\n";
        }
    }
    std::string dir, base; splitPath(path, dir, base); (void)base;
    if (!dir.empty() && dir != ".") makeDirs(dir);
    return writeFile(path, out);
}

void Config::set(const std::string& section, const std::string& key, const std::string& value) {

    bool fresh = data_.find(section) == data_.end();
    auto& m = data_[section];
    m[key] = value;
    if (fresh) sectionOrder_.push_back(section);
}

bool Config::get(const std::string& section, const std::string& key, std::string& out) const {
    auto s = data_.find(section);
    if (s == data_.end()) return false;
    auto k = s->second.find(key);
    if (k == s->second.end()) return false;
    out = k->second;
    return true;
}

std::string Config::getStr(const std::string& s, const std::string& k, const std::string& def) const {
    std::string v; return get(s, k, v) ? v : def;
}

long long Config::getInt(const std::string& s, const std::string& k, long long def) const {
    std::string v; if (!get(s, k, v)) return def;
    try { return std::stoll(v); } catch (...) { return def; }
}

bool Config::getBool(const std::string& s, const std::string& k, bool def) const {
    std::string v; if (!get(s, k, v)) return def;
    v = lower(v);
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

long long Config::parseSize(const std::string& raw, long long def) {
    std::string s;
    for (char c : raw) if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    if (s.empty()) return def;
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    if (i == 0) return def;
    double num;
    try { num = std::stod(s.substr(0, i)); } catch (...) { return def; }
    std::string unit = lower(s.substr(i));
    long long mult = 1;
    if (unit.rfind("kb", 0) == 0) mult = 1024LL;
    else if (unit.rfind("mb", 0) == 0) mult = 1024LL * 1024;
    else if (unit.rfind("gb", 0) == 0) mult = 1024LL * 1024 * 1024;
    else if (unit.rfind("tb", 0) == 0) mult = 1024LL * 1024 * 1024 * 1024;
    else if (!unit.empty() && unit != "b") return def;
    double d = num * static_cast<double>(mult);
    if (d > 9.0e18) return def;
    return static_cast<long long>(d);
}

}
