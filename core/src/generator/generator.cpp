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

// core/src/generator/generator.cpp
#include <hpm/generator/generator.h>

#include <hpm/util/fs_utils.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include "hpm/config.h"

namespace hpm {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// 公共
// ─────────────────────────────────────────────

Result<GenerationPlan> Generator::plan() {
    GenerationPlan p;

    planDriverHeader(p);
    planDriverSource(p);

    for (const auto& inst : collectInstances()) {
        auto st = planInstance(p, inst.instanceName, inst.driver,
                               inst.version, inst.bindings, inst.config);
        if (!st.ok()) return st.error();
    }

    auto st = planChipDriver(p);
    if (!st.ok()) return st.error();

    planMakefile(p);

    return p;
}

Status Generator::write(const GenerationPlan& plan) {
    std::error_code ec;
    fs::create_directories(driverDir_, ec);
    if (ec) {
        return Error(ErrorCode::IoError,
                     "创建目录失败: " + driverDir_.string() + " - " + ec.message());
    }

    for (const auto& f : plan.files) {
        auto path = driverDir_ / f.relativePath;
        auto st = util::writeFileAtomic(path, f.content);
        if (!st.ok()) return st.error();
    }
    return {};
}

Status Generator::generate() {
    auto p = plan();
    if (!p) return p.error();
    return write(*p);
}

// ─────────────────────────────────────────────
// 实例收集 / 宏值
// ─────────────────────────────────────────────

std::vector<Generator::InstanceInfo> Generator::collectInstances() const {
    std::vector<InstanceInfo> out;
    for (const auto& d : state_.userDevices) {
        out.push_back({d.name, d.driver, d.version, d.bindings, d.config});
    }
    for (const auto& d : state_.intermediateDevices) {
        out.push_back({d.name, d.driver, d.version, d.bindings, d.config});
    }
    return out;
}

std::string Generator::macroValue(const std::string& v) const {
    for (const auto& d : state_.userDevices) {
        if (d.name == v) return v;
    }
    for (const auto& d : state_.intermediateDevices) {
        if (d.name == v) return v;
    }
    return "hpm_pin_" + v;
}

// ─────────────────────────────────────────────
// 查找芯片包目录
// ─────────────────────────────────────────────

fs::path Generator::findChipPackageDir() const {
    if (!fs::exists(packageCacheDir_)) return {};

    fs::path dir = packageCacheDir_;
    std::string name = chip_.name;

    auto slash = chip_.name.find('/');
    if (slash != std::string::npos) {
        auto domain = chip_.name.substr(0, slash);
        name = chip_.name.substr(slash + 1);
        dir = dir / domain;
    }

    if (!fs::exists(dir)) return {};

    // 芯片包目录名：<chip>-<version>（两段）
    std::string prefix = name + "-";
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        auto n = entry.path().filename().string();
        if (n.rfind(prefix, 0) != 0) continue;
        if (fs::exists(entry.path() / HPM_FILE_CHIP)) return entry.path();
    }
    return {};
}

// ─────────────────────────────────────────────
// driver.h
// ─────────────────────────────────────────────

