# 驱动虚拟化程序 JSON 接口规范

> 适用协议：`hpm-debug/2` 中的设备侧载荷  
> 设备 UI 版本：`hpm-ui/1`  
> 传输：由 hpm debug 宿主转发，调试程序通过 `emit_json` 发送 JSON object，通过 `HPM_DEBUG_EVENT_JSON` 接收 JSON object。

本文只规定 JSON。调试程序不需要了解 GUI、WebSocket 或进程管理，只需要按下面的 JSON 结构发送和接收。

---

## 1. 消息总览

调试程序与宿主之间只有五类出站 JSON 保留键，和五类入站命令。

| 方向 | JSON 顶层键 | 用途 |
|---|---|---|
| 调试程序 → 宿主 | `$ui` | 声明设备是输入还是输出、通道、类型、控件 |
| 调试程序 → 宿主 | `$state` | 上报通道当前值，增量更新 |
| 调试程序 → 宿主 | `$event` | 上报一次性事件 |
| 调试程序 → 宿主 | `$error` | 上报设备错误 |
| 调试程序 → 宿主 | `$html` | 提供自定义 HTML 面板，用于特殊展示和输入 |
| 宿主 → 调试程序 | `cmd: "describe"` | 请求重新发送 `$ui` |
| 宿主 → 调试程序 | `cmd: "set"` | 设置一个或多个通道 |
| 宿主 → 调试程序 | `cmd: "invoke"` | 触发动作型通道 |
| 宿主 → 调试程序 | `cmd: "get"` | 请求回发 `$state` |
| 宿主 → 调试程序 | 其他 JSON | 由驱动自行解释，宿主原样转发 |

保留键必须以 `$` 开头。非保留键的 JSON 会作为普通设备 JSON 输出，不会触发设备面板逻辑。

所有出站消息必须是 **JSON object**，不能是数组、字符串或数字。

---

## 2. 设备元数据 `$ui`

### 2.1 顶层结构

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "output",
    "title": "LED",
    "description": "板载 LED",
    "channels": [],
    "state": {},
    "pins": [],
    "order": 10
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `schema` | string | 是 | 固定为 `"hpm-ui/1"` |
| `role` | string | 是 | `input` / `output` / `bidirectional` |
| `title` | string | 否 | 设备显示名，缺省用设备名 |
| `description` | string | 否 | 设备说明 |
| `channels` | array | 是 | 通道数组，最多 128 个 |
| `state` | object | 否 | 初始状态，键为通道 `id` |
| `pins` | string[] | 否 | 该设备绑定的引脚名，如 `["P1.0"]` |
| `order` | number | 否 | 设备在面板中的排序，越小越靠前 |

`role` 决定设备出现在哪个面板：

- `input`：输入设备，通常提供用户可操作的控件；
- `output`：输出设备，通常只显示设备状态；
- `bidirectional`：双向设备，上下两个面板都会出现，只显示对应方向的通道。

未发送 `$ui` 的设备仍可工作，但 `role` 视为 `unknown`，`channels` 为空，GUI 只提供通用 JSON 收发面板。

### 2.2 通道结构

