#pragma once

#include <string>
#include <map>
#include <vector>

namespace vd {

class Config {
public:
    Config() = default;

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    void set(const std::string& section, const std::string& key, const std::string& value);
    bool get(const std::string& section, const std::string& key, std::string& out) const;
    std::string getStr(const std::string& section, const std::string& key, const std::string& def = "") const;
    long long getInt(const std::string& section, const std::string& key, long long def = 0) const;
    bool getBool(const std::string& section, const std::string& key, bool def = false) const;

    static long long parseSize(const std::string& s, long long def = 0);

    const std::map<std::string, std::map<std::string, std::string>>& all() const { return data_; }

private:

    std::map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string> sectionOrder_;
};

}
