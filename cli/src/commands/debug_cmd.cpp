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

// cli/src/commands/debug_cmd.cpp
#include "debug_cmd.h"

#include "../cmd_context.h"
#include "../output.h"

#include <hpm/debug/host.h>
#include <hpm/json/json_io.h>
#include <hpm/manifest/project_loader.h>
#include <hpm/util/fs_utils.h>
#include <hpm/util/process.h>

#include <toml++/toml.hpp>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace hpm::cli {

namespace {

namespace fs = std::filesystem;

// 当前 hpm 可执行文件所在目录
fs::path executableDir() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) return fs::current_path();
    return exe.parent_path();
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// 选固件：
//   1. --firmware 显式指定
//   2. build/firmware.{hex,ihx,bin} 中修改时间最新的
Result<fs::path> selectFirmware(const fs::path& buildDir,
                                const std::string& explicitPath) {
    if (!explicitPath.empty()) {
        fs::path p = fs::absolute(explicitPath);
        if (!fs::exists(p)) {
            return Error(ErrorCode::NotFound,
                         "固件文件不存在: " + p.string());
        }
        return p;
    }

    const std::vector<fs::path> candidates = {
        buildDir / "firmware.hex",
        buildDir / "firmware.ihx",
        buildDir / "firmware.bin",
    };

    std::optional<fs::path> best;
    fs::file_time_type bestTime{};
    std::error_code ec;

    for (const auto& candidate : candidates) {
        if (!fs::is_regular_file(candidate, ec) || ec) continue;
        auto time = fs::last_write_time(candidate, ec);
        if (ec) continue;
        if (!best || time > bestTime) {
            best = candidate;
            bestTime = time;
        }
    }

    if (!best) {
        return Error(ErrorCode::NotFound,
                     "未找到已编译固件（build/firmware.hex|ihx|bin），"
                     "请先运行 hpm build");
    }
    return fs::absolute(*best);
}

// 读取 mcus 配置里的共享内存名，仅用于展示
std::string configValue(const fs::path& configPath, const std::string& section,
                        const std::string& key, const std::string& fallback) {
    try {
        auto tbl = toml::parse_file(configPath.string());
        if (auto* sec = tbl[section].as_table()) {
            if (auto value = (*sec)[key].value<std::string>()) {
                return *value;
            }
        }
    } catch (const toml::parse_error&) {
        // 真正的配置校验交给 mcus
    }
    return fallback;
}

// 根据模型 arch 推导 ISA 动态库名：libmcus_isa_<arch>.so
fs::path defaultIsaPath(const std::string& arch) {
    return fs::path("libmcus_isa_" + arch + ".so");
}

Result<fs::path> locateIsa(const fs::path& projectDir,
                           const std::string& explicitPath,
                           const std::string& arch) {
    if (!explicitPath.empty()) {
        fs::path p = fs::absolute(explicitPath);
        if (!fs::is_regular_file(p)) {
            return Error(ErrorCode::NotFound,
                         "ISA 库不存在: " + p.string());
        }
        return p;
    }

    if (const char* env = std::getenv("HPM_ISA"); env && *env) {
        fs::path p = fs::absolute(env);
        if (fs::is_regular_file(p)) return p;
    }

    const fs::path dir = executableDir();
    const fs::path libName = defaultIsaPath(arch);

    const std::vector<fs::path> candidates = {
        dir / ".." / "lib" / libName,
        dir / "lib" / libName,
        dir / libName,
        projectDir / "build" / "lib" / libName,
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        auto normalized = fs::weakly_canonical(candidate, ec);
        if (!ec && fs::is_regular_file(normalized)) return normalized;
    }

    return Error(ErrorCode::NotFound,
                 "找不到 ISA 库 " + libName.string() +
                     "（可用 --isa 指定，例如 build/lib/libmcus_isa_" +
                     arch + ".so）");
}

Result<fs::path> locateMcus(const std::string& explicitPath) {
    if (!explicitPath.empty()) {
        auto p = util::findExecutable(explicitPath);
        if (!p) {
            return Error(p.error().code(),
                         "找不到 mcus 可执行文件: " + explicitPath);
        }
        return *p;
    }

    if (const char* env = std::getenv("HPM_MCUS"); env && *env) {
        auto p = util::findExecutable(env);
        if (p) return *p;
    }

    fs::path sibling = executableDir() / "mcus";
    if (util::findExecutable(sibling.string())) {
        return fs::weakly_canonical(sibling);
    }

    auto p = util::findExecutable("mcus");
    if (p) return *p;

    return Error(ErrorCode::NotFound,
                 "找不到 mcus 可执行文件（应随 hpm 一起构建到 build/bin/mcus）");
}

} // namespace