```json
{
  "id": "brightness",
  "name": "亮度",
  "direction": "in",
  "type": "int",
  "widget": "number_slider",
  "unit": "%",
  "min": 0,
  "max": 100,
  "step": 1,
  "precision": 0,
  "default": 50,
  "readonly": false,
  "group": "显示",
  "order": 10,
  "description": "背光亮度",
  "secret": false,
  "confirm": false,
  "placeholder": "",
  "rows": 3
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | 是 | 通道唯一标识，`^[A-Za-z_][A-Za-z0-9_]*$`，最长 64 |
| `name` | string | 是 | 显示名 |
| `direction` | string | 是 | `in` / `out` / `inout`，见 2.3 |
| `type` | string | 是 | 数据类型，见 2.4 |
| `widget` | string | 否 | 控件建议，见 2.5 |
| `unit` | string | 否 | 单位，如 `%`、`°C`、`Hz` |
| `min` | number | 否 | 数值最小值 |
| `max` | number | 否 | 数值最大值 |
| `step` | number | 否 | 数值步长 |
| `precision` | number | 否 | 小数位数 |
| `default` | 任意 JSON | 否 | 默认值 |
| `readonly` | boolean | 否 | 只读，默认 `false` |
| `group` | string | 否 | 分组名 |
| `order` | number | 否 | 通道排序 |
| `description` | string | 否 | 提示文本 |
| `secret` | boolean | 否 | 敏感值，GUI 建议遮挡 |
| `confirm` | boolean | 否 | 操作前二次确认 |
| `placeholder` | string | 否 | 输入占位符 |
| `rows` | number | 否 | 文本域/表格行数 |
| `options` | array | 否 | `enum` 的选项 |
| `columns` | array | 否 | `table` 的列定义 |

### 2.3 `direction`

`direction` 以 GUI 为参照：

| 值 | 含义 | 典型用途 |
|---|---|---|
| `in` | GUI → 调试程序 | 按钮、开关、目标温度 |
| `out` | 调试程序 → GUI | LED、传感器读数、屏幕 |
| `inout` | 双向 | 舵机目标角度 + 当前角度 |

- `in` 且 `readonly=false`：GUI 显示控件并发送输入；
- `out`：GUI 只显示值；
- `inout`：输出面板显示回读值，输入面板显示控件；
- `readonly=true`：即使 `direction=in` 也只显示，不可操作。

### 2.4 `type` 与值类型

| `type` | JSON 值 | 说明 |
|---|---|---|
| `bool` | boolean | 开关、灯、按键状态 |
| `int` | number（整数） | 整数数值 |
| `uint` | number（非负整数） | 无符号整数 |
| `float` | number | 浮点数 |
| `enum` | string / number / boolean | 必须匹配 `options[].value` |
| `string` | string | 文本 |
| `color` | string | `#RRGGBB` |
| `bytes` | string | 十六进制字符串，例如 `"01a5ff"` |
| `action` | 无状态值 | 只支持 `invoke`，用于按钮/触发 |
| `event` | 无状态值 | 只用于 `$event` |
| `table` | array<object> | 只读表格，元素是对象 |
| `json` / `object` | 任意 JSON | 通用兜底 |
| `image` | string | 图片 data URL 或 http(s) URL |
| `chart` | array<number> 或 array<object> | 图表数据，可由 GUI 降级 |
| `custom` | 任意 JSON | 自定义渲染器，GUI 可降级 |

类型与 `direction` 的推荐组合：

```json
{"type":"bool","direction":"in"}
{"type":"bool","direction":"out"}
{"type":"int","direction":"in"}
{"type":"float","direction":"out"}
{"type":"enum","direction":"in"}
{"type":"string","direction":"out"}
{"type":"action","direction":"in"}
{"type":"event","direction":"out"}
```

`action` 不应出现在 `$state` 中；`event` 不应出现在 `$state` 中。

### 2.5 `widget`

`widget` 是可选的显示/交互建议。省略时由 `type + direction` 推导。GUI 可以忽略不支持的 `widget` 并降级。

| 类型 | 输入侧建议 | 输出侧建议 |
|---|---|---|
| `bool` | `switch`、`toggle`、`button` | `lamp`、`status` |
| 数值 | `number`、`slider`、`number_slider`、`stepper` | `value`、`progress`、`gauge`、`sparkline` |
| `enum` | `select`、`radio`、`segmented` | `badge`、`text` |
| `string` | `text`、`textarea` | `text`、`code` |
| `color` | `color` | `swatch` |
| `bytes` | `hex` | `hexdump` |
| `action` | `button` | — |
| `event` | — | `event` |
| `table` | — | `table` |
| `json` | `json` | `json` |
| `image` | — | `image` |
| `chart` | — | `chart` |

只要 `widget` 与 `type` 不匹配，GUI 必须按类型降级，不能报错。

### 2.6 `options`（`enum` 专用）

