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
#include <algorithm>
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
/*
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
*/
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

struct ChipDebug {
    fs::path packageDir;
    fs::path debugDir;
};

Result<ChipDebug> findChipDebug(const fs::path& packagesDir,
                                const std::string& mcu) {
    std::error_code ec;
    std::vector<fs::path> matches;
    for (const auto& entry : fs::directory_iterator(packagesDir, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const fs::path chipJson = entry.path() / "chip.json";
        if (!fs::is_regular_file(chipJson)) continue;
        auto text = util::readFile(chipJson);
        if (!text) continue;
        auto j = json::parse(*text);
        if (!j) continue;
        if (j->getString("name") == mcu &&
            fs::is_directory(entry.path() / "debug")) {
            matches.push_back(entry.path());
        }
    }
    if (matches.empty()) {
        return Error(ErrorCode::NotFound,
                     "找不到芯片包 debug: " + mcu);
    }
    std::sort(matches.begin(), matches.end(), [](const fs::path& a,
                                                const fs::path& b) {
        return a.filename().string() > b.filename().string();
    });
    return ChipDebug{matches.front(), matches.front() / "debug"};
}

Result<fs::path> findChipIsaSource(const fs::path& debugDir,
                                   const std::string& arch) {
    std::error_code ec;
    const fs::path exact = debugDir / ("isa_" + arch + ".cpp");
    if (fs::is_regular_file(exact, ec)) return exact;

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(debugDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("isa_", 0) == 0 && entry.path().extension() == ".cpp") {
            candidates.push_back(entry.path());
        }
    }
    if (candidates.size() == 1) return candidates.front();
    return Error(ErrorCode::NotFound,
                 "芯片包 debug 里找不到 isa_" + arch + ".cpp");
}

