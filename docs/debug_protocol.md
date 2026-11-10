# HPM Debug JSON 协议

> 协议版本：`hpm-debug/2`
> 传输：stdin/stdout，一行一个 JSON object（JSON Lines）

`hpm debug --json` 让外部程序以子进程方式控制模拟器、监控引脚/设备、dump 内存。

---

## 1. 启动

通常情况下不需要指定模型、ISA 或配置：

```bash
hpm debug --json
```

自动解析规则：

- `project.json` 的 `mcu` 对应芯片包；
- 模型从芯片包 `debug/<mcu>.json` 读取；
- `debug/isa_<arch>.cpp` 在 `build/hpm-debug/isa/` 现场编译；
- 模拟器配置固定使用项目根目录的 `debug.toml`。

需要时仍可覆盖：

```bash
hpm debug --json \
  --model <model.json> \
  --config <debug.toml> \
  --isa <lib.so>
```

| 参数 | 必填 | 默认 | 说明 |
|---|---|---|---|
| `--model <json>` | 否 | 芯片包 `debug/*.json` | 覆盖芯片模型 |
| `--config <toml>` | 否 | 项目 `debug.toml` | 覆盖模拟器配置 |
| `--isa <so>` | 否 | 现场编译芯片包 ISA | 覆盖 ISA 动态库；也可用 `HPM_ISA` |
| `--mcus <bin>` | 否 | `build/bin/mcus` | 模拟器路径 |
| `--firmware <file>` | 否 | `build/` 下最新 | 固件覆盖 |
| `--json` | 否 | — | JSON Lines |
| `--run` | 否 | 暂停 | 启动即运行 |
| `--allow-missing-debug` | 否 | — | 跳过无 debug 的设备 |
| `--host-cc <cc>` | 否 | `cc` | 设备插件 C 编译器 |
| `--host-cxx <cxx>` | 否 | `c++` | 芯片 ISA C++ 编译器 |
| `--dry-run` | 否 | — | 只打印命令 |

- **stdout**：只有 JSON Lines。
- **stderr**：hpm/mcus 日志、编译错误。
- 默认启动即暂停；`--run` 启动后立即运行。
- 所有 stdout 消息都带单调递增的 `seq`。
- 芯片 ISA 使用 `build/lib/libmcus_isa_sdk.a` 和 `build/include/mcus`；安装后也可使用 `/usr/local/lib`、`/usr/local/include`。

---

## 2. 握手

```json
{"device":"mcu","type":"event","event":"ready","seq":0,
 "protocol":"hpm-debug/2","status":"paused",
 "features":["pin.list","pin.subscribe","pin.unsubscribe","pin.snapshot",
             "device.list","device.describe","device.state",
             "device.snapshot","device.updated","device.error",
             "mem.read","mem.regions",
             "status.event","breakpoint.object","seq"],
 "limits":{"max_breakpoints":32,"max_mem_read":262144,
           "pin_subscribe_min_ms":10,"pin_subscribe_max_ms":1000},
 "model":{"name":"ch552g","arch":"8051","pins":17,
          "spaces":["code","iram","sfr","xram"]},
 "devices":["led"]}
```

GUI 按 `features` 启用功能；旧 host 没有 `features` 时按 v1 降级。

---

## 3. 请求 / 响应

```json
{"id":1,"device":"mcu","cmd":"pause"}
```

```json
{"id":1,"device":"mcu","type":"response","ok":true,"seq":1,
 "result":{"status":"paused","pc":211,"stop_reason":"pause"}}
```

- `id` 可选，响应原样带回。
- `device` 必填。
- `device="mcu"` 控制模拟器；其他值发给设备插件。
- 错误：

```json
{"id":4,"device":"mcu","type":"response","ok":false,"seq":5,
 "error":{"code":"out_of_range","message":"..."}}
```

---

## 4. mcu 命令

### 4.1 状态

```json
{"id":1,"device":"mcu","cmd":"status"}
```

```json
{"id":1,"device":"mcu","type":"response","ok":true,
 "result":{
   "status":"paused","pc":211,"virtual_time":123,
   "halted":false,
   "stop_reason":"breakpoint","stop_addr":211,
   "breakpoints":[
     {"addr":211,"enabled":true,"hit_count":1}
   ]}}
```

`status`：`running` / `paused` / `halted` / `dump_done`。

### 4.2 运行控制

```json
{"device":"mcu","cmd":"pause"}
{"device":"mcu","cmd":"resume"}
{"device":"mcu","cmd":"run"}
{"device":"mcu","cmd":"step","count":1}
{"device":"mcu","cmd":"reset"}
```

- `step` 完成后必为 `paused` 或 `halted`。
- `reset` 保留断点。
- 状态迁移会异步发 `status` 事件：

```json
{"device":"mcu","type":"event","event":"status","seq":6,
 "time":123,"status":"paused","pc":211,"halted":false,
 "stop_reason":"breakpoint","stop_addr":211,
 "breakpoint":{"addr":211,"hit_count":1}}
```

