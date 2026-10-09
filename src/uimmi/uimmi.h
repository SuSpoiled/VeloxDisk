#pragma once
#include <string>
#include <cstdint>

namespace vd {

class Uimmi {
public:
    struct Info {
        uint32_t major = 0;
        uint32_t minor = 0;
        uint32_t build = 0;
        bool isWin64 = false;
        bool supportedOs = false;

        std::string describe() const;
    };

    static Info detect();
};

}
