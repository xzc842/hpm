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

// core/src/util/fs_utils.cpp
#include <hpm/util/fs_utils.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace hpm::util {

Status writeFileAtomic(const std::filesystem::path& path,
                       const std::string& content) {
    namespace fs = std::filesystem;

    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return Error(ErrorCode::IoError,
                         "创建目录失败: " + parent.string() + " - " + ec.message());
        }
    }

    auto tmp = path;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            return Error(ErrorCode::IoError, "无法写入: " + tmp.string());
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp);
        return Error(ErrorCode::IoError,
                     "重命名失败: " + path.string() + " - " + ec.message());
    }
    return {};
}

Status writeBinaryFile(const std::filesystem::path& path,
                       const std::vector<uint8_t>& data) {
    namespace fs = std::filesystem;

    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return Error(ErrorCode::IoError,
                         "创建目录失败: " + parent.string() + " - " + ec.message());
        }
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return Error(ErrorCode::IoError, "无法写入: " + path.string());
    }
    if (!data.empty()) {
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    if (!f) {
        return Error(ErrorCode::IoError, "写入失败: " + path.string());
    }
    return {};
}

Result<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return Error(ErrorCode::IoError, "无法打开: " + path.string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}

namespace {

bool isExecutable(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
#ifndef _WIN32
    return ::access(path.c_str(), X_OK) == 0;
#else
    return true;
#endif
}

} // namespace

Result<std::filesystem::path> findExecutable(const std::string& name) {
    namespace fs = std::filesystem;

    if (name.empty()) {
        return Error(ErrorCode::InvalidArgument, "可执行文件名不能为空");
    }

    fs::path p(name);
    if (p.has_parent_path()) {
        std::error_code ec;
        auto abs = fs::absolute(p, ec);
        if (ec) {
            return Error(ErrorCode::IoError,
                         "无法解析路径: " + name + " - " + ec.message());
        }
        if (!isExecutable(abs)) {
            return Error(ErrorCode::NotFound,
                         "找不到可执行文件: " + abs.string());
        }
        return abs;
    }

    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) {
        return Error(ErrorCode::NotFound, "PATH 为空，找不到可执行文件: " + name);
    }

#ifdef _WIN32
    constexpr char kSeparator = ';';
    const std::vector<std::string> suffixes = {"", ".exe", ".cmd", ".bat"};
#else
    constexpr char kSeparator = ':';
    const std::vector<std::string> suffixes = {""};
#endif

    std::istringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, kSeparator)) {
        if (dir.empty()) dir = ".";
        for (const auto& suffix : suffixes) {
            fs::path candidate = fs::path(dir) / (name + suffix);
            if (isExecutable(candidate)) {
                std::error_code ec;
                auto abs = fs::absolute(candidate, ec);
                return ec ? candidate : abs;
            }
        }
    }

    return Error(ErrorCode::NotFound,
                 "找不到可执行文件: " + name + "（可用 --mcus 指定路径）");
}

} // namespace hpm::util