`stop_reason`：`breakpoint` / `step` / `pause` / `reset` / `halt` / `error`。

### 4.3 断点

```json
{"device":"mcu","cmd":"breakpoint.set","addr":"0x00D3"}
{"device":"mcu","cmd":"breakpoint.clear","addr":"0x00D3"}
{"device":"mcu","cmd":"breakpoint.clear_all"}
{"device":"mcu","cmd":"breakpoint.list"}
{"device":"mcu","cmd":"breakpoint.enable","addr":"0x00D3"}
{"device":"mcu","cmd":"breakpoint.disable","addr":"0x00D3"}
```

响应中的 `breakpoints` 是对象数组：

```json
[{"addr":211,"enabled":true,"hit_count":0}]
```

最多 32 个；重复设置幂等；删除不存在的地址不报致命错误。

### 4.4 引脚列表

```json
{"id":20,"device":"mcu","cmd":"pin.list"}
```

```json
{"id":20,"device":"mcu","type":"response","ok":true,
 "result":{"pins":[
   {"name":"P1.0","number":0,"value":1,
    "function":0,"function_name":"gpio",
    "functions":["gpio","t2_cap1","tin0"],
    "direction":0,"direction_name":"input",
    "devices":["led"]}
 ]}}
```

- 返回模型全部引脚，不只是已绑定的。
- `devices` 是绑定到该引脚的用户设备，没有则空数组。
- 引脚名统一为 `P1.0` 形式。

### 4.5 引脚订阅

```json
{"id":21,"device":"mcu","cmd":"pin.subscribe",
 "pins":["*"],"mode":"change","interval_ms":50}
```

- `pins`：引脚名数组，`"*"` 表示全部。
- `mode`：`change`（默认，只发变化）或 `all`（周期全量）。
- `interval_ms`：10..1000，默认 50。
- 幂等；重复订阅相同/不同参数会替换当前订阅。

订阅成功后：

```json
{"device":"mcu","type":"event","event":"pin.snapshot","seq":9,"time":0,
 "pins":[{"name":"P1.0","value":0,"changed":7,...}]}
```

之后按间隔批量推送：

```json
{"device":"mcu","type":"pin","seq":18,"time":4231,
 "pins":[{"name":"P1.0","value":1,"changed":1,...}]}
```

- 同一间隔内多次变化只保留最后值，`changed` 按位或。
- `changed`：1=value，2=function，4=direction。
- 退订：

```json
{"device":"mcu","cmd":"pin.unsubscribe"}
```

### 4.6 引脚读写

```json
{"device":"mcu","cmd":"pin.read","pin":"P1.1"}
{"device":"mcu","cmd":"pin.set","pin":"P1.1","value":0}
```

`pin.set` 会向订阅者发 `pin` 事件；未知引脚返回错误。

### 4.7 设备列表

```json
{"device":"mcu","cmd":"device.list"}
```

```json
{"device":"mcu","type":"response","ok":true,
 "result":{"devices":[
   {"device":"led","driver":"gpio-led-ch552","version":"1.0.0",
    "online":true,"role":"output","title":"LED",
    "channels":[{"id":"power","name":"电源","direction":"out",
                 "type":"bool","widget":"lamp","readonly":true}]}
 ]}}
```

没有提供 `$ui` 的设备也会返回，`role="unknown"`，`channels=[]`。

### 4.8 设备描述

```json
{"id":31,"device":"mcu","cmd":"device.describe","device_name":"led"}
```

或：

```json
{"id":31,"device":"led","cmd":"describe"}
```

响应：

```json
{"id":31,"device":"led","type":"response","ok":true,
 "result":{"schema":"hpm-ui/1","role":"output","title":"LED",
  "channels":[
    {"id":"power","name":"电源","direction":"out",
     "type":"bool","widget":"lamp","readonly":true}
  ],
  "state":{"power":false},
  "online":true}}
```

`channels[]` 字段：

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 设备内唯一 |
| `name` | 是 | 显示名 |
| `direction` | 是 | `in` / `out` / `inout` |
| `type` | 是 | `bool/int/uint/float/enum/string/color/bytes/action/event/table/json/image/chart/custom` |
| `widget` | 否 | 控件建议 |
| `unit` | 否 | 单位 |
| `min/max/step` | 否 | 数值范围 |
| `precision` | 否 | 小数位 |
| `default` | 否 | 默认值 |
| `options` | 否 | enum 选项 |
| `readonly` | 否 | 只读 |
| `group/order` | 否 | 分组/排序 |
| `description` | 否 | 提示 |
| `secret` | 否 | 敏感值 |
| `confirm` | 否 | 二次确认 |
| `columns` | 否 | table 列定义 |

未知类型/控件 GUI 应降级为 JSON 面板。

### 4.9 设备状态

设备状态事件：

```json
{"device":"led","type":"state","seq":13,"time":578,
 "online":true,"values":{"power":true}}
```

