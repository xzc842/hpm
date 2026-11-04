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

// cli/src/commands/build_cmd.cpp
#include "build_cmd.h"

#include "../cmd_context.h"
#include "../output.h"

#include <hpm/util/process.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace hpm::cli {

void BuildCmd::setup(CLI::App& app) {
    (void)app;
}

int BuildCmd::run(CmdContext& ctx) {
    namespace fs = std::filesystem;

    auto driverDir = ctx.driverDir();
    if (!fs::exists(driverDir)) {
        Output::error("driver/ 不存在，先运行 hpm sync");
        return 1;
    }

    Output::step("编译");
    auto st = util::runProcess("make",
                               {"-C", driverDir.string()},
                               ctx.verbose());
    if (!st.ok()) {
        Output::error("编译失败: " + st.error().message());
        return 1;
    }

    Output::success("编译完成");

    const std::vector<std::pair<std::string, std::string>> artifacts = {
        {"firmware.hex", "HEX"},
        {"firmware.bin", "BIN"},
        {"firmware.elf", "ELF"},
    };
    for (const auto& [file, kind] : artifacts) {
        auto path = ctx.buildDir() / file;
        if (fs::exists(path)) {
            Output::detail("产物: " + path.string() + " (" + kind + ")");
        }
    }
    return 0;
}

} // namespace hpm::cli