fs::path firstExisting(const std::vector<fs::path>& paths) {
    std::error_code ec;
    for (const auto& p : paths) {
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

Result<fs::path> compileChipIsa(const fs::path& source,
                                const fs::path& projectDir,
                                const fs::path& buildDir,
                                const std::string& arch,
                                const std::string& hostCxx,
                                bool verbose) {
    const fs::path exeDir = executableDir();
    const fs::path sdk = firstExisting({
        exeDir / ".." / "lib" / "libmcus_isa_sdk.a",
        buildDir / "lib" / "libmcus_isa_sdk.a",
        projectDir / "build" / "lib" / "libmcus_isa_sdk.a",
        "/usr/local/lib/libmcus_isa_sdk.a",
        "/usr/lib/libmcus_isa_sdk.a",
        "/usr/lib64/libmcus_isa_sdk.a",
    });
    if (sdk.empty()) {
        return Error(ErrorCode::NotFound,
                     "找不到 libmcus_isa_sdk.a（需要先构建 hpm/mcus）");
    }

    const fs::path includeDir = firstExisting({
        exeDir / ".." / "include" / "mcus" / "instruction.h",
        buildDir / "include" / "mcus" / "instruction.h",
        projectDir / "build" / "include" / "mcus" / "instruction.h",
        "/usr/local/include/mcus/instruction.h",
        "/usr/include/mcus/instruction.h",
    }).parent_path().parent_path();
    if (includeDir.empty()) {
        return Error(ErrorCode::NotFound,
                     "找不到 mcus SDK 头文件（需要 build/include/mcus）");
    }

    const fs::path outDir = buildDir / "hpm-debug" / "isa";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path out = outDir / ("libmcus_isa_" + arch + ".so");

    std::vector<std::string> args = {
        "-std=c++17", "-O2", "-fPIC", "-shared",
        "-I", includeDir.string(),
        source.string(),
        sdk.string(),
        "-o", out.string(),
    };
    auto st = util::runProcess(hostCxx, args, verbose);
    if (!st.ok()) {
        return Error(ErrorCode::BuildFailed,
                     "编译芯片 ISA 失败: " + st.error().message());
    }
    return out;
}

Result<fs::path> findChipJitSource(const fs::path& debugDir,
                                    const std::string& arch) {
    std::error_code ec;
    const fs::path exact = debugDir / ("isa_jit_" + arch + ".cpp");
    if (fs::is_regular_file(exact, ec)) return exact;
    return Error(ErrorCode::NotFound,
                 "找不到芯片 JIT 源: " + exact.string());
}

Result<fs::path> compileChipJit(const fs::path& source,
                                const fs::path& projectDir,
                                const fs::path& buildDir,
                                const std::string& arch,
                                const std::string& hostCxx,
                                bool verbose) {
    const fs::path exeDir = executableDir();
    const fs::path sdk = firstExisting({
        exeDir / ".." / "lib" / "libmcus_isa_sdk.a",
        buildDir / "lib" / "libmcus_isa_sdk.a",
        projectDir / "build" / "lib" / "libmcus_isa_sdk.a",
        "/usr/local/lib/libmcus_isa_sdk.a",
        "/usr/lib/libmcus_isa_sdk.a",
        "/usr/lib64/libmcus_isa_sdk.a",
    });
    if (sdk.empty()) {
        return Error(ErrorCode::NotFound,
                     "找不到 libmcus_isa_sdk.a（需要先构建 hpm/mcus）");
    }
    const fs::path includeDir = firstExisting({
        exeDir / ".." / "include" / "mcus" / "instruction.h",
        buildDir / "include" / "mcus" / "instruction.h",
        projectDir / "build" / "include" / "mcus" / "instruction.h",
        "/usr/local/include/mcus/instruction.h",
        "/usr/include/mcus/instruction.h",
    }).parent_path().parent_path();
    if (includeDir.empty()) {
        return Error(ErrorCode::NotFound,
                     "找不到 mcus SDK 头文件（需要 build/include/mcus）");
    }
    const fs::path outDir = buildDir / "hpm-debug" / "jit";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path out = outDir / ("libmcus_jit_" + arch + ".so");
    std::vector<std::string> args = {
        "-std=c++17", "-O2", "-fPIC", "-shared",
        "-I", includeDir.string(),
        source.string(),
        sdk.string(),
        "-o", out.string(),
    };
    auto st = util::runProcess(hostCxx, args, verbose);
    if (!st.ok()) {
        return Error(ErrorCode::BuildFailed,
                     "编译芯片 JIT 失败: " + st.error().message());
    }
    return out;
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

    if (const char* home = std::getenv("HOME"); home && *home) {
        const fs::path userMcus = fs::path(home) / ".local" / "bin" / "mcus";
        if (util::findExecutable(userMcus.string())) {
            return fs::weakly_canonical(userMcus);
        }
    }

    auto p = util::findExecutable("mcus");
    if (p) return *p;

    return Error(ErrorCode::NotFound,
                 "找不到 mcus 可执行文件（应随 hpm 一起构建到 build/bin/mcus）");
}

} // namespace

void DebugCmd::setup(CLI::App& app) {
    app.add_option("--model", modelPath_,
                   "覆盖芯片包 debug 里的模型文件");
    app.add_option("--config", configPath_,
                   "覆盖项目 debug.toml");
    app.add_option("--isa", isaPath_,
                   "覆盖现场编译的芯片 ISA 动态库");
    app.add_option("--mcus", mcusPath_,
                   "mcus 可执行文件（默认 build/bin/mcus）");
    app.add_option("--firmware", firmwarePath_,
                   "固件文件（默认使用 build/ 下最新的 firmware.hex|ihx|bin）");
    app.add_flag("--dry-run", dryRun_, "只打印启动命令，不运行模拟器");
    app.add_flag("--json", jsonMode_, "JSON Lines 模式（外部程序调用）");
    app.add_flag("--run", runImmediately_, "启动后立即运行（默认暂停）");
    app.add_flag("--allow-missing-debug", allowMissing_,
                 "允许没有 debug/debug.c 的设备");
    app.add_flag("--jit", useJit_,
                 "编译并启用芯片包 debug/isa_jit_<arch>.cpp");
    app.add_option("--host-cc", hostCc_,
                   "编译设备 debug 插件用的 C 编译器（默认 cc）");
    app.add_option("--host-cxx", hostCxx_,
                   "编译芯片 ISA 用的 C++ 编译器（默认 c++）");
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

    // 模型 / 配置：默认从芯片包 debug 与项目 debug.toml 解析
    ChipDebug chip;
    if (modelPath_.empty()) {
        auto found = findChipDebug(ctx.packagesDir(), proj->mcu);
        if (!found) {
            Output::error(found.error().message());
            return 1;
        }
        chip = *found;
    } else {
        // 显式模型时仍尝试找芯片包，用于编译 ISA
        auto found = findChipDebug(ctx.packagesDir(), proj->mcu);
        if (found) chip = *found;
    }

    fs::path modelPath;
    if (!modelPath_.empty()) {
        modelPath = fs::absolute(modelPath_);
    } else {
        const fs::path preferred = chip.debugDir / (proj->mcu + ".json");
        if (fs::is_regular_file(preferred)) {
            modelPath = preferred;
        } else {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(chip.debugDir, ec)) {
                if (ec) break;
                if (entry.is_regular_file() &&
                    entry.path().extension() == ".json") {
                    modelPath = entry.path();
                    break;
                }
            }
        }
    }

    fs::path configPath;
    if (!configPath_.empty()) {
        configPath = fs::absolute(configPath_);
    } else {
        configPath = fs::absolute(ctx.projectDir() / "debug.toml");
    }

    if (!fs::is_regular_file(modelPath)) {
        Output::error("模型文件不存在: " + modelPath.string());
        return 1;
    }
    if (!fs::is_regular_file(configPath)) {
        Output::error("配置文件不存在: " + configPath.string() +
                      "（项目目录需要 debug.toml）");
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

    fs::path isa;
    if (!isaPath_.empty()) {
        auto explicitIsa = locateIsa(ctx.projectDir(), isaPath_, arch);
        if (!explicitIsa) {
            Output::error(explicitIsa.error().message());
            return 1;
        }
        isa = *explicitIsa;
    } else if (chip.debugDir.empty()) {
        Output::error("找不到芯片包 debug，无法编译 ISA");
        return 1;
    } else {
        auto source = findChipIsaSource(chip.debugDir, arch);
        if (!source) {
            Output::error(source.error().message());
            return 1;
        }
        Output::detail("ISA 源: " + source->string());
        auto compiled = compileChipIsa(*source, ctx.projectDir(),
                                       ctx.buildDir(), arch, hostCxx_,
                                       ctx.verbose());
        if (!compiled) {
            Output::error(compiled.error().message());
            return 1;
        }
        isa = *compiled;
    }

    fs::path jit;
    if (useJit_ && !chip.debugDir.empty()) {
        auto source = findChipJitSource(chip.debugDir, arch);
        if (source) {
            Output::detail("JIT 源: " + source->string());
            auto compiled = compileChipJit(*source, ctx.projectDir(),
                                           ctx.buildDir(), arch, hostCxx_,
                                           ctx.verbose());
            if (!compiled) {
                Output::error(compiled.error().message());
                return 1;
            }
            jit = *compiled;
        }
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
        Output::detail("ISA:  " + isa.string());

        std::vector<std::string> args = {
            "--isa", isa.string(),
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
    options.isaPath = isa;
    options.jitPath = jit;
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
