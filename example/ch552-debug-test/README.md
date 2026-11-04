# CH552 debug 示例

这个示例包含一个**真实可用的 LED 驱动**，`main.c` 通过驱动接口控制 LED，
不再直接操作 `P1`。

```text
project/
├── main.c          # 调用 led_on() / led_off()
├── project.toml    # 声明设备 led，绑定 P1_1
├── project.json
├── driver.json     # 生成的设备/版本/绑定
└── driver/         # 生成的驱动代码
    ├── driver.h    # 包含 led_on/led_off/led_toggle
    ├── led/        # gpio_led 驱动实例
    └── ch552g/     # 芯片驱动

package/
├── manifest.json   # 驱动名 gpio_led，requires.pin = gpio_out
├── src/gpio_led.c  # 通过 hpm_pin_P1_1_* 控制 LED
├── include/gpio_led.h
└── debug/debug.c   # 设备虚拟化：power 通道、$ui/$state/virtual LED
```

`main.c`：

```c
#include "driver.h"

void main(void) {
    hpm_init_all();
    while (1) {
        led_on();
        delay();
        led_off();
        delay();
    }
}
```

## 运行

```bash
./run.sh          # 自动 hpm build，然后启动 CLI（默认暂停）
./run.sh --json   # 自动 hpm build，然后 JSON Lines 模式
```

`debug/debug.c` 把 P1.1 映射成 `power` 通道，GUI/外部程序可以：

- 收到 `power` 的 `$state`（亮/灭）；
- 收到 `[led]` 日志；
- 对只读 LED 通道发送 `set` 会得到 `readonly` 错误。

运行时会自动编译并加载 `debug/debug.c`。

## 直接构建

```bash
cd project
../../../build/bin/hpm build
```

产物是 `project/build/firmware.hex`。

## 手动调试

```bash
cd project
hpm --cache-dir ../.work/cache -C . debug \
  --allow-missing-debug \
  --isa ../../../build/lib/libmcus_isa_8051.so \
  --model ../../../debug/mcus/models/ch552g.json \
  --config ../../../debug/mcus/config/example.toml
```
