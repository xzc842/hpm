# HPM 单片机开发指南（AI 专用）

本文档教 AI 用 HPM 开发单片机程序。**不涉及驱动开发**，只讲怎么用已有的驱动写应用。

---

## 0. 前置知识

HPM 是声明式嵌入式开发工具链。用户只写：

1. **`project.json`**：声明芯片 + 设备 + 引脚
2. **`main.c`**：写应用逻辑，`#include "driver.h"`

工具自动拉驱动、解析依赖、生成代码、编译、烧录。

**AI 不需要知道驱动内部怎么实现**，只需要知道：

- `driver.h` 里有什么函数
- 函数签名是什么
- 怎么在 `project.json` 里声明设备

---

## 1. 工作流程

```
1. 读用户需求（用什么芯片、接什么设备、要什么功能）
2. 写 project.json
3. hpm sync              → 生成 driver/
4. 读 driver/driver.h    → 看有哪些 API
5. 写 main.c             → 调 API
6. hpm build             → 编译
7. hpm flash             → 烧录
```

**关键**：`hpm sync` 之后才能看 `driver.h`，因为驱动依赖决定 API。

---

## 2. `project.json` 格式

```json
{
  "mcu": "<芯片型号>",
  "devices": {
    "<设备名>": {
      "driver": "<驱动名>",
      "<资源key>": "<引脚名>",
      "config": {
        "<配置key>": "<值>"
      }
    }
  }
}
```

### 字段说明

| 字段 | 说明 |
|------|------|
| `mcu` | 芯片型号，如 `ch552g`、`stm32f103c8t6` |
| `devices` | 设备列表，key 是**用户起的设备名** |
| `driver` | 驱动包名，如 `ch552_usb_cdc`、`rc522` |
| `<资源key>` | 驱动 `requires` 的 key，如 `cs`、`sck`、`bus` |
| `config` | 用户配置，由驱动声明 |

### 设备名 vs 驱动名

- **设备名**：用户起的，如 `serial`、`led`、`door_reader`
- **驱动名**：驱动包名，如 `ch552_usb_cdc`、`rc522`

**生成的 API 前缀 = 设备名**。设备名 `serial` → `serial_init()`、`serial_write()`。

### 引脚写法

引脚名由芯片决定：

- **CH552**：`P1_1`、`P3_0`（下划线，不是点）
- **STM32**：`PA4`、`PB6`（大写）

### 示例

**CH552 USB CDC**：

```json
{
  "mcu": "ch552g",
  "devices": {
    "serial": {
      "driver": "ch552_usb_cdc",
      "config": {
        "usb_vid": "0x1209",
        "usb_pid": "0x0001",
        "usb_manufacturer": "XZC",
        "usb_product": "CH552 CDC",
        "usb_serial": "0001",
        "usb_interface": "CDC"
      }
    }
  }
}
```

**STM32 + RC522**：

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

**注意**：`sck`/`miso`/`mosi` 是 SPI 总线的引脚，工具会自动创建中间设备 `__spi_xxx`，用户不用管。

---

## 3. 看 `driver.h`

`hpm sync` 后，`driver/driver.h` 包含所有可用 API。

### 结构

```c
// 芯片 API
void ch552g_init(void);

// 设备 API
void serial_init(void);
void serial_write(char c);
uint8_t serial_available(void);
char serial_read(void);

// 全局
void hpm_init_all(void);
```

### 规则

- **函数名 = `<设备名>_<方法名>`**
- **方法名由驱动定义**，如 `init`、`write`、`read`、`available`
- **参数和返回值由驱动定义**

### 读 `driver.h` 的方法

```bash
hpm sync
cat driver/driver.h
```

**AI 必须读 `driver.h` 才知道 API 签名。**

---

## 4. `main.c` 写法

### 最小结构

```c
#include "driver.h"

void main(void) {
    hpm_init_all();       // 初始化所有设备

    while (1) {
        // 应用逻辑
    }
}
```

### CH552 特殊

**CH552 用 SDCC，`main` 是 `void main(void)`**，不是 `int main(void)`。

**STM32 用 GCC，`main` 是 `int main(void)`**。

### 等 USB 枚举（CH552 USB CDC）

```c
void main(void) {
    hpm_init_all();

    // 等 USB 枚举
    while (!USB_ENUM_OK) {}
    // 等主机打开串口
    while (!CDC_getDTR()) {}

    serial_write('H');
    serial_write('i');
    CDC_flush();

    while (1) {}
}
```