void Generator::planDriverHeader(GenerationPlan& plan) {
    std::ostringstream out;
    out << "// 由 HPM 自动生成，请勿手动修改\n";
    out << "#pragma once\n\n";
    out << "#include <stdint.h>\n";
    out << "#include <stddef.h>\n\n";
    out << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

    // 芯片头
    out << "#include \"" << chip_.name << "/" << chip_.name << ".h\"\n";

    // 设备驱动头：include 裸驱动名（去掉 -<chip> 后缀）
    auto bareDriverName = [](const std::string& full) {
        auto slash = full.find('/');
        if (slash != std::string::npos) {
            auto base = full.substr(slash + 1);
            auto dash = base.rfind('-');
            if (dash != std::string::npos) base = base.substr(0, dash);
            return base;
        }
        auto dash = full.rfind('-');
        if (dash != std::string::npos) return full.substr(0, dash);
        return full;
    };

    for (const auto& dev : state_.userDevices) {
        out << "#include \"" << dev.name << "/" << bareDriverName(dev.driver) << ".h\"\n";
    }
    out << "\n";

    if (!chip_.provides.empty()) {
        out << "// ---------- " << chip_.name << " (chip) ----------\n";
        for (const auto& m : chip_.provides) {
            out << m.declaration(chip_.name + "_") << "\n";
        }
        out << "\n";
    }

    for (const auto& dev : state_.userDevices) {
        out << "// ---------- " << dev.name << " (" << dev.driver << ") ----------\n";
        const DriverManifest* dm = lib_.find(dev.driver);
        if (!dm) continue;

        bool hasInit = false;
        for (const auto& m : dm->provides) {
            if (m.name == "init") hasInit = true;
            out << m.declaration(dev.name + "_") << "\n";
        }
        if (!hasInit) {
            out << "int " << dev.name << "_init(void);\n";
        }
        out << "\n";
    }

    out << "void hpm_init_all(void);\n";

    out << "#ifdef __cplusplus\n}\n#endif\n";

    plan.addFile(HPM_FILE_DRIVER_H, out.str());
}

// ─────────────────────────────────────────────
// driver.c
// ─────────────────────────────────────────────

void Generator::planDriverSource(GenerationPlan& plan) {
    std::ostringstream out;
    out << "// 由 HPM 自动生成，请勿手动修改\n";
    out << "#include \"driver.h\"\n\n";
    out << "void hpm_init_all(void) {\n";

    if (chip_.providesMethod("init")) {
        out << "    " << chip_.name << "_init();\n";
    }

    // 1. 初始化中间设备（比如自动生成的 sys_tick）
    for (const auto& idev : state_.intermediateDevices) {
        out << "    " << idev.name << "_init();\n";
    }

    // 2. 初始化用户设备（app_delay, servo）
    for (const auto& dev : state_.userDevices) {
        out << "    " << dev.name << "_init();\n";
    }

    out << "}\n";

    plan.addFile(HPM_FILE_DRIVER_C, out.str());
}

// ─────────────────────────────────────────────
// 实例目录
// ─────────────────────────────────────────────

