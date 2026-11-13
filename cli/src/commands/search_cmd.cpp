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

// cli/src/commands/search_cmd.cpp
#include "search_cmd.h"

#include "../cmd_context.h"
#include "../output.h"
#include "download_progress.h"

#include <hpm/json/json_io.h>
#include <hpm/registry/registry_client.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace hpm::cli {

namespace {

void printValue(const std::string& label, const std::string& value) {
    if (!value.empty()) Output::detail(label + ": " + value);
}

void printConfig(const std::string& text) {
    if (text.empty()) return;
    auto j = json::parse(text);
    if (!j || !j->isObject()) return;
    auto keys = j->keys();
    if (!keys || keys->empty()) return;

    Output::detail("配置:");
    for (const auto& key : *keys) {
        auto item = j->get(key);
        if (!item) continue;

        std::string value;
        std::string type;
        if (item->isObject()) {
            auto def = item->get("default");
            type = item->getString("type");
            if (def) {
                const bool numeric = type == "int" || type == "uint" ||
                                     type == "float";
                if (numeric && def->isString()) {
                    value = *def->asString();
                } else {
                    value = json::dump(*def, -1);
                }
            }
        } else {
            value = json::dump(*item, -1);
        }

        std::string line = "  " + key + ": " + value;
        if (!type.empty()) line += " (" + type + ")";
        Output::detail(line);
    }
}

void printProvides(const std::string& text) {
    if (text.empty()) return;
    auto j = json::parse(text);
    if (!j || !j->isObject()) return;
    auto methods = j->get("methods");
    if (!methods || !methods->isArray()) return;

    Output::detail("接口:");
    auto n = methods->size();
    for (size_t i = 0; i < (n ? *n : 0); ++i) {
        auto m = methods->at(i);
        if (!m || !m->isObject()) continue;

        const std::string ret = m->getString("return", "int");
        const std::string name = m->getString("name");
        if (name.empty()) continue;

        std::string sig = "  " + ret + " " + name + "(";
        auto params = m->get("params");
        bool first = true;
        if (params && params->isArray()) {
            auto pn = params->size();
            for (size_t k = 0; k < (pn ? *pn : 0); ++k) {
                auto p = params->at(k);
                if (!p || !p->isObject()) continue;
                if (!first) sig += ", ";
                first = false;
                sig += p->getString("type");
                const std::string pname = p->getString("name");
                if (!pname.empty()) sig += " " + pname;
            }
        }
        if (first) sig += "void";
        sig += ")";
        Output::detail(sig);
    }
}

void printPins(const std::string& text) {
    if (text.empty()) return;
    auto j = json::parse(text);
    if (!j || !j->isObject()) return;
    auto keys = j->keys();
    if (!keys || keys->empty()) return;

    Output::detail("引脚:");
    for (const auto& pin : *keys) {
        auto info = j->get(pin);
        if (!info || !info->isObject()) continue;

        std::string caps;
        auto arr = info->get("capabilities");
        if (arr && arr->isArray()) {
            auto n = arr->size();
            for (size_t i = 0; i < (n ? *n : 0); ++i) {
                auto cap = arr->at(i);
                if (!cap || !cap->isObject()) continue;
                const std::string type = cap->getString("type");
                if (type.empty()) continue;
                if (!caps.empty()) caps += ", ";
                caps += type;
            }
        }

        Output::detail("  " + pin + ": " + caps);
    }
}


std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

size_t levenshtein(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1);
    std::vector<size_t> cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;

    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

// 返回 0 表示不相似；分数越大越相似。
int similarity(const std::string& query, const std::string& key) {
    const std::string q = lower(query);
    const std::string k = lower(key);
    if (q.empty()) return 0;

    if (q == k) return 10000;

    if (k.rfind(q, 0) == 0) {
        return 1000 - static_cast<int>(std::min<size_t>(k.size() - q.size(), 99));
    }

    const auto pos = k.find(q);
    if (pos != std::string::npos) {
        return 700 - static_cast<int>(std::min<size_t>(pos, 99));
    }

    if (q.rfind(k, 0) == 0) return 650;

    const size_t dist = levenshtein(q, k);
    if (dist <= 2) return 400 - static_cast<int>(dist * 50);

    return 0;
}

void printExact(const registry::PackageVersion& pv,
                const std::string& name, const std::string& version) {
    Output::step("包信息");
    printValue("名称", name);
    printValue("版本", version);
    printValue("类型", pv.kind);
    printValue("芯片", pv.chip);
    printValue("开发者", pv.developer);
    printValue("许可证", pv.license);
    if (pv.size > 0) {
        Output::detail("大小: " + std::to_string(pv.size) + " bytes");
    }
    printValue("SHA256", pv.sha256);
    printValue("描述", pv.description);
    printValue("下载", pv.url);
    printValue("签名", pv.sigUrl);

    if (!pv.requirements.empty()) {
        Output::detail("依赖:");
        for (const auto& [key, type] : pv.requirements) {
            Output::detail("  " + key + ": " + type);
        }
    }

    printConfig(pv.configJson);
    printProvides(pv.providesJson);
    printPins(pv.pinsJson);
}

json::Value summaryJson(const std::string& name, const std::string& version,
                        const registry::PackageVersion& pv) {
    auto obj = json::Value::object();
    obj.set("name", name);
    obj.set("version", version);
    obj.set("kind", pv.kind);
    obj.set("chip", pv.chip);
    obj.set("developer", pv.developer);
    obj.set("license", pv.license);
    obj.set("size", static_cast<int64_t>(pv.size));
    obj.set("sha256", pv.sha256);
    obj.set("url", pv.url);
    obj.set("sig_url", pv.sigUrl);
    obj.set("description", pv.description);
    return obj;
}

struct Match {
    std::string name;
    std::string version;
    const registry::PackageVersion* pv = nullptr;
    int score = 0;
};

std::vector<Match> findSimilar(const registry::Index& idx,
                               const std::string& query) {
    std::vector<Match> out;
    for (const auto& [name, info] : idx.packages) {
        const int score = similarity(query, name);
        if (score <= 0) continue;

        const std::string version = idx.findLatestVersion(name);
        if (version.empty()) continue;

        const auto* pv = idx.find(name, version);
        if (!pv) continue;

        out.push_back({name, version, pv, score});
    }

    std::sort(out.begin(), out.end(), [](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });

    if (out.size() > 20) out.resize(20);
    return out;
}

} // namespace

