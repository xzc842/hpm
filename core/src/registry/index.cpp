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

// core/src/registry/index.cpp
#include <hpm/registry/index.h>

#include <hpm/json/json_io.h>

namespace hpm::registry {

namespace {

std::string replaceBaseUrl(const std::string& url, const std::string& baseUrl) {
    if (baseUrl.empty()) return url;
    const std::string placeholder = "{baseurl}";
    auto pos = url.find(placeholder);
    if (pos == std::string::npos) return url;
    std::string result = url;
    result.replace(pos, placeholder.size(), baseUrl);
    return result;
}

} // namespace

const PackageVersion* Index::find(const std::string& name,
                                   const std::string& version) const {
    auto it = packages.find(name);
    if (it == packages.end()) return nullptr;
    auto vit = it->second.versions.find(version);
    if (vit == it->second.versions.end()) return nullptr;
    return &vit->second;
}

const PackageVersion* Index::findLatest(const std::string& name) const {
    auto it = packages.find(name);
    if (it == packages.end()) return nullptr;
    if (it->second.versions.empty()) return nullptr;

    const PackageVersion* best = nullptr;
    Version bestVer;
    for (const auto& [ver, info] : it->second.versions) {
        auto v = Version::parse(ver);
        if (!v) continue;
        if (!best || *v > bestVer) {
            best = &info;
            bestVer = *v;
        }
    }
    return best;
}

std::string Index::findLatestVersion(const std::string& name) const {
    auto it = packages.find(name);
    if (it == packages.end()) return {};
    if (it->second.versions.empty()) return {};

    std::string best;
    Version bestVer;
    for (const auto& [ver, _] : it->second.versions) {
        auto v = Version::parse(ver);
        if (!v) continue;
        if (best.empty() || *v > bestVer) {
            best = ver;
            bestVer = *v;
        }
    }
    return best;
}

Result<Index> parseIndex(const std::string& jsonText,
                         const std::string& baseUrl) {
    auto j = json::parse(jsonText);
    if (!j) return j.error();
    if (!j->isObject()) {
        return Error(ErrorCode::InvalidField, "index.json 顶层必须是对象");
    }

    Index idx;
    idx.schema = static_cast<int>(j->getInt("schema", 2));
    if (idx.schema < 2) {
        return Error(ErrorCode::InvalidField,
                     "index.json schema 必须 >= 2（证书模式）");
    }
    idx.baseUrl = baseUrl;
    idx.officialCert = replaceBaseUrl(j->getString("official_cert"), baseUrl);

    // developers
    auto devsJson = j->get("developers");
    if (devsJson && devsJson->isObject()) {
        auto devKeys = devsJson->keys();
        if (devKeys) {
            for (const auto& devName : *devKeys) {
                auto dvJson = devsJson->get(devName);
                if (!dvJson || !dvJson->isObject()) continue;

                DeveloperInfo di;
                di.cert = replaceBaseUrl(dvJson->getString("cert"), baseUrl);
                idx.developers[devName] = std::move(di);
            }
        }
    }

    // domains
    auto domainsJson = j->get("domains");
    if (domainsJson && domainsJson->isObject()) {
        auto domKeys = domainsJson->keys();
        if (domKeys) {
            for (const auto& domName : *domKeys) {
                auto dvJson = domainsJson->get(domName);
                if (!dvJson || !dvJson->isObject()) continue;

                DomainInfo di;
                di.index = replaceBaseUrl(dvJson->getString("index"), baseUrl);
                di.cert  = replaceBaseUrl(dvJson->getString("cert"), baseUrl);
                idx.domains[domName] = std::move(di);
            }
        }
    }

    // packages
    auto pkgs = j->get("packages");
    if (!pkgs) {
        return Error(ErrorCode::MissingField, "index.json 缺少 packages");
    }
    if (!pkgs->isObject()) {
        return Error(ErrorCode::InvalidField, "packages 必须是对象");
    }

    auto names = pkgs->keys();
    if (!names) return names.error();
    for (const auto& name : *names) {
        auto pkgJson = pkgs->get(name);
        if (!pkgJson) return pkgJson.error();

        PackageInfo info;

        auto versionsJson = pkgJson->get("versions");
        if (!versionsJson) continue;
        if (!versionsJson->isObject()) continue;

        auto vers = versionsJson->keys();
        if (!vers) continue;
        for (const auto& ver : *vers) {
            auto vJson = versionsJson->get(ver);
            if (!vJson) continue;

            PackageVersion pv;
            pv.kind        = vJson->getString("kind", "driver");
            pv.chip        = vJson->getString("chip");
            pv.developer   = vJson->getString("developer");
            pv.url         = replaceBaseUrl(vJson->getString("url"), baseUrl);
            pv.sigUrl      = replaceBaseUrl(vJson->getString("sig_url"), baseUrl);
            pv.sha256      = vJson->getString("sha256");
            pv.size        = static_cast<uint64_t>(vJson->getInt("size", 0));
            pv.license     = vJson->getString("license");
            pv.description = vJson->getString("description");

            if (auto v = vJson->get("config"); v && v->isObject()) {
                pv.configJson = json::dump(*v, -1);
            }
            if (auto v = vJson->get("provides"); v && v->isObject()) {
                pv.providesJson = json::dump(*v, -1);
            }
            if (auto v = vJson->get("pins"); v && v->isObject()) {
                pv.pinsJson = json::dump(*v, -1);
            }

            auto reqJson = vJson->get("requires");
            if (reqJson && reqJson->isObject()) {
                auto reqKeys = reqJson->keys();
                if (reqKeys) {
                    for (const auto& rk : *reqKeys) {
                        auto rv = reqJson->get(rk);
                        if (rv && rv->isObject()) {
                            pv.requirements[rk] = rv->getString("type");
                        }
                    }
                }
            }

            info.versions[ver] = std::move(pv);
        }

        idx.packages[name] = std::move(info);
    }

    return idx;
}

} // namespace hpm::registry