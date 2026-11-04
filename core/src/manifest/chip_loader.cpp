// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// core/src/manifest/chip_loader.cpp
#include <hpm/manifest/chip_loader.h>

#include <hpm/json/json_io.h>
#include <hpm/manifest/field_reader.h>

namespace hpm::manifest {

namespace {

Result<PinCapability> loadCapability(const json::Value& obj,
                                     const std::string& ctx) {
    FieldReader r(obj, ctx);
    PinCapability cap;
    auto type = r.requireString("type");
    if (!type) return type.error();
    cap.type = *type;
    cap.instance = r.optString("instance");
    cap.channel = static_cast<int>(r.optInt("channel", 0));
    return cap;
}

Result<Pin> loadPin(const std::string& name, const json::Value& obj) {
    FieldReader r(obj, "pins." + name);
    Pin p;
    p.name = name;
    p.shareable = r.optBool("shareable", false);

    auto capsJson = r.optArray("capabilities");
    if (!capsJson) return capsJson.error();
    for (size_t i = 0; i < capsJson->size(); ++i) {
        auto cap = loadCapability((*capsJson)[i],
                                   "pins." + name + ".capabilities[" +
                                       std::to_string(i) + "]");
        if (!cap) return cap.error();
        p.capabilities.push_back(std::move(*cap));
    }
    return p;
}

Result<ToolchainSpec> loadToolchain(const json::Value& obj) {
    FieldReader r(obj, "toolchain");
    ToolchainSpec spec;

    // name 可选（兼容旧格式）
    spec.name = r.optString("name");
    spec.prefix = r.optString("prefix");
    spec.cc = r.optString("cc");
    spec.objcopy = r.optString("objcopy");
    spec.size = r.optString("size");

    // 至少要有一个标识
    if (spec.name.empty() && spec.prefix.empty() && spec.cc.empty()) {
        return Error(ErrorCode::MissingField,
                     "toolchain 需要 name、prefix 或 cc 之一");
    }

    auto ver = r.optString("version");
    if (!ver.empty()) {
        auto range = VersionRange::parse(ver);
        if (!range) return range.error();
        spec.version = *range;
    }
    spec.url = r.optString("url");
    return spec;
}

Result<FlashConfig> loadFlash(const json::Value& obj) {
    FieldReader r(obj, "flash");
    FlashConfig fc;
    auto tool = r.requireString("tool");
    if (!tool) return tool.error();
    fc.tool = *tool;

    auto argsJson = r.optArray("args");
    if (!argsJson) return argsJson.error();
    for (const auto& a : *argsJson) {
        auto s = a.asString();
        if (!s) {
            return Error(ErrorCode::InvalidField,
                         "flash.args 元素应为字符串");
        }
        fc.args.push_back(*s);
    }
    fc.baseAddress = r.optString("base_address");
    return fc;
}

Result<ChipBuildConfig> loadBuild(const json::Value& obj) {
    FieldReader r(obj, "build");
    ChipBuildConfig bc;

    bc.cpu = r.optString("cpu");
    bc.ldscript = r.optString("ldscript");
    bc.startup = r.optString("startup");

    auto flagsJson = r.optArray("flags");
    if (!flagsJson) return flagsJson.error();
    for (const auto& f : *flagsJson) {
        auto s = f.asString();
        if (!s) {
            return Error(ErrorCode::InvalidField,
                         "build.flags 元素应为字符串");
        }
        bc.flags.push_back(*s);
    }
    return bc;
}

Result<ProvidedMethod> loadProvidedMethod(const json::Value& v,
                                          const std::string& ctx) {
    if (v.isString()) {
        auto s = v.asString();
        if (!s) return s.error();
        ProvidedMethod m;
        m.name = *s;
        return m;
    }
    if (!v.isObject()) {
        return Error(ErrorCode::InvalidField, ctx + " 应为字符串或对象");
    }
    FieldReader r(v, ctx);
    ProvidedMethod m;
    auto name = r.requireString("name");
    if (!name) return name.error();
    m.name = *name;
    m.returnType = r.optString("return", "int");

    auto paramsJson = r.optArray("params");
    if (!paramsJson) return paramsJson.error();
    for (size_t i = 0; i < paramsJson->size(); ++i) {
        auto p = (*paramsJson)[i];
        if (!p.isObject()) {
            return Error(ErrorCode::InvalidField,
                         ctx + ".params[" + std::to_string(i) + "] 必须是对象");
        }
        FieldReader pr(p, ctx + ".params[" + std::to_string(i) + "]");
        MethodParam mp;
        auto type = pr.requireString("type");
        if (!type) return type.error();
        mp.type = *type;
        mp.name = pr.optString("name");
        m.params.push_back(std::move(mp));
    }
    return m;
}

} // namespace

Result<Chip> loadChip(const json::Value& json) {
    if (!json.isObject()) {
        return Error(ErrorCode::InvalidField, "芯片清单顶层必须是对象");
    }

    FieldReader r(json);

    Chip chip;

    auto name = r.requireString("name");
    if (!name) return name.error();
    chip.name = *name;

    chip.family = r.optString("family");
    chip.package = r.optString("package");
    chip.qemuMachine = r.optString("qemu_machine");
    chip.license = r.optString("license"); 

    auto tcJson = r.requireObject("toolchain");
    if (!tcJson) return tcJson.error();
    auto tc = loadToolchain(*tcJson);
    if (!tc) return tc.error();
    chip.toolchain = std::move(*tc);

    auto flashJson = r.requireObject("flash");
    if (!flashJson) return flashJson.error();
    auto fc = loadFlash(*flashJson);
    if (!fc) return fc.error();
    chip.flash = std::move(*fc);

    // build（可选，默认空）
    if (json.has("build")) {
        auto bcJson = r.requireObject("build");
        if (!bcJson) return bcJson.error();
        auto bc = loadBuild(*bcJson);
        if (!bc) return bc.error();
        chip.build = std::move(*bc);
    }

    // provides（可选）：芯片驱动提供的方法，如 init
    auto provJson = r.optObject("provides");
    if (provJson && provJson->isObject()) {
        auto methodsJson = provJson->get("methods");
        if (methodsJson && methodsJson->isArray()) {
            auto n = methodsJson->size();
            if (!n) return n.error();
            for (size_t i = 0; i < *n; ++i) {
                auto item = methodsJson->at(i);
                if (!item) return item.error();
                auto m = loadProvidedMethod(
                    *item, "provides.methods[" + std::to_string(i) + "]");
                if (!m) return m.error();
                chip.provides.push_back(std::move(*m));
            }
        }
    }

    auto pinsJson = r.requireObject("pins");
    if (!pinsJson) return pinsJson.error();
    FieldReader pr(*pinsJson, "pins");
    auto keys = pr.keys();
    if (!keys) return keys.error();
    for (const auto& pname : *keys) {
        auto obj = pinsJson->get(pname);
        if (!obj) return obj.error();
        if (!obj->isObject()) {
            return Error(ErrorCode::InvalidField,
                         "pins." + pname + " 必须是对象");
        }
        auto p = loadPin(pname, *obj);
        if (!p) return p.error();
        chip.pins.push_back(std::move(*p));
    }

    return chip;
}

Result<Chip> loadChipFile(const std::filesystem::path& path) {
    auto json = json::parseFile(path);
    if (!json) return json.error();
    return loadChip(*json);
}

} // namespace hpm::manifest