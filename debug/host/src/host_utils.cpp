// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// debug/host/src/host_utils.cpp
#include "host_utils.h"

#include <hpm/debug/plugin.h>

#include <algorithm>
#include <cctype>

namespace hpm::debug {

std::string canonicalPin(std::string name) {
    std::replace(name.begin(), name.end(), '_', '.');
    return name;
}

std::string sanitizeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::string readCString(const char* s) { return s ? s : ""; }

std::string toHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

std::string statusName(uint32_t status) {
    switch (status) {
        case 0: return "running";
        case 1: return "paused";
        case 2: return "dump_done";
        case 3: return "halted";
        default: return "unknown";
    }
}

const char* levelName(int level) {
    switch (level) {
        case HPM_DEBUG_LOG_TRACE: return "trace";
        case HPM_DEBUG_LOG_DEBUG: return "debug";
        case HPM_DEBUG_LOG_WARN: return "warn";
        case HPM_DEBUG_LOG_ERROR: return "error";
        default: return "info";
    }
}

} // namespace hpm::debug