void SearchCmd::setup(CLI::App& app) {
    app.add_option("package", spec_,
                   "包名/关键字，或 包名@版本号；支持 domain/包名")
        ->required();
    app.add_flag("--json", json_, "以 JSON 输出");
    app.add_flag("--skip-signature", skipSignature_, "跳过索引签名验证（仅开发）");
}

int SearchCmd::run(CmdContext& ctx) {
    std::string fullName = spec_;
    std::string version;

    auto at = spec_.rfind('@');
    if (at != std::string::npos) {
        fullName = spec_.substr(0, at);
        version = spec_.substr(at + 1);
    }

    if (fullName.empty()) {
        Output::error("包名不能为空");
        return 1;
    }

    registry::RegistryConfig cfg = registry::RegistryConfig::load();
    cfg.cacheDir = ctx.cacheDir();
    cfg.skipSignature = skipSignature_;
    DownloadProgressDisplay progress;
    cfg.transferObserver = [&](const registry::TransferEvent& ev) {
        progress.update(ev);
    };

    registry::RegistryClient client(cfg);

    std::string domain;
    std::string name;
    registry::RegistryClient::splitName(fullName, domain, name);

    const registry::Index* idx = nullptr;
    std::optional<registry::Index> mainIdx;

    if (domain.empty()) {
        auto fetched = client.fetchIndex();
        if (!fetched) {
            Output::error(fetched.error().message());
            return 1;
        }
        mainIdx = std::move(*fetched);
        idx = &*mainIdx;
    } else {
        auto fetched = client.fetchDomainIndex(domain);
        if (!fetched) {
            Output::error(fetched.error().message());
            return 1;
        }
        idx = *fetched;
    }

    const bool hasExactName = idx->packages.count(name) > 0;

    if (hasExactName && !version.empty()) {
        const auto* pv = idx->find(name, version);
        if (!pv) {
            Output::error("找不到版本: " + name + "@" + version);
            const auto latest = idx->findLatestVersion(name);
            if (!latest.empty()) Output::detail("最新版本: " + latest);
            return 1;
        }

        if (json_) {
            auto requiresJson = json::Value::object();
            for (const auto& [key, type] : pv->requirements) {
                requiresJson.set(key, type);
            }
            auto out = summaryJson(name, version, *pv);
            out.set("requires", requiresJson);
            if (auto j = json::parse(pv->configJson); j) out.set("config", *j);
            if (auto j = json::parse(pv->providesJson); j) out.set("provides", *j);
            if (auto j = json::parse(pv->pinsJson); j) out.set("pins", *j);
            std::cout << json::dump(out, -1) << "\n";
            return 0;
        }

        printExact(*pv, name, version);
        return 0;
    }

    if (hasExactName) {
        version = idx->findLatestVersion(name);
        const auto* pv = idx->find(name, version);
        if (!pv) {
            Output::error("包没有可用版本: " + name);
            return 1;
        }

        if (json_) {
            auto requiresJson = json::Value::object();
            for (const auto& [key, type] : pv->requirements) {
                requiresJson.set(key, type);
            }
            auto out = summaryJson(name, version, *pv);
            out.set("requires", requiresJson);
            if (auto j = json::parse(pv->configJson); j) out.set("config", *j);
            if (auto j = json::parse(pv->providesJson); j) out.set("provides", *j);
            if (auto j = json::parse(pv->pinsJson); j) out.set("pins", *j);
            std::cout << json::dump(out, -1) << "\n";
            return 0;
        }

        printExact(*pv, name, version);
        return 0;
    }

    // 没有精确包名：相似匹配
    auto matches = findSimilar(*idx, name);
    if (matches.empty()) {
        Output::error("找不到包: " + name);
        return 1;
    }

    if (json_) {
        auto arr = json::Value::array();
        for (const auto& m : matches) {
            arr.push(summaryJson(m.name, m.version, *m.pv));
        }
        auto out = json::Value::object();
        out.set("query", name);
        out.set("exact", false);
        out.set("matches", arr);
        std::cout << json::dump(out, -1) << "\n";
        return 0;
    }

    Output::step("相似包");
    Output::detail("未找到精确匹配: " + name);
    for (const auto& m : matches) {
        std::string line = m.name + "@" + m.version;
        if (!m.pv->kind.empty()) line += "  [" + m.pv->kind + "]";
        if (!m.pv->chip.empty()) line += "  chip=" + m.pv->chip;
        if (!m.pv->developer.empty()) line += "  by " + m.pv->developer;
        if (!m.pv->description.empty()) line += "  " + m.pv->description;
        Output::detail("  " + line);
    }
    Output::detail("查看详情: hpm search <包名>");

    return 0;
}

} // namespace hpm::cli
