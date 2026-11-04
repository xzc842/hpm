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

// debug/host/src/host.cpp
#include <hpm/debug/host.h>
#include <hpm/debug/plugin.h>

#include <hpm/json/json_io.h>
#include <hpm/manifest/driver_loader.h>
#include <hpm/manifest/state_loader.h>
#include <hpm/model/driver_manifest.h>
#include <hpm/model/driver_state.h>
#include <hpm/util/fs_utils.h>
#include <hpm/util/process.h>

#include <mcus/event_stream.h>
#include <mcus/model.h>
#include <mcus/shared_memory.h>

#include <toml++/toml.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hpm::debug {

namespace {

namespace fs = std::filesystem;

std::string canonicalPin(std::string name) {
    std::replace(name.begin(), name.end(), '_', '.');
    return name;
}

std::string sanitizeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::string readCString(const char* s) { return s ? s : ""; }

std::string statusName(uint32_t status) {
    switch (status) {
        case 0: return "running";
        case 1: return "paused";
        case 2: return "dump_done";
        case 3: return "halted";
        default: return "unknown";
    }
}

std::string eventTypeName(uint32_t type) {
    switch (type) {
        case HPM_DEBUG_EVENT_PIN: return "pin";
        case HPM_DEBUG_EVENT_MMIO: return "mmio";
        case HPM_DEBUG_EVENT_BUS: return "bus";
        case HPM_DEBUG_EVENT_IRQ: return "irq";
        case HPM_DEBUG_EVENT_JSON: return "json";
        default: return "unknown";
    }
}

} // namespace

struct Host::Impl {
    explicit Impl(HostOptions o) : opts(std::move(o)) {}

    HostOptions opts;

    mcus::Model model;
    std::unique_ptr<mcus::SharedMemory> shm;
    std::unique_ptr<mcus::EventStream> events;

    std::string shmName;
    std::string streamName;
    std::string sessionConfig;

    pid_t mcusPid = -1;

    DriverState state;

    struct PluginLib {
        void* handle = nullptr;
        HpmDebugAbiVersionFn abi = nullptr;
        HpmDebugCreateFn create = nullptr;
        HpmDebugDestroyFn destroy = nullptr;
        HpmDebugEventFn event = nullptr;
    };

    struct DeviceCtx {
        Impl* host = nullptr;
        std::string name;
        std::string driver;
        std::string packageDir;
        PluginLib* lib = nullptr;
        void* ctx = nullptr;
        HpmDebugHostApi api{};
        std::map<std::string, std::string> config;
        std::map<std::string, std::string> bindings;
        std::vector<HpmDebugKv> configKv;
        std::vector<HpmDebugKv> bindingKv;
        std::vector<uint32_t> pinIds;
        std::set<std::string> pinNames;
    };

    std::vector<std::unique_ptr<DeviceCtx>> devices;
    std::map<std::string, PluginLib> libs; // driver@version -> lib
    std::map<std::string, std::string> canonicalToPin; // canonical -> model pin
    std::map<std::string, uint32_t> pinNameToId;       // canonical -> index
    std::vector<std::vector<DeviceCtx*>> pinDevices;

    std::set<uint32_t> breakpoints;
    bool bpReported = false;

    mutable std::mutex outMutex;
    std::recursive_mutex pluginMutex;
    std::atomic<bool> stopping{false};
    std::thread eventThread;
    bool ready = false;
    bool cleaned = false;

