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

// tests/unit/test_generator.cpp
#include <catch2/catch_test_macros.hpp>
#include <hpm/generator/generator.h>
#include <hpm/generator/template_engine.h>
#include <hpm/json/json_io.h>
#include <hpm/manifest/chip_loader.h>
#include <hpm/manifest/driver_loader.h>

#include <filesystem>
#include <fstream>

using namespace hpm;
using namespace hpm::manifest;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────
// 模板引擎
// ─────────────────────────────────────────────

TEST_CASE("template engine basic", "[generator]") {
    std::map<std::string, std::string> vars = {
        {"DEVICE_NAME", "door_reader"},
        {"bus", "__spi_a3f2"},
    };
    auto out = renderTemplate(
        "int {{DEVICE_NAME}}_init(void);  // bus={{bus}}", vars);
    REQUIRE(out == "int door_reader_init(void);  // bus=__spi_a3f2");
}

TEST_CASE("template engine unknown key", "[generator]") {
    std::map<std::string, std::string> vars = {{"a", "1"}};
    auto out = renderTemplate("{{a}} {{b}}", vars);
    REQUIRE(out == "1 {{b}}");
}

// ─────────────────────────────────────────────
// 测试夹具
// ─────────────────────────────────────────────

namespace {

DriverLibrary makeLib() {
    DriverLibrary lib;
    {
        const char* text = R"({
            "name": "rc522",
            "version": "1.2.0",
            "requires": {
                "bus": { "type": "spi" },
                "cs":  { "type": "gpio_out" },
                "rst": { "type": "gpio_out", "optional": true }
            },
            "provides": { "methods": ["init", "read_card"] }
        })";
        lib.add(*loadDriverManifest(*json::parse(text)));
    }
    {
        const char* text = R"({
            "name": "spi",
            "version": "1.0.0",
            "requires": {
                "sck":  { "type": "spi_sck" },
                "miso": { "type": "spi_miso" },
                "mosi": { "type": "spi_mosi" }
            },
            "provides": { "methods": ["init", "transfer"] }
        })";
        lib.add(*loadDriverManifest(*json::parse(text)));
    }
    return lib;
}

DriverState makeState() {
    DriverState s;

    Device dr;
    dr.name = "door_reader";
    dr.driver = "rc522";
    dr.version = *Version::parse("1.2.0");
    dr.bindings["bus"] = "__spi_a3f2";
    dr.bindings["cs"]  = "PA4";
    dr.bindings["rst"] = "PA3";
    s.userDevices.push_back(dr);

    IntermediateDevice idev;
    idev.id = "__spi_a3f2";
    idev.name = "__spi_a3f2";
    idev.driver = "spi";
    idev.version = *Version::parse("1.0.0");
    idev.bindings["sck"]  = "PA5";
    idev.bindings["miso"] = "PA6";
    idev.bindings["mosi"] = "PA7";
    s.intermediateDevices.push_back(idev);

    return s;
}

Project makeProject() {
    Project p;
    p.mcu = "stm32f103c8t6";
    return p;
}

Chip makeChip() {
    Chip c;
    c.name = "stm32f103c8t6";
    return c;
}

// 造假的驱动包 + 芯片包
void setupFakePackages(const fs::path& root) {
    // rc522-1.2.0
    {
        auto d = root / "rc522-1.2.0";
        fs::create_directories(d / "src");
        fs::create_directories(d / "include");
        fs::create_directories(d / "debug");
        std::ofstream(d / "src" / "rc522.c") << "// rc522 driver\n";
        std::ofstream(d / "include" / "rc522.h") << "// rc522 header\n";
        std::ofstream(d / "debug" / "debug.c") << "// debug plugin\n";
    }
    // spi-1.0.0
    {
        auto d = root / "spi-1.0.0";
        fs::create_directories(d / "src");
        fs::create_directories(d / "include");
        std::ofstream(d / "src" / "spi.c") << "// spi driver\n";
        std::ofstream(d / "include" / "spi.h") << "// spi header\n";
    }
    // stm32f103c8t6-1.0.0（芯片驱动包）
    {
        auto d = root / "stm32f103c8t6-1.0.0";
        fs::create_directories(d / "src");
        fs::create_directories(d / "include");
        std::ofstream(d / "chip.json") << R"({
            "name": "stm32f103c8t6",
            "toolchain": { "name": "gcc" },
            "flash": { "tool": "echo" },
            "pins": {}
        })";
        std::ofstream(d / "src" / "stm32f103c8t6.c") << "// chip driver\n";
        std::ofstream(d / "src" / "hpm_pin.c") << "// pin driver\n";
        std::ofstream(d / "include" / "stm32f103c8t6.h") << "// chip header\n";
        std::ofstream(d / "include" / "hpm_pin.h") << "// pin header\n";
    }
}

} // namespace