**`USB_ENUM_OK` 和 `CDC_getDTR` 是 `ch552_usb_cdc` 驱动特有的**，其他驱动不一定有。

### 缓冲区放 `__xdata`（CH552）

CH552 内部 RAM 只有 256 字节，局部数组会撑爆。

```c
// 错误：占内部 RAM
void main(void) {
    char buf[64];   // 64 字节内部 RAM
}

// 正确：放 xRAM
__xdata char g_buf[64];

void main(void) {
    // 用 g_buf
}
```

**`__xdata` 是 SDCC 关键字，只在 CH552（SDCC）用。**

### 完整示例

```c
#include "driver.h"

__xdata char g_buf[64];

static void print(const char* s) {
    while (*s) {
        serial_write(*s++);
    }
    CDC_flush();
}

void main(void) {
    hpm_init_all();

    while (!USB_ENUM_OK) {}
    while (!CDC_getDTR()) {}

    print("Hello from CH552\r\n");

    while (1) {
        if (serial_available()) {
            char c = serial_read();
            serial_write(c);
            CDC_flush();
        }
    }
}
```

---

## 5. 常用模式

### 5.1 发字符串

```c
static void print(const char* s) {
    while (*s) {
        serial_write(*s++);
    }
    CDC_flush();
}
```

**`CDC_flush` 是 CH552 USB CDC 特有的**，把缓冲区数据立刻发出去。其他驱动不一定有。

### 5.2 读一行

```c
__xdata char g_line[64];

static unsigned char read_line(unsigned char maxLen) {
    unsigned char len = 0;
    while (1) {
        if (serial_available()) {
            char c = serial_read();
            if (c == '\r' || c == '\n') {
                serial_write('\r');
                serial_write('\n');
                CDC_flush();
                g_line[len] = '\0';
                return len;
            }
            if (c == 0x7F || c == 0x08) {
                if (len > 0) {
                    len--;
                    print("\b \b");
                }
                continue;
            }
            if (len < maxLen - 1) {
                g_line[len++] = c;
                serial_write(c);
                CDC_flush();
            }
        }
    }
}
```

### 5.3 延时

```c
static void delay_ms(unsigned int ms) {
    while (ms--) {
        for (unsigned int i = 0; i < 200; i++) {}
    }
}
```

**不精确**，但够用。CH552 24MHz 下约 1ms。

### 5.4 字符串比较

```c
static unsigned char str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}
```

**不要用 `strcmp`**，SDCC 没有 libc。

### 5.5 命令行

```c
void main(void) {
    hpm_init_all();
    while (!USB_ENUM_OK) {}
    while (!CDC_getDTR()) {}

    while (1) {
        print("> ");
        read_line(64);

        if (str_equal(g_line, "help")) {
            print("commands: help, info, echo\r\n");
        } else if (str_equal(g_line, "info")) {
            print("CH552 USB CDC\r\n");
        } else if (str_equal(g_line, "echo")) {
            print("you: ");
            print(g_line);
            print("\r\n");
        } else if (g_line[0] == '\0') {
            // 空行
        } else {
            print("unknown: ");
            print(g_line);
            print("\r\n");
        }
        print("\r\n");
    }
}
```

---

## 6. 常见错误

### 6.1 `hpm sync` 失败

**症状**：`驱动包未找到`、`引脚冲突`、`工具链不兼容`。

**检查**：

```bash
cat project.json
ls ~/.hpm/packages/
```

**常见原因**：

- 驱动名拼错
- 引脚名不对（CH552 用 `P1_1`，STM32 用 `PA4`）
- 引脚被占用（两个设备用同一个 `cs`）

### 6.2 `hpm build` 失败

**症状**：`undefined reference`、`too many parameters`、`RAM 不够`。

**检查**：

```bash
cat driver/driver.h
```

**常见原因**：

- 调用了 `driver.h` 里没有的函数
- 函数参数不对
- CH552 局部数组太大，改 `__xdata`

### 6.3 程序跑飞

**症状**：烧录后没反应，或者串口乱码。

**检查**：

- CH552：`while (!USB_ENUM_OK) {}` 等 USB 枚举
- CH552：`while (!CDC_getDTR()) {}` 等主机打开串口
- STM32：启动文件、链接脚本对不对

---

## 7. AI 操作流程

### 步骤 1：读需求

用户说「用 CH552 做一个串口回显程序」。

**AI 提取**：