    // ---------- 输出 ----------
    void writeLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(outMutex);
        std::cout << line << "\n";
        std::cout.flush();
    }

    void writeErr(const std::string& line) {
        std::lock_guard<std::mutex> lock(outMutex);
        std::cerr << line << "\n";
        std::cerr.flush();
    }

    void writeJsonLine(const json::Value& v) {
        std::lock_guard<std::mutex> lock(outMutex);
        std::cout << json::dump(v, -1) << "\n";
        std::cout.flush();
    }

    void deviceLog(const DeviceCtx& dev, int level, const std::string& msg) {
        if (opts.jsonMode) {
            auto out = json::Value::object();
            out.set("device", dev.name);
            out.set("type", "log");
            out.set("level", levelName(level));
            out.set("time", static_cast<uint64_t>(readVirtualTime()));
            out.set("message", msg);
            writeJsonLine(out);
        } else {
            writeLine("[" + dev.name + "] " + msg);
        }
    }

    void deviceJson(const DeviceCtx& dev, const std::string& text) {
        auto parsed = json::parse(text);
        if (!parsed || !parsed->isObject()) {
            deviceLog(dev, HPM_DEBUG_LOG_ERROR,
                      "emit_json 不是合法 JSON object");
            return;
        }

        if (opts.jsonMode) {
            auto out = json::Value::object();
            out.set("device", dev.name);
            out.set("type", "json");
            out.set("time", static_cast<uint64_t>(readVirtualTime()));
            out.set("data", *parsed);
            writeJsonLine(out);
        } else {
            writeLine("[" + dev.name + "] " + json::dump(*parsed, -1));
        }
    }

    static const char* levelName(int level) {
        switch (level) {
            case HPM_DEBUG_LOG_TRACE: return "trace";
            case HPM_DEBUG_LOG_DEBUG: return "debug";
            case HPM_DEBUG_LOG_WARN: return "warn";
            case HPM_DEBUG_LOG_ERROR: return "error";
            default: return "info";
        }
    }

    // ---------- 基础读取 ----------
    uint64_t readVirtualTime() const {
        return shm ? shm->control()->virtual_time.load() : 0;
    }

    uint32_t readStatus() const {
        return shm ? shm->control()->status.load()
                   : static_cast<uint32_t>(mcus::ControlStatus::Paused);
    }

    uint32_t readPc() const {
        return shm ? shm->control()->pc.load() : 0;
    }

    static int64_t readAddr(const json::Value& req, const char* key,
                            int64_t fallback = -1) {
        if (!req.has(key)) return fallback;
        auto v = req.get(key);
        if (!v) return fallback;
        if (v->isInt()) {
            auto i = v->asInt();
            return i ? *i : fallback;
        }
        if (v->isString()) {
            auto str = v->asString();
            if (!str) return fallback;
            return static_cast<int64_t>(
                std::strtoll(str->c_str(), nullptr, 0));
        }
        return fallback;
    }

    // ---------- 配置与进程 ----------
    bool writeSessionConfig() {
        try {
            auto tbl = toml::parse_file(opts.configPath.string());
            if (!tbl.contains("simulator")) {
                tbl.insert("simulator", toml::table{});
            }
            auto* sim = tbl["simulator"].as_table();
            if (!sim) {
                writeErr("error 配置里 simulator 不是表");
                return false;
            }

            const std::string suffix = std::to_string(static_cast<long long>(::getpid()));
            shmName = "/hpm_debug_" + suffix + "_shm";
            streamName = "/hpm_debug_" + suffix + "_stream";

            sim->insert_or_assign("shm_name", shmName);
            sim->insert_or_assign("stream_name", streamName);
            sim->insert_or_assign("start_paused", true);
            sim->insert_or_assign("stay_alive", true);

            sessionConfig =
                fs::absolute(opts.buildDir / "hpm-debug" / "session.toml")
                    .string();
            std::error_code ec;
            fs::create_directories(fs::path(sessionConfig).parent_path(), ec);

            std::ofstream out(sessionConfig);
            if (!out) {
                writeErr("error 无法写入会话配置: " + sessionConfig);
                return false;
            }
            out << toml::toml_formatter{tbl};
            return true;
        } catch (const std::exception& e) {
            writeErr(std::string("error 配置解析失败: ") + e.what());
            return false;
        }
    }

    bool spawnSimulator() {
        std::vector<std::string> args = {
            "--isa", opts.isaPath.string(),
            "--model", opts.modelPath.string(),
            "--firmware", opts.firmwarePath.string(),
            "--config", sessionConfig,
        };

        mcusPid = ::fork();
        if (mcusPid < 0) {
            writeErr("error fork 失败");
            return false;
        }
        if (mcusPid == 0) {
            ::signal(SIGINT, SIG_DFL);
            ::signal(SIGTERM, SIG_DFL);
            if (::chdir(opts.projectDir.c_str()) != 0) {
                std::perror("hpm debug: chdir");
                ::_exit(127);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(opts.mcusPath.c_str()));
            for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execvp(opts.mcusPath.c_str(), argv.data());
            std::perror("hpm debug: exec mcus");
            ::_exit(127);
        }
        return true;
    }


    bool waitForSimulator() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(mcusPid, &status, WNOHANG);
            if (r == mcusPid) {
                writeErr("error mcus 提前退出");
                mcusPid = -1;
                return false;
            }
            try {
                shm = std::make_unique<mcus::SharedMemory>(
                    model, shmName, false);
                events = std::make_unique<mcus::EventStream>(
                    streamName, 0, false);
                return true;
            } catch (const std::exception&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        writeErr("error 等待 mcus 共享内存超时");
        return false;
    }

    // ---------- 包定位 / 插件 ----------
    std::optional<fs::path> findPackageDir(const std::string& driver,
                                           const Version& version) const {
        std::string domain;
        std::string name = driver;
        const auto slash = driver.find('/');
        if (slash != std::string::npos) {
            domain = driver.substr(0, slash);
            name = driver.substr(slash + 1);
        }

        fs::path root = opts.packagesDir;
        if (!domain.empty()) root /= domain;

        std::error_code ec;
        if (!fs::exists(root, ec)) return std::nullopt;

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;
            const auto manifestPath = entry.path() / "manifest.json";
            if (!fs::exists(manifestPath, ec)) continue;

            auto dm = manifest::loadDriverManifestFile(manifestPath);
            if (!dm || !(dm->version == version)) continue;

            const std::string dirName = entry.path().filename().string();
            const bool nameMatches =
                dm->name == name ||
                name.rfind(dm->name, 0) == 0 ||
                dirName.rfind(dm->name, 0) == 0 ||
                dm->name.rfind(name, 0) == 0;
            if (nameMatches) return entry.path();
        }
        return std::nullopt;
    }

    bool locateAndCheck() {
        std::vector<std::string> missing;
        for (const auto& dev : state.userDevices) {
            auto pkg = findPackageDir(dev.driver, dev.version);
            if (!pkg) {
                writeErr("error 找不到驱动包: " + dev.driver + "@" +
                         dev.version.toString());
                return false;
            }
            const fs::path entry = *pkg / "debug" / "debug.c";
            if (!fs::is_regular_file(entry)) {
                missing.push_back("  - " + dev.name + " (" + dev.driver + "@" +
                                  dev.version.toString() + ")");
            }
        }

        if (!missing.empty() && !opts.allowMissing) {
            writeErr("error 以下设备没有 debug/debug.c，无法虚拟化：");
            for (const auto& m : missing) writeErr(m);
            writeErr("  请在驱动包中添加 debug/debug.c，"
                     "或使用 --allow-missing-debug 跳过。");
            return false;
        }
        return true;
    }

    std::string pluginIncludeDir() const {
        std::error_code ec;
        fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        if (ec || exe.empty()) return {};
        return (exe.parent_path() / ".." / "include").lexically_normal().string();
    }

    bool buildPlugin(const Device& dev, const fs::path& pkg,
                     std::string& soPath) {
        const std::string key = dev.driver + "@" + dev.version.toString();
        auto it = libs.find(key);
        if (it != libs.end()) {
            soPath = pluginPath(key);
            return true;
        }

        std::vector<fs::path> sources;
        const fs::path debugDir = pkg / "debug";
        std::error_code ec;
        for (const auto& entry :
             fs::recursive_directory_iterator(debugDir, ec)) {
            if (ec) break;
            if (entry.is_regular_file() && entry.path().extension() == ".c") {
                sources.push_back(entry.path());
            }
        }
        if (sources.empty()) {
            writeErr("error " + dev.driver + " 的 debug/ 里没有 .c 文件");
            return false;
        }

        soPath = pluginPath(key);
        fs::create_directories(fs::path(soPath).parent_path(), ec);

        std::vector<std::string> args = {
            "-std=c11", "-O2", "-fPIC", "-shared", "-Wall",
            "-I", (pkg / "include").string(),
            "-I", debugDir.string(),
            "-I", pluginIncludeDir(),
        };
        for (const auto& src : sources) args.push_back(src.string());
        args.push_back("-o");
        args.push_back(soPath);
        args.push_back("-lm");

        if (opts.verbose) {
            std::string cmd = opts.hostCc;
            for (const auto& a : args) cmd += " " + a;
            writeErr("cc " + cmd);
        }

        auto st = util::runProcess(opts.hostCc, args, false);
        if (!st.ok()) {
            writeErr("error 编译 debug 插件失败: " + dev.driver);
            writeErr(st.error().message());
            return false;
        }

        libs.emplace(key, PluginLib{});
        return true;
    }

    std::string pluginPath(const std::string& key) const {
        return (opts.buildDir / "hpm-debug" / "plugins" /
                (sanitizeName(key) + ".so")).string();
    }

    bool loadPlugin(const Device& dev) {
        const std::string key = dev.driver + "@" + dev.version.toString();
        auto it = libs.find(key);
        if (it == libs.end()) {
            writeErr("error 插件未编译: " + key);
            return false;
        }
        PluginLib& lib = it->second;
        if (lib.handle) return true;

        void* handle = ::dlopen(pluginPath(key).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            writeErr("error dlopen 失败: " + std::string(::dlerror()));
            return false;
        }

        lib.handle = handle;
        lib.abi = reinterpret_cast<HpmDebugAbiVersionFn>(
            ::dlsym(handle, "hpm_debug_abi_version"));
        lib.create = reinterpret_cast<HpmDebugCreateFn>(
            ::dlsym(handle, "hpm_debug_create"));
        lib.destroy = reinterpret_cast<HpmDebugDestroyFn>(
            ::dlsym(handle, "hpm_debug_destroy"));
        lib.event = reinterpret_cast<HpmDebugEventFn>(
            ::dlsym(handle, "hpm_debug_event"));

        if (!lib.abi || !lib.create || !lib.destroy || !lib.event) {
            writeErr("error 插件缺少 ABI 符号: " + key);
            return false;
        }
        if (lib.abi() != HPM_DEBUG_ABI_VERSION) {
            writeErr("error 插件 ABI 版本不匹配: " + key);
            return false;
        }
        return true;
    }

    // ---------- 设备上下文 ----------
    void flattenBindings(const std::map<std::string, std::string>& bindings,
                         const std::string& prefix,
                         std::map<std::string, std::string>& out,
                         int depth) const {
        if (depth > 8) return;
        for (const auto& [key, value] : bindings) {
            const std::string full = prefix.empty() ? key : prefix + "." + key;
            if (const auto* idev = state.findIntermediate(value)) {
                flattenBindings(idev->bindings, full, out, depth + 1);
            } else {
                out[full] = value;
            }
        }
    }

    void buildPinMaps() {
        for (size_t i = 0; i < model.pins.size(); ++i) {
            const std::string canon = canonicalPin(model.pins[i].name);
            pinNameToId[canon] = static_cast<uint32_t>(i);
            canonicalToPin[canon] = model.pins[i].name;
        }
        pinDevices.resize(model.pins.size());
    }

    uint32_t pinIdFor(const std::string& raw) const {
        const auto it = pinNameToId.find(canonicalPin(raw));
        return it == pinNameToId.end() ? UINT32_MAX : it->second;
    }

    bool prepareBuild() {
        if (!locateAndCheck()) return false;

        for (const auto& dev : state.userDevices) {
            auto pkg = findPackageDir(dev.driver, dev.version);
            if (!pkg) {
                writeErr("error 找不到驱动包: " + dev.driver);
                return false;
            }
            const bool hasDebug =
                fs::is_regular_file(*pkg / "debug" / "debug.c");
            if (!hasDebug) continue; // --allow-missing-debug

            std::string soPath;
            if (!buildPlugin(dev, *pkg, soPath)) return false;
            if (!loadPlugin(dev)) return false;

            auto ctx = std::make_unique<DeviceCtx>();
            ctx->host = this;
            ctx->name = dev.name;
            ctx->driver = dev.driver;
            ctx->packageDir = pkg->string();
            ctx->lib = &libs.at(dev.driver + "@" + dev.version.toString());
            ctx->config = dev.config;
            flattenBindings(dev.bindings, "", ctx->bindings, 0);

            for (const auto& [key, value] : ctx->config) {
                ctx->configKv.push_back(
                    HpmDebugKv{key.c_str(), value.c_str()});
            }
            for (const auto& [key, value] : ctx->bindings) {
                ctx->bindingKv.push_back(
                    HpmDebugKv{key.c_str(), value.c_str()});
                const uint32_t id = pinIdFor(value);
                if (id != UINT32_MAX) {
                    ctx->pinIds.push_back(id);
                    ctx->pinNames.insert(canonicalPin(value));
                }
            }
            if (opts.verbose) {
                writeErr("debug device " + ctx->name + " bindings=" +
                         std::to_string(ctx->bindings.size()) + " pins=" +
                         std::to_string(ctx->pinIds.size()));
            }

            ctx->api.abi_version = HPM_DEBUG_ABI_VERSION;
            ctx->api.host = ctx.get();
            ctx->api.log = &Impl::hostLog;
            ctx->api.emit_json = &Impl::hostEmitJson;
            ctx->api.get_pin = &Impl::hostGetPin;
            ctx->api.set_pin = &Impl::hostSetPin;
            ctx->api.inject_mmio = &Impl::hostInjectMmio;
            ctx->api.raise_irq = &Impl::hostRaiseIrq;
            ctx->api.bus_write = &Impl::hostBusWrite;

            HpmDebugDeviceInfo info{};
            info.device = ctx->name.c_str();
            info.driver = ctx->driver.c_str();
            info.config = ctx->configKv.data();
            info.config_count = ctx->configKv.size();
            info.bindings = ctx->bindingKv.data();
            info.binding_count = ctx->bindingKv.size();

            devices.push_back(std::move(ctx));
        }
        return true;
    }

    bool createPlugins() {
        for (auto& ctx : devices) {
            HpmDebugDeviceInfo info{};
            info.device = ctx->name.c_str();
            info.driver = ctx->driver.c_str();
            info.config = ctx->configKv.data();
            info.config_count = ctx->configKv.size();
            info.bindings = ctx->bindingKv.data();
            info.binding_count = ctx->bindingKv.size();

            ctx->ctx = ctx->lib->create(&ctx->api, &info);
            if (!ctx->ctx) {
                writeErr("error 设备插件初始化失败: " + ctx->name);
                return false;
            }
            for (uint32_t id : ctx->pinIds) {
                if (id < pinDevices.size()) pinDevices[id].push_back(ctx.get());
            }
        }
        return true;
    }

    // ---------- Host API 回调 ----------
    static void hostLog(void* host, int level, const char* message) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (dev && dev->host) dev->host->deviceLog(*dev, level, readCString(message));
    }

    static void hostEmitJson(void* host, const char* jsonText) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (dev && dev->host) dev->host->deviceJson(*dev, readCString(jsonText));
    }

    static int hostGetPin(void* host, const char* pin, uint32_t* value,
                          uint32_t* function, uint32_t* direction) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host) return -1;
        const std::string canon = canonicalPin(readCString(pin));
        if (!dev->pinNames.count(canon)) return -1;
        const auto it = dev->host->pinNameToId.find(canon);
        if (it == dev->host->pinNameToId.end()) return -1;
        const auto* pins = dev->host->shm->pins();
        const size_t idx = it->second;
        if (value) *value = pins[idx].value.load();
        if (function) *function = pins[idx].function.load();
        if (direction) *direction = pins[idx].direction.load();
        return 0;
    }

    static int hostSetPin(void* host, const char* pin, uint32_t value) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host) return -1;
        return dev->host->setPin(canonicalPin(readCString(pin)), value);
    }

    static int hostInjectMmio(void* host, const char* space, uint32_t addr,
                              uint32_t value, uint8_t width) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host) return -1;
        return dev->host->injectMmio(readCString(space), addr, value, width);
    }

    static int hostRaiseIrq(void* host, uint32_t irq) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host) return -1;
        return dev->host->raiseIrq(irq);
    }

    static int hostBusWrite(void* host, uint32_t, const uint8_t*, size_t,
                            uint32_t) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host) return -1;
        // 总线引擎接入前先返回“未实现”
        return -1;
    }

    // ---------- 引脚/注入 ----------
    int setPin(const std::string& canon, uint32_t value) {
        std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
        const auto it = pinNameToId.find(canon);
        if (it == pinNameToId.end() || !shm) return -1;
        auto* pins = shm->pins();
        const uint32_t idx = it->second;
        const uint32_t old = pins[idx].value.load();
        pins[idx].value.store(value);
        if (old == value) return 0;

        HpmDebugPinEvent pe{};
        pe.time = readVirtualTime();
        pe.pin_id = idx;
        pe.pin = canonicalToPin[canon].c_str();
        pe.value = value;
        pe.function = pins[idx].function.load();
        pe.direction = pins[idx].direction.load();
        pe.changed = 1;
        HpmDebugEvent ev{};
        ev.type = HPM_DEBUG_EVENT_PIN;
        ev.u.pin = pe;
        for (auto* dev : pinDevices[idx]) {
            if (dev->ctx && dev->lib && dev->lib->event) {
                dev->lib->event(dev->ctx, &ev);
            }
        }
        return 0;
    }

    int injectMmio(const std::string& space, uint32_t addr, uint32_t value,
                   uint8_t width) {
        if (!shm) return -1;
        int spaceIdx = -1;
        int mmioIdx = 0;
        std::vector<std::string> seen;
        for (const auto& r : model.regions) {
            if (!r.is_mmio) continue;
            if (std::find(seen.begin(), seen.end(), r.space) == seen.end()) {
                if (r.space == space) {
                    spaceIdx = mmioIdx;
                    break;
                }
                seen.push_back(r.space);
                ++mmioIdx;
            }
        }
        if (spaceIdx < 0) return -1;

        auto* ih = shm->inject_header();
        auto* slots = shm->inject_slots();
        const uint32_t cap = ih->capacity.load();
        const uint32_t tail = ih->tail.load();
        const uint32_t head = ih->head.load();
        if (tail - head >= cap) return -1;
        auto& s = slots[tail % cap];
        s.addr.store(addr);
        s.value.store(value);
        s.width.store(width);
        s.ready.store(1);
        s.space_idx.store(static_cast<uint32_t>(spaceIdx));
        ih->tail.store(tail + 1);
        return 0;
    }

    int raiseIrq(uint32_t irq) {
        if (!shm) return -1;
        uint8_t* bm = shm->interrupt_bitmap();
        const size_t size = shm->interrupt_bitmap_size();
        const size_t byte = irq / 8;
        if (byte >= size) return -1;
        bm[byte] = static_cast<uint8_t>(bm[byte] | (1u << (irq % 8)));
        return 0;
    }

    // ---------- mcu 控制 ----------
    int mcuStatus(json::Value& result) {
        result = json::Value::object();
        result.set("status", statusName(readStatus()));
        result.set("pc", static_cast<uint64_t>(readPc()));
        result.set("virtual_time", static_cast<uint64_t>(readVirtualTime()));
        result.set("halted", static_cast<bool>(shm->control()->halted.load()));
        auto bps = json::Value::array();
        for (uint32_t addr : breakpoints) {
            bps.push(static_cast<uint64_t>(addr));
        }
        result.set("breakpoints", bps);
        return 0;
    }

    bool waitStatus(uint32_t wanted, int timeoutMs = 1000) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (readStatus() == wanted) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return readStatus() == wanted;
    }

    int mcuPause() {
        shm->control()->command.store(
            static_cast<uint32_t>(mcus::ControlCommand::Pause));
        return waitStatus(static_cast<uint32_t>(mcus::ControlStatus::Paused))
                   ? 0
                   : -1;
    }

    int mcuResume() {
        bpReported = false;
        shm->control()->command.store(
            static_cast<uint32_t>(mcus::ControlCommand::Resume));
        // 可能一恢复就命中断点，不能强等 Running
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 0;
    }

    int mcuStep(uint32_t count) {
        bpReported = false;
        if (count == 0) count = 1;
        shm->control()->step_count.store(count);
        shm->control()->command.store(
            static_cast<uint32_t>(mcus::ControlCommand::Step));
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            const uint32_t st = readStatus();
            if (st == static_cast<uint32_t>(mcus::ControlStatus::Paused) ||
                st == static_cast<uint32_t>(mcus::ControlStatus::Halted)) {
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return -1;
    }

    int mcuReset() {
        shm->control()->command.store(
            static_cast<uint32_t>(mcus::ControlCommand::Reset));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return 0;
    }

    void applyBreakpoints() {
        auto* bh = shm->breakpoint_header();
        auto* slots = shm->breakpoints();
        const uint32_t cap = static_cast<uint32_t>(shm->breakpoint_capacity());
        uint32_t i = 0;
        for (uint32_t addr : breakpoints) {
            if (i >= cap) break;
            slots[i].addr.store(addr);
            slots[i].enabled.store(1);
            ++i;
        }
        bh->count.store(i);
        bh->sorted.store(1);
        bh->hit.store(0);
        bh->hit_addr.store(0);
        bpReported = false;
        if (i > 0) {
            bh->min_addr.store(*breakpoints.begin());
            bh->max_addr.store(*breakpoints.rbegin());
        } else {
            bh->min_addr.store(0);
            bh->max_addr.store(0);
        }
    }

    int bpSet(uint32_t addr) {
        if (breakpoints.size() >= shm->breakpoint_capacity()) return -1;
        breakpoints.insert(addr);
        applyBreakpoints();
        return 0;
    }

    int bpClear(uint32_t addr) {
        breakpoints.erase(addr);
        applyBreakpoints();
        return 0;
    }

    // ---------- 事件 ----------
    void dispatchEvent(const mcus::DebugEvent& ev) {
        switch (ev.type) {
            case mcus::DebugEventType::Pin: {
                const uint32_t id = ev.u.pin.pin_id;
                if (id >= model.pins.size()) break;
                HpmDebugPinEvent pe{};
                pe.time = ev.time;
                pe.pin_id = id;
                const std::string canon = canonicalPin(model.pins[id].name);
                pe.pin = canon.c_str();
                pe.value = ev.u.pin.value;
                pe.function = ev.u.pin.function;
                pe.direction = ev.u.pin.direction;
                pe.changed = ev.u.pin.changed;

                HpmDebugEvent out{};
                out.type = HPM_DEBUG_EVENT_PIN;
                out.u.pin = pe;
                std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
                if (id < pinDevices.size()) {
                    for (auto* dev : pinDevices[id]) {
                        if (dev->ctx && dev->lib->event) {
                            dev->lib->event(dev->ctx, &out);
                        }
                    }
                }
                break;
            }
            case mcus::DebugEventType::Mmio: {
                const uint32_t sidx = ev.u.mmio.space_idx;
                const std::string space =
                    sidx < model.space_order.size() ? model.space_order[sidx]
                                                    : "";
                HpmDebugMmioEvent me{};
                me.time = ev.time;
                me.space = space.c_str();
                me.addr = ev.u.mmio.addr;
                me.value = ev.u.mmio.value;
                me.width = ev.u.mmio.width;
                me.access = static_cast<uint8_t>(ev.u.mmio.access);

                HpmDebugEvent out{};
                out.type = HPM_DEBUG_EVENT_MMIO;
                out.u.mmio = me;
                std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
                for (auto& dev : devices) {
                    if (dev->ctx && dev->lib->event) {
                        dev->lib->event(dev->ctx, &out);
                    }
                }
                break;
            }
            case mcus::DebugEventType::Irq: {
                HpmDebugIrqEvent ie{};
                ie.time = ev.time;
                ie.number = ev.u.irq.number;
                ie.pending = ev.u.irq.pending;
                HpmDebugEvent out{};
                out.type = HPM_DEBUG_EVENT_IRQ;
                out.u.irq = ie;
                std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
                for (auto& dev : devices) {
                    if (dev->ctx && dev->lib->event) {
                        dev->lib->event(dev->ctx, &out);
                    }
                }
                break;
            }
            case mcus::DebugEventType::Bus:
                // 总线引擎接入后再路由
                break;
        }
    }

    void eventLoop() {
        while (!stopping.load()) {
            bool got = false;
            mcus::DebugEvent ev{};
            while (events && events->pop(ev)) {
                got = true;
                dispatchEvent(ev);
            }
            if (!got) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (shm) {
                auto* bh = shm->breakpoint_header();
                if (bh->hit.load() != 0 && !bpReported) {
                    bpReported = true;
                    const uint32_t addr = bh->hit_addr.load();
                    const uint32_t pc = readPc();
                    if (opts.jsonMode) {
                        auto out = json::Value::object();
                        out.set("device", "mcu");
                        out.set("type", "event");
                        out.set("event", "breakpoint");
                        out.set("addr", static_cast<uint64_t>(addr));
                        out.set("pc", static_cast<uint64_t>(pc));
                        out.set("time", static_cast<uint64_t>(readVirtualTime()));
                        writeJsonLine(out);
                    } else {
                        writeLine("[mcu] 断点命中 0x" +
                                  std::to_string(addr) + " pc=0x" +
                                  std::to_string(pc));
                    }
                }
            }
            int status = 0;
            if (mcusPid > 0) {
                pid_t r = ::waitpid(mcusPid, &status, WNOHANG);
                if (r == mcusPid) {
                    stopping.store(true);
                    break;
                }
            }
        }
    }

    void sendInitialPinSnapshot() {
        if (!shm) return;
        std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
        const auto* pins = shm->pins();
        for (auto& dev : devices) {
            for (uint32_t id : dev->pinIds) {
                if (id >= model.pins.size()) continue;
                const std::string canon = canonicalPin(model.pins[id].name);
                HpmDebugPinEvent pe{};
                pe.time = readVirtualTime();
                pe.pin_id = id;
                pe.pin = canon.c_str();
                pe.value = pins[id].value.load();
                pe.function = pins[id].function.load();
                pe.direction = pins[id].direction.load();
                pe.changed = 7;

                HpmDebugEvent ev{};
                ev.type = HPM_DEBUG_EVENT_PIN;
                ev.u.pin = pe;
                if (dev->ctx && dev->lib->event) {
                    dev->lib->event(dev->ctx, &ev);
                }
            }
        }
    }

    // ---------- JSON 协议 ----------
    void writeReady() {
        auto out = json::Value::object();
        out.set("device", "mcu");
        out.set("type", "event");
        out.set("event", "ready");
        out.set("protocol", "hpm-debug/1");
        out.set("status", statusName(readStatus()));
        auto names = json::Value::array();
        for (const auto& d : devices) names.push(d->name);
        out.set("devices", names);
        writeJsonLine(out);
    }

    json::Value makeResponse(const json::Value& req, bool ok,
                             const json::Value& result,
                             const std::string& error = "") {
        auto out = json::Value::object();
        if (req.has("id")) out.set("id", req.get("id").value());
        out.set("device", "mcu");
        out.set("type", "response");
        out.set("ok", ok);
        if (ok) out.set("result", result);
        else {
            auto err = json::Value::object();
            err.set("code", "error");
            err.set("message", error);
            out.set("error", err);
        }
        return out;
    }

    bool handleMcuRequest(const json::Value& req, json::Value& response) {
        const std::string cmd = req.getString("cmd", "");
        json::Value result = json::Value::object();
        std::string error;

        if (cmd == "status") {
            mcuStatus(result);
        } else if (cmd == "pause") {
            if (mcuPause() != 0) error = "pause 超时";
            else mcuStatus(result);
        } else if (cmd == "resume" || cmd == "run") {
            if (mcuResume() != 0) error = "resume 超时";
            else mcuStatus(result);
        } else if (cmd == "step") {
            const int64_t n = req.getInt("count", 1);
            if (mcuStep(static_cast<uint32_t>(n)) != 0) error = "step 超时";
            else mcuStatus(result);
        } else if (cmd == "reset") {
            if (mcuReset() != 0) error = "reset 失败";
            else mcuStatus(result);
        } else if (cmd == "breakpoint.set") {
            const int64_t addr = readAddr(req, "addr");
            if (addr < 0 || bpSet(static_cast<uint32_t>(addr)) != 0)
                error = "断点设置失败（最多 32 个）";
            else mcuStatus(result);
        } else if (cmd == "breakpoint.clear") {
            const int64_t addr = readAddr(req, "addr");
            if (addr < 0) error = "缺少 addr";
            else {
                bpClear(static_cast<uint32_t>(addr));
                mcuStatus(result);
            }
        } else if (cmd == "breakpoint.clear_all") {
            breakpoints.clear();
            applyBreakpoints();
            mcuStatus(result);
        } else if (cmd == "breakpoint.list") {
            mcuStatus(result);
        } else if (cmd == "pin.read") {
            const std::string pin = req.getString("pin", "");
            const uint32_t id = pinIdFor(pin);
            if (id == UINT32_MAX) {
                error = "未知引脚: " + pin;
            } else {
                const auto* pins = shm->pins();
                result.set("pin", canonicalToPin[canonicalPin(pin)]);
                result.set("value", pins[id].value.load());
                result.set("function", pins[id].function.load());
                result.set("direction", pins[id].direction.load());
            }
        } else if (cmd == "pin.set") {
            const std::string pin = req.getString("pin", "");
            const int64_t value = req.getInt("value", -1);
            if (value < 0 || setPin(canonicalPin(pin),
                                    static_cast<uint32_t>(value)) != 0)
                error = "设置引脚失败: " + pin;
            else
                result.set("ok", true);
        } else if (cmd == "reg.read") {
            const std::string space = req.getString("space", "");
            const int64_t addr = readAddr(req, "addr");
            const int64_t width = req.getInt("width", 8);
            if (addr < 0 || width <= 0) {
                error = "参数错误";
            } else {
                result.set("value", shm->read_reg(
                                       space, static_cast<uint32_t>(addr),
                                       static_cast<uint8_t>(width)));
            }
        } else if (cmd == "irq.inject") {
            const int64_t irq = req.getInt("number", -1);
            if (irq < 0 || raiseIrq(static_cast<uint32_t>(irq)) != 0)
                error = "中断注入失败";
            else
                result.set("ok", true);
        } else if (cmd == "shutdown") {
            result.set("ok", true);
            stopping.store(true);
        } else {
            error = "未知 mcu 命令: " + cmd;
        }

        response = makeResponse(req, error.empty(), result, error);
        return error.empty();
    }

    bool handleJsonLine(const std::string& line) {
        auto parsed = json::parse(line);
        if (!parsed || !parsed->isObject()) {
            writeErr("error 无效 JSON 输入");
            return false;
        }
        const json::Value& req = *parsed;
        const std::string device = req.getString("device", "");
        if (device.empty()) {
            writeErr("error JSON 缺少 device");
            return false;
        }
        if (device == "mcu") {
            json::Value response;
            handleMcuRequest(req, response);
            writeJsonLine(response);
            return true;
        }

        DeviceCtx* target = nullptr;
        for (auto& d : devices) {
            if (d->name == device) {
                target = d.get();
                break;
            }
        }
        if (!target) {
            auto response = makeResponse(req, false, json::Value::object(),
                                         "未知设备: " + device);
            writeJsonLine(response);
            return false;
        }

        std::string payload = "{}";
        if (req.has("data")) {
            payload = json::dump(req.get("data").value(), -1);
        }
        HpmDebugEvent ev{};
        ev.type = HPM_DEBUG_EVENT_JSON;
        ev.u.json = payload.c_str();
        {
            std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
            if (target->ctx && target->lib->event) {
                target->lib->event(target->ctx, &ev);
            }
        }
        if (req.has("id")) {
            auto response = makeResponse(req, true, json::Value::object());
            writeJsonLine(response);
        }
        return true;
    }

    void jsonLoop() {
        std::string line;
        while (!stopping.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            handleJsonLine(line);
        }
        stopping.store(true);
    }

    // ---------- CLI ----------
    void printPins() {
        const auto* pins = shm->pins();
        for (size_t i = 0; i < model.pins.size(); ++i) {
            writeLine(model.pins[i].name + " value=" +
                      std::to_string(pins[i].value.load()) +
                      " func=" + std::to_string(pins[i].function.load()) +
                      " dir=" + std::to_string(pins[i].direction.load()));
        }
    }

    void printRegs() {
        for (const auto& r : model.registers) {
            const uint32_t v = shm->read_reg(r.space, r.addr, r.width);
            writeLine(r.space + ":" + std::to_string(r.addr) + " " + r.name +
                      " = " + std::to_string(v));
        }
    }

    void printDevices() {
        for (const auto& d : devices) {
            writeLine(d->name + " (" + d->driver + ") pins=" +
                      std::to_string(d->pinIds.size()));
        }
    }

    void cliHelp() {
        writeLine("status | run | pause | step [n] | reset");
        writeLine("bp <addr> | delete <addr> | bl");
        writeLine("pins | regs | devices | send <device> <json> | quit");
    }

    void cliLoop() {
        writeLine("hpm debug：输入 help 查看命令");
        std::string line;
        while (!stopping.load()) {
            std::cout << "hpm> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (line.empty()) continue;

            std::istringstream ss(line);
            std::string cmd;
            ss >> cmd;

            if (cmd == "quit" || cmd == "exit") {
                stopping.store(true);
                break;
            } else if (cmd == "help") {
                cliHelp();
            } else if (cmd == "status") {
                json::Value result;
                mcuStatus(result);
                writeLine(json::dump(result, -1));
            } else if (cmd == "run" || cmd == "continue" || cmd == "c") {
                mcuResume();
            } else if (cmd == "pause") {
                mcuPause();
            } else if (cmd == "step") {
                uint32_t n = 1;
                ss >> n;
                mcuStep(n);
            } else if (cmd == "reset") {
                mcuReset();
            } else if (cmd == "bp" || cmd == "break") {
                std::string addr;
                ss >> addr;
                if (!addr.empty()) {
                    const uint32_t a = static_cast<uint32_t>(
                        std::strtoul(addr.c_str(), nullptr, 0));
                    if (bpSet(a) == 0) writeLine("断点已设置");
                    else writeLine("断点设置失败");
                }
            } else if (cmd == "delete") {
                std::string addr;
                ss >> addr;
                if (!addr.empty()) {
                    bpClear(static_cast<uint32_t>(
                        std::strtoul(addr.c_str(), nullptr, 0)));
                    writeLine("断点已删除");
                }
            } else if (cmd == "bl") {
                for (uint32_t a : breakpoints)
                    writeLine("0x" + std::to_string(a));
            } else if (cmd == "pins") {
                printPins();
            } else if (cmd == "regs") {
                printRegs();
            } else if (cmd == "devices") {
                printDevices();
            } else if (cmd == "send") {
                std::string device;
                ss >> device;
                std::string payload;
                std::getline(ss, payload);
                if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                DeviceCtx* target = nullptr;
                for (auto& d : devices) {
                    if (d->name == device) {
                        target = d.get();
                        break;
                    }
                }
                if (!target) {
                    writeLine("未知设备: " + device);
                    continue;
                }
                HpmDebugEvent ev{};
                ev.type = HPM_DEBUG_EVENT_JSON;
                ev.u.json = payload.c_str();
                std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
                target->lib->event(target->ctx, &ev);
            } else {
                writeLine("未知命令: " + cmd);
            }
        }
        stopping.store(true);
    }

    // ---------- 生命周期 ----------
    void shutdown() {
        if (cleaned) return;
        cleaned = true;
        stopping.store(true);

        if (eventThread.joinable()) eventThread.join();

        for (auto& d : devices) {
            if (d->ctx && d->lib && d->lib->destroy) {
                d->lib->destroy(d->ctx);
                d->ctx = nullptr;
            }
        }
        for (auto& [key, lib] : libs) {
            if (lib.handle) {
                ::dlclose(lib.handle);
                lib.handle = nullptr;
            }
        }
        if (mcusPid > 0) {
            ::kill(mcusPid, SIGTERM);
            int status = 0;
            ::waitpid(mcusPid, &status, 0);
            mcusPid = -1;
        }
    }

    ~Impl() { shutdown(); }

    int run() {
        auto st = manifest::loadDriverStateFile(opts.projectDir / "driver.json");
        if (!st) {
            writeErr("error 读取 driver.json 失败: " + st.error().message());
            return 1;
        }
        state = std::move(*st);

        try {
            model = mcus::Model::load_from_file(opts.modelPath.string());
        } catch (const std::exception& e) {
            writeErr(std::string("error 加载 mcus 模型失败: ") + e.what());
            return 1;
        }

        buildPinMaps();
        if (!prepareBuild()) return 1;
        if (!writeSessionConfig()) return 1;
        if (!spawnSimulator()) return 1;
        if (!waitForSimulator()) return 1;
        if (!createPlugins()) return 1;

        ready = true;
        eventThread = std::thread([this] { eventLoop(); });

        if (opts.jsonMode) {
            writeReady();
            sendInitialPinSnapshot();
            if (opts.runImmediately) mcuResume();
            jsonLoop();
        } else {
            writeLine("模拟器已启动，状态: " + statusName(readStatus()));
            sendInitialPinSnapshot();
            if (opts.runImmediately) mcuResume();
            cliLoop();
        }

        shutdown();
        return 0;
    }
};

Host::Host(HostOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Host::~Host() = default;

int Host::run() { return impl_->run(); }

} // namespace hpm::debug
