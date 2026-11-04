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

// dev/src/commands/validate_cmd.cpp
#include "validate_cmd.h"

#include <hpm/manifest/chip_loader.h>
#include <hpm/manifest/driver_loader.h>
#include "hpm/config.h"

#include <filesystem>
#include <iostream>

namespace hpm::cli {

namespace fs = std::filesystem;

namespace {

int errors = 0;
int warnings = 0;

void ok(const std::string& msg) {
    std::cout << "\033[32mok\033[0m " << msg << "\n";
}

void warn(const std::string& msg) {
    std::cout << "\033[33mwarn\033[0m " << msg << "\n";
    warnings++;
}

void error(const std::string& msg) {
    std::cout << "\033[31merror\033[0m " << msg << "\n";
    errors++;
}

} // namespace

int runValidate(DevContext& ctx) {
    auto dir = ctx.path();
    if (!fs::exists(dir)) {
        error("目录不存在: " + dir.string());
        return 1;
    }

    // 1. manifest.json 或 chip.json
    auto manifest = dir / HPM_FILE_MANIFEST;
    auto chipJson = dir / HPM_FILE_CHIP;

    if (fs::exists(manifest)) {
        auto dm = hpm::manifest::loadDriverManifestFile(manifest);
        if (!dm) {
            error("manifest.json: " + dm.error().message());
        } else {
            ok("manifest.json: " + dm->name + "@" + dm->version.toString());
        }
    } else if (fs::exists(chipJson)) {
        auto chip = hpm::manifest::loadChipFile(chipJson);
        if (!chip) {
            error("chip.json: " + chip.error().message());
        } else {
            ok("chip.json: " + chip->name);
        }
    } else {
        error("缺少 manifest.json 或 chip.json");
    }

    // 2. LICENSE
    if (!fs::exists(dir / "LICENSE")) {
        warn("缺少 LICENSE 文件");
    }

    // 3. src/ 和 include/
    if (!fs::exists(dir / HPM_DIR_SRC)) {
        warn("缺少 src/ 目录");
    }
    if (!fs::exists(dir / HPM_DIR_INCLUDE)) {
        warn("缺少 include/ 目录");
    }

    // 4. debug/：可选，存在则必须有 debug.c
    const fs::path debugDir = dir / "debug";
    if (fs::exists(debugDir)) {
        if (!fs::is_regular_file(debugDir / "debug.c")) {
            error("debug/ 存在但缺少 debug/debug.c");
        } else {
            ok("debug/debug.c（设备虚拟化入口）");
        }
    }

    // 5. .c 在 src/，.h 在 include/；debug/ 例外
    if (fs::exists(dir / HPM_DIR_SRC) && fs::exists(dir / HPM_DIR_INCLUDE)) {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            auto rel = fs::relative(entry.path(), dir);
            if (!rel.empty() && *rel.begin() == "debug") continue;

            auto ext = entry.path().extension();
            auto parent = entry.path().parent_path().filename().string();

            if (ext == ".c" && parent != HPM_DIR_SRC) {
                warn(".c 文件不在 src/: " + entry.path().string());
            }
            if (ext == ".h" && parent != HPM_DIR_INCLUDE && parent != HPM_DIR_SRC) {
                warn(".h 文件不在 include/: " + entry.path().string());
            }
        }
    }

    // 6. ld/ 目录（ARM 芯片）
    if (fs::exists(chipJson)) {
        auto chip = hpm::manifest::loadChipFile(chipJson);
        if (chip && !chip->build.ldscript.empty()) {
            auto ldPath = dir / chip->build.ldscript;
            if (!fs::exists(ldPath)) {
                error("链接脚本不存在: " + ldPath.string());
            }
        }
    }

    // 汇总
    std::cout << "\n";
    if (errors == 0 && warnings == 0) {
        ok("校验通过");
        return 0;
    }
    if (errors == 0) {
        ok("校验通过，" + std::to_string(warnings) + " 个警告");
        return 0;
    }
    error(std::to_string(errors) + " 个错误，" + std::to_string(warnings) + " 个警告");
    return 1;
}

} // namespace hpm::cli