void DebugCmd::setup(CLI::App& app) {
    app.add_option("--model", modelPath_,
                   "mcus 芯片模型文件（JSON，必填）")
        ->required();
    app.add_option("--config", configPath_,
                   "mcus 配置文件（TOML，必填）")
        ->required();
    app.add_option("--isa", isaPath_,
                   "ISA 动态库（默认按模型 arch 推导）");
    app.add_option("--mcus", mcusPath_,
                   "mcus 可执行文件（默认 build/bin/mcus）");
    app.add_option("--firmware", firmwarePath_,
                   "固件文件（默认使用 build/ 下最新的 firmware.hex|ihx|bin）");
    app.add_flag("--dry-run", dryRun_, "只打印启动命令，不运行模拟器");
    app.add_flag("--json", jsonMode_, "JSON Lines 模式（外部程序调用）");
    app.add_flag("--run", runImmediately_, "启动后立即运行（默认暂停）");
    app.add_flag("--allow-missing-debug", allowMissing_,
                 "允许没有 debug/debug.c 的设备");
    app.add_option("--host-cc", hostCc_,
                   "编译设备 debug 插件用的宿主机编译器（默认 cc）");
}

int DebugCmd::run(CmdContext& ctx) {
    Output::setJsonMode(jsonMode_);
    Output::step("检查编译产物");

    auto proj = manifest::loadProjectFile(ctx.projectFile());
    if (!proj) {
        Output::error(proj.error().message());
        return 1;
    }
    Output::detail("MCU: " + proj->mcu);

    if (!fs::exists(ctx.driverDir())) {
        Output::error("未找到 driver/，请先运行 hpm sync");
        return 1;
    }
    if (!fs::exists(ctx.buildDir())) {
        Output::error("未找到 build/，请先运行 hpm build");
        return 1;
    }

    auto firmware = selectFirmware(ctx.buildDir(), firmwarePath_);
    if (!firmware) {
        Output::error(firmware.error().message());
        return 1;
    }
    Output::detail("固件: " + firmware->string());

    // 模型 / 配置
    const fs::path modelPath = fs::absolute(modelPath_);
    const fs::path configPath = fs::absolute(configPath_);

    if (!fs::is_regular_file(modelPath)) {
        Output::error("模型文件不存在: " + modelPath.string());
        return 1;
    }
    if (!fs::is_regular_file(configPath)) {
        Output::error("配置文件不存在: " + configPath.string());
        return 1;
    }

    auto modelText = util::readFile(modelPath);
    if (!modelText) {
        Output::error(modelText.error().message());
        return 1;
    }
    auto modelJson = json::parse(*modelText);
    if (!modelJson) {
        Output::error("模型文件不是合法 JSON: " + modelJson.error().message());
        return 1;
    }

    const std::string modelName = modelJson->getString("name", "");
    const std::string arch = modelJson->getString("arch", "");
    if (arch.empty()) {
        Output::error("模型文件缺少 arch 字段: " + modelPath.string());
        return 1;
    }
    if (arch.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
        std::string::npos) {
        Output::error("模型 arch 字段包含非法字符: " + arch);
        return 1;
    }

    auto isa = locateIsa(ctx.projectDir(), isaPath_, arch);
    if (!isa) {
        Output::error(isa.error().message());
        return 1;
    }

    auto mcus = locateMcus(mcusPath_);
    if (!mcus) {
        Output::error(mcus.error().message());
        return 1;
    }

    if (dryRun_) {
        Output::step("启动 mcus 模拟器");
        Output::detail("模型: " +
                       (modelName.empty() ? modelPath.string() : modelName) +
                       " (" + arch + ")");
        Output::detail("固件: " + firmware->string());
        Output::detail("配置: " + configPath.string());
        Output::detail("ISA:  " + isa->string());

        std::vector<std::string> args = {
            "--isa", isa->string(),
            "--model", modelPath.string(),
            "--firmware", firmware->string(),
            "--config", configPath.string(),
        };
        std::cout << "    " << shellQuote(mcus->string());
        for (const auto& a : args) std::cout << ' ' << shellQuote(a);
        std::cout << "\n";
        Output::info("--dry-run 模式，不启动模拟器");
        return 0;
    }

    hpm::debug::HostOptions options;
    options.projectDir = ctx.projectDir();
    options.buildDir = ctx.buildDir();
    options.packagesDir = ctx.packagesDir();
    options.modelPath = modelPath;
    options.configPath = configPath;
    options.firmwarePath = *firmware;
    options.mcusPath = *mcus;
    options.isaPath = *isa;
    options.jsonMode = jsonMode_;
    options.runImmediately = runImmediately_;
    options.allowMissing = allowMissing_;
    options.verbose = ctx.verbose();
    options.hostCc = hostCc_;

    Output::step("设备虚拟化");
    hpm::debug::Host host(std::move(options));
    return host.run();
}

} // namespace hpm::cli
