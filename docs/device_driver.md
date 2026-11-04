# HPM 设备驱动开发指南（给 AI 的技能文档）

## 一、你要做什么

你要为一个硬件设备（如传感器、显示屏、通信模块、存储芯片）写一个 HPM 设备驱动包。驱动包是一个目录，包含：

- `manifest.json`：声明驱动的依赖、提供的方法、用户可配的字段
- `src/<name>.c`：驱动源码，用 HPM 的宏写
- `include/<name>.h`：驱动内部头文件
- `LICENSE`：许可证
- `README.md`：说明（可选）

**驱动不直接操作硬件**，而是通过芯片驱动提供的引脚、总线接口操作。

---

## 二、目录结构

```
<name>-<version>/
├── manifest.json
├── LICENSE
├── README.md
├── src/
│   └── <name>.c
└── include/
    └── <name>.h
```

- `<name>`：驱动名，小写，跟目录名一致（如 `rc522`、`bmp280`、`ssd1306`）
- `<version>`：semver（如 `1.0.0`、`0.2.1`）

---

## 三、`manifest.json` 格式

```json
{
  "name": "rc522",
  "version": "1.0.0",
  "description": "RC522 RFID reader",
  "license": "MIT",
  "toolchain": [],
  "config": {
    "key": {
      "type": "string",
      "length": 16,
      "default": "HPM"
    }
  },
  "requires": {
    "bus": { "type": "spi", "methods": ["init", "transfer"] },
    "cs":  { "type": "gpio_out", "methods": ["init", "write"] },
    "rst": { "type": "gpio_out", "methods": ["init", "write"], "optional": true }
  },
  "provides": {
    "methods": [
      { "name": "init", "return": "int", "params": [] },
      {
        "name": "read_card",
        "return": "int",
        "params": [
          { "type": "unsigned char*", "name": "uid" },
          { "type": "unsigned char", "name": "len" }
        ]
      }
    ]
  }
}
```

### 字段说明

| 字段 | 必填 | 说明 |
|------|------|------|
| `name` | 是 | 驱动名，跟目录名一致 |
| `version` | 是 | semver |
| `description` | 否 | 说明 |
| `license` | 是 | SPDX 标识（`MIT`、`Apache-2.0`、`BSD-3-Clause`） |
| `toolchain` | 否 | 支持的编译器（`["sdcc"]`、`["gcc"]`、空数组表示任意） |
| `config` | 否 | 用户可配字段 |
| `requires` | 否 | 资源需求 |
| `provides` | 是 | 提供的方法 |

### `config` schema

```json
"config": {
  "usb_vid": { "type": "int", "default": "0x1209" },
  "usb_manufacturer": { "type": "string", "length": 16, "default": "HPM" }
}
```

| 字段 | 说明 |
|------|------|
| `type` | `int` 或 `string` |
| `length` | 字符串长度（`type: "string"` 时） |
| `default` | 默认值 |

**用户不配时用 `default`。字符串会被工具补空格/截断到 `length`。**

### `requires`

每个 key 对应 `hpm_dev.h` 里的宏名。

```json
"requires": {
  "bus": { "type": "spi", "methods": ["transfer"] },
  "cs":  { "type": "gpio_out", "methods": ["write"] },
  "int": { "type": "gpio_in", "methods": ["read"], "optional": true }
}
```

| 字段 | 说明 |
|------|------|
| `type` | 驱动名（如 `spi`）或引脚能力（如 `gpio_out`） |
| `methods` | 需要下层驱动提供的方法 |
| `optional` | 可选资源 |

**`type` 是驱动名**：工具递归创建中间设备（如 spi 总线）。
**`type` 是引脚能力**：从用户 `project.json` 的 bindings 取引脚。

**引脚能力类型**：
- `gpio_out` / `gpio_in`
- `spi_sck` / `spi_miso` / `spi_mosi`
- `i2c_scl` / `i2c_sda`
- `uart_tx` / `uart_rx`

### `provides`

```json
"provides": {
  "methods": [
    { "name": "init", "return": "int", "params": [] },
    {
      "name": "read",
      "return": "int",
      "params": [
        { "type": "unsigned char*", "name": "buf" },
        { "type": "unsigned char", "name": "len" }
      ]
    }
  ]
}
```

