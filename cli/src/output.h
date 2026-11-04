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

// cli/src/output.h
#pragma once

#include <string>

namespace hpm::cli {

// 输出辅助：带颜色、前缀
class Output {
public:
    static void info(const std::string& msg);
    static void success(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);
    static void step(const std::string& msg);   // 阶段标题
    static void detail(const std::string& msg); // 缩进细节

    // 是否用彩色（非 tty 时关闭）
    static void setColor(bool on);

    // JSON 模式：所有人类可读输出改到 stderr，保持 stdout 只有 JSON
    static void setJsonMode(bool on);
};

} // namespace hpm::cli