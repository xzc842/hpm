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

// core/include/hpm/capability/driver_library.h
#pragma once

#include <hpm/model/driver_manifest.h>
#include <hpm/result.h>

#include <map>
#include <string>
#include <vector>

namespace hpm {

// 驱动清单集合：按 "<driver>-<chip>" 索引
class DriverLibrary {
public:
    void add(DriverManifest manifest) {
        manifests_[manifest.name] = std::move(manifest);
    }

    // 精确匹配
    const DriverManifest* find(const DriverName& name) const {
        auto it = manifests_.find(name);
        return it == manifests_.end() ? nullptr : &it->second;
    }

    // 带芯片自动补全：
    //   1. 精确 name
    //   2. <name>-<mcu>
    //   3. <name>-*
    //   4. 域包 domain/<base> 同样回退
    const DriverManifest* find(const DriverName& name,
                               const std::string& mcu) const {
        if (const auto* d = find(name)) return d;
        if (mcu.empty()) return nullptr;

        if (const auto* d = find(name + "-" + mcu)) return d;
        if (const auto* d = find(name + "-*")) return d;

        auto slash = name.find('/');
        if (slash != std::string::npos) {
            auto domain = name.substr(0, slash);
            auto base = name.substr(slash + 1);
            if (const auto* d = find(domain + "/" + base + "-" + mcu)) return d;
            if (const auto* d = find(domain + "/" + base + "-*")) return d;
        }
        return nullptr;
    }

    bool has(const DriverName& name) const {
        return manifests_.count(name) > 0;
    }

    size_t size() const noexcept { return manifests_.size(); }

    const std::map<DriverName, DriverManifest>& all() const noexcept {
        return manifests_;
    }

private:
    std::map<DriverName, DriverManifest> manifests_;
};

} // namespace hpm