| 字段 | 说明 |
|------|------|
| `name` | 方法名，生成 `<device>_<name>` |
| `return` | 返回类型，默认 `int` |
| `params` | 参数列表，每项 `{ type, name }` |

**参数类型用 C 类型**，如 `unsigned char*`、`int`、`void`。

---

## 四、源码写法

### 4.1 `include/<name>.h`

```c
#pragma once
#include "hpm_dev.h"

// 驱动内部声明（如果需要在多个 .c 文件间共享）
```

**`hpm_dev.h` 由工具生成**，包含 `DEVICE_NAME` 和资源宏。

### 4.2 `src/<name>.c`

```c
#include "<name>.h"
#include <stddef.h>

// ── 拼接宏（每个驱动都要写）──
#define HPM_CAT_(a, b) a##b
#define HPM_CAT(a, b)  HPM_CAT_(a, b)
#define HPM_FN(name)   HPM_CAT(DEVICE_NAME, HPM_CAT(_, name))
#define HPM_CALL(prefix, method)  HPM_CAT(prefix, HPM_CAT(_, method))

// ── 实现 provides 里的方法 ──
int HPM_FN(init)(void) {
    // 初始化硬件
    HPM_CALL(cs, init)();
    HPM_CALL(cs, write)(1);
    HPM_CALL(bus, init)();
    return 0;
}

int HPM_FN(read)(unsigned char* buf, unsigned char len) {
    unsigned char tx[4] = {0x01, 0x02, 0x03, 0x04};
    unsigned char rx[4] = {0};
    HPM_CALL(bus, transfer)(tx, rx, sizeof(tx));
    for (unsigned char i = 0; i < len && i < 4; i++) {
        buf[i] = rx[i];
    }
    return 0;
}
```

### 4.3 关键规则

**1. 函数名用 `HPM_FN(name)` 拼接**

`HPM_FN(read)` 展开成 `<DEVICE_NAME>_read`，如 `door_reader_read`。

**2. 调用下层用 `HPM_CALL(prefix, method)`**

`HPM_CALL(cs, write)(1)` 展开成 `hpm_pin_PA4_write(1)`（如果 `cs` 宏是 `hpm_pin_PA4`）。

**3. 不要用 `printf`、`malloc`**

驱动是硬件抽象层，不该有 IO 副作用。裸机没有 libc。

**4. 不要硬编码引脚名**

用宏（`cs`、`bus`），让工具注入。

**5. 资源 key 不要和 `DEVICE_NAME` 相同**

例如设备名是 `led`，资源 key 也写 `led`：

```json
"requires": { "led": { "type": "gpio_out" } }
```

生成 `hpm_dev.h` 后：

```c
#define DEVICE_NAME  led
#define led          hpm_pin_P1_1
```

`HPM_FN(init)` 会先展开 `DEVICE_NAME`，再被 `led` 宏二次展开成
`hpm_pin_P1_1_init`，函数名就错了。

**资源 key 用 `pin`、`cs`、`rst` 这类名字，不要和设备名重复。**

**6. C 预处理器只匹配独立 token**

`cs` 宏不会匹配 `cs_init` 里的 `cs`。必须用 `HPM_CALL(cs, init)`。

**6. 共享数据用 `extern`**

头文件 `extern` 声明，`.c` 里定义，避免多个 `.c` include 导致重复符号。

---

## 五、`config` 的使用

用户 `project.json`：

```json
{
  "mcu": "ch552g",
  "devices": {
    "serial": {
      "driver": "ch552_usb_cdc",
      "config": {
        "usb_vid": "0x1209",
        "usb_pid": "0x0001",
        "usb_manufacturer": "XZC"
      }
    }
  }
}
```

工具生成 `driver/serial/config.h`：

```c
#pragma once

#define usb_vid  0x1209
#define usb_pid  0x0001
#define usb_manufacturer  "XZC             "
```

**驱动源码里 `#include "config.h"`（先 include `<name>.h`，再 include `config.h`）**：

```c
#include "<name>.h"
#include "config.h"   // 用户配置
```

**如果 `config` 里有字符串，工具会生成 `__code uint16_t` 数组（如果 `manifest.json` 里 `type: "string"` + `length` 非 0）**：

