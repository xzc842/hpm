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

// debug/host/src/host_utils.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hpm::debug {

std::string canonicalPin(std::string name);
std::string sanitizeName(const std::string& s);
std::string readCString(const char* s);
std::string toHex(const uint8_t* data, size_t len);
std::string statusName(uint32_t status);
const char* levelName(int level);

} // namespace hpm::debug