// ─────────────────────────────────────────────
// 生成计划
// ─────────────────────────────────────────────

TEST_CASE("generator creates per-instance dirs", "[generator]") {
    auto lib   = makeLib();
    auto state = makeState();
    auto proj  = makeProject();
    auto chip  = makeChip();

    auto tmp = fs::temp_directory_path() / "hpm_gen_test";
    fs::remove_all(tmp);
    setupFakePackages(tmp / "packages");

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto plan = gen.plan();
    REQUIRE(plan.ok());

    // door_reader/hpm_dev.h
    bool foundDoorReaderMacro = false;
    for (const auto& f : plan->files) {
        if (f.relativePath == "door_reader/hpm_dev.h") {
            foundDoorReaderMacro = true;
            REQUIRE(f.content.find("#define DEVICE_NAME  door_reader") != std::string::npos);
            REQUIRE(f.content.find("#define cs  hpm_pin_PA4") != std::string::npos);
            REQUIRE(f.content.find("#define rst  hpm_pin_PA3") != std::string::npos);
            REQUIRE(f.content.find("#define bus  __spi_a3f2") != std::string::npos);
        }
    }
    REQUIRE(foundDoorReaderMacro);

    // __spi_a3f2/hpm_dev.h
    bool foundSpiMacro = false;
    for (const auto& f : plan->files) {
        if (f.relativePath == "__spi_a3f2/hpm_dev.h") {
            foundSpiMacro = true;
            REQUIRE(f.content.find("#define DEVICE_NAME  __spi_a3f2") != std::string::npos);
            REQUIRE(f.content.find("#define sck  hpm_pin_PA5") != std::string::npos);
            REQUIRE(f.content.find("#define miso  hpm_pin_PA6") != std::string::npos);
            REQUIRE(f.content.find("#define mosi  hpm_pin_PA7") != std::string::npos);
        }
    }
    REQUIRE(foundSpiMacro);

    // 驱动源码被拷贝到实例目录
    bool foundRc522Src = false;
    for (const auto& f : plan->files) {
        if (f.relativePath == "door_reader/rc522.c") foundRc522Src = true;
    }
    REQUIRE(foundRc522Src);

    // 芯片驱动目录
    bool foundChip = false;
    for (const auto& f : plan->files) {
        if (f.relativePath == "stm32f103c8t6/stm32f103c8t6.c") foundChip = true;
    }
    REQUIRE(foundChip);

    fs::remove_all(tmp);
}

TEST_CASE("generator driver.h has correct signatures", "[generator]") {
    DriverLibrary lib;
    {
        const char* text = R"({
            "name": "testdrv",
            "version": "1.0.0",
            "provides": {
                "methods": [
                    { "name": "init", "return": "int", "params": [] },
                    {
                        "name": "send",
                        "return": "int",
                        "params": [
                            { "type": "const unsigned char*", "name": "buf" },
                            { "type": "size_t", "name": "len" }
                        ]
                    }
                ]
            }
        })";
        lib.add(*loadDriverManifest(*json::parse(text)));
    }

    DriverState state;
    Device d;
    d.name = "my_dev";
    d.driver = "testdrv";
    d.version = *Version::parse("1.0.0");
    state.userDevices.push_back(d);

    Project proj;
    Chip chip;
    chip.name = "testchip";

    auto tmp = fs::temp_directory_path() / "hpm_gen_sig_test";
    fs::remove_all(tmp);
    {
        auto pkg = tmp / "packages" / "testdrv-1.0.0";
        fs::create_directories(pkg / "src");
        std::ofstream(pkg / "src" / "testdrv.c") << "// t\n";
        fs::create_directories(tmp / "packages" / "testchip-1.0.0" / "src");
        std::ofstream(tmp / "packages" / "testchip-1.0.0" / "chip.json") << R"({
            "name": "testchip",
            "toolchain": { "prefix": "arm-none-eabi-" },
            "flash": { "tool": "echo" },
            "pins": {}
        })";
        std::ofstream(tmp / "packages" / "testchip-1.0.0" / "src" / "testchip.c") << "// c\n";
    }

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto plan = gen.plan();
    if (!plan.ok()) {
        WARN("plan error: " << plan.error().message());
    }
    REQUIRE(plan.ok());

    bool found = false;
    for (const auto& f : plan->files) {
        if (f.relativePath == "driver.h") {
            found = true;
            REQUIRE(f.content.find("int my_dev_send(const unsigned char* buf, size_t len);") != std::string::npos);
            REQUIRE(f.content.find("int my_dev_init(void);") != std::string::npos);
        }
    }
    REQUIRE(found);

    fs::remove_all(tmp);
}

