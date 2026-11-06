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

#include "bus_router.h"
#include "host_utils.h"
#include "pin_router.h"

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

} // namespace

struct Host::Impl {
    explicit Impl(HostOptions o) : opts(std::move(o)) {}

    HostOptions opts;

    mcus::Model model;
    std::unique_ptr<mcus::SharedMemory> shm;
    std::unique_ptr<mcus::EventStream> events;
    std::unique_ptr<BusRouter> busRouter;
    std::unique_ptr<PinRouter> pinRouter;

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
        std::string version;
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

        // 设备 UI/状态缓存
        json::Value ui = json::Value::object();
        bool hasUi = false;
        json::Value state = json::Value::object();
        bool online = true;
        uint64_t lastUpdate = 0;
        std::string lastError;
        bool describing = false;
        bool creating = true;
    };

    std::vector<std::unique_ptr<DeviceCtx>> devices;
    std::map<std::string, PluginLib> libs; // driver@version -> lib
    std::map<std::string, std::string> canonicalToPin; // canonical -> model pin
    std::map<std::string, uint32_t> pinNameToId;       // canonical -> index
    std::vector<std::vector<DeviceCtx*>> pinDevices;
    std::vector<std::vector<std::string>> pinDeviceNames;

    std::map<uint32_t, bool> breakpoints; // addr -> enabled
    std::map<uint32_t, uint32_t> bpHitCount;
    bool bpReported = false;
    std::string stopReason;
    uint32_t stopAddr = 0;
    std::string lastStatusEvent;
    std::string lastStopReasonEvent;
    uint64_t outSeq = 0;

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
        auto out = v;
        out.set("seq", static_cast<uint64_t>(outSeq++));
        std::lock_guard<std::mutex> lock(outMutex);
        std::cout << json::dump(out, -1) << "\n";
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

    void deviceJson(DeviceCtx& dev, const std::string& text) {
        auto parsed = json::parse(text);
        if (!parsed || !parsed->isObject()) {
            deviceLog(dev, HPM_DEBUG_LOG_ERROR,
                      "emit_json 不是合法 JSON object");
            return;
        }

        // 设备 UI 描述
        if (parsed->has("$ui")) {
            auto ui = parsed->get("$ui");
            if (!ui || !ui->isObject()) {
                deviceLog(dev, HPM_DEBUG_LOG_ERROR, "$ui 必须是 object");
                return;
            }
            dev.ui = *ui;
            dev.hasUi = true;
            if (opts.jsonMode && !dev.creating) {
                auto out = json::Value::object();
                out.set("device", dev.name);
                out.set("type", "event");
                out.set("event", "device.updated");
                out.set("time", static_cast<uint64_t>(readVirtualTime()));
                out.set("ui", *ui);
                writeJsonLine(out);
            }
            return;
        }

        // 设备状态增量
        if (parsed->has("$state")) {
            auto st = parsed->get("$state");
            if (!st || !st->isObject()) {
                deviceLog(dev, HPM_DEBUG_LOG_ERROR, "$state 必须是 object");
                return;
            }
            auto keys = st->keys();
            if (keys) {
                for (const auto& key : *keys) {
                    auto value = st->get(key);
                    if (value) dev.state.set(key, *value);
                }
            }
            dev.lastUpdate = readVirtualTime();
            if (opts.jsonMode && !dev.creating) {
                auto out = json::Value::object();
                out.set("device", dev.name);
                out.set("type", "state");
                out.set("time", static_cast<uint64_t>(dev.lastUpdate));
                out.set("online", dev.online);
                out.set("values", dev.state);
                writeJsonLine(out);
            }
            return;
        }

        // 设备一次性事件
        if (parsed->has("$event")) {
            auto event = parsed->get("$event");
            if (opts.jsonMode && !dev.creating && event) {
                auto out = json::Value::object();
                out.set("device", dev.name);
                out.set("type", "event");
                out.set("event", "device.event");
                out.set("time", static_cast<uint64_t>(readVirtualTime()));
                out.set("data", *event);
                writeJsonLine(out);
            }
            return;
        }

        // 设备错误
        if (parsed->has("$error")) {
            auto err = parsed->get("$error");
            if (err && err->isObject()) {
                dev.lastError = err->getString("message", "");
                if (err->getBool("fatal", false)) dev.online = false;
                if (opts.jsonMode) {
                    auto out = json::Value::object();
                    out.set("device", dev.name);
                    out.set("type", "error");
                    out.set("time", static_cast<uint64_t>(readVirtualTime()));
                    out.set("error", *err);
                    writeJsonLine(out);
                }
            }
            return;
        }

        if (dev.describing) return;

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

    int spaceIndex(const std::string& space) const {
        for (size_t i = 0; i < model.space_order.size(); ++i) {
            if (model.space_order[i] == space) return static_cast<int>(i);
        }
        return -1;
    }

    int mcuDump(const std::string& space, uint32_t addr, uint32_t length,
                std::string& hex, uint32_t& actual, bool& inRange,
                std::string& error, std::string& errorCode) {
        inRange = false;
        if (!shm) {
            error = "模拟器未连接";
            errorCode = "error";
            return -1;
        }
        const int idx = spaceIndex(space);
        if (idx < 0) {
            error = "未知地址空间: " + space;
            errorCode = "unknown_space";
            return -1;
        }
        const size_t cap = shm->dump_buffer_size();
        if (length == 0) {
            error = "length 必须大于 0";
            errorCode = "out_of_range";
            return -1;
        }
        if (length > cap) {
            error = "length 超过上限 " + std::to_string(cap);
            errorCode = "too_large";
            return -1;
        }
        const mcus::MemoryRegion* region = model.find_region(space, addr);
        if (!region) {
            inRange = false;
            actual = 0;
            hex.clear();
            return 0;
        }
        inRange = true;

        const bool was_running =
            readStatus() == static_cast<uint32_t>(mcus::ControlStatus::Running);
        if (was_running && mcuPause() != 0) {
            error = "dump 前暂停失败";
            errorCode = "busy";
            return -1;
        }

        auto* ctl = shm->control();
        ctl->dump_ready.store(0);
        ctl->dump_addr.store(addr);
        ctl->dump_len.store(length);
        ctl->dump_space_idx.store(static_cast<uint32_t>(idx));
        ctl->command.store(static_cast<uint32_t>(mcus::ControlCommand::Dump));

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ctl->dump_ready.load() != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (ctl->dump_ready.load() == 0) {
            if (was_running) mcuResume();
            error = "dump 超时";
            errorCode = "timeout";
            return -1;
        }

        actual = ctl->dump_len.load();
        if (actual > length) actual = length;
        if (actual > cap) actual = static_cast<uint32_t>(cap);
        hex = toHex(shm->dump_buffer(), actual);

        if (was_running) mcuResume();
        return 0;
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
            const bool dirMatches =
                dirName == name ||
                dirName.rfind(name + "-", 0) == 0;
            const bool nameMatches =
                dm->name == name ||
                name.rfind(dm->name, 0) == 0 ||
                dm->name.rfind(name, 0) == 0;
            if (nameMatches || dirMatches) return entry.path();
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
            "-pthread",
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
        pinDeviceNames.resize(model.pins.size());
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
            ctx->version = dev.version.toString();
            ctx->packageDir = pkg->string();
            ctx->lib = &libs.at(dev.driver + "@" + dev.version.toString());
            ctx->config = dev.config;
            flattenBindings(dev.bindings, "", ctx->bindings, 0);

            for (const auto& [key, value] : ctx->config) {
                ctx->configKv.push_back(
                    HpmDebugKv{key.c_str(), value.c_str()});
            }
            for (auto& [key, value] : ctx->bindings) {
                const uint32_t id = pinIdFor(value);
                if (id != UINT32_MAX) {
                    const std::string canon = canonicalPin(value);
                    value = canonicalToPin.count(canon) ? canonicalToPin[canon]
                                                        : canon;
                    ctx->pinIds.push_back(id);
                    ctx->pinNames.insert(canon);
                }
                ctx->bindingKv.push_back(
                    HpmDebugKv{key.c_str(), value.c_str()});
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
            ctx->api.bus_send = &Impl::hostBusSend;
            ctx->api.usb_send = &Impl::hostUsbSend;

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
            ctx->creating = false;
            if (!ctx->ctx) {
                writeErr("error 设备插件初始化失败: " + ctx->name);
                return false;
            }
            for (uint32_t id : ctx->pinIds) {
                if (id < pinDevices.size()) {
                    pinDevices[id].push_back(ctx.get());
                    pinDeviceNames[id].push_back(ctx->name);
                }
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

    static int hostBusWrite(void* host, uint32_t xfer_id,
                            const uint8_t* data, size_t len, uint32_t flags) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host || !dev->host->busRouter) return -1;
        return dev->host->busRouter->sendSpiResponse(
            xfer_id, data, len, static_cast<uint8_t>(flags));
    }

    static int hostBusSend(void* host, uint32_t bus_id, uint32_t xfer_id,
                           uint8_t kind, uint8_t direction,
                           const uint8_t* data, size_t len, uint8_t flags) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host || !dev->host->busRouter) return -1;
        return dev->host->busRouter->send(bus_id, xfer_id, kind, direction,
                                          data, len, flags);
    }

    static int hostUsbSend(void* host, uint32_t xfer_id, uint8_t ep,
                           uint8_t token, uint8_t handshake,
                           const uint8_t* data, size_t len) {
        auto* dev = static_cast<DeviceCtx*>(host);
        if (!dev || !dev->host || !dev->host->busRouter) return -1;
        return dev->host->busRouter->sendUsb(xfer_id, ep, token, handshake,
                                             data, len);
    }

    // ---------- 引脚/注入 ----------
    json::Value pinInfoJson(uint32_t id, uint32_t changed = 0) const {
        auto obj = json::Value::object();
        const auto& pin = model.pins[id];
        const auto* pins = shm->pins();
        const std::string canon = canonicalPin(pin.name);
        const uint32_t function = pins[id].function.load();
        const uint32_t direction = pins[id].direction.load();

        obj.set("name", canon);
        obj.set("number", static_cast<uint64_t>(pin.number));
        obj.set("value", pins[id].value.load());
        obj.set("function", function);
        std::string functionName;
        if (function < pin.functions.size()) functionName = pin.functions[function];
        obj.set("function_name", functionName);
        auto functions = json::Value::array();
        for (const auto& f : pin.functions) functions.push(f);
        obj.set("functions", functions);
        obj.set("direction", direction);
        obj.set("direction_name", direction == 0 ? "input" : "output");
        auto devs = json::Value::array();
        for (const auto& d : pinDeviceNames[id]) devs.push(d);
        obj.set("devices", devs);
        if (changed != 0) obj.set("changed", changed);
        return obj;
    }

    // ---------- 设备元数据/状态 ----------
    json::Value deviceInfoJson(const DeviceCtx& d) const {
        auto obj = json::Value::object();
        obj.set("device", d.name);
        obj.set("driver", d.driver);
        obj.set("version", d.version);
        obj.set("online", d.online);
        std::string role = "unknown";
        std::string title = d.name;
        if (d.hasUi) {
            role = d.ui.getString("role", role);
            title = d.ui.getString("title", title);
        }
        obj.set("role", role);
        obj.set("title", title);
        if (d.hasUi && d.ui.has("channels")) {
            auto ch = d.ui.get("channels");
            if (ch) obj.set("channels", *ch);
            else obj.set("channels", json::Value::array());
        } else {
            obj.set("channels", json::Value::array());
        }
        if (!d.lastError.empty()) obj.set("last_error", d.lastError);
        return obj;
    }

    void requestDescribe(DeviceCtx& d) {
        if (d.hasUi || !d.ctx || !d.lib || !d.lib->event) return;
        d.describing = true;
        const char* req = "{\"cmd\":\"describe\"}";
        HpmDebugEvent ev{};
        ev.type = HPM_DEBUG_EVENT_JSON;
        ev.u.json = req;
        {
            std::lock_guard<std::recursive_mutex> lock(pluginMutex);
            d.lib->event(d.ctx, &ev);
        }
        d.describing = false;
    }

    json::Value deviceDescribeResult(DeviceCtx& d) {
        requestDescribe(d);
        json::Value result = d.hasUi ? d.ui : json::Value::object();
        if (!d.hasUi) {
            result.set("schema", "hpm-ui/1");
            result.set("role", "unknown");
            result.set("title", d.name);
            result.set("channels", json::Value::array());
        }
        result.set("state", d.state);
        result.set("online", d.online);
        return result;
    }

    json::Value deviceStateJson(const DeviceCtx& d) const {
        auto obj = json::Value::object();
        obj.set("device", d.name);
        obj.set("online", d.online);
        obj.set("last_update", static_cast<uint64_t>(d.lastUpdate));
        obj.set("values", d.state);
        if (!d.lastError.empty()) obj.set("error", d.lastError);
        return obj;
    }

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
        if (pinRouter) {
            pinRouter->queue(mcus::PinEvent{
                idx, value, pe.function, pe.direction, 1});
        }
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
    void emitStatusEvent() {
        if (!opts.jsonMode || !shm) return;
        const std::string st = statusName(readStatus());
        auto out = json::Value::object();
        out.set("device", "mcu");
        out.set("type", "event");
        out.set("event", "status");
        out.set("time", static_cast<uint64_t>(readVirtualTime()));
        out.set("status", st);
        out.set("pc", static_cast<uint64_t>(readPc()));
        out.set("halted", static_cast<bool>(shm->control()->halted.load()));
        out.set("stop_reason", stopReason);
        out.set("stop_addr", static_cast<uint64_t>(stopAddr));
        if (stopReason == "breakpoint") {
            auto bp = json::Value::object();
            bp.set("addr", static_cast<uint64_t>(stopAddr));
            bp.set("hit_count",
                   static_cast<uint64_t>(bpHitCount[stopAddr]));
            out.set("breakpoint", bp);
        }
        writeJsonLine(out);
        lastStatusEvent = st;
        lastStopReasonEvent = stopReason;
    }

    int mcuStatus(json::Value& result) {
        result = json::Value::object();
        const std::string st = statusName(readStatus());
        result.set("status", st);
        result.set("pc", static_cast<uint64_t>(readPc()));
        result.set("virtual_time", static_cast<uint64_t>(readVirtualTime()));
        result.set("halted", static_cast<bool>(shm->control()->halted.load()));
        result.set("stop_reason", stopReason);
        result.set("stop_addr", static_cast<uint64_t>(stopAddr));
        auto bps = json::Value::array();
        for (const auto& [addr, enabled] : breakpoints) {
            auto obj = json::Value::object();
            obj.set("addr", static_cast<uint64_t>(addr));
            obj.set("enabled", enabled);
            obj.set("hit_count",
                    static_cast<uint64_t>(bpHitCount[addr]));
            bps.push(obj);
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
        stopReason = "pause";
        stopAddr = readPc();
        shm->control()->command.store(
            static_cast<uint32_t>(mcus::ControlCommand::Pause));
        const bool ok =
            waitStatus(static_cast<uint32_t>(mcus::ControlStatus::Paused));
        if (ok) stopAddr = readPc();
        return ok ? 0 : -1;
    }

    int mcuResume() {
        bpReported = false;
        stopReason.clear();
        stopAddr = 0;
        shm->control()->command.store(
            static_cast<uint32_t>(mcus::ControlCommand::Resume));
        // 可能一恢复就命中断点，不能强等 Running
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 0;
    }

    int mcuStep(uint32_t count) {
        bpReported = false;
        stopReason = "step";
        stopAddr = 0;
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
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (readStatus() == static_cast<uint32_t>(mcus::ControlStatus::Halted)) {
            stopReason = "halt";
            stopAddr = readPc();
        }
        return 0;
    }

    int mcuReset() {
        stopReason = "reset";
        stopAddr = 0;
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
        for (const auto& [addr, enabled] : breakpoints) {
            if (i >= cap) break;
            slots[i].addr.store(addr);
            slots[i].enabled.store(enabled ? 1 : 0);
            ++i;
        }
        bh->count.store(i);
        bh->sorted.store(1);
        bh->hit.store(0);
        bh->hit_addr.store(0);
        bpReported = false;
        if (i > 0) {
            bh->min_addr.store(breakpoints.begin()->first);
            bh->max_addr.store(breakpoints.rbegin()->first);
        } else {
            bh->min_addr.store(0);
            bh->max_addr.store(0);
        }
    }

    int bpSet(uint32_t addr) {
        if (!breakpoints.count(addr) &&
            breakpoints.size() >= shm->breakpoint_capacity()) {
            return -1;
        }
        breakpoints[addr] = true;
        bpHitCount.emplace(addr, 0);
        applyBreakpoints();
        return 0;
    }

    int bpClear(uint32_t addr) {
        breakpoints.erase(addr);
        bpHitCount.erase(addr);
        applyBreakpoints();
        return 0;
    }

    int bpEnable(uint32_t addr, bool enabled) {
        auto it = breakpoints.find(addr);
        if (it == breakpoints.end()) return -1;
        it->second = enabled;
        applyBreakpoints();
        return 0;
    }

    // ---------- 事件 ----------
    void broadcastPluginEvent(const HpmDebugEvent& out) {
        std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
        for (auto& dev : devices) {
            if (dev->ctx && dev->lib && dev->lib->event) {
                dev->lib->event(dev->ctx, &out);
            }
        }
    }

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

                if (pinRouter) {
                    pinRouter->queue(mcus::PinEvent{
                        ev.u.pin.pin_id, ev.u.pin.value, ev.u.pin.function,
                        ev.u.pin.direction, ev.u.pin.changed});
                }

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
                if (busRouter) busRouter->route(ev.u.bus);
                break;
            case mcus::DebugEventType::Usb:
                if (busRouter) busRouter->routeUsb(ev.u.usb);
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
            if (pinRouter) pinRouter->flush();
            if (shm) {
                auto* bh = shm->breakpoint_header();
                if (bh->hit.load() != 0 && !bpReported) {
                    bpReported = true;
                    const uint32_t addr = bh->hit_addr.load();
                    const uint32_t pc = readPc();
                    stopReason = "breakpoint";
                    stopAddr = addr;
                    ++bpHitCount[addr];
                    if (opts.jsonMode) {
                        auto out = json::Value::object();
                        out.set("device", "mcu");
                        out.set("type", "event");
                        out.set("event", "breakpoint");
                        out.set("addr", static_cast<uint64_t>(addr));
                        out.set("pc", static_cast<uint64_t>(pc));
                        out.set("time", static_cast<uint64_t>(readVirtualTime()));
                        out.set("hit_count",
                                static_cast<uint64_t>(bpHitCount[addr]));
                        writeJsonLine(out);
                        emitStatusEvent();
                    } else {
                        writeLine("[mcu] 断点命中 0x" +
                                  std::to_string(addr) + " pc=0x" +
                                  std::to_string(pc));
                        emitStatusEvent();
                    }
                }
                // 其他状态迁移（halt 等）
                const std::string st = statusName(readStatus());
                if (st != lastStatusEvent ||
                    (st == "paused" && stopReason != lastStopReasonEvent)) {
                    if (st == "halted") {
                        stopReason = "halt";
                        stopAddr = readPc();
                    } else if (st == "running") {
                        stopReason.clear();
                        stopAddr = 0;
                    } else if (st == "paused" && stopReason.empty()) {
                        stopReason = "pause";
                        stopAddr = readPc();
                    }
                    emitStatusEvent();
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
        out.set("protocol", "hpm-debug/2");
        out.set("status", statusName(readStatus()));

        auto features = json::Value::array();
        for (const char* f : {
                 "pin.list", "pin.subscribe", "pin.unsubscribe",
                 "pin.snapshot",
                 "device.list", "device.describe", "device.state",
                 "device.snapshot", "device.updated", "device.error",
                 "mem.read", "mem.regions",
                 "status.event", "breakpoint.object", "seq"}) {
            features.push(f);
        }
        out.set("features", features);

        auto limits = json::Value::object();
        limits.set("max_breakpoints",
                   static_cast<uint64_t>(shm->breakpoint_capacity()));
        limits.set("max_mem_read",
                   static_cast<uint64_t>(shm->dump_buffer_size()));
        limits.set("pin_subscribe_min_ms", static_cast<uint64_t>(10));
        limits.set("pin_subscribe_max_ms", static_cast<uint64_t>(1000));
        out.set("limits", limits);

        auto modelObj = json::Value::object();
        modelObj.set("name", model.name);
        modelObj.set("arch", model.arch);
        modelObj.set("pins", static_cast<uint64_t>(model.pins.size()));
        auto spaces = json::Value::array();
        for (const auto& sp : model.space_order) spaces.push(sp);
        modelObj.set("spaces", spaces);
        out.set("model", modelObj);

        auto names = json::Value::array();
        for (const auto& d : devices) names.push(d->name);
        out.set("devices", names);
        lastStatusEvent = statusName(readStatus());
        lastStopReasonEvent = stopReason;
        writeJsonLine(out);
    }

    json::Value makeResponse(const json::Value& req, bool ok,
                             const json::Value& result,
                             const std::string& error = "",
                             const std::string& errorCode = "error",
                             const std::string& deviceName = "mcu") {
        auto out = json::Value::object();
        if (req.has("id")) out.set("id", req.get("id").value());
        out.set("device", deviceName);
        out.set("type", "response");
        out.set("ok", ok);
        if (ok) out.set("result", result);
        else {
            auto err = json::Value::object();
            err.set("code", errorCode);
            err.set("message", error);
            out.set("error", err);
        }
        return out;
    }

    bool handleMcuRequest(const json::Value& req, json::Value& response) {
        const std::string cmd = req.getString("cmd", "");
        json::Value result = json::Value::object();
        std::string error;
        std::string errorCode = "error";

        if (cmd == "status") {
            mcuStatus(result);
        } else if (cmd == "pin.list") {
            auto pins = json::Value::array();
            for (uint32_t i = 0; i < model.pins.size(); ++i) {
                pins.push(pinInfoJson(i));
            }
            result.set("pins", pins);
        } else if (cmd == "pin.subscribe") {
            auto pinsJson = req.get("pins");
            if (!pinsJson || !pinsJson->isArray()) {
                error = "pins 必须是数组";
                errorCode = "invalid_argument";
            } else {
                std::set<uint32_t> ids;
                bool all = false;
                auto size = pinsJson->size();
                for (size_t i = 0; i < (size ? *size : 0); ++i) {
                    auto item = pinsJson->at(i);
                    if (!item || !item->isString()) continue;
                    const std::string name = *item->asString();
                    if (name == "*") {
                        all = true;
                        continue;
                    }
                    const uint32_t id = pinIdFor(name);
                    if (id == UINT32_MAX) {
                        error = "未知引脚: " + name;
                        errorCode = "unknown_pin";
                        break;
                    }
                    ids.insert(id);
                }
                const std::string mode = req.getString("mode", "change");
                const int64_t interval = req.getInt("interval_ms", 50);
                if (error.empty()) {
                    if (mode != "change" && mode != "all") {
                        error = "mode 必须是 change 或 all";
                        errorCode = "invalid_argument";
                    } else if (interval < 10 || interval > 1000) {
                        error = "interval_ms 必须在 10..1000";
                        errorCode = "invalid_argument";
                    } else {
                        if (all) {
                            ids.clear();
                            for (uint32_t i = 0; i < model.pins.size(); ++i)
                                ids.insert(i);
                        }
                        if (pinRouter) {
                            pinRouter->subscribe(
                                ids, all, mode,
                                static_cast<uint32_t>(interval));
                            pinRouter->snapshot(ids);
                        }
                        result.set("subscribed", static_cast<uint64_t>(ids.size()));
                        result.set("mode", mode);
                        result.set("interval_ms", interval);
                    }
                }
            }
        } else if (cmd == "pin.unsubscribe") {
            if (pinRouter) pinRouter->unsubscribe();
            result.set("ok", true);
        } else if (cmd == "device.list") {
            auto arr = json::Value::array();
            for (const auto& d : devices) arr.push(deviceInfoJson(*d));
            result.set("devices", arr);
        } else if (cmd == "device.describe") {
            const std::string target =
                req.getString("device_name", req.getString("name", ""));
            DeviceCtx* found = nullptr;
            for (auto& d : devices) {
                if (d->name == target) {
                    found = d.get();
                    break;
                }
            }
            if (!found) {
                error = "未知设备: " + target;
                errorCode = "unknown_device";
            } else {
                result = deviceDescribeResult(*found);
            }
        } else if (cmd == "device.snapshot") {
            auto arr = json::Value::array();
            for (const auto& d : devices) arr.push(deviceStateJson(*d));
            result.set("devices", arr);
        } else if (cmd == "device.state") {
            const std::string target = req.getString("device_name", "");
            if (!target.empty()) {
                DeviceCtx* found = nullptr;
                for (auto& d : devices) {
                    if (d->name == target) {
                        found = d.get();
                        break;
                    }
                }
                if (!found) {
                    error = "未知设备: " + target;
                    errorCode = "unknown_device";
                } else {
                    result = deviceStateJson(*found);
                }
            } else {
                auto arr = json::Value::array();
                for (const auto& d : devices) arr.push(deviceStateJson(*d));
                result.set("devices", arr);
            }
        } else if (cmd == "mem.regions") {
            auto arr = json::Value::array();
            for (const auto& r : model.regions) {
                auto obj = json::Value::object();
                obj.set("name", r.name);
                obj.set("space", r.space);
                obj.set("base", static_cast<uint64_t>(r.base));
                obj.set("size", static_cast<uint64_t>(r.size));
                obj.set("mmio", r.is_mmio);
                arr.push(obj);
            }
            result.set("regions", arr);
        } else if (cmd == "breakpoint.enable" ||
                   cmd == "breakpoint.disable") {
            const int64_t addr = readAddr(req, "addr");
            if (addr < 0 ||
                bpEnable(static_cast<uint32_t>(addr),
                         cmd == "breakpoint.enable") != 0) {
                error = "断点不存在";
                errorCode = "not_found";
            } else {
                mcuStatus(result);
            }
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
            bpHitCount.clear();
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
        } else if (cmd == "mem.read") {
            const std::string space = req.getString("space", "");
            const int64_t addr = readAddr(req, "addr");
            const int64_t length = req.getInt("length", 0);
            if (addr < 0 || length <= 0) {
                error = "需要 space/addr/length";
                errorCode = "out_of_range";
            } else {
                std::string hex;
                uint32_t actual = 0;
                bool inRange = false;
                std::string dumpError;
                std::string dumpCode;
                if (mcuDump(space, static_cast<uint32_t>(addr),
                            static_cast<uint32_t>(length), hex, actual,
                            inRange, dumpError, dumpCode) != 0) {
                    error = dumpError;
                    errorCode = dumpCode;
                } else {
                    result.set("space", space);
                    result.set("addr", static_cast<uint64_t>(addr));
                    result.set("requested_length",
                               static_cast<uint64_t>(length));
                    result.set("length", static_cast<uint64_t>(actual));
                    result.set("actual_length",
                               static_cast<uint64_t>(actual));
                    result.set("truncated",
                               static_cast<bool>(inRange && actual < length));
                    result.set("in_range", inRange);
                    result.set("encoding", "hex");
                    result.set("data", hex);
                }
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

        response = makeResponse(req, error.empty(), result, error, errorCode,
                                "mcu");
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
                                         "未知设备: " + device, "unknown_device",
                                         device);
            writeJsonLine(response);
            return false;
        }

        const std::string cmd = req.getString("cmd", "");
        if (cmd == "describe") {
            auto response = makeResponse(req, true, deviceDescribeResult(*target),
                                         "", "error", device);
            writeJsonLine(response);
            return true;
        }

        std::string payload = "{}";
        if (req.has("data")) {
            payload = json::dump(req.get("data").value(), -1);
        }
        HpmDebugEvent ev{};
        ev.type = HPM_DEBUG_EVENT_JSON;
        ev.u.json = payload.c_str();
        int rc = 0;
        {
            std::lock_guard<std::recursive_mutex> pluginLock(pluginMutex);
            if (target->ctx && target->lib->event) {
                rc = target->lib->event(target->ctx, &ev);
            }
        }
        if (req.has("id")) {
            auto result = json::Value::object();
            result.set("applied", rc == 0);
            auto response = makeResponse(
                req, rc == 0, result,
                rc == 0 ? "" : "设备拒绝输入", "device_error", device);
            writeJsonLine(response);
        }
        return rc == 0;
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
        writeLine("pins | regs | mem <space> <addr> <len>");
        writeLine("devices | send <device> <json> | quit");
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
                for (const auto& [a, enabled] : breakpoints) {
                    writeLine("0x" + std::to_string(a) +
                              (enabled ? "" : " (disabled)"));
                }
            } else if (cmd == "pins") {
                printPins();
            } else if (cmd == "regs") {
                printRegs();
            } else if (cmd == "devices") {
                printDevices();
            } else if (cmd == "mem") {
                std::string space;
                std::string addrStr;
                std::string lenStr;
                ss >> space >> addrStr >> lenStr;
                if (space.empty() || addrStr.empty() || lenStr.empty()) {
                    writeLine("用法: mem <space> <addr> <len>");
                    continue;
                }
                const uint32_t addr = static_cast<uint32_t>(
                    std::strtoul(addrStr.c_str(), nullptr, 0));
                const uint32_t len = static_cast<uint32_t>(
                    std::strtoul(lenStr.c_str(), nullptr, 0));
                std::string hex;
                uint32_t actual = 0;
                bool inRange = false;
                std::string error;
                std::string errorCode;
                if (mcuDump(space, addr, len, hex, actual, inRange, error,
                            errorCode) != 0) {
                    writeLine("dump 失败 [" + errorCode + "]: " + error);
                } else if (!inRange) {
                    writeLine("地址不在空间 " + space + " 内");
                } else {
                    writeLine(space + ":" + addrStr + " (" +
                              std::to_string(actual) + " bytes)");
                    for (size_t i = 0; i < hex.size(); i += 32) {
                        writeLine(hex.substr(i, 32));
                    }
                }
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
        busRouter = std::make_unique<BusRouter>(
            events.get(), [this](const HpmDebugEvent& out) {
                broadcastPluginEvent(out);
            });
        pinRouter = std::make_unique<PinRouter>(
            [this](uint32_t id, uint32_t changed) {
                return pinInfoJson(id, changed);
            },
            [this](const json::Value& out) {
                if (opts.jsonMode) writeJsonLine(out);
            },
            [this] { return static_cast<uint64_t>(readVirtualTime()); });
        if (!createPlugins()) return 1;

        ready = true;
        stopReason = "pause";
        stopAddr = readPc();
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
