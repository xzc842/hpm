/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// core/include/hpm/config.h
#pragma once

// ============================================================
// HPM 全局配置
// ============================================================

// ── 根证书 / 官方证书 ──
// 根 CA 证书（PEM）固定在客户端中，根私钥不进入仓库。
#define HPM_ROOT_CA_PEM R"PEM(-----BEGIN CERTIFICATE-----
MIIBcDCCASKgAwIBAgIUbxJStO+IYuUjGZ2GqCKWHabO2a4wBQYDK2VwMCQxFDAS
BgNVBAMMC0hQTSBSb290IENBMQwwCgYDVQQKDANIUE0wHhcNMjYxMTA2MTExMDM3
WhcNNDYxMTAxMTExMDM3WjAkMRQwEgYDVQQDDAtIUE0gUm9vdCBDQTEMMAoGA1UE
CgwDSFBNMCowBQYDK2VwAyEAxtoW0XMlNVBZWUco9jk2zH7FyigIbHVpPypatiOF
EBSjZjBkMBIGA1UdEwEB/wQIMAYBAf8CAQIwDgYDVR0PAQH/BAQDAgEGMB0GA1Ud
DgQWBBS9zLfXrrDF6+fT36E+3FTx7VRHSjAfBgNVHSMEGDAWgBS9zLfXrrDF6+fT
36E+3FTx7VRHSjAFBgMrZXADQQDLg8qbPEDKmB6i/Wcr7f1e57K9sWEVjw0QQKL/
OL5NFoAb12sRoQmdCgGrPIxf9iVQZlxL7uBFOvL5LF46jREA
-----END CERTIFICATE-----
)PEM"

#define HPM_OFFICIAL_CERT_URL \
    "https://raw.githubusercontent.com/AstercoreTeam/hpm-repo/main/official.crt"

// ── 默认 Registry ──
// 默认索引地址
#define HPM_DEFAULT_REGISTRY_URL \
    "https://raw.githubusercontent.com/AstercoreTeam/hpm-repo/main/index.json"

// ── 环境变量名 ──
#define HPM_ENV_REGISTRY          "HPM_REGISTRY"
#define HPM_ENV_OFFICIAL_CERT     "HPM_OFFICIAL_CERT"
#define HPM_ENV_ROOT_CA           "HPM_ROOT_CA"
#define HPM_ENV_HOME              "HPM_HOME"
#define HPM_ENV_SKIP_SIGNATURE    "HPM_SKIP_SIGNATURE"

// ── 缓存目录 ──
// 默认缓存目录（相对 HOME）
#define HPM_CACHE_DIR_NAME        ".hpm"
// 包目录
#define HPM_PACKAGES_DIR_NAME     "packages"
// tarball 缓存目录
#define HPM_TARBALLS_DIR_NAME     "cache/tarballs"
// 开发者公钥缓存目录
#define HPM_KEYS_DIR_NAME         "cache/keys"
// 临时文件目录
#define HPM_TMP_DIR_NAME          "cache/tmp"
// 工具链目录
#define HPM_TOOLCHAINS_DIR_NAME   "toolchains"

// ── 文件名 ──
#define HPM_FILE_PROJECT          "project.json"
#define HPM_FILE_DRIVER_STATE     "driver.json"
#define HPM_FILE_MANIFEST         "manifest.json"
#define HPM_FILE_CHIP             "chip.json"
#define HPM_FILE_TOOLCHAIN        "toolchain.json"
#define HPM_FILE_LICENSE          "LICENSE"
#define HPM_FILE_INDEX            "index.json"
#define HPM_FILE_INDEX_SIG        "index.json.sig"
#define HPM_FILE_OFFICIAL_CERT    "official.crt"
#define HPM_FILE_OFFICIAL_CERT_FP "official.crt.fingerprint"

// ── 目录名 ──
#define HPM_DIR_DRIVER            "driver"
#define HPM_DIR_BUILD             "build"
#define HPM_DIR_SRC               "src"
#define HPM_DIR_INCLUDE           "include"
#define HPM_DIR_LD                "ld"
#define HPM_DIR_TEST              "test"

// ── 生成的文件名 ──
#define HPM_FILE_DRIVER_H         "driver.h"
#define HPM_FILE_DRIVER_C         "driver.c"
#define HPM_FILE_HPM_H            "hpm.h"
#define HPM_FILE_HPM_DEV_H        "hpm_dev.h"
#define HPM_FILE_MAKEFILE         "Makefile"

// ── 版本 ──
#define HPM_VERSION_STR           "0.1.0"

// ── HTTP ──
#define HPM_HTTP_TIMEOUT          30L
#define HPM_HTTP_DOWNLOAD_TIMEOUT 600L
#define HPM_USER_AGENT            "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:158.0) Gecko/20100101 Firefox/158.0"

// ── 进度条 ──
#define HPM_PROGRESS_BAR_WIDTH    30

// ── 签名 ──
#define HPM_SIG_ALGORITHM         "SHA256"

// ── 工具链平台标识 ──
// 这些在运行时由 ToolchainManager::platform() 返回
// linux-x64 / darwin-x64 / win32-x64

// HPM 开发者中心
#define HPM_DEV_CENTER_URL        "https://hpm.astercore.dpdns.org"
#define HPM_DEV_CENTER_APP_ID     "1bdcb2b136d43431"
#define HPM_DEV_CENTER_APP_KEY    "2942c1a1b37c104914f4dd8f96a399bf"
#define HPM_DEV_CENTER_API        "https://hpm.astercore.dpdns.org/api/webhooks/apps"
#define HPM_DEV_CENTER_TOKEN_API  "https://hpm.astercore.dpdns.org/api/public/oauth/token"
#define HPM_DEV_CENTER_OAUTH_URL  "https://hpm.astercore.dpdns.org/oauth/authorize"