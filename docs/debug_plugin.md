# HPM 设备虚拟化插件开发规范

> 适用版本：`hpm-debug/2`、插件 ABI `1`

驱动包可以在根目录下提供 `debug/`，让设备在 `hpm debug` 中被虚拟化。
`hpm sync` 生成 `driver/` 时**不会**复制 `debug/`，它只用于调试会话。

---

## 1. 目录结构

```text
<driver>-<chip>-<version>/
├── manifest.json
├── src/
├── include/
├── ld/                    # 可选
└── debug/                 # 可选；存在即表示该设备支持虚拟化
    ├── debug.c            # 入口，必须有
    ├── xxx.c
    └── xxx.h
```

- 入口固定为 `debug/debug.c`。
- `debug/` 下所有 `.c` 会被一起编译成一个 `.so`。
- `debug/` 不参与固件编译，不进入生成的 `driver/`。

---

## 2. 编译方式

`hpm debug` 用**宿主机编译器**（默认 `cc`）编译：

```bash
cc -std=c11 -O2 -fPIC -shared -Wall \
   -I<package>/include \
   -I<package>/debug \
   -I<build>/include \
   <package>/debug/*.c \
   -o <project>/build/hpm-debug/plugins/<driver>.so \
   -lm
```

- 插件只能依赖 `hpm/debug/plugin.h`。
- 不链接 `libhpm_core`、`libmcus_core`。
- 一个驱动编译一个 `.so`；同一驱动被多个设备使用时只加载一次，通过 `create` 创建多个实例。
- 插件状态必须放在 `ctx` 中，不能使用全局可变状态。

---

## 3. 插件导出符号

```c
uint32_t hpm_debug_abi_version(void);

void* hpm_debug_create(const HpmDebugHostApi* api,
                       const HpmDebugDeviceInfo* info);

void hpm_debug_destroy(void* ctx);

int hpm_debug_event(void* ctx, const HpmDebugEvent* ev);
```

- `hpm_debug_abi_version` 必须返回 `HPM_DEBUG_ABI_VERSION`。
- `create` 返回 `NULL` 表示初始化失败。
- `event` 返回 0 正常；负数表示致命错误，宿主会禁用该设备。
- 所有回调都在同一线程串行调用，插件不要自己开线程或阻塞。

---

## 4. 设备信息

```c
typedef struct {
    const char* device;        /* 用户设备名，如 "led" */
    const char* driver;        /* 驱动名 */
    const HpmDebugKv* config;  /* 驱动 config 打平后的键值 */
    size_t config_count;
    const HpmDebugKv* bindings; /* 资源 key -> 引脚/实例，已展开 */
    size_t binding_count;
} HpmDebugDeviceInfo;
```

示例：`project.json` 中的设备：

```toml
[devices.led]
driver = "gpio-led"
led = "P1_1"
```

插件收到的 `bindings`：

```text
led -> P1.1
```

中间设备会被展开，例如：

```text
bus      -> __spi_a3f2
bus.sck  -> P1.5
bus.mosi -> P1.6
bus.miso -> P1.7
```

中间设备本身不会被虚拟化，也不会加载插件。

---

## 5. 事件

### 5.1 PIN

```c
typedef struct {
    uint64_t time;
    uint32_t pin_id;
    const char* pin;      /* 归一化后的引脚名，如 "P1.1" */
    uint32_t value;
    uint32_t function;
    uint32_t direction;
    uint32_t changed;     /* 1=value, 2=function, 4=direction */
} HpmDebugPinEvent;
```

- **只在引脚状态变化时产生**，不需要轮询。
- 会话开始时会对每个绑定引脚发一条初始快照事件（`changed=7`）。
- 外部程序 `pin.set` 修改引脚时，也会收到 PIN 事件。

### 5.2 MMIO

```c
typedef struct {
    uint64_t time;
    const char* space;
    uint32_t addr;
    uint32_t value;
    uint8_t width;
    uint8_t access;   /* 0=read, 1=write */
} HpmDebugMmioEvent;
```

### 5.3 BUS

总线事务通过统一事件流传递：

```c
typedef struct {
    uint64_t time;
    uint32_t bus_id;
    uint32_t xfer_id;
    uint8_t kind;         /* SPI_XFER / I2C_START / ... */
    uint8_t direction;    /* 0=to host, 1=to mcu */
    uint8_t flags;
    uint16_t len;
    const uint8_t* data;
} HpmDebugBusEvent;
```

> 总线引擎当前仍在接入中；ABI 已预留，后续阶段启用。

### 5.4 JSON

外部程序输入：

```json
{"device":"led","data":{"power":"on"}}
```

插件收到 `HPM_DEBUG_EVENT_JSON`，`ev->u.json` 是 `data` 对象的 JSON 字符串。