Status Generator::planInstance(GenerationPlan& plan,
                               const std::string& instanceName,
                               const std::string& driverName,
                               const Version& version,
                               const std::map<ResourceKey, std::string>& bindings,
                               const std::map<std::string, std::string>& config) {
    fs::path instDir = instanceName;

    const DriverManifest* dm = lib_.find(driverName);

    // 检查工具链兼容性
    if (dm && !dm->toolchain.empty()) {
        auto cc = chip_.toolchain.resolveCc();
        bool ok = false;
        for (const auto& t : dm->toolchain) {
            if (cc.find(t) != std::string::npos) { ok = true; break; }
        }
        if (!ok) {
            std::string supported;
            for (const auto& t : dm->toolchain) supported += t + " ";
            return Error(ErrorCode::InvalidArgument,
                         "驱动 " + driverName + " 不支持工具链 " + cc +
                         "（支持: " + supported + "）");
        }
    }

    // 先拷驱动源码
    auto st = copyPackageSources(plan, driverName, version, instDir);
    if (!st.ok()) return st.error();

    // hpm_dev.h
    {
        std::ostringstream out;
        out << "// 由 HPM 自动生成，请勿手动修改\n";
        out << "#pragma once\n\n";
        
        // 【修复 1】：使用宏守卫避免 DEVICE_NAME 重定义
        out << "#ifdef DEVICE_NAME\n";
        out << "#undef DEVICE_NAME\n";
        out << "#endif\n";
        
        out << "#define DEVICE_NAME  " << instanceName << "\n\n";
        for (const auto& [k, v] : bindings) {
            out << "#define " << k << "  " << macroValue(v) << "\n";
        }
        plan.addFile(instDir / "hpm_dev.h", out.str());
    }

    // config.h
    {
        std::ostringstream out;
        out << "// 由 HPM 自动生成，请勿手动修改\n";
        out << "#pragma once\n\n";

        // 【修复 2】：如果是中间设备，向上寻找它的父设备配置并继承
        std::map<std::string, std::string> finalConfig = config;
        if (instanceName.rfind("__", 0) == 0) { // 名字以 __ 开头，是中间设备
            for (const auto& userDev : state_.userDevices) {
                for (const auto& [key, val] : userDev.bindings) {
                    if (val == instanceName) {
                        // 找到了依赖它的顶层设备，把顶层设备的 config 继承过来
                        for (const auto& [ck, cv] : userDev.config) {
                            finalConfig[ck] = cv;
                        }
                        break;
                    }
                }
            }
        }

        // 普通配置（使用合并后的 finalConfig）
        for (const auto& [k, v] : finalConfig) {
            ConfigField cf;
            if (dm && dm->config.count(k)) {
                cf = dm->config.at(k);
            }

            if (cf.type == "string") {
                std::string s = v;
                if (cf.length > 0) {
                    if (static_cast<int>(s.size()) < cf.length) {
                        s.append(cf.length - s.size(), ' ');
                    } else if (static_cast<int>(s.size()) > cf.length) {
                        s = s.substr(0, cf.length);
                    }
                }
                out << "#define " << k << "  \"" << s << "\"\n";
            } else {
                out << "#define " << k << "  " << v << "\n";
            }
        }

        // 字符串描述符数组检查也要用 finalConfig
        bool hasStrings = false;
        for (const auto& [k, v] : finalConfig) {
            if (dm && dm->config.count(k) && dm->config.at(k).type == "string") {
                hasStrings = true;
                break;
            }
        }

        if (hasStrings) {
            out << "\n// 字符串描述符（只在 usb_descr.c 里生成）\n";
            out << "#define HPM_USB_STRINGS_DEFINED 1\n";
            out << "#ifdef HPM_GEN_STRINGS\n";

            auto genArray = [&](const std::string& key, const std::string& name) {
                auto it = finalConfig.find(key); // 改成 finalConfig
                if (it == finalConfig.end()) return;
                auto cfIt = dm->config.find(key);
                if (cfIt == dm->config.end()) return;
                if (cfIt->second.type != "string") return;

                std::string s = it->second;
                int len = cfIt->second.length;
                if (len > 0) {
                    if (static_cast<int>(s.size()) < len) {
                        s.append(len - s.size(), ' ');
                    } else if (static_cast<int>(s.size()) > len) {
                        s = s.substr(0, len);
                    }
                }

                out << "__code uint16_t " << name << "[] = {\n";
                out << "    (USB_DESCR_TYP_STRING << 8) | (2 + " << len << " * 2),\n    ";
                for (int i = 0; i < len; ++i) {
                    if (i > 0) out << ", ";
                    out << "'" << s[i] << "'";
                }
                out << "\n};\n";
            };

            genArray("usb_manufacturer", "ManufDescr");
            genArray("usb_product", "ProdDescr");
            genArray("usb_serial", "SerDescr");
            genArray("usb_interface", "InterfDescr");

            out << "#endif\n";
        }

        plan.addFile(instDir / "config.h", out.str());
    }

    return {};
}

// ─────────────────────────────────────────────
// 拷贝包源码
// ─────────────────────────────────────────────

Status Generator::copyPackageSources(GenerationPlan& plan,
                                     const std::string& driver,
                                     const Version& version,
                                     const std::filesystem::path& targetDir) {
    // driver 可能是 "domain/name"
    fs::path pkgBaseDir = packageCacheDir_;
    std::string name = driver;

    auto slash = driver.find('/');
    if (slash != std::string::npos) {
        auto domain = driver.substr(0, slash);
        name = driver.substr(slash + 1);
        pkgBaseDir = pkgBaseDir / domain;
    }

    auto pkgDir = pkgBaseDir / (name + "-" + version.toString());
    if (!fs::exists(pkgDir)) {
        return Error(ErrorCode::NotFound,
                     "驱动包未找到: " + pkgDir.string());
    }

    for (const char* sub : {HPM_DIR_SRC, HPM_DIR_INCLUDE, HPM_DIR_LD}) {
        auto dir = pkgDir / sub;
        if (!fs::exists(dir)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            auto rel = fs::relative(entry.path(), dir);
            auto target = targetDir / rel;

            auto content = util::readFile(entry.path());
            if (!content) return content.error();

            plan.addFile(target, *content);
        }
    }

    return {};
}

