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

// cli/src/commands/flash_cmd.cpp
#include "flash_cmd.h"

#include "../cmd_context.h"
#include "../output.h"

#include <hpm/manifest/chip_loader.h>
#include <hpm/manifest/project_loader.h>
#include <hpm/util/process.h>
#include "hpm/config.h"
#include <filesystem>

namespace hpm::cli {

namespace {

namespace fs = std::filesystem;

// 在 packagesDir 下找芯片驱动的 chip.json（芯片包目录名 <chip>-<version>）
Result<Chip> loadChip(const fs::path& packagesDir, const std::string& mcu) {
    if (!fs::exists(packagesDir)) {
        return Error(ErrorCode::NotFound, "包目录不存在");
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
        return Error(ErrorCode::NotFound, "域目录不存在: " + dir.string());
    }

    std::string prefix = name + "-";
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        auto n = entry.path().filename().string();
        if (n.rfind(prefix, 0) != 0) continue;
        auto chipJson = entry.path() / HPM_FILE_CHIP;
        if (fs::exists(chipJson)) return manifest::loadChipFile(chipJson);
    }
    return Error(ErrorCode::NotFound, "未找到芯片驱动: " + mcu);
}

// 替换 {bin}/{elf}/{hex} 占位符
std::string subst(const std::string& tmpl, const fs::path& buildDir) {
    std::string s = tmpl;
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("{bin}", (buildDir / "firmware.bin").string());
    replaceAll("{elf}", (buildDir / "firmware.elf").string());
    replaceAll("{hex}", (buildDir / "firmware.hex").string());
    return s;
}

} // namespace

void FlashCmd::setup(CLI::App& app) {
    (void)app;
}

int FlashCmd::run(CmdContext& ctx) {
    auto proj = manifest::loadProjectFile(ctx.projectFile());
    if (!proj) {
        Output::error(proj.error().message());
        return 1;
    }

    auto chip = loadChip(ctx.packagesDir(), proj->mcu);
    if (!chip) {
        Output::error(chip.error().message());
        return 1;
    }

    if (chip->flash.tool.empty()) {
        Output::error("芯片驱动未声明烧录工具");
        return 1;
    }

    Output::step("烧录固件");
    Output::detail("工具: " + chip->flash.tool);

    std::vector<std::string> args;
    for (const auto& a : chip->flash.args) {
        args.push_back(subst(a, ctx.buildDir()));
    }

    auto st = util::runProcess(chip->flash.tool, args, ctx.verbose());
    if (!st.ok()) {
        Output::error("烧录失败: " + st.error().message());
        return 1;
    }

    Output::success("烧录完成");
    return 0;
}

} // namespace hpm::cli