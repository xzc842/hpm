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

#include "creat_cmd.h"

#include <hpm/config.h>
#include <hpm/json/json_io.h>
#include <hpm/util/fs_utils.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include "hpm/config.h"

namespace hpm::cli {

namespace fs = std::filesystem;

namespace {

void ok(const std::string& msg) {
    std::cout << "\033[32mok\033[0m " << msg << "\n";
}

void err(const std::string& msg) {
    std::cerr << "\033[31merror\033[0m " << msg << "\n";
}

const char* kMitLicense =
"MIT License\n"
"\n"
"Copyright (c) 2026 HPM contributors\n"
"\n"
"Permission is hereby granted, free of charge, to any person obtaining a copy\n"
"of this software and associated documentation files (the \"Software\"), to deal\n"
"in the Software without restriction, including without limitation the rights\n"
"to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
"copies of the Software, and to permit persons to whom the Software is\n"
"furnished to do so, subject to the following conditions:\n"
"\n"
"The above copyright notice and this permission notice shall be included in all\n"
"copies or substantial portions of the Software.\n"
"\n"
"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
"SOFTWARE.\n";

// 生成 driver manifest.json
std::string makeDriverManifest(const std::string& name,
                               const std::string& chip,
                               const std::string& version,
                               const std::string& license) {
    auto root = json::Value::object();
    root.set("name", json::Value(name));
    root.set("chip", json::Value(chip));
    root.set("version", json::Value(version));
    root.set("description", json::Value(name + " driver"));
    root.set("license", json::Value(license));

    auto cfg = json::Value::object();
    root.set("config", cfg);

    auto req = json::Value::object();
    root.set("requires", req);

    auto provides = json::Value::object();
    auto methods = json::Value::array();

    auto initM = json::Value::object();
    initM.set("name", json::Value("init"));
    initM.set("return", json::Value("int"));
    initM.set("params", json::Value::array());
    methods.push(initM);

    provides.set("methods", methods);
    root.set("provides", provides);

    return json::dump(root, 2) + "\n";
}

// 生成 chip.json
std::string makeChipJson(const std::string& name,
                         const std::string& version,
                         const std::string& license) {
    (void)version;
    auto root = json::Value::object();
    root.set("name", json::Value(name));
    root.set("chip", json::Value(name));
    root.set("family", json::Value(""));
    root.set("package", json::Value(""));
    root.set("license", json::Value(license));

    auto build = json::Value::object();
    build.set("cpu", json::Value(""));
    auto flags = json::Value::array();
    flags.push(json::Value("-mthumb"));
    build.set("flags", flags);
    build.set("ldscript", json::Value("ld/" + name + ".ld"));
    build.set("startup", json::Value("src/startup.c"));
    root.set("build", build);

    auto tc = json::Value::object();
    tc.set("cc", json::Value("arm-none-eabi-gcc"));
    tc.set("objcopy", json::Value("arm-none-eabi-objcopy"));
    tc.set("size", json::Value("arm-none-eabi-size"));
    root.set("toolchain", tc);

    auto flash = json::Value::object();
    flash.set("tool", json::Value("st-flash"));
    auto args = json::Value::array();
    args.push(json::Value("write"));
    args.push(json::Value("{bin}"));
    args.push(json::Value("0x8000000"));
    flash.set("args", args);
    root.set("flash", flash);

    auto provides = json::Value::object();
    auto methods = json::Value::array();
    auto initM = json::Value::object();
    initM.set("name", json::Value("init"));
    initM.set("return", json::Value("void"));
    initM.set("params", json::Value::array());
    methods.push(initM);
    provides.set("methods", methods);
    root.set("provides", provides);

    auto pins = json::Value::object();
    root.set("pins", pins);

    return json::dump(root, 2) + "\n";
}

std::string makeDriverC(const std::string& name) {
    return
"#include \"" + name + ".h\"\n"
"#include <stddef.h>\n"
"\n"
"#define HPM_CAT_(a, b) a##b\n"
"#define HPM_CAT(a, b)  HPM_CAT_(a, b)\n"
"#define HPM_FN(name)   HPM_CAT(DEVICE_NAME, HPM_CAT(_, name))\n"
"#define HPM_CALL(prefix, method)  HPM_CAT(prefix, HPM_CAT(_, method))\n"
"\n"
"int HPM_FN(init)(void) {\n"
"    // TODO: 初始化硬件\n"
"    return 0;\n"
"}\n";
}

std::string makeDriverH(const std::string& name) {
    return
"#pragma once\n"
"#include \"hpm_dev.h\"\n"
"\n"
"// " + name + " 驱动内部头\n";
}

std::string makeChipC(const std::string& name) {
    return
"#include \"" + name + ".h\"\n"
"\n"
"void " + name + "_init(void) {\n"
"    // TODO: 芯片初始化（时钟、外设）\n"
"}\n";
}

std::string makeChipH(const std::string& name) {
    return
"#pragma once\n"
"\n"
"void " + name + "_init(void);\n";
}

std::string makeHpmPinH() {
    return
"#pragma once\n"
"#include <stdint.h>\n"
"\n"
"// 每个引脚提供 init / write / read\n"
"// void hpm_pin_<PIN>_init(void);\n"
"// void hpm_pin_<PIN>_write(int level);\n"
"// int  hpm_pin_<PIN>_read(void);\n";
}

std::string makeHpmPinC() {
    return
"#include \"hpm_pin.h\"\n"
"#include \"CH552.H\"  // 改成你的芯片头文件\n"
"\n"
"// 示例：\n"
"// #define DEF_PIN(port, bit) \\\n"
"//     void hpm_pin_##port##_##bit##_init(void) { ... } \\\n"
"//     void hpm_pin_##port##_##bit##_write(int level) { ... } \\\n"
"//     int hpm_pin_##port##_##bit##_read(void) { ... }\n"
"\n"
"// DEF_PIN(PA, 0)\n";
}

std::string makeLinkerScript(const std::string& name) {
    (void)name;
    return
"ENTRY(Reset_Handler)\n"
"\n"
"MEMORY {\n"
"    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K\n"
"    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K\n"
"}\n"
"\n"
"SECTIONS {\n"
"    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH\n"
"    .text : { *(.text*) *(.rodata*) } > FLASH\n"
"\n"
"    _sidata = LOADADDR(.data);\n"
"    .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT > FLASH\n"
"    .bss : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM\n"
"\n"
"    _estack = ORIGIN(RAM) + LENGTH(RAM);\n"
"}\n";
}

std::string makeReadme(const std::string& name, const std::string& type) {
    std::string header = (type == "chip")
        ? (name + "-0.1.0")
        : (name + "-*-0.1.0");
    return
"# " + name + "\n"
"\n"
"HPM " + type + " package.\n"
"\n"
"## 结构\n"
"\n"
"```\n"
+ header + "/\n"
"├── " + (type == "chip" ? HPM_FILE_CHIP : HPM_FILE_MANIFEST) + "\n"
"├── LICENSE\n"
"├── README.md\n"
"├── src/\n"
"└── include/\n"
"```\n";
}

} // namespace

void CreateCmd::setup(CLI::App& app) {
    app.add_option("name", name_, "包名（如 rc522、stm32f103c8t6）")->required();
    app.add_option("--type", type_, "driver / chip")->check(CLI::IsMember({"driver", "chip"}));
    app.add_option("--version", version_, "版本号（默认 0.1.0）");
    app.add_option("--license", license_, "许可证（默认 MIT）");
    app.add_option("--dir", dir_, "输出目录（默认当前目录）");
    app.add_option("--chip", chip_, "目标芯片（默认 * 通用）");
}

int CreateCmd::run(DevContext& ctx) {
    (void)ctx;

    if (name_.empty()) {
        err("包名不能为空");
        return 1;
    }

    fs::path base = dir_.empty() ? fs::current_path() : fs::path(dir_);

    // 芯片包 <chip>-<version>；驱动包 <driver>-<chip>-<version>
    std::string chipSeg = (type_ == "chip") ? "" : (chip_ + "-");
    fs::path pkgDir = base / (name_ + "-" + chipSeg + version_);

    if (fs::exists(pkgDir)) {
        err("目录已存在: " + pkgDir.string());
        return 1;
    }

    std::error_code ec;
    fs::create_directories(pkgDir / HPM_DIR_SRC, ec);
    fs::create_directories(pkgDir / HPM_DIR_INCLUDE, ec);

    if (type_ == "chip") {
        fs::create_directories(pkgDir / HPM_DIR_LD, ec);
    }

    // manifest.json / chip.json
    {
        std::string content;
        if (type_ == "chip") {
            content = makeChipJson(name_, version_, license_);
        } else {
            content = makeDriverManifest(name_, chip_, version_, license_);
        }
        fs::path path = pkgDir / (type_ == "chip" ? HPM_FILE_CHIP : HPM_FILE_MANIFEST);
        auto st = util::writeFileAtomic(path, content);
        if (!st.ok()) { err(st.error().message()); return 1; }
    }

    // LICENSE
    {
        auto st = util::writeFileAtomic(pkgDir / "LICENSE", kMitLicense);
        if (!st.ok()) { err(st.error().message()); return 1; }
    }

    // README.md
    {
        auto st = util::writeFileAtomic(pkgDir / "README.md",
                                        makeReadme(name_, type_));
        if (!st.ok()) { err(st.error().message()); return 1; }
    }

    // src / include
    if (type_ == "chip") {
        util::writeFileAtomic(pkgDir / HPM_DIR_SRC / (name_ + ".c"), makeChipC(name_));
        util::writeFileAtomic(pkgDir / HPM_DIR_SRC / "hpm_pin.c", makeHpmPinC());
        util::writeFileAtomic(pkgDir / HPM_DIR_INCLUDE / (name_ + ".h"), makeChipH(name_));
        util::writeFileAtomic(pkgDir / HPM_DIR_INCLUDE / "hpm_pin.h", makeHpmPinH());
        util::writeFileAtomic(pkgDir / HPM_DIR_LD / (name_ + ".ld"), makeLinkerScript(name_));
    } else {
        util::writeFileAtomic(pkgDir / HPM_DIR_SRC / (name_ + ".c"), makeDriverC(name_));
        util::writeFileAtomic(pkgDir / HPM_DIR_INCLUDE / (name_ + ".h"), makeDriverH(name_));
    }

    ok("已创建 " + pkgDir.string());
    std::cout << "\n";
    return 0;
}

} // namespace hpm::cli