```json
{
  "id": "mode",
  "name": "模式",
  "direction": "in",
  "type": "enum",
  "default": "auto",
  "options": [
    {"value": "auto", "label": "自动", "color": "#16baaa"},
    {"value": "manual", "label": "手动"},
    {"value": "off", "label": "关闭", "color": "#ff5722"}
  ]
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `value` | string / number / boolean | 是 | 实际发送值 |
| `label` | string | 否 | 显示名 |
| `color` | string | 否 | 显示颜色 |

建议 `value` 使用字符串，便于 GUI 控件直接回传。

### 2.7 `columns`（`table` 专用）

```json
{
  "id": "keys",
  "name": "按键矩阵",
  "direction": "out",
  "type": "table",
  "columns": [
    {"id": "name", "name": "按键", "type": "string"},
    {"id": "pressed", "name": "按下", "type": "bool"},
    {"id": "level", "name": "电平", "type": "int", "unit": "V"}
  ]
}
```

`table` 的值必须是对象数组：

```json
{
  "keys": [
    {"name": "K1", "pressed": true, "level": 3},
    {"name": "K2", "pressed": false, "level": 0}
  ]
}
```

第一版 GUI 的 `table` 为只读展示；需要可编辑的复杂结构时使用 `json` 类型。

---

## 3. 设备状态 `$state`

### 3.1 格式

```json
{
  "$state": {
    "power": true,
    "brightness": 80
  }
}
```

- 键必须是已声明的通道 `id`；
- 只更新出现的键，未出现的键保持原值；
- `null` 表示该通道当前未知/失效；
- 值必须符合通道 `type`；
- 不要包含 `device`、`time`、`type`、`seq`，这些由宿主添加。

### 3.2 初始状态

推荐在 `$ui` 中直接给初始值：

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "output",
    "title": "LED",
    "channels": [
      {"id": "power", "name": "电源", "direction": "out", "type": "bool", "widget": "lamp"}
    ],
    "state": {"power": false}
  }
}
```

也可以创建后单独发送一条 `$state`。

### 3.3 状态更新时机

- 设备状态变化时；
- 收到 `set` 或 `invoke` 后；
- 收到 `get` 后；
- 初始化完成后。

状态可高频发送，但建议对同一通道做合并，避免每毫秒刷新。

---

## 4. 一次性事件 `$event`

