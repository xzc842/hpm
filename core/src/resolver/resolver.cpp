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

// core/src/resolver/resolver.cpp
#include <hpm/resolver/resolver.h>

#include <hpm/resolver/id_generator.h>

#include <algorithm>

namespace hpm {

namespace {

bool samePins(const std::map<ResourceKey, std::string>& a,
              const std::map<ResourceKey, std::string>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end() || it->second != v) return false;
    }
    return true;
}

// existing_.driver 可能是 "rc522"、"rc522-ch552g"、"rc522-*"
// 入参 driver 是 project.toml 里的裸名（"rc522"）
bool driverMatches(const std::string& existingDriver,
                   const std::string& bareName,
                   const std::string& mcu) {
    if (existingDriver == bareName) return true;
    if (!mcu.empty()) {
        if (existingDriver == bareName + "-" + mcu) return true;
        if (existingDriver == bareName + "-*") return true;
    }
    return false;
}

} // namespace

Result<DriverState> Resolver::resolve() {
    DriverState out;

    for (const auto& dev : project_.devices) {
        auto node = expandUserDevice(dev);
        if (!node) return node.error();

        auto st = buildState(**node, out);
        if (!st.ok()) return st.error();
    }

    return out;
}

Result<std::unique_ptr<ResolvedNode>>
Resolver::expandUserDevice(const Device& dev) {
    const DriverManifest* dm = lib_.find(dev.driver, project_.mcu);
    if (!dm) {
        return Error(ErrorCode::NotFound,
                     "找不到驱动: " + dev.driver + "（设备 " + dev.name + "）");
    }

    auto node = std::make_unique<ResolvedNode>();
    node->driver = dm->name;                       // 带 chip
    node->version = lockVersion(dev.driver);
    node->instanceName = dev.name;
    node->isTopLevel = true;
    node->config = dev.config;

    std::vector<std::string> visited = {dm->name};
    std::set<std::string>    consumed;

    auto st = expandRequirements(*dm, dev, dev.name, visited, consumed, *node);
    if (!st.ok()) return st.error();

    for (const auto& [k, v] : dev.bindings) {
        if (consumed.count(k)) continue;
        node->pins[k] = v;
    }

    return node;
}

Status Resolver::expandRequirements(const DriverManifest& dm,
                                    const Device& topDevice,
                                    const std::string& topName,
                                    std::vector<std::string>& visited,
                                    std::set<std::string>& consumed,
                                    ResolvedNode& node) {
    for (const auto& req : dm.requirements) {
        const DriverManifest* childDm = lib_.find(req.type, project_.mcu);

        if (childDm) {
            if (std::find(visited.begin(), visited.end(), childDm->name) != visited.end()) {
                return Error(ErrorCode::CyclicDependency,
                             "驱动循环依赖: " + topName + " → " + childDm->name);
            }

            visited.push_back(childDm->name);
            auto child = expandChild(*childDm, topDevice, topName,
                                     req.key, visited, consumed);
            visited.pop_back();

            if (!child) return child.error();
            node.children[req.key] = std::move(*child);

        } else {
            auto it = topDevice.bindings.find(req.key);
            if (it == topDevice.bindings.end()) {
                if (req.optional) continue;
                return Error(ErrorCode::MissingField,
                             "设备 " + topName + " 缺少必填字段 " + req.key +
                                 "（驱动 " + dm.name + " 需要）");
            }
            if (consumed.count(req.key)) {
                return Error(ErrorCode::NameConflict,
                             "设备 " + topName + " 的字段 " + req.key +
                                 " 被多个驱动同时需要");
            }
            consumed.insert(req.key);
            node.pins[req.key] = it->second;
        }
    }
    return {};
}