// ─────────────────────────────────────────────
// 芯片驱动
// ─────────────────────────────────────────────

Status Generator::planChipDriver(GenerationPlan& plan) {
    auto chipPkgDir = findChipPackageDir();
    if (chipPkgDir.empty()) {
        return Error(ErrorCode::NotFound,
                     "未找到芯片驱动包: " + chip_.name +
                         "（在 " + packageCacheDir_.string() + " 下）");
    }

    fs::path chipTargetDir = chip_.name;

    for (const char* sub : {HPM_DIR_SRC, HPM_DIR_INCLUDE, HPM_DIR_LD}) {
        auto dir = chipPkgDir / sub;
        if (!fs::exists(dir)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            auto rel = fs::relative(entry.path(), dir);
            auto target = chipTargetDir / rel;

            auto content = util::readFile(entry.path());
            if (!content) return content.error();

            plan.addFile(target, *content);
        }
    }

    return {};
}

// ─────────────────────────────────────────────
// Makefile
// ─────────────────────────────────────────────

std::vector<std::string> Generator::collectSources(const GenerationPlan& plan) const {
    std::vector<std::string> srcs;
    std::set<std::string> seen;

    srcs.push_back(HPM_FILE_DRIVER_C);
    seen.insert(HPM_FILE_DRIVER_C);

    for (const auto& f : plan.files) {
        if (f.relativePath.extension() != ".c") continue;
        auto s = f.relativePath.string();
        if (seen.count(s)) continue;
        seen.insert(s);
        srcs.push_back(s);
    }

    srcs.push_back("../main.c");
    return srcs;
}

std::vector<std::string> Generator::collectIncludes() const {
    std::vector<std::string> includes;
    includes.push_back(".");
    includes.push_back(chip_.name);
    for (const auto& inst : collectInstances()) {
        includes.push_back(inst.instanceName);
    }
    return includes;
}

void Generator::planMakefile(GenerationPlan& plan) {
    auto cc = chip_.toolchain.resolveCc();
    if (cc.find("sdcc") != std::string::npos) {
        planMakefileSdcc(plan);
    } else {
        planMakefileGcc(plan);
    }
}

// ─────────────────────────────────────────────
// SDCC 风格
// ─────────────────────────────────────────────

void Generator::planMakefileSdcc(GenerationPlan& plan) {
    std::ostringstream out;

    auto srcs = collectSources(plan);
    auto includes = collectIncludes();

    out << "# 由 HPM 自动生成（SDCC）\n\n";
    out << "CC := " << chip_.toolchain.resolveCc() << "\n";
    out << "PACKIHX := " << chip_.toolchain.resolveObjcopy() << "\n\n";

    out << "INCLUDES := \\\n";
    for (size_t i = 0; i < includes.size(); ++i) {
        out << "    -I" << includes[i];
        if (i + 1 < includes.size()) out << " \\";
        out << "\n";
    }
    out << "\n";

    out << "CFLAGS :=";
    for (const auto& f : chip_.build.flags) {
        out << " " << f;
    }
    out << " $(INCLUDES)\n";
    out << "LDFLAGS :=\n\n";

    out << "SRCS := \\\n";
    for (size_t i = 0; i < srcs.size(); ++i) {
        out << "    " << srcs[i];
        if (i + 1 < srcs.size()) out << " \\";
        out << "\n";
    }
    out << "\n";

    out << "OBJS := $(SRCS:.c=.rel)\n";
    out << "TARGET := ../build/firmware.ihx\n";
    out << "HEX    := ../build/firmware.hex\n\n";

    out << "all: $(TARGET) $(HEX)\n\n";

    out << "$(TARGET): $(OBJS)\n";
    out << "\t@mkdir -p ../build\n";
    out << "\t$(CC) $(OBJS) -o $@ $(LDFLAGS)\n\n";

    out << "$(HEX): $(TARGET)\n";
    out << "\t$(PACKIHX) $< > $@\n\n";

    out << "%.rel: %.c\n";
    out << "\t$(CC) $(CFLAGS) -c $< -o $@\n\n";

    out << ".PHONY: clean\n";
    out << "clean:\n";
    out << "\tfind . -name \"*.rel\" -delete\n";
    out << "\tfind . -name \"*.lst\" -delete\n";
    out << "\tfind . -name \"*.sym\" -delete\n";
    out << "\tfind . -name \"*.asm\" -delete\n";
    out << "\trm -f $(TARGET) $(HEX)\n";

    plan.addFile("Makefile", out.str());
}

