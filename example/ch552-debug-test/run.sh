#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$HERE/.work"

rm -rf "$WORK"
mkdir -p "$WORK/cache/packages"

cp -r "$HERE/project" "$WORK/project"
cp -r "$HERE/package" "$WORK/cache/packages/gpio_led-ch552g-1.0.0"

# 如果没有生成过 driver/，用本地缓存里的芯片包重新 sync
if [[ ! -d "$WORK/project/driver" ]]; then
    CHIP_PKG="${HOME}/.hpm/packages/ch552g-1.0.0"
    if [[ -d "$CHIP_PKG" ]]; then
        cp -r "$CHIP_PKG" "$WORK/cache/packages/"
    fi
    "$ROOT/build/bin/hpm" --cache-dir "$WORK/cache" \
        -C "$WORK/project" sync --offline >&2
fi

# 先编译完整项目（main.c + driver/）
"$ROOT/build/bin/hpm" -C "$WORK/project" build >&2

# 虚拟化插件尚未提供时，允许启动模拟器但不加载设备插件
DEBUG_EXTRA=()
if [[ ! -f "$HERE/package/debug/debug.c" ]]; then
    DEBUG_EXTRA+=(--allow-missing-debug)
fi

# 默认暂停；加 --json 或 --run 会透传给 hpm debug
exec "$ROOT/build/bin/hpm" \
  --cache-dir "$WORK/cache" \
  -C "$WORK/project" \
  debug \
  "${DEBUG_EXTRA[@]}" \
  --isa "$ROOT/build/lib/libmcus_isa_8051.so" \
  --model "$ROOT/debug/mcus/models/ch552g.json" \
  --config "$ROOT/debug/mcus/config/example.toml" \
  "$@"