Result<std::unique_ptr<ResolvedNode>>
Resolver::expandChild(const DriverManifest& childDm,
                      const Device& topDevice,
                      const std::string& topName,
                      const std::string& parentKey,
                      std::vector<std::string>& visited,
                      std::set<std::string>& consumed) {
    auto child = std::make_unique<ResolvedNode>();
    child->driver = childDm.name;                  // 带 chip
    child->version = lockVersion(childDm.name);
    child->isTopLevel = false;

    for (const auto& subReq : childDm.requirements) {
        const DriverManifest* subChildDm = lib_.find(subReq.type, project_.mcu);

        if (subChildDm) {
            if (std::find(visited.begin(), visited.end(), subChildDm->name) != visited.end()) {
                return Error(ErrorCode::CyclicDependency,
                             "驱动循环依赖: " + topName + " → " + subChildDm->name);
            }
            visited.push_back(subChildDm->name);
            auto sub = expandChild(*subChildDm, topDevice, topName,
                                   subReq.key, visited, consumed);
            visited.pop_back();
            if (!sub) return sub.error();
            child->children[subReq.key] = std::move(*sub);
        } else {
            auto it = topDevice.bindings.find(subReq.key);
            if (it == topDevice.bindings.end()) {
                if (subReq.optional) continue;
                return Error(ErrorCode::MissingField,
                             "设备 " + topName + " 缺少引脚 " + subReq.key +
                                 "（被 " + childDm.name + " 需要，由 " +
                                 parentKey + " 引入）");
            }
            if (consumed.count(subReq.key)) {
                return Error(ErrorCode::NameConflict,
                             "设备 " + topName + " 的字段 " + subReq.key +
                                 " 被多个驱动同时需要");
            }
            consumed.insert(subReq.key);
            child->pins[subReq.key] = it->second;
        }
    }

    if (const std::string* name = findIntermediateName(childDm.name, child->pins)) {
        child->instanceName = *name;
        return child;
    }

    const IntermediateDevice* reuse =
        findExistingIntermediate(childDm.name, child->pins);
    if (reuse) {
        child->instanceName = reuse->name;
        rememberIntermediate(childDm.name, child->pins, reuse->name);
        return child;
    }

    child->instanceName = generateIntermediateId(childDm.name);
    rememberIntermediate(childDm.name, child->pins, child->instanceName);

    return child;
}

Status Resolver::buildState(const ResolvedNode& root, DriverState& out) {
    Device userDev;
    userDev.name = root.instanceName;
    userDev.driver = root.driver;                  // 带 chip
    userDev.version = root.version;
    userDev.userDeclared = true;
    userDev.config = root.config;

    for (const auto& [k, v] : root.pins) {
        userDev.bindings[k] = v;
    }

    for (const auto& [key, childPtr] : root.children) {
        userDev.bindings[key] = childPtr->instanceName;
    }

    out.userDevices.push_back(std::move(userDev));

    for (const auto& [key, childPtr] : root.children) {
        auto st = buildIntermediate(*childPtr, out);
        if (!st.ok()) return st.error();
    }

    return {};
}

Status Resolver::buildIntermediate(const ResolvedNode& node,
                                   DriverState& out) {
    IntermediateDevice idev;
    idev.id = node.instanceName;
    idev.name = node.instanceName;
    idev.driver = node.driver;                     // 带 chip
    idev.version = node.version;
    idev.config = node.config;

    for (const auto& [k, v] : node.pins) {
        idev.bindings[k] = v;
    }

    for (const auto& [key, childPtr] : node.children) {
        idev.bindings[key] = childPtr->instanceName;
    }

    bool exists = false;
    for (const auto& e : out.intermediateDevices) {
        if (e.name == idev.name) { exists = true; break; }
    }
    if (!exists) {
        out.intermediateDevices.push_back(std::move(idev));
    }

    for (const auto& [key, childPtr] : node.children) {
        auto st = buildIntermediate(*childPtr, out);
        if (!st.ok()) return st.error();
    }

    return {};
}

const std::string* Resolver::findIntermediateName(
    const std::string& driver,
    const std::map<ResourceKey, std::string>& pins) {
    IntermediateKey key{driver, pins};
    auto it = created_.find(key);
    return it == created_.end() ? nullptr : &it->second;
}

void Resolver::rememberIntermediate(
    const std::string& driver,
    const std::map<ResourceKey, std::string>& pins,
    const std::string& name) {
    created_[IntermediateKey{driver, pins}] = name;
}

const IntermediateDevice* Resolver::findExistingIntermediate(
    const std::string& driver,
    const std::map<ResourceKey, std::string>& pins) const {
    if (!existing_) return nullptr;
    for (const auto& idev : existing_->intermediateDevices) {
        if (idev.driver != driver) continue;
        if (samePins(idev.bindings, pins)) return &idev;
    }
    return nullptr;
}

Version Resolver::lockVersion(const std::string& driver) const {
    if (existing_) {
        for (const auto& d : existing_->userDevices) {
            if (driverMatches(d.driver, driver, project_.mcu)) return d.version;
        }
        for (const auto& d : existing_->intermediateDevices) {
            if (driverMatches(d.driver, driver, project_.mcu)) return d.version;
        }
    }
    if (auto* dm = lib_.find(driver, project_.mcu)) {
        return dm->version;
    }
    return Version{};
}

} // namespace hpm