```json
{
  "$event": {
    "channel": "pressed",
    "value": true,
    "data": {}
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `channel` | string | 否 | 关联通道 `id` |
| `value` | 任意 JSON | 否 | 事件值 |
| `data` | object | 否 | 事件附加数据 |
| `name` | string | 否 | 事件名，缺省用 `channel` |

`$event` 不进入状态快照，只用于日志、提示、瞬时动作。`type: "event"` 的通道通常通过这里上报。

示例：

```json
{"$event":{"name":"irq","data":{"line":3,"level":1}}}
```

---

## 5. 设备错误 `$error`

```json
{
  "$error": {
    "code": "i2c_timeout",
    "message": "I2C 无响应",
    "channel": "brightness",
    "fatal": false
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `code` | string | 是 | 机器可读错误码 |
| `message` | string | 是 | 人类可读说明 |
| `channel` | string | 否 | 关联通道 |
| `fatal` | boolean | 否 | `true` 表示设备离线 |

`fatal=true` 后宿主标记设备 `online=false`；非致命错误只记录最后错误，设备继续工作。

---

## 5.5 自定义 HTML 面板 `$html`

当标准通道类型无法表达设备形态时，调试程序可以直接提供一块 HTML。
GUI 会把它放进 `sandbox="allow-scripts"` 的 iframe 中渲染，CSS 和脚本与主界面隔离。

### 5.5.1 消息结构

```json
{
  "$html": {
    "id": "wave-panel",
    "title": "波形面板",
    "placement": "both",
    "height": 260,
    "html": "<canvas id=\"wave\"></canvas><button onclick=\"hpm.invoke('refresh')\">刷新</button>",
    "css": "#wave{width:100%;height:220px;background:#111;border-radius:4px}"
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | 是 | 设备内唯一，同一 `id` 的新消息会替换旧面板 |
| `title` | string | 否 | 面板标题，缺省用设备名 |
| `placement` | string | 否 | `output` / `input` / `both`，默认 `output` |
| `height` | number | 否 | iframe 高度，60–1200，默认 220 |
| `html` | string | 是 | HTML 片段，最长 256 KiB |
| `css` | string | 否 | 追加到面板内的 CSS，最长 64 KiB |
| `remove` | boolean | 否 | `true` 时删除该 `id` 的面板 |

规则：

- `$html` 是普通设备 JSON 的保留键，宿主会原样转发，不需要 HPM 改接口；
- 可以在 `create` 中发送，也可以随时发送；不要在 `describe` 回调里发送（宿主会丢弃该阶段的普通 JSON）；
- 同一设备可以有多个 `$html` 块，用 `id` 区分；
- 面板内容变化时重新发送同一 `id`；
- `$html` 适合静态布局/初始化，频繁变化的数据请用 `$state` + 面板内 `hpm.onState`，避免反复重建 iframe；
- `placement` 只决定放在输入区、输出区还是两处都放；
- HTML 运行在无 `allow-same-origin` 的沙箱 iframe 中，不能访问父页面 DOM；
- 不建议在 HTML 中写 `</script>` 字符串，按普通 HTML 片段组织即可。

### 5.5.2 面板内可用的 `hpm` 对象

GUI 会在 iframe 内注入：

```json
{
  "device": "led",
  "send": "function(payload)",
  "set": "function(channel, value)",
  "invoke": "function(channel)",
  "raw": "function(data)",
  "resize": "function(height)",
  "log": "function(message, level)",
  "onState": "function(values, message)"
}
```

示例：

```html
<button onclick="hpm.set('power', true)">开灯</button>
<button onclick="hpm.invoke('press')">按下</button>
<button onclick="hpm.raw({cmd:'get', channels:['power']})">查询</button>
<button onclick="hpm.resize(360)">加高</button>
<script>
  hpm.onState = function (values) {
    document.getElementById("wave").dataset.power = values.power;
  };
</script>
```

面板通过 `postMessage` 发给 GUI 的 JSON 必须带 `hpm: true`：

```json
{"hpm":true,"cmd":"set","channel":"power","value":true}
{"hpm":true,"cmd":"invoke","channel":"press"}
{"hpm":true,"cmd":"raw","data":{"cmd":"get","channels":["power"]}}
{"hpm":true,"cmd":"resize","height":360}
{"hpm":true,"cmd":"log","level":"info","message":"panel ready"}
```

### 5.5.3 GUI 推给面板的状态

设备状态变化时，GUI 会向所有属于该设备的 HTML 面板发送：

```json
{"hpm":true,"type":"state","device":"led","values":{"power":true}}
```

面板可以设置 `hpm.onState(values)`，也可以监听：

```html
<script>
  window.addEventListener("hpm-state", function (event) {
    console.log(event.detail.values);
  });
</script>
```

### 5.5.4 完整 JSON 示例

```json
{
  "$html": {
    "id": "scope",
    "title": "示波器",
    "placement": "both",
    "height": 300,
    "css": "body{background:#101318;color:#c7ccd4}#scope{width:100%;height:250px;display:block}",
    "html": "<canvas id=\"scope\"></canvas><button onclick=\"hpm.invoke('trigger')\">触发</button>",
    "remove": false
  }
}
```

面板内的 JS：

```html
<script>
  const canvas = document.getElementById("scope");
  const ctx = canvas.getContext("2d");
  canvas.width = canvas.clientWidth;
  canvas.height = canvas.clientHeight;

  function draw(values) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = "#16baaa";
    ctx.beginPath();
    (values.samples || []).forEach(function (v, i) {
      const x = (i / Math.max(1, values.samples.length - 1)) * canvas.width;
      const y = canvas.height - (Number(v) || 0) * canvas.height;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  hpm.onState = function (values) { draw(values); };
</script>
```

---

## 6. 宿主发给调试程序的命令

宿主会把外部请求的 `data` 原样序列化后交给调试程序；`describe` 是宿主直接生成的请求。

### 6.1 `describe`

```json
{"cmd":"describe"}
```

要求：收到后重新发送完整 `$ui`。必须幂等，任何状态下都可调用。

### 6.2 `set`

设置单个通道：

```json
{"cmd":"set","channel":"power","value":true}
```

批量设置：

```json
{"cmd":"set","values":{"power":true,"brightness":80}}
```

批量设置应尽可能原子处理；无法应用的通道通过 `$error` 的 `channel` 指明。

### 6.3 `invoke`

触发动作型通道：

```json
{"cmd":"invoke","channel":"press"}
```

动作通道不应保存状态；如需反馈，用 `$event` 或 `$state`。

### 6.4 `get`

请求回发状态：

```json
{"cmd":"get","channels":["power","brightness"]}
```

`channels` 省略时回发全部已声明通道。收到后发送 `$state`。

### 6.5 其他 JSON

不属于以上命令的 `data` 可以自行解释。建议也使用 `cmd` 字段区分，并保持向后兼容。

---

## 7. 宿主归一化后的消息（供调试参考）

调试程序发送的保留键会被宿主转换；调试程序不需要自己添加 `device`、`time`、`seq`。

`$ui`：

```json
{"$ui":{"schema":"hpm-ui/1","role":"output","title":"LED","channels":[]}}
```

宿主输出：

```json
{"device":"led","type":"event","event":"device.updated","seq":5,"time":123,
 "ui":{"schema":"hpm-ui/1","role":"output","title":"LED","channels":[]}}
```

`$state`：

```json
{"$state":{"power":true}}
```

宿主输出：

```json
{"device":"led","type":"state","seq":13,"time":578,
 "online":true,"values":{"power":true}}
```

`$error`：

```json
{"$error":{"code":"i2c_timeout","message":"I2C 无响应","channel":"brightness"}}
```

宿主输出：

```json
{"device":"led","type":"error","seq":14,"time":579,
 "error":{"code":"i2c_timeout","message":"I2C 无响应","channel":"brightness"}}
```

`$html`：

```json
{"$html":{"id":"scope","title":"示波器","placement":"both","height":260,
          "html":"<canvas id=\"scope\"></canvas>"}}
```

宿主输出（按普通设备 JSON 透传）：

```json
{"device":"led","type":"json","seq":15,"time":580,
 "data":{"$html":{"id":"scope","title":"示波器","placement":"both",
                  "height":260,"html":"<canvas id=\"scope\"></canvas>"}}}
```

GUI 会识别 `data.$html` 并渲染自定义面板；调试程序不需要自己拼接 `device`、`time`、`seq`。

---

## 8. 完整 JSON 示例

### 8.1 输出设备：LED

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "output",
    "title": "LED",
    "channels": [
      {
        "id": "power",
        "name": "电源",
        "direction": "out",
        "type": "bool",
        "widget": "lamp",
        "readonly": true
      },
      {
        "id": "brightness",
        "name": "亮度",
        "direction": "out",
        "type": "int",
        "widget": "value",
        "unit": "%",
        "min": 0,
        "max": 100
      }
    ],
    "state": {"power": false, "brightness": 0}
  }
}
```

状态更新：

```json
{"$state":{"power":true,"brightness":80}}
```

### 8.2 输入设备：按键

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "input",
    "title": "按键",
    "channels": [
      {
        "id": "pressed",
        "name": "按下",
        "direction": "in",
        "type": "bool",
        "widget": "button"
      },
      {
        "id": "level",
        "name": "电平",
        "direction": "in",
        "type": "bool",
        "widget": "switch",
        "default": false
      }
    ]
  }
}
```

收到：

```json
{"cmd":"invoke","channel":"pressed"}
```

或：

```json
{"cmd":"set","channel":"level","value":true}
```

反馈：

```json
{"$event":{"channel":"pressed","value":true}}
{"$state":{"level":true}}
```

### 8.3 输入设备：数值目标

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "input",
    "title": "温控目标",
    "channels": [
      {
        "id": "target",
        "name": "目标温度",
        "direction": "in",
        "type": "float",
        "widget": "number_slider",
        "unit": "°C",
        "min": 16,
        "max": 32,
        "step": 0.5,
        "precision": 1,
        "default": 25
      }
    ]
  }
}
```

收到：

```json
{"cmd":"set","channel":"target","value":26.5}
```

### 8.4 双向设备：屏幕 + 触摸

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "bidirectional",
    "title": "屏幕",
    "channels": [
      {
        "id": "text",
        "name": "显示文本",
        "direction": "out",
        "type": "string",
        "widget": "text"
      },
      {
        "id": "touch",
        "name": "触摸",
        "direction": "in",
        "type": "action",
        "widget": "button"
      }
    ],
    "state": {"text": "Ready"}
  }
}
```

### 8.5 表格设备：键盘矩阵

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "output",
    "title": "键盘矩阵",
    "channels": [
      {
        "id": "keys",
        "name": "按键状态",
        "direction": "out",
        "type": "table",
        "columns": [
          {"id": "name", "name": "按键", "type": "string"},
          {"id": "pressed", "name": "按下", "type": "bool"}
        ]
      }
    ],
    "state": {
      "keys": [
        {"name": "K1", "pressed": false},
        {"name": "K2", "pressed": true}
      ]
    }
  }
}
```

