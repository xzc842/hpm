# HPM · Hardware Package Manager

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![GitHub](https://img.shields.io/badge/GitHub-xzc842%2Fhpm-181717?logo=github)](https://github.com/xzc842/hpm)
[![GitCode](https://img.shields.io/badge/GitCode-xzc842%2Fhpm-C71D23?logo=git)](https://gitcode.com/xzc842/hpm)

**硬件包管理器** 用一份清单描述硬件，自动拉取驱动、解析依赖、生成代码、编译烧录。
---

## 目录

- [设计理念](#设计理念)
- [安装](#安装)
- [快速开始](#快速开始)
- [命令参考](#命令参考)
- [清单文件](#清单文件)
- [核心概念](#核心概念)
- [工作原理](#工作原理)
- [已验证平台](#已验证平台)
- [Roadmap](#roadmap)
- [参与贡献](#参与贡献)

---

## 设计理念

传统嵌入式开发：

1. 读芯片数据手册，找引脚复用表
2. 下载厂商 SDK，配置 IDE
3. 手动写外设初始化代码
4. 遇到引脚冲突，改代码重来
5. 换芯片时，所有代码重写

HPM 的答案：

1. **声明式**：用户描述「要什么」，工具决定「怎么做」
2. **分层解耦**：驱动是模板，硬件是参数，生成期绑定
3. **能力检查**：所有接口契约在生成期验证，不等到运行时
4. **资源独占**：引脚是唯一资源，冲突在解析时报错
5. **可复现**：`driver.json` 锁定版本和 ID

---

## 安装
### 从Release下载

[下载最新版本](https://github.com/AstercoreTeam/hpm/releases/latest)

### 从源码编译

需要 CMake 3.20+、C++20 编译器。

```bash
git clone --recurse-submodules https://github.com/xzc842/hpm.git
cd hpm
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cp build/bin/hpm /usr/local/bin/
```

如果克隆时没有带子模块：

```bash
git submodule update --init --recursive
```

### 工具链

HPM 会从芯片驱动读取需要的工具链。当前版本需要手动安装：

```bash
# ARM Cortex-M
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi

# 8051（CH552）
sudo apt install sdcc

# 烧录工具
# STM32: st-flash（stlink-tools）
# CH552: wchisp（cargo install wchisp 或从源码编译）
```

自动下载工具链在 Roadmap 中。

---

## 快速开始

### 1. 初始化项目

```bash
mkdir my-project && cd my-project
hpm init --mcu ch552g
```

生成 `project.json` 和 `.gitignore`。

### 2. 编辑清单

`project.toml`

HPM 的清单文件默认使用 TOML。同一目录下若同时存在 `project.toml` 和 `project.json`，**以 `project.toml` 为准**，并自动回写同步 `project.json`。

```toml
mcu = "ch552g"

[devices.serial]
driver = "ch552_usb_cdc"

[devices.serial.config]
usb_vid              = "0x1209"
usb_pid              = "0x0001"
usb_device_version   = "0x0100"
usb_max_power_ma     = "100"
usb_manufacturer     = "XZC"
usb_product          = "CH552 TEST CDC"
usb_serial           = "0001"
usb_interface        = "CDC"
```


### 3. 写应用

`main.c`：

```c
#include "driver.h"

void main(void) {
    hpm_init_all();

    serial_write('H');
    serial_write('P');
    serial_write('M');
    serial_write('\n');

    while (1) {
        if (serial_available()) {
            serial_write(serial_read());
        }
    }
}
```

**只 include `driver.h`。**

### 4. 同步、编译、烧录

```bash
hpm sync
hpm build
hpm flash
```

### 5. 在模拟器中运行

`hpm debug` 会先检查固件是否已编译，然后启动随 HPM 一起构建的 mcus 模拟器。
芯片模型和模拟器配置文件由用户在命令行指定：

```bash
hpm debug \
  --isa build/lib/libmcus_isa_8051.so \
  --model debug/mcus/models/ch552g.json \
  --config debug/mcus/config/example.toml
```

`--isa` 是 ISA 动态库路径，可以显式指定；不指定时按模型的 `arch` 自动推导
（如 `8051` → `build/lib/libmcus_isa_8051.so`），也支持 `HPM_ISA` 环境变量。

默认使用 `build/` 下最新的 `firmware.hex` / `firmware.ihx` / `firmware.bin`；
默认启动后暂停，等待 `run` / `step`；`--run` 可启动即运行。

| 参数 | 必填 | 默认 | 说明 |
|---|---|---|---|
| `--model <json>` | 是 | — | mcus 芯片模型 |
| `--config <toml>` | 是 | — | mcus 配置 |
| `--isa <so>` | 否 | 按 arch 推导 | ISA 动态库 |
| `--mcus <bin>` | 否 | `build/bin/mcus` | 模拟器 |
| `--firmware <file>` | 否 | `build/` 下最新固件 | 覆盖固件 |
| `--json` | 否 | — | JSON Lines 模式 |
| `--run` | 否 | 暂停 | 启动即运行 |
| `--allow-missing-debug` | 否 | — | 允许设备无 debug 插件 |
| `--host-cc <cc>` | 否 | `cc` | 编译设备插件 |
| `--dry-run` | 否 | — | 只打印命令 |

设备可以来自驱动包的 `debug/debug.c`，会被编译成插件并在会话中加载。
外部程序调用使用 JSON 协议：

```bash
hpm debug --json --isa build/lib/libmcus_isa_8051.so \
  --model ... --config ...
```

详见 [设备虚拟化插件规范](docs/debug_plugin.md) 与 [JSON 协议](docs/debug_protocol.md)。
`--dry-run` 可以只打印最终启动命令。

> 更多示例见[example](example/)
---

## 命令参考

### `hpm`

| 命令 | 说明 |
|------|------|
| `hpm init [--mcu <型号>]` | 初始化项目 |
| `hpm sync [--dry-run]` | 拉取依赖、解析、生成 `driver/` |
| `hpm build` | 编译固件到 `build/` |
| `hpm flash` | 烧录固件 |
| `hpm debug [--json] --model <model.json> --config <config.toml>` | 在 mcus 模拟器中运行固件并加载设备插件 |
| `hpm clean` | 删除 `build/` |
| `hpm dev --export` | 生成 `hpm.h`（底层 API） |

**全局选项**：

| 选项 | 说明 |
|------|------|
| `-C, --project-dir <dir>` | 项目目录（默认当前） |
| `--cache-dir <dir>` | 缓存目录（默认 `~/.hpm`） |
| `-v, --verbose` | 详细输出 |
| `--version` | 显示版本 |

---

## 清单文件

### `project.toml`

```toml
mcu = "ch552g"

[devices.<设备名>]
driver = "<驱动名>"

[devices.<设备名>.<资源key>]
```

- `<设备名>`：应用语义名（TOML 表名），决定生成的 API 前缀，如 `serial` → `serial_write()`
- `driver`：驱动包名字
- `<资源key>`：跟驱动 `requires` 的 key 一致；简单值可直接写在 `[devices.<设备名>]` 下，复杂结构（如 `config` 子表）用 `[devices.<设备名>.<资源key>]` 展开

> 也支持 JSON 格式：`project.json` 仍可读取。推荐直接使用 `project.toml`。

---

## 技术说明
见 [ARCHITECTURE.md](ARCHITECTURE.md)

---

## 已验证平台

### CH552G（8051 + USB CDC）

| 项目 | 内容 |
|------|------|
| 芯片驱动 | `ch552g-1.0.0` |
| 设备驱动 | `usb_cdc-ch552-1.0.0` |
| 工具链 | `sdcc` + `packihx` |
| 烧录 | `hpm flash` |
| 验证内容 | USB 枚举、串口收发、回显、交互程序 |

### STM32F103C8T6

| 项目 | 内容 |
|------|------|
| 芯片驱动 | `stm32f103c8t6-1.0.0` |
| 设备驱动 | 无 |
| 工具链 | `arm-none-eabi-gcc` + `st-flash` |
| 烧录 | `hpm flash` |
| 验证内容 | LED点灯 |

### STC90C51

| 项目 | 内容 |
|------|------|
| 芯片驱动 | `stc90c51-0.1.0` |
| 设备驱动 | `pwm-stc90c51-1.0.0` `time-stc90c51-1.0.0` |
| 工具链 | `sdcc` + `stcgal` |
| 烧录 | `hpm flash` |
| 验证内容 | PWM驱动舵机，定时器 |

---

## Roadmap

### 已完成

- [x] semver、版本约束
- [x] JSON 读写（值语义，不抛异常）
- [x] 清单解析（project / driver / state / chip）
- [x] 能力检查（引脚、方法、名字）
- [x] 依赖解析（两阶段，中间设备复用）
- [x] 代码生成（实例目录、hpm_dev.h、Makefile、方法签名）
- [x] CLI（init / sync / build / flash / clean / dev）
- [x] 裸机编译（ARM 交叉工具链、链接脚本、启动文件）
- [x] 芯片驱动 `provides`（`hpm_init_all` 调芯片初始化）
- [x] 工具链显式声明（`cc`/`objcopy`/`prefix`）
- [x] 方法签名结构化（`return` + `params`）
- [x] CH552G USB CDC 真机验证
- [x] Makefile 源文件扫描（支持一个驱动多个 `.c`）
- [x] `hpm.h` 完整实现
- [x] registry：从 GitHub 拉驱动包和索引
- [x] 签名校验：包完整性
- [x] 多镜像支持：项目 / 用户 / 全局优先级
- [x] hpm-dev：驱动开发工具（validate）
- [x] 许可证处理：驱动包带 license
- [x] hpm-dev：驱动开发工具（init / test / publish）
- [x] 自研 MCUS 通用架构单片机模拟器：`hpm debug` 支持单片机和设备虚拟化测试
- [x] hpm-gui：图形化生成 `project.json`
- [x] KiCad 导入：从原理图生成 `project.json`

### 进行中

- [ ] **适配Windows**
- [ ] **toolchain 管理**：自动下载工具链
- [ ] **AI 驱动包生成**：从文档 + 开源代码整理驱动包

### 计划中


- [ ] **虚拟仿真环境**：通过blender插件将虚拟设备添加并绑定到3d场景，在godot模拟器上进行仿真，计划通过**[影梦启动器](https://gitcode.com/astercore/YingmengStudio)**生成虚拟环境（通过插件接入hpm-debug）

---

## 参与贡献

### 开发

见 [CONTRIBUTING.md](CONTRIBUTING.md)

### 驱动包贡献

见 [写驱动包](docs/device_driver.md)。

### 芯片驱动贡献

见 [写芯片驱动](docs/mcu_driver.md)。优先支持：

- ESP32-C3
- RP2040
- CH32V003


### 文档

[docs/](docs/) 目录下

## 致谢

本项目的开发离不开以下优秀开源项目和社区的贡献，在此我们表示由衷的感谢。

| 项目 | 用途 | 开源协议 |
| :--- | :--- | :--- |
| [nlohmann/json](https://github.com/nlohmann/json.git) | JSON 解析 | MIT |
| [fmt](https://github.com/fmtlib/fmt.git) | 字符串格式化 | MIT |
| [CLI11](https://github.com/CLIUtils/CLI11.git) | 命令行解析 | BSD-3-Clause |
| [Catch2](https://github.com/catchorg/Catch2.git) | 单元测试 | BSL-1.0 |
| [libcurl](https://github.com/curl/curl.git) | HTTP 客户端（registry 用） | curl（MIT 类） |
| [toml++](https://github.com/marzer/tomlplusplus.git) | TOML 清单解析 | MIT |
| [OpenSSL](https://github.com/openssl/openssl.git) | 签名校验 / HTTPS | Apache-2.0 |
| [libzip](https://github.com/nih-at/libzip.git) | 包打包 / 解包 | BSD-3-Clause |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib.git) | HTTP 客户端 / OAuth 回调 server | MIT |

## 开源许可

Apache-2.0 [LICENSE](LICENSE)

## 贡献

欢迎提交 Issue 和 Pull Request。请参阅[CONTRIBUTING.md](CONTRIBUTING.md)、[ToDo文档](ToDo.md)

## 联系方式
---

作者

Xue Zicheng <xuezicheng842@outlook.com>

GitCode: https://gitcode.com/xzc842

---
