# HPM 原生 Windows 移植方案

本文档描述将 HPM 从 Linux/macOS 移植到原生 Windows的计划。

---

## 一、目标

| 项目 | 目标 |
|------|------|
| 运行环境 | Windows 10 1809+ / Windows 11，x64 |
| 工具链 | MSVC 2022 或 MinGW-w64 |
| 依赖 | 全部通过 vcpkg 获取，不要求用户手动编译 |
| 用户体验 | hpm、hpm-dev命令可用 |
| 路径 | 支持中文路径、长路径（> 260 字符） |
| 子进程 | 调用 `sdcc.exe`、`arm-none-eabi-gcc.exe`、`make.exe`、`tar.exe`、`openssl.exe` |

---

## 二、移植范围

| 模块 | 工作量 |
|------|--------|
| `core/util/process` | 大 |
| `core/util/fs_utils` | 中 |
| `core/registry/registry_client` | 中 |
| `core/registry/official_cert` | 小 |
| `core/toolchain/toolchain_manager` | 大 |
| `core/generator/generator` | 中 |
| `cli/`、`dev/` | 小 |
| `CMakeLists.txt` | 中 |
| `hpm-dev publish` | 小 |


---

## 三、关键问题与方案

### 3.1 子进程调用

**问题**：`util::runProcess` 用 `popen`，Windows 下 `_popen` 行为不同（无 shell 参数转义、无 `2>&1` 重定向、编码问题）。

**方案**：

- **不直接用 `_popen`**，改用 `CreateProcessW`（宽字符版）
- 参数用 `CommandLineToArgvW` 的反向逻辑构造命令行，或直接用 `CreateProcessW` 的 `lpApplicationName` + `lpCommandLine`
- 捕获 stdout / stderr：创建匿名管道，`STARTUPINFO` 里设 `hStdOutput` / `hStdError`
- 读管道用 `ReadFile` + 线程，或 `PeekNamedPipe` 轮询
- 编码：Windows 控制台默认 GBK，`CreateProcessW` 用 UTF-16，输出管道读出来是 UTF-16 或 GBK，需要转 UTF-8
- 退出码：`GetExitCodeProcess`

**抽象**：`runProcess` 签名不变（`program` + `args` + `verbose`），内部按平台分支。

**子进程列表**：

| 命令 | Windows 对应 |
|------|-------------|
| `tar` | `tar.exe`（Win10 1803+ 自带） |
| `openssl` | vcpkg 安装的 `openssl.exe`，或独立分发 |
| `sdcc` | 用户安装的 SDCC，或 HPM 自动下载 |
| `make` | `mingw32-make.exe` 或 `nmake.exe`，或 HPM 自动下载 |
| `arm-none-eabi-gcc` | 用户安装，或 HPM 自动下载 |

**不依赖 shell**：所有命令直接 `CreateProcessW`，不经过 `cmd.exe`，避免转义问题。

### 3.2 路径处理

**问题**：

