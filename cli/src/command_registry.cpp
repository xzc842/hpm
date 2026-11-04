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

// cli/src/command_registry.cpp
#include "command_registry.h"

#include "commands/build_cmd.h"
#include "commands/clean_cmd.h"
#include "commands/debug_cmd.h"
#include "commands/dev_export_cmd.h"
#include "commands/flash_cmd.h"
#include "commands/init_cmd.h"
#include "commands/sync_cmd.h"

namespace hpm::cli {

CommandRegistry::CommandRegistry() {
    commands_.push_back(std::make_unique<InitCmd>());
    commands_.push_back(std::make_unique<SyncCmd>());
    commands_.push_back(std::make_unique<BuildCmd>());
    commands_.push_back(std::make_unique<FlashCmd>());
    commands_.push_back(std::make_unique<CleanCmd>());
    commands_.push_back(std::make_unique<DebugCmd>());
    commands_.push_back(std::make_unique<DevExportCmd>());
}

void CommandRegistry::registerAll(CLI::App& app) {
    for (auto& cmd : commands_) {
        auto* sub = app.add_subcommand(cmd->name(), cmd->description());
        cmd->setup(*sub);
    }
}

int CommandRegistry::dispatch(const std::string& name, CmdContext& ctx) {
    for (auto& cmd : commands_) {
        if (name == cmd->name()) {
            return cmd->run(ctx);
        }
    }
    return -1;
}

std::vector<std::string> CommandRegistry::names() const {
    std::vector<std::string> out;
    for (const auto& c : commands_) out.push_back(c->name());
    return out;
}

} // namespace hpm::cli