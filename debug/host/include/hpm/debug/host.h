/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// debug/host/include/hpm/debug/host.h
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace hpm::debug {

struct HostOptions {
    std::filesystem::path projectDir;
    std::filesystem::path buildDir;
    std::filesystem::path packagesDir;
    std::filesystem::path modelPath;
    std::filesystem::path configPath;
    std::filesystem::path firmwarePath;
    std::filesystem::path mcusPath;
    std::filesystem::path isaPath;
    std::filesystem::path jitPath;

    bool jsonMode = false;
    bool runImmediately = false;
    bool allowMissing = false;
    bool verbose = false;

    std::string hostCc = "cc";
};

class Host {
public:
    explicit Host(HostOptions options);
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpm::debug