- Windows 路径分隔符 `\`，Unix `/`
- 长路径限制 260 字符
- 中文路径
- 盘符、UNC 路径（`\\server\share`）

**方案**：

- 全部用 `std::filesystem::path`，不手动拼字符串
- 路径拼接用 `/` 运算符，`std::filesystem` 自动处理分隔符
- 长路径：所有文件操作前，用 `\\?\` 前缀包装绝对路径（`std::filesystem` 在 MSVC 上部分支持，需要显式处理）
- 中文路径：`std::filesystem::path` 用 `wchar_t` 存储，`u8string()` / `u16string()` 转换
- 包目录名：`<domain>/<name>` 转 `<domain>_<name>` 时，用 `std::filesystem::path::preferred_separator` 判断，不硬编码 `/`
- 临时目录：`std::filesystem::temp_directory_path()`，Windows 下是 `%TEMP%`

**建议**：项目根目录、缓存目录、工具链目录，全部用绝对路径（`std::filesystem::absolute()`），避免相对路径在 `CreateProcessW` 的 `lpCurrentDirectory` 里出问题。

### 3.3 环境变量与缓存目录

**问题**：Linux 用 `HOME`，Windows 用 `USERPROFILE`。

**方案**：

- `HPM_HOME` 优先
- Windows：`USERPROFILE` → `%LOCALAPPDATA%\hpm`
- macOS：`HOME` → `~/Library/Application Support/hpm`
- Linux：`HOME` → `~/.hpm`

**默认路径**：

| 平台 | 默认缓存目录 |
|------|-------------|
| Windows | `%LOCALAPPDATA%\hpm` |
| macOS | `~/Library/Application Support/hpm` |
| Linux | `~/.hpm` |

### 3.4 工具链管理

**问题**：`ToolchainManager::platform()` 返回 `linux-x64`，Windows 需要 `win32-x64`。工具链下载源需要提供 Windows 版。

**方案**：

| 平台标识 | 工具链 |
|---------|--------|
| `win32-x64` | `arm-none-eabi-gcc` Windows 版（ARM 官方提供） |
| `win32-x64` | `sdcc` Windows 版（SDCC 官网提供 `.zip`） |
| `win32-x64` | `make`（`mingw32-make.exe`，或从 MSYS2 拿单文件） |
| `win32-x64` | `tar.exe`（Win10 自带，不需要下载） |
| `win32-x64` | `openssl.exe`（vcpkg 提供，或独立分发） |

**解压**：Windows 版工具链是 `.zip`，不是 `.tar.gz`。`extractTarball` 要支持 `.zip`，用 `tar.exe -xf`（Win10 的 `tar.exe` 支持 zip），或 `Expand-Archive`（PowerShell），或 libarchive。

**工具链描述**：`toolchain.json` 的 `urls` 加 `win32-x64` 字段，`sha256` 也加。

**PATH**：工具链安装到 `%LOCALAPPDATA%\hpm\toolchains\<name>-<version>-win32-x64\`，`bin` 子目录加进子进程的 `PATH` 环境变量（不修改系统 PATH）。

### 3.5 Makefile 生成

**问题**：生成的 Makefile 用 `/` 路径、`rm -rf`、`mkdir -p` 等 Unix 命令。

**方案**：

- 路径用 `/`（GNU Make 在 Windows 上也认 `/`），不改成 `\`
- `mkdir -p` 换成 `if not exist ... mkdir ...`，或者用 `$(shell)` 判断
- `rm -rf` 换成 `rmdir /s /q`


### 3.6 构建系统（HPM 自身）

**问题**：HPM 用 CMake，依赖 libcurl、OpenSSL、fmt、nlohmann/json、CLI11、Catch2。

**方案**：

- **vcpkg**：`vcpkg install curl openssl fmt nlohmann-json cli11 catch2`
- CMake 用 `CMAKE_TOOLCHAIN_FILE` 指向 vcpkg
- 或者：`FetchContent` 拉源码，Windows 上编译（libcurl / OpenSSL 编译复杂，不推荐）
- **推荐 vcpkg**，`vcpkg.json` 声明依赖：

```json
{
  "name": "hpm",
  "version": "0.1.0",
  "dependencies": [
    "curl",
    "openssl",
    "fmt",
    "nlohmann-json",
    "cli11",
    "catch2"
  ]
}
```

**CMakePresets.json** 提供 Windows 预设：

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "windows-msvc",
      "generator": "Visual Studio 17 2022",
      "binaryDir": "${sourceDir}/build",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    }
  ]
}
```

### 3.7 libcurl 在 Windows 上的注意事项

**问题**：

- libcurl 需要初始化 `curl_global_init(CURL_GLOBAL_DEFAULT)`，Windows 上要加 `CURL_GLOBAL_WIN32`（初始化 Winsock）
- TLS 后端：vcpkg 默认用 Schannel（Windows 原生），不是 OpenSSL
- 代理：Windows 用系统代理（IE 设置），libcurl 要 `CURLOPT_PROXY` 或读注册表

**方案**：

- `curl_global_init(CURL_GLOBAL_DEFAULT | CURL_GLOBAL_WIN32)`
- TLS 后端用 Schannel（vcpkg 默认），不强制 OpenSSL
- 代理：`curl_easy_setopt(curl, CURLOPT_PROXY, "")`，libcurl 自动读系统代理

### 3.8 OpenSSL 在 Windows 上的注意事项

**问题**：

- vcpkg 安装的 OpenSSL 是静态库或动态库，要 `PATH` 里有 `libcrypto-3-x64.dll`
- `openssl.exe` 命令要能找到

**方案**：

- 静态链接 OpenSSL（vcpkg 的 `openssl:x64-windows-static`）
- 或者：动态链接，`hpm.exe` 旁边放 `libcrypto-3-x64.dll`、`libssl-3-x64.dll`
- `openssl.exe` 命令：vcpkg 安装的 `openssl.exe` 在 `vcpkg/installed/x64-windows/tools/openssl/`，HPM 要能找到，或者要求用户装 OpenSSL 到 PATH

**建议**：**签名验证用 OpenSSL 库**（`EVP_*`），不调 `openssl.exe` 命令。这样不依赖外部 `openssl.exe`。

**但 `hpm-dev publish` 现在调 `openssl dgst` / `openssl pkeyutl` 命令**，要改成调 OpenSSL 库的 `EVP_DigestSign` / `EVP_DigestVerify`，不依赖命令行。

### 3.9 控制台编码

**问题**：Windows 控制台默认 GBK，HPM 输出 UTF-8（中文），会乱码。

**方案**：

- 启动时 `SetConsoleOutputCP(CP_UTF8)`（Windows 10 1803+ 支持 UTF-8 控制台）
- 或者：所有输出用 `std::wcout`，但跨平台麻烦
- 或者：检测控制台编码，转换输出

**推荐**：`SetConsoleOutputCP(CP_UTF8)` + `SetConsoleCP(CP_UTF8)`，Windows 10 1803+ 默认支持。

