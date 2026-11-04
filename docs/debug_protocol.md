# HPM Debug JSON 协议

> 协议版本：`hpm-debug/1`
> 传输：stdin/stdout，一行一个 JSON object（JSON Lines）

`hpm debug --json` 让外部程序以子进程方式控制模拟器、给设备发输入、收设备输出。

---

## 1. 启动

```bash
hpm debug --json \
  --model <model.json> \
  --config <config.toml>
```

- **stdout**：只有 JSON Lines，不能混人类可读文本。
- **stderr**：hpm/mcus 的日志、编译错误、诊断信息。
- 默认启动即暂停。
- `--run` 启动后立即运行。

---

## 2. 握手

启动完成后，第一行是 ready：

```json
{"device":"mcu","type":"event","event":"ready",
 "protocol":"hpm-debug/1","status":"paused",
 "devices":["led","screen"]}
```

- `devices` 是当前会话成功加载的用户设备。
- `status` 初始为 `paused`。

---

## 3. 请求 / 响应

请求：

```json
{"id":1,"device":"mcu","cmd":"pause"}
```

响应：

```json
{"id":1,"device":"mcu","type":"response","ok":true,
 "result":{"status":"paused"}}
```

- `id` 可选；存在时响应原样带回。
- `device` 必填。
- `device = "mcu"` 表示控制模拟器。
- 其他 `device` 表示发给对应设备插件。

错误：

```json
{"id":4,"device":"mcu","type":"response","ok":false,
 "error":{"code":"error","message":"..."}}
```

---

## 4. `mcu` 命令

### 4.1 状态

```json
{"id":1,"device":"mcu","cmd":"status"}
```

```json
{"id":1,"device":"mcu","type":"response","ok":true,
 "result":{"status":"paused","pc":182,"virtual_time":0,
           "halted":false,"breakpoints":[214]}}
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

- `step` 执行完一定进入 `paused`（或 `halted`）。
- 默认 `reset` 保留断点。

### 4.3 断点

```json
{"device":"mcu","cmd":"breakpoint.set","addr":"0x00D3"}
{"device":"mcu","cmd":"breakpoint.clear","addr":"0x00D3"}
{"device":"mcu","cmd":"breakpoint.clear_all"}
{"device":"mcu","cmd":"breakpoint.list"}
```

`addr` 支持数字或 `"0x..."` 字符串；最多 32 个断点。

命中时异步事件：

```json
{"device":"mcu","type":"event","event":"breakpoint",
 "addr":211,"pc":211,"time":145257}
```

### 4.4 引脚

```json
{"device":"mcu","cmd":"pin.read","pin":"P1.1"}
```

```json
{"device":"mcu","type":"response","ok":true,
 "result":{"pin":"P1.1","value":1,"function":0,"direction":0}}
```

```json
{"device":"mcu","cmd":"pin.set","pin":"P1.1","value":0}
```

### 4.5 寄存器 / 中断

```json
{"device":"mcu","cmd":"reg.read","space":"sfr","addr":"0x90","width":8}
{"device":"mcu","cmd":"irq.inject","number":1}
```

### 4.6 关闭

```json
{"device":"mcu","cmd":"shutdown"}
```

或关闭 stdin。宿主会停止 mcus、卸载插件并退出。

---

## 5. 设备输入

```json
{"id":2,"device":"led","data":{"power":"on"}}
```

- `data` 会作为 JSON 字符串传给插件。
- 插件可以返回日志/JSON，宿主异步输出。
- 若带 `id`，宿主会先返回一个 `ok:true` 响应。

---

## 6. 设备输出

### 日志

```json
{"device":"led","type":"log","level":"info","time":123,"message":"on"}
```

### JSON

```json
{"device":"led","type":"json","time":123,"data":{"brightness":80}}
```

### 引脚事件

```json
{"device":"led","type":"pin","time":123,
 "pin":"P1.1","value":1,"function":0,"direction":0}
```

> 当前实现只把 PIN 事件路由给设备插件；如果需要把原始 PIN 流暴露给外部程序，可在后续加 `--expose-pins`。

---

## 7. 总线事件（预留）

```json
{"device":"flash","type":"bus","time":123,
 "bus":"spi0","xfer_id":7,"tx":"A5","flags":["cs"]}
```

```json
{"id":8,"device":"flash","type":"bus",
 "xfer_id":7,"rx":"FF","flags":["cs"]}
```

总线引擎接入后启用；ABI 已预留。

---

## 8. 完整示例

```bash
(
  sleep 2
  echo '{"id":1,"device":"mcu","cmd":"run"}'
  sleep 1
  echo '{"id":2,"device":"led","data":{"power":"on"}}'
  sleep 1
  echo '{"id":3,"device":"mcu","cmd":"pause"}'
  echo '{"id":4,"device":"mcu","cmd":"status"}'
  echo '{"id":5,"device":"mcu","cmd":"shutdown"}'
) | hpm debug --json --model ... --config ...
```

---

## 9. 约束

- 一行一个 JSON，不能跨行。
- 输出顺序：设备事件可能和响应交错，外部程序按 `type`/`device` 分发。
- 不要依赖字段顺序。
- 未知 `device` 返回错误。
- 未知 `cmd` 返回错误。
- stdout 只用于协议；调试信息必须写 stderr。