```c
// config.h
#define HPM_USB_STRINGS_DEFINED 1
#ifdef HPM_GEN_STRINGS
__code uint16_t usb_manufacturer_descr[] = {
    (0x03 << 8) | (2 + 16 * 2),
    'X', 'Z', 'C', ...
};
#endif
```

**驱动源码里 `#define HPM_GEN_STRINGS 1` + `#include "config.h"`，用 `usb_manufacturer_descr`。**

---

## 六、完整示例：RC522 驱动

### `manifest.json`

```json
{
  "name": "rc522",
  "version": "1.2.0",
  "description": "RC522 RFID reader",
  "license": "MIT",
  "requires": {
    "bus": { "type": "spi", "methods": ["init", "transfer"] },
    "cs":  { "type": "gpio_out", "methods": ["init", "write"] },
    "rst": { "type": "gpio_out", "methods": ["init", "write"], "optional": true }
  },
  "provides": {
    "methods": [
      { "name": "init", "return": "int", "params": [] },
      {
        "name": "read_card",
        "return": "int",
        "params": [
          { "type": "unsigned char*", "name": "uid" },
          { "type": "unsigned char", "name": "len" }
        ]
      }
    ]
  }
}
```

### `include/rc522.h`

```c
#pragma once
#include "hpm_dev.h"

// RC522 寄存器地址
#define RC522_REG_COMMAND    0x01
#define RC522_REG_VERSION    0x37
```

### `src/rc522.c`

```c
#include "rc522.h"
#include <stddef.h>

#define HPM_CAT_(a, b) a##b
#define HPM_CAT(a, b)  HPM_CAT_(a, b)
#define HPM_FN(name)   HPM_CAT(DEVICE_NAME, HPM_CAT(_, name))
#define HPM_CALL(prefix, method)  HPM_CAT(prefix, HPM_CAT(_, method))

// 读寄存器
static unsigned char read_reg(unsigned char reg) {
    unsigned char tx[2] = { (reg << 1) | 0x80, 0x00 };
    unsigned char rx[2] = {0};
    HPM_CALL(bus, transfer)(tx, rx, 2);
    return rx[1];
}

// 写寄存器
static void write_reg(unsigned char reg, unsigned char val) {
    unsigned char tx[2] = { (reg << 1) & 0x7E, val };
    unsigned char rx[2] = {0};
    HPM_CALL(bus, transfer)(tx, rx, 2);
}

int HPM_FN(init)(void) {
    HPM_CALL(cs, init)();
    HPM_CALL(cs, write)(1);
#ifdef rst
    HPM_CALL(rst, init)();
    HPM_CALL(rst, write)(0);
    HPM_CALL(rst, write)(1);
#endif
    HPM_CALL(bus, init)();

    // 软复位
    write_reg(RC522_REG_COMMAND, 0x0F);
    return 0;
}

int HPM_FN(read_card)(unsigned char* uid, unsigned char len) {
    unsigned char version = read_reg(RC522_REG_VERSION);
    if (version == 0x00 || version == 0xFF) {
        return -1;   // 硬件未连接
    }

    // 假 UID（真实场景需要防冲突）
    for (unsigned char i = 0; i < len && i < 4; i++) {
        uid[i] = i + 1;
    }
    return 0;
}
```

---

## 七、开发流程

### 1. 初始化驱动包

```bash
hpm-dev creat rc522 --type driver
```

生成：

```
rc522-0.1.0/
├── manifest.json
├── LICENSE
├── README.md
├── src/rc522.c
└── include/rc522.h
```

### 2. 编辑 `manifest.json`

填 `requires` 和 `provides`。

### 3. 实现 `src/rc522.c`

用 `HPM_FN` 和 `HPM_CALL` 写。

### 4. 校验

```bash
hpm-dev validate rc522-0.1.0
```

### 5. 复制到 `~/.hpm/packages/`

```bash
cp -r rc522-0.1.0 ~/.hpm/packages/
```

### 6. 测试

`project.json`：

```json
{
  "mcu": "stm32f103c8t6",
  "devices": {
    "door_reader": {
      "driver": "rc522",
      "cs": "PA4",
      "rst": "PA3",
      "sck": "PA5",
      "miso": "PA6",
      "mosi": "PA7"
    }
  }
}
```

