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

// cli/src/commands/debug_cmd.h
#pragma once

#include "../command.h"

#include <string>

namespace hpm::cli {

// hpm debug：检查编译产物，启动 mcus 模拟器
class DebugCmd : public Command {
public:
    const char* name() const override { return "debug"; }
    const char* description() const override {
        return "检查编译产物并在 mcus 模拟器中运行";
    }

    void setup(CLI::App& app) override;
    int run(CmdContext& ctx) override;

private:
    std::string modelPath_;
    std::string configPath_;
    std::string isaPath_;
    std::string mcusPath_;
    std::string firmwarePath_;
    std::string hostCc_ = "cc";
    bool dryRun_ = false;
    bool jsonMode_ = false;
    bool runImmediately_ = false;
    bool allowMissing_ = false;
};

} // namespace hpm::cli
