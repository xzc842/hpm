# HPM 架构设计文档

## 1. 核心工作流
用户通过 `project.toml` 声明硬件需求 -> HPM 解析依赖并检查引脚冲突 -> 生成 `driver/` C代码目录 -> 调用底层工具链编译 -> 烧录。

## 2. 驱动分层体系
- **芯片驱动 (Chip Driver)**: 唯一接触寄存器的层，提供 GPIO 基础能力和时钟初始化。
- **总线驱动 (Bus Driver)**: 管理 SPI/I2C 等，负责引脚申请。
- **设备驱动 (Device Driver)**: 传感器/屏幕等，只负责协议逻辑。
- **用户应用 (Application)**: 仅 `#include "driver.h"`，完全脱离硬件细节。

## 3. 依赖解析 (Resolver)
HPM 的 Resolver 采用“两阶段解析”：
1. **阶段一**：递归展开依赖，生成 `ResolvedNode` 树，并自动创建复用中间虚拟设备（如 `__spi_xxx`）。
2. **阶段二**：将树摊平为 `DriverState`，交由 `CapabilityChecker` 检查所有引脚能力是否满足、是否存在冲突。

## 4. 信任链与分发 (Registry)
基于去中心化的联邦制设计。官方根索引提供开发者域路由，并验证开发者公钥。开发者可在自己的私域中发布驱动包并进行签名。HPM 客户端会在下载时进行 `official.pub -> index.json -> dev.pub -> package.tar.gz` 的链式签名验证。

## 5. 代码生成 (Generator)
将宏注入、配置打平、代码拷贝结合。生成的 `hpm_dev.h` 提供设备宏，`config.h` 提供配置宏，底层依赖 `HPM_CALL` 进行宏拼接。

## 详细说明：

### 设备名 vs 硬件名

- **硬件名**：`P1.1`、`ch552g`、`ch552_usb_cdc` —— 物理事实
- **设备名**：`serial` —— 用户起的应用语义名

清单文件里写的设备名，决定生成的 API：`serial_write()`、`serial_read()`。

### 引脚是唯一资源

芯片驱动提供引脚，每个引脚带「能力」（能做 `gpio_out` / `spi_sck` / `uart_tx`…）。所有上层驱动都建立在引脚资源之上。

每个引脚有 `shareable` 属性：总线信号（SCK/MISO/MOSI）可共享，CS/RST 独占。

### 驱动分层

```
芯片驱动（提供引脚 + 芯片初始化）      ← 唯一碰寄存器的层
    ↓
总线驱动（spi / i2c / uart）         ← 申请引脚，提供总线接口
    ↓
设备驱动（rc522 / bmp280 / usb_cdc） ← 申请总线 + 引脚，提供应用接口
    ↓
用户应用（main.c）                    ← 只 include "driver.h"
```

每一层只依赖下一层的抽象接口。

### 能力检查

每一层声明 `requires` 和 `provides`，工具在生成期验证：

- 引脚能力是否满足（PA5 能不能做 SPI1_SCK）
- 方法是否存在（spi 驱动提不提供 transfer）
- 名字是否冲突
- 引脚是否被占用

### 中间虚拟设备

`rc522` 要 `spi`，用户只写了 `sck`/`miso`/`mosi` 引脚。工具自动创建中间设备 `__spi_a3f2`，把这三个引脚转移给它，`door_reader.bus` 指向它。

- 默认对用户不可见
- 随机 ID 写入 `driver.json`，之后复用
- 用户可改名
- `hpm dev --export` 后可暴露给底层调试

### 宏机制

每个实例一个目录，一份 `hpm_dev.h`：

```c
// driver/door_reader/hpm_dev.h
#define DEVICE_NAME  door_reader
#define bus          __spi_a3f2
#define cs           hpm_pin_PA4
```

驱动源码用拼接宏：

