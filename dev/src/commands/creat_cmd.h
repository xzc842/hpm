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

#pragma once

#include "../dev_context.h"

#include <CLI/CLI.hpp>

namespace hpm::cli {

class CreateCmd {
public:
    void setup(CLI::App& app);
    int run(DevContext& ctx);

private:
    std::string name_;
    std::string type_ = "driver";
    std::string dir_;
    std::string version_ = "0.1.0";
    std::string license_ = "MIT";
    std::string chip_ = "*";   // 目标芯片，驱动包用；芯片包自动 = name
};

} // namespace hpm::cli