# CH552 debug 示例

演示驱动包 `debug/debug.c` 如何把 P1.0 虚拟成 LED，并在 `hpm debug` 中输出事件。

```text
project/
├── main.c          # 翻转 P1.0
├── project.toml
├── project.json
├── driver.json     # 声明虚拟设备 led，绑定 P1_0
└── driver/         # 芯片驱动（已生成，可直接 hpm build）
```

```bash
./run.sh          # 自动 hpm build，然后启动 CLI（默认暂停）
./run.sh --json   # 自动 hpm build，然后 JSON Lines 模式
```

脚本会在 `.work/` 下复制项目、编译 `main.c`、准备本地包缓存，再启动 `hpm debug`。
也可以直接进入 `project/` 手动运行：

```bash
cd project
hpm build
hpm --cache-dir ../.work/cache -C . debug \
  --model ../../../debug/mcus/models/ch552g.json \
  --config ../../../debug/mcus/config/example.toml
```