---

## 6. Host API

```c
typedef struct HpmDebugHostApi {
    uint32_t abi_version;
    void* host;

    void (*log)(void* host, int level, const char* message);
    void (*emit_json)(void* host, const char* json);

    int (*get_pin)(void* host, const char* pin,
                   uint32_t* value, uint32_t* function,
                   uint32_t* direction);

    int (*set_pin)(void* host, const char* pin, uint32_t value);

    int (*inject_mmio)(void* host, const char* space,
                       uint32_t addr, uint32_t value, uint8_t width);

    int (*raise_irq)(void* host, uint32_t irq);

    int (*bus_write)(void* host, uint32_t xfer_id,
                     const uint8_t* data, size_t len, uint32_t flags);
} HpmDebugHostApi;
```

### `log`

- CLI：打印 `[device] message`
- JSON 模式：发送 `{"device":...,"type":"log",...}`

### `emit_json`

必须传 JSON object 字符串。宿主会包装成：

```json
{"device":"led","type":"json","time":123,"data":{"brightness":80}}
```

插件不能覆盖 `device`/`time`/`type`。

### `get_pin`

只能读取该设备绑定过的引脚。按需读取当前快照，不是轮询。

### `set_pin`

驱动芯片输入引脚。适用于按键、MISO 等。

### `inject_mmio`

向芯片注入一个 MMIO 写，适用于寄存器级外设。

### `raise_irq`

触发中断。

### `bus_write`

回应总线事务，`xfer_id` 与收到的 BUS 事件对应。

---

## 6.5 设备 UI / 状态 / 错误

插件通过 `emit_json` 发送保留键：

### `$ui`

设备元数据，用于 `device.list` / `device.describe`：

```json
{"$ui":{
  "schema":"hpm-ui/1",
  "role":"output",
  "title":"LED",
  "channels":[
    {"id":"power","name":"电源","direction":"out",
     "type":"bool","widget":"lamp","readonly":true}
  ]
}}
```

- 可以在 `create` 中直接发；host 会缓存，不会作为普通 JSON 转发。
- host 支持 `describe` 指令：插件收到 `{"cmd":"describe"}` 后可以重新发 `$ui`。
- `role`：`input` / `output` / `bidirectional` / `unknown`。
- 通道字段见协议文档 `device.describe` 一节。

### `$state`

设备状态增量：

```json
{"$state":{"power":true,"brightness":80}}
```

- host 合并到该设备状态缓存。
- 转向外部程序为 `type:"state"` 事件。
- `null` 表示未知/失效。

### `$error`

```json
{"$error":{"code":"i2c_timeout","message":"I2C 无响应",
           "channel":"brightness","fatal":false}}
```

- `fatal=true` 会标记设备 `online=false`。
- 非致命错误只记录最后错误。

### 外部输入

外部程序的 `data` 会以 `HPM_DEBUG_EVENT_JSON` 传入，例如：

```json
{"device":"led","data":{"power":"on"}}
```

插件事件里 `ev->u.json == "{\"power\":\"on\"}"`。

---

## 7. 行为规则

- 不轮询引脚；用 PIN 事件。
- 需要当前值时调用 `get_pin`。
- 不直接访问 mcus 共享内存。
- 不阻塞事件回调。
- 不假设只有一个实例；所有状态放 `ctx`。
- 未知 JSON 输入直接忽略并返回 0，不要崩溃。
- 日志不要每毫秒刷屏；总线/引脚事件可能很密集。

---

## 8. 最小示例

```c
#include <hpm/debug/plugin.h>
#include <stdio.h>
#include <string.h>

static const HpmDebugHostApi* g_api;
static char g_name[64];

uint32_t hpm_debug_abi_version(void) {
    return HPM_DEBUG_ABI_VERSION;
}

void* hpm_debug_create(const HpmDebugHostApi* api,
                       const HpmDebugDeviceInfo* info) {
    g_api = api;
    strncpy(g_name, info->device, sizeof(g_name) - 1);
    return g_name;
}

void hpm_debug_destroy(void* ctx) { (void)ctx; }

int hpm_debug_event(void* ctx, const HpmDebugEvent* ev) {
    (void)ctx;
    if (ev->type == HPM_DEBUG_EVENT_PIN) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s=%u", ev->u.pin.pin, ev->u.pin.value);
        g_api->log(g_api->host, HPM_DEBUG_LOG_INFO, msg);
    }
    return 0;
}
```

---

## 9. 调试

```bash
hpm debug --model <model.json> --config <config.toml>
hpm debug --json --model ... --config ...
```

- 缺少 `debug/debug.c` 默认直接报错。
- `--allow-missing-debug` 可跳过。
- `--host-cc` 指定宿主机编译器。