全量快照：

```json
{"device":"mcu","cmd":"device.snapshot"}
```

```json
{"device":"mcu","type":"response","ok":true,
 "result":{"devices":[
   {"device":"led","online":true,"last_update":578,
    "values":{"power":true}}
 ]}}
```

单设备：

```json
{"device":"mcu","cmd":"device.state","device_name":"led"}
```

设备错误：

```json
{"device":"led","type":"error","seq":14,"time":579,
 "error":{"code":"i2c_timeout","message":"I2C 无响应",
          "channel":"brightness"}}
```

### 4.10 内存空间

```json
{"device":"mcu","cmd":"mem.regions"}
```

```json
{"result":{"regions":[
 {"name":"code","space":"code","base":0,"size":16384,"mmio":false},
 {"name":"iram","space":"iram","base":0,"size":256,"mmio":false},
 {"name":"sfr","space":"sfr","base":128,"size":128,"mmio":true}
]}}
```

### 4.11 内存读取

```json
{"id":10,"device":"mcu","cmd":"mem.read",
 "space":"code","addr":"0x0000","length":256}
```

```json
{"id":10,"device":"mcu","type":"response","ok":true,
 "result":{"space":"code","addr":0,
           "requested_length":256,"length":256,
           "actual_length":256,"truncated":false,
           "in_range":true,"encoding":"hex",
           "data":"02004c..."}}
```

- 最大 256 KB（`limits.max_mem_read`）。
- 运行中会先暂停，dump 完自动恢复。
- 越界：`ok:true`，`in_range:false`，`length:0`。
- 超过上限：`ok:false`，`error.code="too_large"`。
- 未知空间：`error.code="unknown_space"`。
- 起始地址不在区域内：`in_range:false`。
- 实际长度可能小于请求，`truncated=true`。

### 4.12 寄存器 / 中断

```json
{"device":"mcu","cmd":"reg.read","space":"sfr","addr":"0x90","width":8}
{"device":"mcu","cmd":"irq.inject","number":1}
```

### 4.13 关闭

```json
{"device":"mcu","cmd":"shutdown"}
```

或关闭 stdin。

---

## 5. 设备输入 / 输出

### 5.1 输入

```json
{"id":2,"device":"led","data":{"power":"on"}}
```

- `data` 序列化后交给插件。
- 插件返回 0 时响应 `ok:true`，`result.applied=true`；
- 返回非 0 时 `ok:false`，`error.code="device_error"`。

### 5.2 输出

日志：

```json
{"device":"led","type":"log","seq":2,"level":"info",
 "time":123,"message":"on"}
```

普通 JSON：

```json
{"device":"led","type":"json","seq":3,"time":123,
 "data":{"brightness":80}}
```

引脚事件：

```json
{"device":"led","type":"pin","seq":4,"time":123,
 "pin":"P1.1","value":1,"function":0,"direction":0}
```

设备 UI 更新：

```json
{"device":"led","type":"event","event":"device.updated","seq":5,
 "time":123,"ui":{"schema":"hpm-ui/1","role":"output",...}}
```

---

## 6. seq 与重同步

- 所有 stdout 消息带 `seq`，单调递增。
- GUI 发现跳号时可以：
  - `cmd:"status"`
  - `cmd:"pin.list"` / `pin.subscribe`
  - `cmd:"device.snapshot"`
  重新对齐。
- 不要求补发历史事件。

---

## 7. 约束

- 一行一个 JSON，不能跨行。
- 不要依赖字段顺序。
- stdout 只用于协议；日志写 stderr。
- 未知 device/cmd 返回结构化错误。
- 设备插件事件与响应可能交错，按 `type`/`device` 分发。

---

## 8. USB CDC 设备(例)

USB CDC 设备作为**输入设备**出现，不再增加输出设备：

```json
{"role":"input","channels":[
  {"id":"tx","direction":"in","type":"string","widget":"textarea"},
  {"id":"send","direction":"in","type":"bool","widget":"button"}
]}
```

发送：

```json
{"device":"serial","data":{"cmd":"set","channel":"tx","value":"hello\n"}}
{"device":"serial","data":{"cmd":"set","channel":"send","value":true}}
```

或：

```json
{"device":"serial","data":{"cmd":"invoke","channel":"send"}}
```

如果 `tx` 值包含 `\n` 或 `\r`，插件会自动发送，不需要额外的 `send`。通过 send 动作发送时，如果末尾没有换行，插件会自动补 `\n`，方便 shell 类固件处理。
发送内容会以 `TX: ...` 日志输出；固件返回的串口数据由插件通过 `log` 事件写入宿主日志窗口，不通过 `$state.rx`。
PTY 路径（`~/.hpm/usb-cdc/serial.tty`）在启动日志中打印，仍可直接打开。

插件 ABI 当前是 `3`；设备插件需要用 `core/include/hpm/debug/plugin.h` 重新编译。
