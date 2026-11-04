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

// core/include/hpm/util/fs_utils.h
#pragma once

#include <hpm/result.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hpm::util {

// 原子写文件：先写临时文件再 rename
Status writeFileAtomic(const std::filesystem::path& path,
                       const std::string& content);

// 写二进制文件（自动创建父目录）
Status writeBinaryFile(const std::filesystem::path& path,
                       const std::vector<uint8_t>& data);

// 读整个文件
Result<std::string> readFile(const std::filesystem::path& path);

// 判断文件是否存在
bool exists(const std::filesystem::path& path);

// 查找可执行文件：
// - name 含路径分隔符时按路径处理（相对路径相对当前目录）
// - 否则在 PATH 中查找
Result<std::filesystem::path> findExecutable(const std::string& name);

} // namespace hpm::util