### 8.6 颜色设备：RGB LED

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "input",
    "title": "RGB LED",
    "channels": [
      {
        "id": "color",
        "name": "颜色",
        "direction": "in",
        "type": "color",
        "widget": "color",
        "default": "#16baaa"
      }
    ]
  }
}
```

收到：

```json
{"cmd":"set","channel":"color","value":"#ff5722"}
```

### 8.7 未知类型兜底

```json
{
  "$ui": {
    "schema": "hpm-ui/1",
    "role": "bidirectional",
    "title": "自定义设备",
    "channels": [
      {
        "id": "payload",
        "name": "原始数据",
        "direction": "inout",
        "type": "json",
        "widget": "json"
      }
    ]
  }
}
```

GUI 找不到对应控件时必须显示 JSON 编辑器/只读 JSON，而不是报错。

### 8.8 自定义 HTML 面板

```json
{
  "$html": {
    "id": "custom-io",
    "title": "自定义面板",
    "placement": "both",
    "height": 240,
    "html": "<button onclick=\"hpm.set('power', true)\">开</button><button onclick=\"hpm.set('power', false)\">关</button><div id=\"state\"></div>",
    "css": "button{margin:2px;padding:4px 8px}#state{margin-top:6px;color:#16baaa}"
  }
}
```

面板内脚本读取状态：

```html
<script>
  hpm.onState = function (values) {
    document.getElementById("state").textContent = "power = " + values.power;
  };
