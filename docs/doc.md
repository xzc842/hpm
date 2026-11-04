# HPM 开发文档

**版本**：v0.1.0
**最后更新**：2026-10-29

---

## 目录

- [1. 项目概览](#1-项目概览)
- [2. 目录结构](#2-目录结构)
- [3. core 模块](#3-core-模块)
  - [3.1 result / types / config](#31-result--types--config)
  - [3.2 json](#32-json)
  - [3.3 model](#33-model)
  - [3.4 manifest](#34-manifest)
  - [3.5 capability](#35-capability)
  - [3.6 resolver](#36-resolver)
  - [3.7 generator](#37-generator)
  - [3.8 registry](#38-registry)
  - [3.9 util](#39-util)
- [4. cli 模块](#4-cli-模块)
- [5. dev 模块（hpm-dev）](#5-dev-模块hpm-dev)
- [6. tests 模块](#6-tests-模块)
- [7. 数据流](#7-数据流)
- [8. 关键设计](#8-关键设计)

---

## 1. 项目概览

**HPM（Hardware Package Manager）** 是一个声明式嵌入式开发工具链。

**核心理念**：驱动是模板，硬件是参数，工具是渲染器。

**三个工具**：

| 工具 | 受众 | 职责 |
|------|------|------|
| `hpm` | 应用开发者 | 写固件：配置、拉驱动、编译、烧录 |
| `hpm-dev` | 驱动作者 | 开发驱动包：init / validate / publish |
| `hpm-gui` | 所有人 | 图形化生成 `project.json`（未开始） |

**三个工具共享 `core/` 库**。

---

## 2. 目录结构

```
hpm/
├── CMakeLists.txt              顶层构建
├── cmake/
│   └── HpmOptions.cmake        编译选项
├── third_party/                第三方库（FetchContent 拉取）
├── core/                       共享核心库（hpm_core）
│   ├── include/hpm/            公共头
│   └── src/                    实现
├── cli/                        hpm 命令行
├── dev/                        hpm-dev 命令行
├── debug/                      mcus 模拟器 + debug 宿主运行时（debug/host）
├── gui/                        hpm-gui（未开始）
├── tests/                      测试
└── docs/                       文档
```

---

## 3. core 模块

### 3.1 result / types / config

#### `core/include/hpm/result.h`

**作用**：统一错误处理和返回值类型。

| 类型 / 函数 | 作用 |
|------------|------|
| `enum class ErrorCode` | 错误码枚举（`Ok`、`NotFound`、`PinConflict`、`NetworkError`、`BuildFailed` 等） |
| `class Error` | 错误对象，含 `code()` 和 `message()` |
| `template <typename T> class Result` | 要么是 `T`，要么是 `Error`。`ok()`、`value()`、`error()`、`operator*`、`operator->` |
| `using Status = Result<void>` | 无返回值的 Result |

#### `core/include/hpm/types.h`

**作用**：基础类型别名。

| 别名 | 实际类型 |
|------|---------|
| `DeviceName` | `std::string` |
| `DriverName` | `std::string` |
| `PinName` | `std::string` |
| `ResourceKey` | `std::string` |
| `ResourceType` | `std::string` |
| `MethodName` | `std::string` |

#### `core/include/hpm/config.h`

**作用**：全局配置（硬编码集中）。

| 宏 | 作用 |
|----|------|
| `HPM_OFFICIAL_PUB_URL` | 官方公钥默认地址 |
| `HPM_DEFAULT_REGISTRY_URL` | 默认索引地址 |
| `HPM_ENV_REGISTRY` | 环境变量名 `HPM_REGISTRY` |
| `HPM_ENV_OFFICIAL_PUB` | 环境变量名 `HPM_OFFICIAL_PUB` |
| `HPM_ENV_HOME` | 环境变量名 `HPM_HOME` |
| `HPM_ENV_SKIP_SIGNATURE` | 环境变量名 `HPM_SKIP_SIGNATURE` |
| `HPM_CACHE_DIR_NAME` | `.hpm` |
| `HPM_PACKAGES_DIR_NAME` | `packages` |
| `HPM_TARBALLS_DIR_NAME` | `cache/tarballs` |
| `HPM_FILE_PROJECT` | `project.json` |
| `HPM_FILE_DRIVER_STATE` | `driver.json` |
| `HPM_FILE_OFFICIAL_PUB` | `official.pub` |
| `HPM_FILE_OFFICIAL_FP` | `official.pub.fingerprint` |
| `HPM_FILE_MANIFEST` | `manifest.json` |
| `HPM_FILE_CHIP` | `chip.json` |
| `HPM_FILE_DRIVER_H` | `driver.h` |
| `HPM_FILE_DRIVER_C` | `driver.c` |
| `HPM_FILE_HPM_H` | `hpm.h` |
| `HPM_FILE_MAKEFILE` | `Makefile` |
| `HPM_FILE_LICENSE` | `LICENSE` |
| `HPM_DIR_DRIVER` | `driver` |
| `HPM_DIR_BUILD` | `build` |
| `HPM_DIR_SRC` | `src` |
| `HPM_DIR_INCLUDE` | `include` |
| `HPM_DIR_LD` | `ld` |
| `HPM_VERSION_STR` | `0.1.0` |
| `HPM_HTTP_TIMEOUT` | `30L` |
| `HPM_HTTP_DOWNLOAD_TIMEOUT` | `600L` |
| `HPM_USER_AGENT` | `hpm/0.1.0` |
| `HPM_PROGRESS_BAR_WIDTH` | `30` |

#### `core/include/hpm/hpm.h`

**作用**：公共 API 总入口。

| 函数 | 作用 |
|------|------|
| `const char* version()` | 返回 `HPM_VERSION_STR` |

---

### 3.2 json

#### `core/include/hpm/json/json_value.h`

**作用**：JSON 值的值语义封装（不抛异常）。

| 类型 / 方法 | 作用 |
|------------|------|
| `enum class Type` | `Null`、`Bool`、`Int`、`Double`、`String`、`Array`、`Object` |
| `class Value` | JSON 值。值语义，拷贝深拷贝 |
| `Value()` | 构造 null |
| `Value(bool/int32_t/int64_t/uint32_t/uint64_t/double/const char*/std::string)` | 构造各类型 |
| `static Value array()` | 构造空数组 |
| `static Value object()` | 构造空对象 |
| `type()` / `isNull()` / `isBool()` / … | 类型查询 |
| `asBool()` / `asInt()` / `asDouble()` / `asString()` | 标量访问，类型不符返回 `Error` |
| `size()` / `at(index)` / `push(v)` | 数组访问 |
| `has(key)` / `get(key)` / `set(key, v)` / `keys()` | 对象访问 |
| `getBool/getInt/getDouble/getString(key, default)` | 带默认值的便捷访问 |
| `impl()` | 内部 `Impl` 访问（供 `json_io` 用） |

#### `core/src/json/json_value.cpp`

**作用**：`Value` 的实现。

**关键点**：
- 内部用 `std::unique_ptr<Impl>`，`Impl` 定义在 `json_impl.h`（内部头）
- `Impl` 持有 `nlohmann::json data`
- 值语义：拷贝时深拷贝，移动时转移

#### `core/src/json/json_impl.h`

**作用**：`Value::Impl` 的定义，只给 `json_*.cpp` 用。

| 类型 | 作用 |
|------|------|
| `struct Value::Impl` | 持有 `nlohmann::json data` |

#### `core/include/hpm/json/json_io.h`

**作用**：JSON 解析、序列化、文件读写。

| 函数 | 作用 |
|------|------|
| `Result<Value> parse(const std::string& text)` | 从字符串解析 |
| `Result<Value> parseFile(const std::filesystem::path& path)` | 从文件解析 |
| `std::string dump(const Value& v, int indent = 2)` | 序列化 |
| `Result<void> writeFile(const Value& v, const path& p, int indent = 2)` | 原子写文件 |

#### `core/src/json/json_io.cpp`

**作用**：上述函数的实现。原子写：先写 `.tmp`，再 rename。

---

### 3.3 model

#### `core/include/hpm/model/version.h`

**作用**：semver 版本。

| 类型 / 方法 | 作用 |
|------------|------|
| `class Version` | 版本号（major.minor.patch[-prerelease][+build]） |
| `static Result<Version> parse(const std::string&)` | 解析 |
| `major()` / `minor()` / `patch()` / `prerelease()` / `build()` | 访问字段 |
| `isPrerelease()` | 是否预发布 |
| `toString()` | 序列化 |
| `operator<=>` / `operator==` | 比较（预发布 < 正式版，build 不参与比较） |

#### `core/src/model/version.cpp`

**作用**：`Version` 实现。

**内部函数**：
- `parseNumber(s)`：解析点分数字
- `split(s, delim)`：切分
- `comparePrerelease(a, b)`：预发布标识符比较

#### `core/include/hpm/model/version_range.h`

**作用**：版本约束。

| 类型 / 方法 | 作用 |
|------------|------|
| `class VersionRange` | 版本约束（支持 `^`、`~`、`>=`、`<`、`*`、范围） |
| `static Result<VersionRange> parse(const std::string&)` | 解析约束表达式 |
| `matches(const Version&)` | 版本是否满足约束 |
| `toString()` | 原始表达式 |

#### `core/src/model/version_range.cpp`

**作用**：`VersionRange` 实现。

**内部**：
- `trim(s)`：去空白
- 解析 token：`>=`、`<=`、`>`、`<`、`=`、`^`、`~`、精确
- `^1.2.0` → `>=1.2.0 <2.0.0`
- `~1.2.0` → `>=1.2.0 <1.3.0`

#### `core/include/hpm/model/pin_capability.h`

**作用**：引脚能力。

| 类型 | 作用 |
|------|------|
| `struct PinCapability` | 引脚能力：`type`、`instance`、`channel` |

#### `core/include/hpm/model/pin.h`

**作用**：引脚。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct Pin` | 引脚：`name`、`capabilities`、`shareable` |
| `hasCapability(type, instance)` | 引脚是否有某种能力 |

#### `core/include/hpm/model/requirement.h`

**作用**：驱动的一条需求。

| 类型 | 作用 |
|------|------|
| `struct Requirement` | `key`、`type`、`methods`、`optional` |

#### `core/include/hpm/model/provided_method.h`

**作用**：驱动提供的方法。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct MethodParam` | `type`、`name` |
| `struct ProvidedMethod` | `name`、`returnType`、`params` |
| `paramList()` | 生成 `(type name, ...)` 字符串 |
| `declaration(prefix)` | 生成 `returnType prefix name (params);` |

#### `core/include/hpm/model/driver_manifest.h`

**作用**：驱动清单。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct ConfigField` | `type`（`int`/`string`）、`defaultValue`、`length` |
| `struct DriverManifest` | `name`、`version`、`description`、`license`、`toolchain`、`config`、`requirements`、`provides` |
| `providesMethod(name)` | 是否提供某个方法 |
| `findRequirement(key)` | 找某个需求 |

#### `core/include/hpm/model/device.h`

**作用**：设备实例。

| 类型 | 作用 |
|------|------|
| `struct Device` | `name`、`driver`、`version`、`bindings`、`config`、`userDeclared` |

#### `core/include/hpm/model/intermediate_device.h`

**作用**：中间虚拟设备。

| 类型 | 作用 |
|------|------|
| `struct IntermediateDevice` | `id`、`name`、`driver`、`version`、`bindings`、`config` |

#### `core/include/hpm/model/project.h`

**作用**：项目。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct Project` | `mcu`、`devices` |
| `findDevice(name)` | 按名字找设备（const 和非 const 两个版本） |

#### `core/include/hpm/model/driver_state.h`

**作用**：driver.json 的内存表示。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct DriverState` | `userDevices`、`intermediateDevices` |
| `findUserDevice(name)` | 找用户设备 |
| `findIntermediate(id)` | 找中间设备（按 id 或 name） |

#### `core/include/hpm/model/toolchain_spec.h`

**作用**：工具链描述。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct ToolchainSpec` | `name`、`prefix`、`cc`、`objcopy`、`size`、`version`、`url` |
| `resolveCc()` | 解析 `cc`（优先 `cc`，其次 `prefix + gcc`，最后 `name`） |
| `resolveObjcopy()` | 同上 |
| `resolveSize()` | 同上 |

#### `core/include/hpm/model/flash_config.h`

**作用**：烧录配置。

| 类型 | 作用 |
|------|------|
| `struct FlashConfig` | `tool`、`args`、`baseAddress` |

#### `core/include/hpm/model/chip.h`

**作用**：芯片模型。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct ChipBuildConfig` | `cpu`、`flags`、`ldscript`、`startup` |
| `struct Chip` | `name`、`family`、`package`、`license`、`qemuMachine`、`pins`、`toolchain`、`flash`、`build`、`provides` |
| `findPin(name)` | 找引脚 |
| `providesMethod(name)` | 是否提供某个方法 |

---

### 3.4 manifest

#### `core/include/hpm/manifest/field_reader.h`

**作用**：字段读取辅助，统一错误消息。

| 类型 / 方法 | 作用 |
|------------|------|
| `class FieldReader` | 封装 `json::Value`，按 key 读取 |
| `requireString(key)` / `requireInt` / `requireBool` / `requireObject` / `requireArray` | 必填字段，缺失或类型错返回 `Error` |
| `optString(key, def)` / `optInt` / `optBool` | 可选字段，缺失返回默认值 |
| `optObject(key)` / `optArray(key)` | 可选对象/数组 |
| `has(key)` | 存在性 |
| `keys()` | 所有 key |
| `raw()` | 底层 `json::Value` |

#### `core/src/manifest/field_reader.cpp`

**作用**：`FieldReader` 实现。错误消息带路径（`devices.door_reader.driver`）。

#### `core/include/hpm/manifest/project_loader.h`

| 函数 | 作用 |
|------|------|
| `Result<Project> loadProject(const json::Value&)` | 从 JSON 加载 |
| `Result<Project> loadProjectFile(const path&)` | 从文件加载 |

#### `core/src/manifest/project_loader.cpp`

**作用**：解析 `project.json`。

**内部**：
- `loadDevice(name, obj)`：解析单个设备（`driver`、`version`、`config`、`bindings`）

#### `core/include/hpm/manifest/driver_loader.h`

| 函数 | 作用 |
|------|------|
| `Result<DriverManifest> loadDriverManifest(const json::Value&)` | 从 JSON 加载 |
| `Result<DriverManifest> loadDriverManifestFile(const path&)` | 从文件加载 |

#### `core/src/manifest/driver_loader.cpp`

**作用**：解析驱动清单。

**内部**：
- `loadRequirement(key, obj)`：解析一条需求
- `loadProvidedMethod(v, ctx)`：解析一个方法（支持字符串和对象）

#### `core/include/hpm/manifest/state_loader.h`

| 函数 | 作用 |
|------|------|
| `Result<DriverState> loadDriverState(const json::Value&)` | 从 JSON 加载 |
| `Result<DriverState> loadDriverStateFile(const path&)` | 从文件加载 |
| `json::Value saveDriverState(const DriverState&)` | 序列化 |

#### `core/src/manifest/state_loader.cpp`

**作用**：`driver.json` 读写。

**内部**：
- `loadDeviceEntry(name, obj, userDeclared)`：解析设备条目（含 `config`）
- `saveDevice(d)`：序列化设备

#### `core/include/hpm/manifest/chip_loader.h`

| 函数 | 作用 |
|------|------|
| `Result<Chip> loadChip(const json::Value&)` | 从 JSON 加载 |
| `Result<Chip> loadChipFile(const path&)` | 从文件加载 |

#### `core/src/manifest/chip_loader.cpp`

**作用**：解析 `chip.json`。

**内部**：
- `loadCapability(obj, ctx)`：解析引脚能力
- `loadPin(name, obj)`：解析引脚
- `loadToolchain(obj)`：解析工具链（`name`/`prefix`/`cc`/`objcopy`/`size`）
- `loadFlash(obj)`：解析烧录配置
- `loadBuild(obj)`：解析构建配置
- `loadProvidedMethod(v, ctx)`：解析芯片提供的方法（同 `driver_loader`）

---

### 3.5 capability

#### `core/include/hpm/capability/check_report.h`

**作用**：检查报告。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct CheckIssue` | `code`、`message`、`location` |
| `class CheckReport` | `errors`、`warnings` |
| `addError(code, msg, loc)` | 加错误 |
| `addWarning(msg, loc)` | 加警告 |
| `ok()` | 无错误 |
| `errors()` / `warnings()` | 访问列表 |
| `toError()` | 汇总成一条 `Error` |

#### `core/include/hpm/capability/driver_library.h`

**作用**：驱动清单集合。

| 方法 | 作用 |
|------|------|
| `add(manifest)` | 添加驱动清单 |
| `find(name)` | 按名字找 |
| `has(name)` | 是否存在 |
| `size()` | 数量 |
| `all()` | 所有清单 |

#### `core/include/hpm/capability/pin_allocator.h`

**作用**：引脚分配表。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct PinUse` | `device`、`key`、`shareable` |
| `class PinAllocator` | 引脚占用记录 |
| `allocate(pin, device, key, report)` | 申请引脚，冲突报错 |
| `users(pin)` | 某引脚被谁用了 |

#### `core/src/capability/pin_allocator.cpp`

**作用**：`PinAllocator` 实现。

#### `core/include/hpm/capability/pin_matcher.h`

**作用**：引脚能力匹配。

| 类型 / 方法 | 作用 |
|------------|------|
| `class PinMatcher` | 引脚能力检查 |
| `check(pin, req, device, report)` | 引脚是否满足需求 |

#### `core/src/capability/pin_matcher.cpp`

**作用**：`PinMatcher` 实现。

**内部**：
- `typeMatches(pin, reqType)`：`spi` 匹配 `spi_sck/miso/mosi` 任一

#### `core/include/hpm/capability/method_resolver.h`

**作用**：方法能力匹配（现在被 `capability_checker` 内联，保留头文件）。

#### `core/include/hpm/capability/capability_checker.h`

**作用**：能力检查总调度。

| 方法 | 作用 |
|------|------|
| `check(const DriverState&)` | 检查整个状态（主入口） |
| `check(const Project&)` | 兼容重载（把 `Project.devices` 当 `userDevices`） |
| `checkDevice(state, device, allocator, report)` | 检查一个用户设备 |
| `checkIntermediate(state, device, allocator, report)` | 检查一个中间设备 |
| `checkNames(state, report)` | 名字唯一性 |

#### `core/src/capability/capability_checker.cpp`

**作用**：`CapabilityChecker` 实现。

**检查顺序**：
1. 名字唯一性（user + intermediate 全局）
2. 每个用户设备的 requires
3. 每个中间设备的 requires

**`bindings` 值判定**：
- 是用户设备 → 检查方法能力
- 是中间设备 → 检查方法能力
- 否则当引脚 → 检查能力 + 登记占用

---

### 3.6 resolver

#### `core/include/hpm/resolver/id_generator.h`

| 函数 | 作用 |
|------|------|
| `std::string generateIntermediateId(prefix)` | 生成 `__spi_a3f2` 形式随机 ID |

#### `core/src/resolver/id_generator.cpp`

**作用**：`generateIntermediateId` 实现。`__<prefix>_<4 位十六进制>`。

#### `core/include/hpm/resolver/resolved_node.h`

**作用**：解析树的节点。

| 类型 | 作用 |
|------|------|
| `struct ResolvedNode` | `driver`、`version`、`instanceName`、`pins`、`config`、`children`、`isTopLevel` |

#### `core/include/hpm/resolver/resolver.h`

**作用**：依赖解析（两阶段）。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct IntermediateKey` | 中间设备索引键：`driver` + `pins` |
| `class Resolver` | 解析器 |
| `resolve()` | 主入口，返回 `DriverState` |
| `expandUserDevice(dev)` | 阶段 1：展开一个用户设备 |
| `expandRequirements(dm, topDevice, topName, visited, consumed, node)` | 阶段 1：递归展开 requirements |
| `expandChild(childDm, topDevice, topName, parentKey, visited, consumed)` | 阶段 1：展开子驱动 |
| `buildState(root, out)` | 阶段 2：树 → `DriverState` |
| `buildIntermediate(node, out)` | 阶段 2：递归写中间设备 |
| `findIntermediateName(driver, pins)` | 从 `created_` 缓存查找 |
| `rememberIntermediate(driver, pins, name)` | 记录到 `created_` |
| `findExistingIntermediate(driver, pins)` | 从 `existing_`（driver.json）查找 |
| `lockVersion(driver)` | 版本锁定（优先 `existing_`） |

#### `core/src/resolver/resolver.cpp`

**作用**：`Resolver` 实现。

**两阶段**：
- **阶段 1**：递归展开成 `ResolvedNode` 树
- **阶段 2**：把树转成 `DriverState`

**中间设备复用**：按 `(driver, pins)` 查 `created_`（本次解析）+ `existing_`（driver.json）

**循环检测**：`visited` 集合

**同名 key 冲突**：`consumed` 集合（每设备独立）

---

### 3.7 generator

#### `core/include/hpm/generator/generation_plan.h`

**作用**：待写文件列表。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct PlannedFile` | `relativePath`、`content`、`overwrite` |
| `struct GenerationPlan` | `files` |
| `addFile(rel, content)` | 添加文件 |

#### `core/include/hpm/generator/template_engine.h`

| 函数 | 作用 |
|------|------|
| `std::string renderTemplate(tmpl, vars)` | `{{var}}` 替换 |

#### `core/src/generator/template_engine.cpp`

**作用**：`renderTemplate` 实现。未找到的 key 保留 `{{key}}`。

#### `core/include/hpm/generator/hpm_h_generator.h`

**作用**：生成 `hpm.h`（底层 API）。

| 类型 / 方法 | 作用 |
|------------|------|
| `class HpmHGenerator` | 生成器 |
| `generate()` | 生成 `hpm.h` 内容 |
| `generatePinApi(out)` | 生成引脚函数声明 |
| `generateChipApi(out)` | 生成芯片初始化声明 |
| `generateUserDevices(out)` | 生成用户设备方法声明 |
| `generateIntermediateDevices(out)` | 生成中间设备方法声明 |

#### `core/src/generator/hpm_h_generator.cpp`

**作用**：`HpmHGenerator` 实现。

#### `core/include/hpm/generator/generator.h`

**作用**：代码生成器。

| 类型 / 方法 | 作用 |
|------------|------|
| `class Generator` | 生成器 |
| `plan()` | 构建生成计划 |
| `write(plan)` | 写文件到 `driverDir_` |
| `generate()` | `plan()` + `write()` |
| `planDriverHeader(plan)` | 生成 `driver.h` |
| `planDriverSource(plan)` | 生成 `driver.c` |
| `planMakefile(plan)` | 生成 `Makefile`（根据工具链选 SDCC / GCC） |
| `planMakefileSdcc(plan)` | 生成 SDCC 风格 Makefile |
| `planMakefileGcc(plan)` | 生成 GCC 风格 Makefile |
| `collectSources(plan)` | 收集所有 `.c` |
| `collectIncludes()` | 收集所有 `-I` 路径 |
| `planInstance(plan, instanceName, driverName, version, bindings, config)` | 生成一个实例目录 |
| `planChipDriver(plan)` | 拷贝芯片驱动 |
| `copyPackageSources(plan, driver, version, targetDir)` | 拷贝驱动包源码 |
| `macroValue(bindingValue)` | 计算宏值（实例名或 `hpm_pin_<pin>`） |
| `findChipPackageDir()` | 查找芯片包目录 |
| `collectInstances()` | 收集所有实例（user + intermediate） |
| `struct InstanceInfo` | `instanceName`、`driver`、`version`、`bindings`、`config` |

#### `core/src/generator/generator.cpp`

**作用**：`Generator` 实现。

**生成结构**：
```
driver/
├── driver.h
├── driver.c
├── Makefile
├── <instance>/
│   ├── hpm_dev.h
│   ├── config.h
│   ├── <driver>.c
│   └── <driver>.h
└── <chip>/
    ├── <chip>.c
    ├── hpm_pin.c
    ├── startup.c
    └── <chip>.ld
```

**`config.h` 生成**：
- 普通配置：`#define key value`（数字原样，字符串加引号）
- 字符串配置：补空格到 `length`，超出截断
- 字符串描述符数组：在 `#ifdef HPM_GEN_STRINGS` 里生成 `__code uint16_t <name>[]`

**工具链兼容性检查**：`dm->toolchain` 和 `chip_.toolchain.resolveCc()` 匹配

---

### 3.8 registry

#### `core/include/hpm/registry/index.h`

**作用**：索引数据结构。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct DeveloperInfo` | `pub`、`pubSig` |
| `struct PackageVersion` | `kind`、`developer`、`url`、`sigUrl`、`sha256`、`size`、`license`、`requirements` |
| `struct PackageInfo` | `versions` |
| `struct Index` | `schema`、`officialPub`、`baseUrl`、`developers`、`packages` |
| `find(name, version)` | 找某个版本 |
| `findLatest(name)` | 找最新版本 |
| `findLatestVersion(name)` | 找最新版本号 |
| `parseIndex(jsonText, baseUrl)` | 解析，替换 `{baseurl}` |

#### `core/src/registry/index.cpp`

**作用**：`Index` 实现。

**内部**：
- `replaceBaseUrl(url, baseUrl)`：`{baseurl}` → `baseUrl`

#### `core/include/hpm/registry/signature.h`

**作用**：OpenSSL 签名验证。

| 函数 | 作用 |
|------|------|
| `Result<bool> verifySignature(pubKeyPem, sigFile, dataFile)` | 验证签名 |
| `bool isValidPublicKey(pubKeyPem)` | 公钥格式检查 |
| `std::string sha256File(file)` | SHA256 |
| `std::string publicKeyFingerprint(pubKeyPem)` | 指纹（`XX:XX:XX:...`） |

#### `core/src/registry/signature.cpp`

**作用**：上述函数实现。

#### `core/include/hpm/registry/official_key.h`

**作用**：官方公钥管理。

| 类型 / 方法 | 作用 |
|------------|------|
| `class OfficialKeyManager` | 官方公钥管理 |
| `ensure()` | 下载 + 验证 + 指纹确认，返回公钥路径 |
| `static officialUrl()` | 官方公钥地址（`HPM_OFFICIAL_PUB` 或硬编码） |
| `localPath()` | 本地公钥路径 |
| `fingerprintPath()` | 指纹路径 |
| `download(dest)` | 下载 |
| `promptFingerprint(fp, changed)` | 提示用户确认指纹 |

#### `core/src/registry/official_key.cpp`

**作用**：`OfficialKeyManager` 实现。

#### `core/include/hpm/registry/registry_client.h`

**作用**：Registry 客户端。

| 类型 / 方法 | 作用 |
|------------|------|
| `struct RegistryConfig` | `registries`、`cacheDir`、`offline`、`skipSignature` |
| `static RegistryConfig::load()` | 从环境变量和 `~/.hpm/config.json` 加载 |
| `using DownloadProgress = std::function<void(uint64_t, uint64_t)>` | 进度回调 |
| `class RegistryClient` | 客户端 |
| `fetchIndex(progress)` | 拉索引（多镜像 fallback + 签名验证 + `{baseurl}` 替换） |
| `ensurePackage(name, version, progress)` | 确保包在本地（缓存或下载 + 签名验证 + SHA256） |
| `packagesDir()` | `~/.hpm/packages` |
| `tarballsDir()` | `~/.hpm/cache/tarballs` |
| `httpGet(url, progress)` | HTTP GET |
| `httpDownload(url, dest, progress)` | HTTP 下载 |
| `fetchAndVerifyDeveloperKey(devName)` | 下载 + 验证开发者公钥 |
| `verifyPackageSignature(tarball, sigUrl, devName)` | 验证包签名 |
| `extractTarball(tarball, destDir)` | 解压 tar.gz |

#### `core/src/registry/registry_client.cpp`

**作用**：`RegistryClient` 实现。

**内部**：
- `writeToString` / `writeToFile`：libcurl 写回调
- `struct ProgressCtx`：进度上下文
- `int progressCallback(...)`：libcurl 进度回调

---

### 3.9 util

#### `core/include/hpm/util/fs_utils.h`

| 函数 | 作用 |
|------|------|
| `Status writeFileAtomic(path, content)` | 原子写文件 |
| `Result<std::string> readFile(path)` | 读整个文件 |
| `bool exists(path)` | 文件是否存在 |

#### `core/src/util/fs_utils.cpp`

**作用**：上述函数实现。原子写：先写 `.tmp`，再 rename。

#### `core/include/hpm/util/process.h`

| 函数 | 作用 |
|------|------|
| `Status runProcess(program, args, verbose)` | 调子进程，捕获输出 |

#### `core/src/util/process.cpp`

**作用**：`runProcess` 实现。用 `popen` / `pclose`，shell 转义。

#### `core/include/hpm/util/progress_bar.h`

| 类型 / 方法 | 作用 |
|------------|------|
| `class ProgressBar` | 终端进度条 |
| `update(current, total)` | 更新进度 |
| `done()` | 完成 |
| `fail()` | 失败 |

---

## 4. cli 模块

### `cli/src/main.cpp`

**作用**：入口，CLI11 解析，全局选项（`-C`、`--cache-dir`、`-v`），分发子命令。

### `cli/src/cmd_context.h/.cpp`

**作用**：命令上下文。

| 方法 | 作用 |
|------|------|
| `projectDir()` / `setProjectDir(p)` | 项目目录 |
| `cacheDir()` / `setCacheDir(p)` | 缓存目录 |
| `projectFile()` | `<project>/project.json` |
| `driverStateFile()` | `<project>/driver.json` |
| `driverDir()` | `<project>/driver` |
| `buildDir()` | `<project>/build` |
| `packagesDir()` | `<cache>/packages` |
| `verbose()` / `setVerbose(v)` | 详细输出 |
| `static defaultCacheDir()` | 默认缓存目录（`$HPM_HOME` 或 `~/.hpm`） |

### `cli/src/command.h`

**作用**：命令基类。`name()`、`description()`、`setup(app)`、`run(ctx)`。

### `cli/src/command_registry.h/.cpp`

**作用**：注册所有命令，分发。`registerAll(app)`、`dispatch(name, ctx)`、`names()`。

### `cli/src/output.h/.cpp`

**作用**：输出辅助。`info`、`success`、`warn`、`error`、`step`、`detail`、`setColor`。

### `cli/src/commands/init_cmd.h/.cpp`

**作用**：`hpm init`。生成 `project.json` 模板 + `.gitignore`。

### `cli/src/commands/sync_cmd.h/.cpp`

**作用**：`hpm sync`。核心流程：

1. 读 `project.json` → `Project`
2. 读已有 `driver.json` → `DriverState`
3. 确保依赖包在本地（在线：`RegistryClient`；离线：检查本地）
4. 加载芯片驱动 → `Chip`
5. 加载驱动库 → `DriverLibrary`
6. `Resolver.resolve()` → `DriverState`
7. `CapabilityChecker.check(state)` → 报错就停
8. 写 `driver.json`
9. `Generator.generate()` → `driver/`

**选项**：`--dry-run`、`--offline`、`--skip-signature`

### `cli/src/commands/build_cmd.h/.cpp`

**作用**：`hpm build`。调 `make -C driver`。

### `cli/src/commands/flash_cmd.h/.cpp`

**作用**：`hpm flash`。读芯片驱动 `flash` 配置，调烧录器。

### `cli/src/commands/debug_cmd.h/.cpp`

**作用**：`hpm debug`。检查 `driver/`、`build/` 和固件是否存在，然后启动 mcus 模拟器。

- `--model`：mcus 芯片模型（必填）
- `--config`：mcus 配置（必填）
- `--isa`：ISA 动态库（默认按模型 `arch` 推导为 `libmcus_isa_<arch>.so`）
- `--mcus`：模拟器路径（默认 `build/bin/mcus`）
- `--firmware`：覆盖固件（默认 `build/` 下最新的 `firmware.hex|ihx|bin`）
- `--dry-run`：只打印启动命令

mcus 源码通过 `debug/mcus` 软链接引入，由 `debug/CMakeLists.txt` 编译为
`build/bin/mcus`、`build/lib/libmcus_core.so`、`build/lib/libmcus_isa_8051.so`。

`debug/host/` 是 `hpm debug` 的宿主运行时：

- 根据 `driver.json` 找到用户设备的驱动包；
- 检查 `debug/debug.c`，编译成 `build/hpm-debug/plugins/*.so`；
- `dlopen` 插件并按设备 `create` 实例；
- 消费 mcus 统一 `DebugEventStream`（MMIO/PIN/BUS/IRQ）；
- 提供 CLI REPL 和 `--json` JSON Lines 协议；
- 通过 mcus 共享内存执行 pause/resume/step/reset/断点/引脚/中断控制。

插件 ABI 见 `core/include/hpm/debug/plugin.h`，规范见 `docs/debug_plugin.md`，
外部协议见 `docs/debug_protocol.md`。

### `cli/src/commands/clean_cmd.h/.cpp`

**作用**：`hpm clean`。删 `build/`。

### `cli/src/commands/dev_export_cmd.h/.cpp`

**作用**：`hpm dev --export`。生成 `hpm.h`。

---

## 5. dev 模块（hpm-dev）

### `dev/src/main.cpp`

**作用**：入口，CLI11 解析，子命令 `init`、`validate`、`publish`。

### `dev/src/dev_context.h/.cpp`

**作用**：上下文。`path()` / `setPath(p)`。

### `dev/src/commands/init_cmd.h/.cpp`

**作用**：`hpm-dev init`。生成驱动包骨架。

| 方法 | 作用 |
|------|------|
| `setup(app)` | 选项：`name`、`--type`、`--version`、`--license`、`--dir` |
| `run(ctx)` | 生成目录 + 文件 |
| `makeDriverManifest` | 生成 `manifest.json` |
| `makeChipJson` | 生成 `chip.json` |
| `makeDriverC` / `makeDriverH` | 生成驱动源码 |
| `makeChipC` / `makeChipH` | 生成芯片源码 |
| `makeHpmPinH` / `makeHpmPinC` | 生成引脚源码 |
| `makeLinkerScript` | 生成链接脚本 |
| `makeReadme` | 生成 README |

### `dev/src/commands/validate_cmd.h/.cpp`

**作用**：`hpm-dev validate`。校验驱动包格式。

**检查项**：
- `manifest.json` / `chip.json` 存在且格式正确
- `LICENSE` 存在
- `src/` / `include/` 存在
- `.c` 在 `src/`，`.h` 在 `include/`
- `ldscript` 指向的文件存在

### `dev/src/commands/publish_cmd.h/.cpp`

**作用**：`hpm-dev publish`。打包 + 签名。

| 方法 | 作用 |
|------|------|
| `setup(app)` | 选项：`path`、`-k,--key`、`-o,--out` |
| `run(ctx)` | 读 manifest / chip → tar.gz → 签名 → 打印 SHA256 / size |

---

## 6. tests 模块

### `tests/unit/test_version.cpp`

**作用**：semver、版本约束测试。

### `tests/unit/test_json.cpp`

**作用**：JSON 读写测试。

### `tests/unit/test_model.cpp`

**作用**：model 类型测试。

### `tests/unit/test_manifest.cpp`

**作用**：清单解析测试。

### `tests/unit/test_capability.cpp`

**作用**：能力检查测试。

### `tests/unit/test_resolver.cpp`

**作用**：依赖解析测试。

### `tests/unit/test_generator.cpp`

**作用**：代码生成测试。

### `tests/unit/test_toolchain.cpp`

**作用**：工具链测试。

---

## 7. 数据流

### `hpm sync`

```
project.json
    ↓ manifest::loadProject
Project
    ↓ RegistryClient（在线）/ 本地检查（离线）
确保包在本地
    ↓ manifest::loadChipFile
Chip
    ↓ manifest::loadDriverManifestFile（扫所有包）
DriverLibrary
    ↓ Resolver.resolve
DriverState
    ↓ CapabilityChecker.check
检查通过
    ↓ manifest::saveDriverState
driver.json
    ↓ Generator.generate
driver/
```

### `hpm build`

```
driver/Makefile
    ↓ util::runProcess("make", {"-C", driverDir})
build/firmware.elf / .hex
```

### `hpm flash`

```
project.json → Project → Chip → FlashConfig
    ↓ util::runProcess(flash.tool, flash.args)
芯片
```

### `hpm-dev init`

```
hpm-dev init <name> --type driver|chip
    ↓ 生成骨架
<name>-0.1.0/
├── manifest.json / chip.json
├── LICENSE
├── README.md
├── src/
└── include/
```

### `hpm-dev publish`

```
<name>-<version>/
    ↓ 读 manifest / chip
    ↓ tar -czf
    ↓ openssl dgst -sha256 -sign
publish/<name>-<version>.tar.gz
publish/<name>-<version>.tar.gz.sig
```

---

## 8. 关键设计

### 8.1 驱动分层

```
芯片驱动（提供引脚 + 芯片初始化）      ← 唯一碰寄存器的层
    ↓
总线驱动（spi / i2c / uart）         ← 申请引脚，提供总线接口
    ↓
设备驱动（rc522 / bmp280 / usb_cdc） ← 申请总线 + 引脚，提供应用接口
    ↓
用户应用（main.c）                    ← 只 include "driver.h"
```

### 8.2 宏体系

**`hpm_dev.h`**（每个实例一份，工具生成）：

```c
#define DEVICE_NAME  door_reader
#define bus          __spi_a3f2
#define cs           hpm_pin_PA4
```

**`config.h`**（每个实例一份，工具生成）：

```c
#define usb_vid  0x1209
#define usb_manufacturer  "HPM             "
```

**驱动源码用拼接宏**：

```c
#define HPM_CAT_(a, b) a##b
#define HPM_CAT(a, b)  HPM_CAT_(a, b)
#define HPM_FN(name)   HPM_CAT(DEVICE_NAME, HPM_CAT(_, name))
#define HPM_CALL(prefix, method)  HPM_CAT(prefix, HPM_CAT(_, method))

int HPM_FN(init)(void) { ... }
```

### 8.3 中间设备

`rc522` 要 `spi`，用户只写了 `sck`/`miso`/`mosi` 引脚。工具自动创建中间设备 `__spi_a3f2`，把三个引脚转移给它，`door_reader.bus` 指向它。

### 8.4 信任链

```
official.pub（根信任）
    ↓ 验证
index.json.sig → index.json
    ↓ 验证
<developer>.pub.sig → <developer>.pub
    ↓ 验证
<package>.tar.gz.sig → <package>.tar.gz
```

### 8.5 两阶段解析

**阶段 1**：递归展开成 `ResolvedNode` 树
**阶段 2**：把树摊平成 `DriverState`

### 8.6 SDCC / GCC Makefile

`Generator::planMakefile` 根据 `chip.toolchain.resolveCc()` 判断：

- `sdcc` → SDCC 风格（`.rel`、`.ihx`、`packihx`）
- 其他 → GCC 风格（`.o`、`.elf`、`objcopy`）

### 8.7 `{baseurl}` 占位符

`index.json` 里 URL 可以写 `{baseurl}/alice/ch552g-1.0.0.tar.gz`，`RegistryClient` 在拉取索引后，把 `{baseurl}` 替换成**当前索引 URL 的目录**。

**好处**：索引里的 URL 不写死域名，镜像可以改域名，不用改 `index.json`。

---

**文档版本**：v0.1.0
**最后更新**：2026-10-29