**进度条**：Windows 控制台不支持 ANSI 转义序列（除非 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`），要 `SetConsoleMode` 开启。

### 3.10 长路径

**问题**：Windows 默认路径限制 260 字符，`std::filesystem` 在 MSVC 上部分支持长路径，需要 `\\?\` 前缀。

**方案**：

- 所有 `std::filesystem::path` 操作前，用 `std::filesystem::absolute()` 转绝对路径
- 绝对路径加 `\\?\` 前缀（`\\?\C:\...` 或 `\\?\UNC\server\share\...`）
- 或者：用户在注册表开启 `LongPathsEnabled`，HPM 检测并提示

**建议**：HPM 内部用 `\\?\` 前缀包装，不依赖注册表。

### 3.11 原子写文件

**问题**：`util::writeFileAtomic` 用 `std::filesystem::rename`，Windows 上 `rename` 不能覆盖已存在文件。

**方案**：

- Windows：用 `MoveFileExW` + `MOVEFILE_REPLACE_EXISTING`
- 或者：先 `remove` 目标文件，再 `rename`
- 或者：用 `ReplaceFileW`（更安全，但要求目标存在）

**推荐**：`MoveFileExW` + `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`。

### 3.12 文件权限

**问题**：Linux 的 `chmod +x` 在 Windows 上无意义。`.key` 文件的权限（600）在 Windows 上用 ACL，不是 `chmod`。

**方案**：

- Windows 上不检查文件权限，只警告「私钥文件应放在安全位置」
- 或者：用 `icacls` 设置 ACL（复杂，不推荐）

### 3.13 符号链接

**问题**：`std::filesystem::create_symlink` 在 Windows 上要求管理员权限或开发者模式。

**方案**：HPM 不用符号链接，全用拷贝。

### 3.14 换行符

**问题**：生成的 `Makefile` / `driver.h` 用 `\n`，Windows 编辑器可能显示异常。

**方案**：

- 生成的文件用 `\n`（LF），不转换
- 或者：按平台用 `\r\n`（CRLF）
- **推荐**：统一用 `\n`，Windows 编辑器（VS Code、Notepad++）都能处理

### 3.15 测试

**问题**：Catch2 测试在 Windows 上跑，路径、临时目录、子进程调用都要测。

**方案**：

- 单元测试：跟 Linux 一样，`ctest` 跑
- 集成测试：`hpm sync` + `hpm build` 在 Windows 上跑，用 mock 包
- CI：GitHub Actions `windows-latest`，vcpkg 缓存

---

## 四、分阶段实施

### 阶段 1：HPM 自身能编译（1-2 周）

| 任务 | 说明 |
|------|------|
| vcpkg 集成 | `vcpkg.json` + `CMakePresets.json` |
| CMake 修 | `find_package(CURL)` / `find_package(OpenSSL)` 在 Windows 上能用 |
| `util::fs_utils` | 原子写用 `MoveFileExW` |
| `util::process` | `CreateProcessW` 实现 |
| 控制台 UTF-8 | `SetConsoleOutputCP` |
| 环境变量 | `USERPROFILE` |
| 编译通过 | `hpm.exe`、`hpm-dev.exe` 能跑 `--version` |

### 阶段 2：基础功能能用（2-3 周）

| 任务 | 说明 |
|------|------|
| `hpm-dev init` | 生成包骨架，路径正确 |
| `hpm-dev validate` | 读包，校验 |
| `hpm-dev publish` | 签名（用 OpenSSL 库，不调 `openssl.exe`） |
| `hpm sync --offline` | 离线模式，用本地包 |
| `hpm build` | 生成 Makefile 或 `build.bat`，调工具链 |
| 工具链管理 | 下载 Windows 版工具链，解压 `.zip` |
| 长路径 | `\\?\` 前缀 |

### 阶段 3：在线功能能用（1-2 周）

| 任务 | 说明 |
|------|------|
| libcurl 初始化 | `CURL_GLOBAL_WIN32` |
| 系统代理 | libcurl 自动读 |
| 官方公钥 | 下载、指纹提示 |
| 索引拉取 | 多镜像、`{baseurl}` |
| 签名验证 | OpenSSL 库 |
| `hpm sync` 完整流程 | 在线下载 + 验证 |

### 阶段 4：CH552 / STM32 真机测试（1 周）

| 任务 | 说明 |
|------|------|
| SDCC 工具链 | Windows 版 SDCC 下载、解压、PATH |
| `wchisp` | Windows 版（Rust 编译，或预编译） |
| `st-flash` | Windows 版（stlink-tools） |
| CH552 烧录 | 真机验证 |
| STM32 烧录 | 真机验证 |

### 阶段 5：发布（1 周）

| 任务 | 说明 |
|------|------|
| 单元测试 | `ctest` 在 Windows 上跑 |
| 集成测试 | `hpm sync` + `hpm build` |
| 二进制发布 | `hpm-windows-x64.zip`，含 `hpm.exe`、`hpm-dev.exe`、DLL |

---
——Xue Zicheng 2026-10-26