```c
#define HPM_CAT_(a, b) a##b
#define HPM_CAT(a, b)  HPM_CAT_(a, b)
#define HPM_FN(name)   HPM_CAT(DEVICE_NAME, HPM_CAT(_, name))
#define HPM_CALL(prefix, method)  HPM_CAT(prefix, HPM_CAT(_, method))

int HPM_FN(init)(void) {              // → door_reader_init
    HPM_CALL(cs, init)();             // → hpm_pin_PA4_init
    HPM_CALL(cs, write)(1);           // → hpm_pin_PA4_write(1)
    HPM_CALL(bus, init)();            // → __spi_a3f2_init
    return 0;
}
```

同一份 `rc522.c`，编译两次（`-Idoor_reader` 和 `-Igate_reader`），生成两套符号。驱动源码不改。

### 芯片初始化

芯片驱动也可以声明 `provides.methods`（如 `init`）。`hpm_init_all()` 自动调芯片初始化：

```c
// driver/driver.c（生成）
void hpm_init_all(void) {
    ch552g_init();       // 芯片初始化（时钟等）
    serial_init();       // 设备初始化（USB CDC）
}
```

用户 `main.c` 只需 `#include "driver.h"`。

---

## 工作原理

### `hpm sync`

```
1. 读 project.json → Project
2. 读已有 driver.json（可空）
3. 从本地包目录加载芯片驱动 → Chip
4. 从本地包目录加载所有驱动 → DriverLibrary
5. Resolver.resolve() → DriverState       （依赖解析，创建中间设备）
6. CapabilityChecker.check(state)         （能力检查、冲突检测）
7. 写 driver.json
8. Generator.generate() → driver/         （代码生成）
```

### 依赖解析（两阶段）

**阶段 1**：递归展开成 `ResolvedNode` 树
- 从用户设备的 bindings 里按 key 取值
- 遇到 `req.type` 是驱动名 → 创建子节点（中间设备）
- 检测循环依赖、同名 key 冲突

**阶段 2**：把树摊平成 `DriverState`
- 根节点 → `user_devices`
- 子节点 → `intermediate_devices`
- 中间设备复用：按 `(driver, pins)` 查已有状态

### 代码生成

对每个实例（用户设备 + 中间设备）：
1. 创建 `driver/<instance>/` 目录
2. 生成 `hpm_dev.h`（`DEVICE_NAME` + 资源宏）
3. 拷贝驱动源码
4. 拷贝芯片驱动（全局一份）

生成 `driver.h`（芯片 + 设备 API）、`driver.c`（`hpm_init_all`）、`Makefile`。

---

## 调试（mcus 模拟器）

`hpm debug` 把已编译固件交给 `mcus` 模拟器执行：

```
hpm build → build/firmware.hex|ihx|bin
    ↓
hpm debug --model <chip.json> --config <mcus.toml>
    ↓ 检查 driver/、build/、固件
    ↓ 按模型 arch 选 ISA 动态库（8051 → libmcus_isa_8051.so）
    ↓ fork/exec
mcus --isa <isa.so> --model ... --firmware ... --config ...
```

- mcus 源码通过 `debug/mcus` 软链接引入，由 hpm 的 CMake 一起编译：
  `build/bin/mcus`、`build/lib/libmcus_core.so`、`build/lib/libmcus_isa_8051.so`。
- ISA 是外部动态库插件，必须导出 `mcus_isa_abi_version` / `mcus_isa_arch` /
  `mcus_isa_create` / `mcus_isa_destroy`（见 mcus 的 `include/mcus/isa_plugin.h`）。
- 固件支持裸二进制 `.bin` 和 Intel HEX `.hex`/`.ihx`。
- mcus 使用统一 `DebugEventStream`（MMIO / PIN / BUS / IRQ），支持 attach 模式；
  GPIO 只在变化时发事件，不轮询。
- `hpm debug` 在宿主机编译驱动包里的 `debug/debug.c` 为 `.so`，按用户设备加载；
  中间设备不虚拟化，只用于展开绑定。
- 默认启动即暂停；CLI 交互或 `--json` JSON Lines 协议控制 mcu 和设备插件。