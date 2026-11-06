#!/usr/bin/env python3
# Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# -*- coding: utf-8 -*-
"""
扫描 HPM 项目里的硬编码字符串

检测：
1. 文件名字符串：".json"、".h"、".c"、".ld" 等
2. 目录名字符串："src"、"include"、"ld"、"driver"、"build" 等
3. URL 字符串："http://"、"https://"、"file://"
4. 环境变量名："HPM_" 开头
5. 缓存路径：".hpm"、"packages"、"cache"、"tarballs"、"toolchains"
6. 版本号："0.1.0" 等
7. 文件名宏：已用 HPM_FILE_* 的不报

用法：
    python3 check-hardcode.py [项目根目录]
"""

import os
import re
import sys
from pathlib import Path

# 要检查的目录
CHECK_DIRS = ["core", "cli", "dev", "gui"]

# 排除的目录
EXCLUDE_DIRS = {"build", "third_party", "tests", ".git", "driver", "packages"}

# 排除的文件
EXCLUDE_FILES = {"config.h"}

# 已知的硬编码模式
PATTERNS = [
    # 文件名
    (r'"([a-zA-Z_][a-zA-Z0-9_]*\.(json|h|c|ld|sig|pub|fingerprint|txt|md|tar|gz|hex|bin|elf|ihx|rel|o))"',
     "文件名"),
    # 目录名
    (r'"([a-z_]+)"',
     "可能是目录名"),
    # URL
    (r'"(https?://[^"]+)"',
     "URL"),
    (r'"(file://[^"]+)"',
     "file:// URL"),
    # 环境变量
    (r'"(HPM_[A-Z_]+)"',
     "环境变量"),
    # 版本号
    (r'"(\d+\.\d+\.\d+)"',
     "版本号"),
    # 路径片段
    (r'"(cache|packages|tarballs|toolchains|keys|tmp)"',
     "路径片段"),
]

# 已知的例外（这些是合理的硬编码）
EXCEPTIONS = {
    # 字符串常量
    "http://", "https://", "file://",
    "GET", "POST", "PUT", "DELETE",
    "Content-Type", "application/json",
    "User-Agent", "hpm/0.1.0",
    # 类型名
    "int", "void", "char", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "size_t",
    "const", "static", "extern", "volatile", "unsigned", "signed",
    "struct", "enum", "union", "typedef",
    # JSON 字段
    "name", "version", "driver", "mcu", "devices", "pins", "capabilities",
    "type", "instance", "channel", "shareable", "requires", "provides",
    "methods", "params", "return", "optional", "kind", "developer",
    "url", "sig_url", "sha256", "size", "license", "cert",
    "schema", "official_cert", "registries", "offline", "packages",
    "versions", "description", "family", "package", "build", "toolchain",
    "flash", "cpu", "flags", "ldscript", "startup", "qemu_machine",
    "tool", "args", "base_address", "prefix", "cc", "objcopy",
    "device", "pin", "value", "type", "pins",
    # 其他
    "true", "false", "null", "y", "n", "yes", "no",
    "ok", "error", "warn", "info",
}

# 已知用宏替换的字符串
REPLACED = {
    "project.json": "HPM_FILE_PROJECT",
    "driver.json": "HPM_FILE_DRIVER_STATE",
    "manifest.json": "HPM_FILE_MANIFEST",
    "chip.json": "HPM_FILE_CHIP",
    "driver.h": "HPM_FILE_DRIVER_H",
    "driver.c": "HPM_FILE_DRIVER_C",
    "hpm.h": "HPM_FILE_HPM_H",
    "Makefile": "HPM_FILE_MAKEFILE",
    "LICENSE": "HPM_FILE_LICENSE",
    "official.crt": "HPM_FILE_OFFICIAL_CERT",
    "official.crt.fingerprint": "HPM_FILE_OFFICIAL_CERT_FP",
    "driver": "HPM_DIR_DRIVER",
    "build": "HPM_DIR_BUILD",
    "src": "HPM_DIR_SRC",
    "include": "HPM_DIR_INCLUDE",
    "ld": "HPM_DIR_LD",
    "packages": "HPM_PACKAGES_DIR_NAME",
    "cache/tarballs": "HPM_TARBALLS_DIR_NAME",
    ".hpm": "HPM_CACHE_DIR_NAME",
    "HPM_REGISTRY": "HPM_ENV_REGISTRY",
    "HPM_OFFICIAL_CERT": "HPM_ENV_OFFICIAL_CERT",
    "HPM_ROOT_CA": "HPM_ENV_ROOT_CA",
    "HPM_HOME": "HPM_ENV_HOME",
    "HPM_SKIP_SIGNATURE": "HPM_ENV_SKIP_SIGNATURE",
    "hpm/0.1.0": "HPM_USER_AGENT",
    "0.1.0": "HPM_VERSION_STR",
}


def should_skip(path: Path) -> bool:
    """跳过测试、第三方、生成的目录"""
    parts = path.parts
    for part in parts:
        if part in EXCLUDE_DIRS:
            return True
    if path.name in EXCLUDE_FILES:
        return True
    return False


def scan_file(path: Path):
    """扫描单个文件"""
    findings = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.readlines()
    except UnicodeDecodeError:
        return findings

    for lineno, line in enumerate(lines, 1):
        # 跳过注释行
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
            continue

        # 跳过 #include 行
        if stripped.startswith("#include"):
            continue

        # 跳过 #define 行（宏定义本身）
        if stripped.startswith("#define"):
            continue

        for pattern, desc in PATTERNS:
            for match in re.finditer(pattern, line):
                value = match.group(1) if match.groups() else match.group(0)

                # 跳过例外
                if value in EXCEPTIONS:
                    continue

                # 跳过已知用宏替换的
                if value in REPLACED:
                    findings.append({
                        "line": lineno,
                        "value": value,
                        "desc": desc,
                        "suggestion": REPLACED[value],
                        "text": stripped,
                    })
                else:
                    findings.append({
                        "line": lineno,
                        "value": value,
                        "desc": desc,
                        "suggestion": None,
                        "text": stripped,
                    })

    return findings


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    if not root.exists():
        print(f"目录不存在: {root}")
        sys.exit(1)

    print(f"扫描目录: {root}\n")

    total = 0
    for dir_name in CHECK_DIRS:
        dir_path = root / dir_name
        if not dir_path.exists():
            continue

        print(f"=== {dir_name}/ ===\n")

        for path in sorted(dir_path.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix not in {".h", ".cpp", ".c", ".hpp"}:
                continue
            if should_skip(path):
                continue

            findings = scan_file(path)
            if not findings:
                continue

            rel = path.relative_to(root)
            print(f"  {rel}")

            for f in findings:
                marker = "  ✗" if f["suggestion"] else "  ?"
                print(f"{marker} L{f['line']:4d}  {f['value']!r}")
                print(f"         {f['desc']}")
                if f["suggestion"]:
                    print(f"         → 用 {f['suggestion']}")
                print()

            total += len(findings)

    print(f"\n=== 总计: {total} 处硬编码 ===\n")
    if total > 0:
        print("建议：")
        print("  - ✗ 标记的应该用 config.h 里的宏")
        print("  - ? 标记的需要人工判断是否合理")


if __name__ == "__main__":
    main()