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

// cli/src/commands/dev_export_cmd.cpp
#include "dev_export_cmd.h"

#include "../cmd_context.h"
#include "../output.h"

#include <hpm/capability/driver_library.h>
#include <hpm/generator/hpm_h_generator.h>
#include <hpm/manifest/chip_loader.h>
#include <hpm/manifest/driver_loader.h>
#include <hpm/manifest/project_loader.h>
#include <hpm/manifest/state_loader.h>
#include <hpm/util/fs_utils.h>
#include "hpm/config.h"

#include <filesystem>
#include <optional>

namespace hpm::cli {

namespace {

namespace fs = std::filesystem;

// 从包目录加载芯片驱动（芯片包目录名 <chip>-<version>）
Result<Chip> loadChipFromCache(const fs::path& packagesDir,
                                const std::string& mcu) {
    if (!fs::exists(packagesDir)) {
        return Error(ErrorCode::NotFound,
                     "包目录不存在: " + packagesDir.string());
    }

    fs::path dir = packagesDir;
    std::string name = mcu;

    auto slash = mcu.find('/');
    if (slash != std::string::npos) {
        auto domain = mcu.substr(0, slash);
        name = mcu.substr(slash + 1);
        dir = dir / domain;
    }

    if (!fs::exists(dir)) {
        return Error(ErrorCode::NotFound,
                     "域目录不存在: " + dir.string());
    }

    std::string prefix = name + "-";
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        auto n = entry.path().filename().string();
        if (n.rfind(prefix, 0) != 0) continue;

        auto chipJson = entry.path() / HPM_FILE_CHIP;
        if (fs::exists(chipJson)) {
            return manifest::loadChipFile(chipJson);
        }
    }
    return Error(ErrorCode::NotFound,
                 "未找到芯片驱动: " + mcu);
}

// 从包目录加载所有驱动清单（key = <driver>-<chip>）
DriverLibrary loadAllDrivers(const fs::path& packagesDir) {
    DriverLibrary lib;
    if (!fs::exists(packagesDir)) return lib;

    auto scan = [&](const fs::path& dir, const std::string& domain) {
        if (!fs::exists(dir)) return;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_directory()) continue;
            auto mf = entry.path() / HPM_FILE_MANIFEST;
            if (!fs::exists(mf)) continue;

            // 目录名 <name>-<chip>-<version>
            std::string chip = "*";
            auto dirName = entry.path().filename().string();
            auto p2 = dirName.rfind('-');
            if (p2 != std::string::npos) {
                auto p1 = dirName.rfind('-', p2 - 1);
                if (p1 != std::string::npos) {
                    chip = dirName.substr(p1 + 1, p2 - p1 - 1);
                }
            }

            auto dm = manifest::loadDriverManifestFile(mf);
            if (dm.ok()) {
                std::string baseName = dm->name;
                if (!domain.empty()) {
                    baseName = domain + "/" + baseName;
                }
                dm->name = baseName + "-" + chip;
                lib.add(std::move(*dm));
            }
        }
    };

    scan(packagesDir, "");

    for (const auto& entry : fs::directory_iterator(packagesDir)) {
        if (!entry.is_directory()) continue;
        if (fs::exists(entry.path() / HPM_FILE_MANIFEST) ||
            fs::exists(entry.path() / HPM_FILE_CHIP)) {
            continue;
        }
        scan(entry.path(), entry.path().filename().string());
    }

    return lib;
}

} // namespace

void DevExportCmd::setup(CLI::App& app) {
    app.add_flag("--export", export_, "生成 hpm.h");
}

int DevExportCmd::run(CmdContext& ctx) {
    if (!export_) {
        Output::error("请指定 --export");
        return 1;
    }

    // 1. 读 project.json
    auto proj = manifest::loadProjectFile(ctx.projectFile());
    if (!proj) {
        Output::error(proj.error().message());
        return 1;
    }

    // 2. 读 driver.json
    if (!fs::exists(ctx.driverStateFile())) {
        Output::error("driver.json 不存在，先运行 hpm sync");
        return 1;
    }
    auto state = manifest::loadDriverStateFile(ctx.driverStateFile());
    if (!state) {
        Output::error(state.error().message());
        return 1;
    }

    // 3. 加载芯片驱动
    auto chip = loadChipFromCache(ctx.packagesDir(), proj->mcu);
    if (!chip) {
        Output::error(chip.error().message());
        return 1;
    }

    // 4. 加载驱动库
    auto lib = loadAllDrivers(ctx.packagesDir());

    // 5. 生成 hpm.h
    HpmHGenerator gen(*state, lib, *chip);
    auto content = gen.generate();

    // 6. 写入 driver/hpm.h
    auto path = ctx.driverDir() / HPM_FILE_HPM_H;
    auto st = util::writeFileAtomic(path, content);
    if (!st.ok()) {
        Output::error(st.error().message());
        return 1;
    }

    Output::success("已生成 " + path.string());
    return 0;
}

} // namespace hpm::cli