TEST_CASE("generator Makefile includes all sources", "[generator]") {
    auto lib   = makeLib();
    auto state = makeState();
    auto proj  = makeProject();
    auto chip  = makeChip();

    auto tmp = fs::temp_directory_path() / "hpm_gen_makefile_test";
    fs::remove_all(tmp);
    setupFakePackages(tmp / "packages");

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto plan = gen.plan();
    REQUIRE(plan.ok());

    bool foundMakefile = false;
    for (const auto& f : plan->files) {
        if (f.relativePath == "Makefile") {
            foundMakefile = true;
            REQUIRE(f.content.find("door_reader/rc522.c") != std::string::npos);
            REQUIRE(f.content.find("__spi_a3f2/spi.c") != std::string::npos);
            REQUIRE(f.content.find("stm32f103c8t6/stm32f103c8t6.c") != std::string::npos);
            REQUIRE(f.content.find("-Idoor_reader") != std::string::npos);
            REQUIRE(f.content.find("-I__spi_a3f2") != std::string::npos);
        }
    }
    REQUIRE(foundMakefile);

    fs::remove_all(tmp);
}

TEST_CASE("generator writes files to disk", "[generator]") {
    auto lib   = makeLib();
    auto state = makeState();
    auto proj  = makeProject();
    auto chip  = makeChip();

    auto tmp = fs::temp_directory_path() / "hpm_gen_write_test";
    fs::remove_all(tmp);
    setupFakePackages(tmp / "packages");

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto st = gen.generate();
    REQUIRE(st.ok());

    REQUIRE(fs::exists(tmp / "driver" / "driver.h"));
    REQUIRE(fs::exists(tmp / "driver" / "driver.c"));
    REQUIRE(fs::exists(tmp / "driver" / "Makefile"));
    REQUIRE(fs::exists(tmp / "driver" / "door_reader" / "hpm_dev.h"));
    REQUIRE(fs::exists(tmp / "driver" / "door_reader" / "rc522.c"));
    REQUIRE(fs::exists(tmp / "driver" / "__spi_a3f2" / "hpm_dev.h"));
    REQUIRE(fs::exists(tmp / "driver" / "__spi_a3f2" / "spi.c"));
    REQUIRE(fs::exists(tmp / "driver" / "stm32f103c8t6" / "stm32f103c8t6.c"));
    REQUIRE(fs::exists(tmp / "driver" / "stm32f103c8t6" / "hpm_pin.c"));

    fs::remove_all(tmp);
}

TEST_CASE("generator missing package", "[generator]") {
    auto lib   = makeLib();
    auto state = makeState();
    auto proj  = makeProject();
    auto chip  = makeChip();

    auto tmp = fs::temp_directory_path() / "hpm_gen_missing_test";
    fs::remove_all(tmp);
    // 不建 packages

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto plan = gen.plan();
    REQUIRE_FALSE(plan.ok());
    REQUIRE(plan.error().code() == ErrorCode::NotFound);

    fs::remove_all(tmp);
}

TEST_CASE("generator missing chip package", "[generator]") {
    auto lib   = makeLib();
    auto state = makeState();
    auto proj  = makeProject();
    auto chip  = makeChip();

    auto tmp = fs::temp_directory_path() / "hpm_gen_missing_chip_test";
    fs::remove_all(tmp);

    // 只建 rc522 和 spi，不建芯片包
    {
        auto d = tmp / "packages" / "rc522-1.2.0";
        fs::create_directories(d / "src");
        std::ofstream(d / "src" / "rc522.c") << "// rc522\n";
    }
    {
        auto d = tmp / "packages" / "spi-1.0.0";
        fs::create_directories(d / "src");
        std::ofstream(d / "src" / "spi.c") << "// spi\n";
    }

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto plan = gen.plan();
    REQUIRE_FALSE(plan.ok());
    REQUIRE(plan.error().code() == ErrorCode::NotFound);

    fs::remove_all(tmp);
}
TEST_CASE("generator excludes driver debug sources", "[generator]") {
    auto lib   = makeLib();
    auto state = makeState();
    auto proj  = makeProject();
    auto chip  = makeChip();

    auto tmp = fs::temp_directory_path() / "hpm_gen_debug_test";
    fs::remove_all(tmp);
    setupFakePackages(tmp / "packages");

    Generator gen(state, lib, proj, chip, tmp / "packages", tmp / "driver");
    auto plan = gen.plan();
    REQUIRE(plan.ok());

    for (const auto& f : plan->files) {
        REQUIRE(f.relativePath.string().find("debug") == std::string::npos);
    }
}