- 芯片：`ch552g`
- 设备：USB CDC
- 功能：回显

### 步骤 2：写 `project.json`

```json
{
  "mcu": "ch552g",
  "devices": {
    "serial": {
      "driver": "ch552_usb_cdc",
      "config": {
        "usb_vid": "0x1209",
        "usb_pid": "0x0001",
        "usb_manufacturer": "XZC",
        "usb_product": "CH552 CDC",
        "usb_serial": "0001",
        "usb_interface": "CDC"
      }
    }
  }
}
```

**`config` 的 key 由驱动声明**，AI 不知道就留空，驱动有默认值。

### 步骤 3：`hpm sync`

```bash
hpm sync
```

**如果失败，看错误**：

- `驱动包未找到` → 驱动名错，或者没在 `~/.hpm/packages/`
- `引脚冲突` → 改引脚
- `工具链不兼容` → 换芯片或换驱动

### 步骤 4：读 `driver.h`

```bash
cat driver/driver.h
```

**AI 看 `driver.h` 里有什么函数**：

```c
void serial_init(void);
void serial_write(char c);
uint8_t serial_available(void);
char serial_read(void);
void hpm_init_all(void);
```

**知道**：

- `serial_write(char)` 发一个字符
- `serial_available()` 返回可读字节数
- `serial_read()` 读一个字符

**但 `driver.h` 里没有 `CDC_flush`**，因为它在 `serial/usb_cdc.h` 里，`driver.h` 只是应用 API。

**AI 可以读 `driver/serial/*.h` 看更多**：

```bash
cat driver/serial/usb_cdc.h
cat driver/serial/usb_handler.h
```

**`USB_ENUM_OK`、`CDC_getDTR`、`CDC_flush` 在这里。**

### 步骤 5：写 `main.c`

```c
#include "driver.h"

__xdata char g_buf[64];

static void print(const char* s) {
    while (*s) {
        serial_write(*s++);
    }
    CDC_flush();
}

void main(void) {
    hpm_init_all();

    while (!USB_ENUM_OK) {}
    while (!CDC_getDTR()) {}

    print("Echo ready\r\n");

    while (1) {
        if (serial_available()) {
            char c = serial_read();
            serial_write(c);
            CDC_flush();
        }
    }
}
```

### 步骤 6：`hpm build`

```bash
hpm build
```

**如果失败，看错误**：

- `too many parameters` → 函数签名不对，看 `driver.h`
- `RAM 不够` → 局部数组改 `__xdata`
- `undefined reference` → 函数不存在，看 `driver.h`

### 步骤 7：`hpm flash`

```bash
hpm flash
```

**CH552**：需要 Bootloader 模式（按住 BOOT 键插 USB）。

**STM32**：st-flash 自动烧录。

---

## 8. 驱动特有的 API

**不同驱动有不同的额外 API，`driver.h` 里只有通用的。** AI 要读驱动目录下的头文件。

### CH552 USB CDC

| API | 头文件 | 说明 |
|-----|--------|------|
| `USB_ENUM_OK` | `usb_handler.h` | USB 枚举完成标志 |
| `CDC_getDTR()` | `usb_cdc.h` | 主机打开串口 |
| `CDC_flush()` | `usb_cdc.h` | 立刻发缓冲区 |
| `CDC_write(char)` | `usb_cdc.h` | 同 `serial_write` |
| `CDC_read()` | `usb_cdc.h` | 同 `serial_read` |

**`USB_ENUM_OK` 是 `volatile unsigned char`，`while (!USB_ENUM_OK) {}` 等它变 1。**

### RC522

`driver.h` 里：

```c
int door_reader_init(void);
int door_reader_read_card(void);
```

**参数和返回值看 `driver.h`。**

---

## 9. 最小可用模板

### CH552 USB CDC

```json
// project.json
{
  "mcu": "ch552g",
  "devices": {
    "serial": {
      "driver": "ch552_usb_cdc",
      "config": {
        "usb_vid": "0x1209",
        "usb_pid": "0x0001",
        "usb_manufacturer": "XZC",
        "usb_product": "CH552 CDC",
        "usb_serial": "0001",
        "usb_interface": "CDC"
      }
    }
  }
}
```

```c
// main.c
#include "driver.h"

__xdata char g_buf[64];

static void print(const char* s) {
    while (*s) serial_write(*s++);
    CDC_flush();
}

void main(void) {
    hpm_init_all();
    while (!USB_ENUM_OK) {}
    while (!CDC_getDTR()) {}

    print("ready\r\n");

    while (1) {
        if (serial_available()) {
            serial_write(serial_read());
            CDC_flush();
        }
    }
}
```