</script>
```

---

## 9. 校验与限制

| 项目 | 要求 |
|---|---|
| `$ui.schema` | 必须是 `"hpm-ui/1"` |
| `role` | `input` / `output` / `bidirectional` |
| `channels` | 数组，最多 128 项 |
| `channel.id` | 设备内唯一，最长 64，匹配 `^[A-Za-z_][A-Za-z0-9_]*$` |
| `channel.direction` | `in` / `out` / `inout` |
| `channel.type` | 本文列出的类型之一；未知类型按 `json` 处理 |
| `$state` 键 | 必须对应已声明通道 |
| `$state` 值 | 必须符合通道 `type` |
| `$event` | 必须含 `channel` 或 `name` |
| `$error.code` | 非空字符串 |
| `$html.id` | 设备内唯一；同 `id` 覆盖旧面板 |
| `$html.placement` | `input` / `output` / `both` |
| `$html.html` | 字符串，最长 256 KiB |
| `$html.css` | 字符串，最长 64 KiB |
| `$html.height` | 60–1200，默认 220 |
| 单条 JSON | 建议不超过 64 KiB；`$html` 单条可到 256 KiB |
| JSON 编码 | UTF-8，单行 JSON object |

违反上述要求时，宿主/ GUI 应忽略该条消息或降级处理，不能崩溃。

---

## 10. 调试程序接入检查清单

- [ ] 设备创建后发送一条完整 `$ui`，或能响应 `describe`。
- [ ] `$ui.schema` 为 `"hpm-ui/1"`。
- [ ] `role` 正确声明输入/输出/双向。
- [ ] 每个通道有唯一 `id`、`name`、`direction`、`type`。
- [ ] 数值通道给出合理的 `min`/`max`/`step`/`unit`。
- [ ] `enum` 的 `options` 完整。
- [ ] 初始状态通过 `$ui.state` 或首条 `$state` 提供。
- [ ] 状态变化时发送 `$state`，只包含变化的键。
- [ ] 收到 `set` / `invoke` / `get` 时能正确响应。
- [ ] 能处理 `describe` 并重复发送 `$ui`。
- [ ] 错误通过 `$error` 上报，致命错误标记 `fatal=true`。
- [ ] 特殊界面通过 `$html` 发送，`id` 唯一，更新用同一 `id`，删除用 `remove:true`。
- [ ] 自定义面板只使用 `hpm.set/invoke/raw/resize/log` 回传输入，不依赖父页面 DOM。
- [ ] 不对 `device` / `time` / `seq` / `type` 等宿主字段做假设。
- [ ] 未识别命令返回后不崩溃。
