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

// core/src/resolver/id_generator.cpp
#include <hpm/resolver/id_generator.h>

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>

namespace hpm {

std::string generateIntermediateId(const std::string& prefix) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFF);

    // 把 prefix 里的非字母数字（如 '-'、'/'）替换成 '_'，保证是合法 C 标识符
    std::string safe;
    safe.reserve(prefix.size());
    for (char c : prefix) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            safe += c;
        } else {
            safe += '_';
        }
    }

    std::ostringstream oss;
    oss << "__" << safe << "_"
        << std::hex << std::setw(4) << std::setfill('0') << dist(gen);
    return oss.str();
}

} // namespace hpm