```bash
hpm sync
hpm build
hpm flash
picocom -b 115200 /dev/ttyACM0
```

### STM32 + RC522

```json
// project.json
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

```c
// main.c
#include "driver.h"

int main(void) {
    hpm_init_all();

    while (1) {
        int card = door_reader_read_card();
        if (card == 0) {
            // 读到卡
        }
    }
    return 0;
}
```

```bash
hpm sync
hpm build
hpm flash
```

---

## 10. 命令参考

| 命令 | 说明 |
|------|------|
| `hpm sync` | 拉驱动、解析依赖、生成 `driver/` |
| `hpm sync --offline` | 离线模式，只用本地包 |
| `hpm build` | 编译 |
| `hpm flash` | 烧录 |
| `hpm clean` | 删 `build/` |
| `hpm dev --export` | 生成 `hpm.h`（底层 API） |

**AI 通常只用 `sync`、`build`、`flash`。**

---

## 11. 环境变量

| 变量 | 说明 |
|------|------|
| `HPM_REGISTRY` | 索引 URL（默认 GitHub） |
| `HPM_OFFICIAL_CERT` | 官方证书 URL |
| `HPM_ROOT_CA` | 根证书路径（测试覆盖用） |
| `HPM_HOME` | 缓存目录（默认 `~/.hpm`） |
| `HPM_SKIP_SIGNATURE` | 跳过签名验证（仅开发） |

**AI 不需要改这些，除非用户要求。**

---

## 12. AI 检查清单

写完 `main.c` 后，AI 自检：

- [ ] `#include "driver.h"` 有没有
- [ ] `hpm_init_all()` 有没有调
- [ ] CH552：`void main(void)`，STM32：`int main(void)`
- [ ] CH552：`while (!USB_ENUM_OK) {}` 有没有
- [ ] CH552：局部数组改 `__xdata`
- [ ] 函数名是否在 `driver.h` 里
- [ ] 参数类型是否对
- [ ] 字符串比较用 `str_equal`，不用 `strcmp`
- [ ] 打印用 `print`，不用 `printf`
- [ ] CH552 USB：每次 `serial_write` 后 `CDC_flush()`

---

## 13. 不要做的事

| 不要 | 原因 |
|------|------|
| 写驱动 | 本文档不教 |
| 用 `printf` | SDCC 没 libc |
| 用 `strcmp` / `strcpy` | SDCC 没 libc |
| 用 `malloc` | 裸机没堆 |
| CH552 用 `int main` | SDCC 的 `main` 是 `void` |
| CH552 局部大数组 | 内部 RAM 只有 256 字节 |
| 硬编码引脚 | 用 `project.json` |
| 改 `driver/` 里的文件 | `hpm sync` 会覆盖 |
| 提交 `driver/` 到 git | 生成产物，不提交 |

---

## 14. 调试

### 串口调试

**CH552**：

```bash
picocom -b 115200 /dev/ttyACM0
```

**STM32**：

```bash
picocom -b 115200 /dev/ttyUSB0
```

### 看 USB 设备

```bash
lsusb | grep 1209
# Bus 001 Device 005: ID 1209:0001 XZC CH552 CDC
```

### 看编译产物

```bash
ls -la build/
file build/firmware.elf
```

### 看生成的代码

```bash
cat driver/driver.h        # 用户 API
cat driver/driver.c        # 胶水
cat driver/serial/config.h # 用户配置
```

---

## 15. 总结

**AI 开发 HPM 程序的步骤**：

1. 读需求 → 提取芯片 + 设备 + 功能
2. 写 `project.json` → 声明设备 + 引脚 + 配置
3. `hpm sync` → 生成 `driver/`
4. 读 `driver/driver.h` → 看 API
5. 读 `driver/<device>/*.h` → 看驱动特有 API
6. 写 `main.c` → 调 API
7. `hpm build` → 编译
8. `hpm flash` → 烧录
9. 串口调试

**核心原则**：

- **不写驱动**，只用已有驱动
- **读 `driver.h` 再写 `main.c`**
- **CH552 注意 `__xdata` 和 `void main`**
- **CH552 USB 注意 `USB_ENUM_OK` 和 `CDC_flush`**
- **不提交 `driver/` 到 git**