#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$HERE/.work"

rm -rf "$WORK"
mkdir -p "$WORK/cache/packages"

cp -r "$HERE/project" "$WORK/project"
cp -r "$HERE/package" "$WORK/cache/packages/test-led-ch552-1.0.0"

# 先编译完整项目（main.c + driver/）
"$ROOT/build/bin/hpm" -C "$WORK/project" build >&2

# 默认暂停；加 --json 或 --run 会透传给 hpm debug
exec "$ROOT/build/bin/hpm" \
  --cache-dir "$WORK/cache" \
  -C "$WORK/project" \
  debug \
  --model "$ROOT/debug/mcus/models/ch552g.json" \
  --config "$ROOT/debug/mcus/config/example.toml" \
  "$@"