```bash
hpm sync --offline
hpm build
```

### 7. 发布

首先，确保用户已经注册HPM开发者账号并绑定了私域仓库
如未注册，访问hpm.astercore.dpdns.org
然后
```bash
hpm-dev login
```

如果未创建开发仓库：

```bash
hpm-dev init
```

发布:

```bash
hpm-dev publish rc522-0.1.0 --key alice.key
```

---

## 八、常见错误

| 错误 | 原因 | 解决 |
|------|------|------|
| `undefined reference to cs_init` | `cs` 宏没展开 | 用 `HPM_CALL(cs, init)`，不是 `cs_init` |
| `Initializer element is not a constant expression` | `__code` 数组用了运行时值 | 用字面量，或者 `config.h` 生成数组 |
| `Could not get N consecutive bytes in internal RAM` | 局部变量太大 | 加 `__xdata`（SDCC） |
| `too many parameters` | 函数声明和定义参数不一致 | 检查 `manifest.json` 的 `provides.methods[].params` |
| `config.h: No such file` | `config.h` 没生成 | `manifest.json` 里 `config` 为空时工具也生成 `config.h` |
| `usb_manufacturer redefined` | `config.h` 和驱动默认值冲突 | 驱动里 `#ifndef` 保护 |

---

## 九、检查清单

写驱动时逐条检查：

- [ ] `manifest.json` 有 `name`、`version`、`license`
- [ ] `requires` 的 key 跟 `hpm_dev.h` 的宏名一致
- [ ] `requires` 的 `type` 是驱动名（如 `spi`）或引脚能力（如 `gpio_out`）
- [ ] `provides.methods` 的 `name` 小写，`return` 和 `params` 用 C 类型
- [ ] `src/<name>.c` 有 `HPM_CAT_`、`HPM_CAT`、`HPM_FN`、`HPM_CALL` 宏
- [ ] 所有方法用 `HPM_FN(name)` 定义
- [ ] 所有下层调用用 `HPM_CALL(prefix, method)`
- [ ] 没有 `printf`、`malloc`
- [ ] 没有硬编码引脚名
- [ ] 共享数据用 `extern`（头文件）+ 定义（`.c`）
- [ ] `config` 字段有 `type`、`length`、`default`
- [ ] 有 `LICENSE` 文件
- [ ] `hpm-dev validate` 通过

---

## 十、给 AI 的提示

当你收到「写一个 XXX 设备的 HPM 驱动」时：

1. **查设备的通信协议**：SPI？I2C？UART？GPIO？
2. **确定 `requires`**：
   - 通信协议 → `"bus": { "type": "spi", "methods": ["transfer"] }`
   - 片选 → `"cs": { "type": "gpio_out", "methods": ["init", "write"] }`
   - 复位 → `"rst": { "type": "gpio_out", "methods": ["init", "write"], "optional": true }`
   - 中断 → `"int": { "type": "gpio_in", "methods": ["read"], "optional": true }`
3. **确定 `provides`**：
   - `creat`：初始化
   - `read` / `write` / `read_xxx` / `write_xxx`：具体功能
   - 参数用 C 类型，指针用 `unsigned char*`
4. **写 `manifest.json`**
5. **写 `src/<name>.c`**：用 `HPM_FN` 和 `HPM_CALL`
6. **写 `include/<name>.h`**：寄存器地址等
7. **加 `LICENSE`**（MIT）
8. **`hpm-dev validate`**

**不要问用户「用什么引脚」，引脚由用户 `project.json` 配。**

**不要写 `main.c`，只写驱动包。**

**不要用 `#include "CH552.H"` 等芯片特定头文件，用 `hpm_dev.h` 的宏。**

---

## 十一、参考

- HPM 主仓库：`https://github.com/AstercoreTeam/hpm`
- 驱动包格式规范：`docs/driver_package.md`
- 示例驱动：`~/.hpm/packages/ch552_usb_cdc-1.0.0/`（USB CDC）、`rc522-1.2.0`（SPI 设备）

**写完驱动后，用 `hpm-dev validate` 检查，然后用 `hpm sync --offline` + `hpm build` 测试。**