// ─────────────────────────────────────────────
// GCC 风格
// ─────────────────────────────────────────────

void Generator::planMakefileGcc(GenerationPlan& plan) {
    std::ostringstream out;

    auto srcs = collectSources(plan);
    auto includes = collectIncludes();

    out << "# 由 HPM 自动生成（GCC）\n\n";
    out << "CC := " << chip_.toolchain.resolveCc() << "\n";
    out << "OBJCOPY := " << chip_.toolchain.resolveObjcopy() << "\n";
    out << "SIZE := " << chip_.toolchain.resolveSize() << "\n\n";

    out << "INCLUDES := \\\n";
    for (size_t i = 0; i < includes.size(); ++i) {
        out << "    -I" << includes[i];
        if (i + 1 < includes.size()) out << " \\";
        out << "\n";
    }
    out << "\n";

    // 加入了 -specs=nosys.specs 和 -specs=nano.specs
    out << "CFLAGS := -std=gnu11 -Wall -Wextra -Wno-implicit-function-declaration -Os";
    out << " -specs=nosys.specs -specs=nano.specs";
    for (const auto& f : chip_.build.flags) {
        out << " " << f;
    }
    out << " $(INCLUDES)\n";

    out << "LDFLAGS :=";
    if (!chip_.build.ldscript.empty()) {
        auto s = chip_.build.ldscript;
        auto slash = s.find_last_of('/');
        std::string fname = (slash == std::string::npos) ? s : s.substr(slash + 1);
        out << " -T " << chip_.name << "/" << fname;
    }
    // 链接阶段同样加入 -specs=nosys.specs 防止引用 crt0 和系统调用
    out << " -Wl,--gc-sections -specs=nosys.specs -specs=nano.specs\n\n";

    out << "BUILD_DIR := ../build\n\n";

    out << "SRCS := \\\n";
    for (size_t i = 0; i < srcs.size(); ++i) {
        out << "    " << srcs[i];
        if (i + 1 < srcs.size()) out << " \\";
        out << "\n";
    }
    out << "\n";

    out << "OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))\n\n";

    out << "TARGET := $(BUILD_DIR)/firmware.elf\n";
    out << "BIN    := $(BUILD_DIR)/firmware.bin\n\n";

    out << "all: $(TARGET) $(BIN)\n\n";

    out << "$(TARGET): $(OBJS)\n";
    out << "\t$(CC) $(OBJS) -o $@ $(LDFLAGS)\n\n";

    out << "$(BIN): $(TARGET)\n";
    out << "\t$(OBJCOPY) -O binary $< $@\n\n";

    out << "$(BUILD_DIR)/%.o: %.c\n";
    out << "\t@mkdir -p $(dir $@)\n";
    out << "\t$(CC) $(CFLAGS) -c $< -o $@\n\n";

    out << ".PHONY: clean size\n";
    out << "size: $(TARGET)\n";
    out << "\t$(SIZE) $<\n\n";
    out << "clean:\n";
    out << "\trm -rf $(BUILD_DIR)\n";

    plan.addFile("Makefile", out.str());
}
} // namespace hpm