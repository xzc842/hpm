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

// dev/src/commands/publish_cmd.cpp
#include "publish_cmd.h"

#include "../dev_config.h"
#include "../http.h"

#include <hpm/config.h>
#include <hpm/json/json_io.h>
#include <hpm/manifest/chip_loader.h>
#include <hpm/manifest/driver_loader.h>
#include <hpm/registry/signature.h>
#include <hpm/util/process.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <array>

namespace hpm::cli {

namespace fs = std::filesystem;

namespace {

void ok(const std::string& msg) {
    std::cout << "\033[32mok\033[0m " << msg << "\n";
}
void err(const std::string& msg) {
    std::cerr << "\033[31merror\033[0m " << msg << "\n";
}
void info(const std::string& msg) {
    std::cout << "\033[34minfo\033[0m " << msg << "\n";
}
void detail(const std::string& msg) {
    std::cout << "    " << msg << "\n";
}

std::string readAll(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string detectAlgorithm(const std::string& keyPath) {
    std::string content = readAll(keyPath);

    if (content.find("BEGIN ED25519 PRIVATE KEY") != std::string::npos) {
        return "ed25519";
    }
    if (content.find("BEGIN EC PRIVATE KEY") != std::string::npos) {
        return "ec";
    }
    if (content.find("BEGIN RSA PRIVATE KEY") != std::string::npos) {
        return "rsa";
    }

    if (content.find("BEGIN PRIVATE KEY") != std::string::npos) {
        std::string quoted;
        quoted.reserve(keyPath.size() + 2);
        quoted += '\'';
        for (char c : keyPath) {
            if (c == '\'') {
                quoted += "'\\''";
            } else {
                quoted += c;
            }
        }
        quoted += '\'';

        std::string cmd = "openssl pkey -in " + quoted + " -noout -text 2>&1";

        std::string out;
        if (FILE* pipe = popen(cmd.c_str(), "r")) {
            std::array<char, 4096> buf;
            size_t n;
            while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
                out.append(buf.data(), n);
            }
            pclose(pipe);
        }

        if (out.find("ED25519") != std::string::npos) return "ed25519";
        if (out.find("ED448") != std::string::npos) return "ed448";
        if (out.find("ASN1 OID: prime256v1") != std::string::npos) return "ec";
        if (out.find("ASN1 OID: secp384r1") != std::string::npos) return "ec";
        if (out.find("ASN1 OID: secp521r1") != std::string::npos) return "ec";
        if (out.find("Modulus") != std::string::npos) return "rsa";
        if (out.find("RSA") != std::string::npos) return "rsa";
    }

    return "";
}

auto runGit(const fs::path& cwd, const std::vector<std::string>& args,
            bool verbose = false) {
    std::vector<std::string> full;
    full.reserve(args.size() + 2);
    full.push_back("-C");
    full.push_back(cwd.string());
    full.insert(full.end(), args.begin(), args.end());
    return hpm::util::runProcess("git", full, verbose);
}

std::string readLicense(const fs::path& manifestPath,
                        const fs::path& chipPath,
                        const std::string& fallback = "MIT") {
    fs::path p = fs::exists(manifestPath) ? manifestPath : chipPath;
    if (!fs::exists(p)) return fallback;
    auto j = json::parseFile(p);
    if (!j) return fallback;
    auto lic = j->get("license");
    if (lic) {
        auto s = lic->asString();
        if (s.ok()) {
            std::string v = s.value();
            if (!v.empty()) return v;
        }
    }
    return fallback;
}

} // namespace

void PublishCmd::setup(CLI::App& app) {
    app.add_option("path", pkgDir_, "驱动包目录")->required();
    app.add_option("-k,--key", keyPath_, "签名私钥路径（PEM 格式）")->required();
    app.add_option("-o,--out", outDir_, "输出目录（默认 ./publish）");
    app.add_option("-a,--algorithm", algorithm_,
        "签名算法（auto / rsa / ec / ed25519）")
        ->check(CLI::IsMember({"auto", "rsa", "ec", "ed25519"}));
}

int PublishCmd::run(DevContext& ctx) {
    (void)ctx;

    fs::path pkgDir = fs::absolute(pkgDir_);
    if (!fs::exists(pkgDir)) {
        err("目录不存在: " + pkgDir.string());
        return 1;
    }

    // 1. 读 manifest / chip
    std::string name;
    std::string version;
    std::string chip = "*";
    std::string pkgKey;   // === 提到外层，供 commit / push / 同步使用

    auto manifestPath = pkgDir / "manifest.json";
    auto chipPath = pkgDir / "chip.json";
    const bool isChip = fs::exists(chipPath) && !fs::exists(manifestPath);

    if (fs::exists(manifestPath)) {
        auto dm = hpm::manifest::loadDriverManifestFile(manifestPath);
        if (!dm) {
            err("manifest.json: " + dm.error().message());
            return 1;
        }
        name = dm->name;
        version = dm->version.toString();

        auto mj = json::parseFile(manifestPath);
        if (mj) {
            auto c = mj->get("chip");
            if (c && c->isString()) {
                auto s = c->asString();
                if (s.ok() && !s.value().empty()) chip = s.value();
            }
        }
    } else if (fs::exists(chipPath)) {
        auto chipM = hpm::manifest::loadChipFile(chipPath);
        if (!chipM) {
            err("chip.json: " + chipM.error().message());
            return 1;
        }
        name = chipM->name;
        chip = name;

        auto dirName = pkgDir.filename().string();
        auto dash = dirName.rfind('-');
        if (dash == std::string::npos) {
            err("无法从目录名解析版本: " + dirName);
            return 1;
        }
        version = dirName.substr(dash + 1);
    } else {
        err("缺少 manifest.json 或 chip.json");
        return 1;
    }

    pkgKey = isChip ? name : (name + "-" + chip);   // === 外层赋值

    ok("包: " + pkgKey + "@" + version);

    // 2. 目录名：芯片 <chip>-<version>；驱动 <driver>-<chip>-<version>
    auto expectedDirName = isChip
        ? (name + "-" + version)
        : (name + "-" + chip + "-" + version);
    if (pkgDir.filename().string() != expectedDirName) {
        err("包目录名必须是 " + expectedDirName +
            "，实际是 " + pkgDir.filename().string());
        return 1;
    }

    // 3. 私钥
    if (!fs::exists(keyPath_)) {
        err("私钥不存在: " + keyPath_);
        return 1;
    }

    // 4. 输出根
    fs::path out = outDir_.empty() ? fs::path("publish") : fs::path(outDir_);
    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) {
        err("创建输出目录失败: " + ec.message());
        return 1;
    }

    // 5. tar 名
    auto pkgBase = isChip
        ? (name + "-" + version)
        : (name + "-" + chip + "-" + version);
    auto tarName = pkgBase + ".tar.gz";
    auto sigName = pkgBase + ".tar.gz.sig";
    auto tarPath = out / tarName;
    auto sigPath = out / sigName;

    auto st = hpm::util::runProcess("tar",
        {"-czf", tarPath.string(),
         "-C", pkgDir.parent_path().string(),
         pkgDir.filename().string()},
        false);
    if (!st.ok()) {
        err("打包失败: " + st.error().message());
        return 1;
    }
    ok("打包: " + tarPath.string());

    // 6. 算法
    std::string alg = algorithm_;
    if (alg == "auto") {
        alg = detectAlgorithm(keyPath_);
        if (alg.empty()) {
            err("无法自动检测私钥算法，请用 --algorithm 指定");
            return 1;
        }
    }
    ok("算法: " + alg);

    // 7. 签名 tar.gz
    if (alg == "ed25519" || alg == "ed448") {
        st = hpm::util::runProcess("openssl",
            {"pkeyutl", "-sign", "-inkey", keyPath_,
             "-rawin", "-in", tarPath.string(), "-out", sigPath.string()},
            false);
    } else {
        st = hpm::util::runProcess("openssl",
            {"dgst", "-sha256", "-sign", keyPath_,
             "-out", sigPath.string(), tarPath.string()},
            false);
    }
    if (!st.ok()) {
        err("签名失败: " + st.error().message());
        return 1;
    }
    ok("签名: " + sigPath.string());

    // 8. 包信息
    auto sha = hpm::registry::sha256File(tarPath);
    auto size = fs::file_size(tarPath);
    std::string license = readLicense(manifestPath, chipPath, "MIT");

    std::cout << "\n";
    std::cout << "包信息：\n";
    std::cout << "  name:      " << name << "\n";
    std::cout << "  chip:      " << chip << "\n";
    std::cout << "  version:   " << version << "\n";
    std::cout << "  algorithm: " << alg << "\n";
    std::cout << "  license:   " << license << "\n";
    std::cout << "  sha256:    " << sha << "\n";
    std::cout << "  size:      " << size << "\n\n";

    // 9. dev.json
    auto cfg = loadDevConfig();
    if (!cfg) {
        info("未登录，跳过索引更新");
        info("登录后运行 hpm-dev login，然后重新 publish");
        info("手动上传:");
        detail(tarPath.string());
        detail(sigPath.string());
        return 0;
    }

    // 10. publish/ 必须是 git 仓库
    fs::path repoRoot = out;
    fs::path gitDir = repoRoot / ".git";
    if (!fs::exists(gitDir)) {
        err("publish 目录不是 git 仓库: " + repoRoot.string());
        info("请先运行 hpm-dev init 初始化仓库");
        info("手动上传:");
        detail(tarPath.string());
        detail(sigPath.string());
        return 0;
    }

    // 11. git pull
    info("拉取远程仓库...");
    st = runGit(repoRoot, {"pull", "origin", "main"}, false);
    if (!st.ok()) {
        err("git pull 失败: " + st.error().message());
        info("请手动处理冲突后重新运行 hpm-dev publish");
        return 1;
    }

    // 12. 拷产物
    fs::path devDir = repoRoot / cfg->developer;
    fs::create_directories(devDir, ec);
    if (ec) {
        err("创建开发者目录失败: " + ec.message());
        return 1;
    }
    fs::path devTar = devDir / tarName;
    fs::path devSig = devDir / sigName;
    fs::copy_file(tarPath, devTar, fs::copy_options::overwrite_existing, ec);
    if (ec) { err("拷贝 tar.gz 失败: " + ec.message()); return 1; }
    fs::copy_file(sigPath, devSig, fs::copy_options::overwrite_existing, ec);
    if (ec) { err("拷贝 sig 失败: " + ec.message()); return 1; }
    ok("已放置产物: " + devDir.string());

    // 13. index.json
    info("更新 index.json...");
    fs::path indexPath = repoRoot / "index.json";
    {
        json::Value index;
        if (fs::exists(indexPath)) {
            auto j = json::parseFile(indexPath);
            if (j) index = *j;
        } else {
            index = json::Value::object();
            index.set("schema", json::Value(int64_t(1)));
            index.set("official_pub", json::Value("{baseurl}/official.pub"));
            index.set("developers", json::Value::object());
            index.set("domains", json::Value::object());
            index.set("packages", json::Value::object());
        }

        auto devs = index.get("developers");
        if (!devs || !devs->isObject()) {
            index.set("developers", json::Value::object());
            devs = index.get("developers");
        }
        if (!devs->has(cfg->developer)) {
            auto devObj = json::Value::object();
            devObj.set("pub", json::Value("{baseurl}/" + cfg->developer + "/" +
                                          cfg->developer + ".pub"));
            devObj.set("pub_sig", json::Value("{baseurl}/" + cfg->developer + "/" +
                                              cfg->developer + ".pub.sig"));
            devs->set(cfg->developer, devObj);
        }

        // pkgKey 用外层定义好的

        auto packages = index.get("packages");
        if (!packages || !packages->isObject()) {
            index.set("packages", json::Value::object());
            packages = index.get("packages");
        }
        json::Value pkgInfo;
        if (packages->has(pkgKey)) {
            pkgInfo = *packages->get(pkgKey);
        } else {
            pkgInfo = json::Value::object();
            pkgInfo.set("versions", json::Value::object());
        }
        auto versions = pkgInfo.get("versions");
        if (!versions || !versions->isObject()) {
            pkgInfo.set("versions", json::Value::object());
            versions = pkgInfo.get("versions");
        }

        json::Value requires_    = json::Value::object();
        json::Value description_ = json::Value("");
        json::Value config_      = json::Value::object();
        json::Value provides_    = json::Value::object();
        json::Value pins_        = json::Value::object();
        {
            fs::path mp = fs::exists(manifestPath) ? manifestPath : chipPath;
            if (fs::exists(mp)) {
                auto mj = json::parseFile(mp);
                if (mj) {
                    auto reqs = mj->get("requires");
                    if (reqs) requires_ = *reqs;

                    auto desc = mj->get("description");
                    if (desc && desc->isString()) description_ = *desc;

                    auto cfgv = mj->get("config");
                    if (cfgv && cfgv->isObject()) config_ = *cfgv;

                    auto prov = mj->get("provides");
                    if (prov && prov->isObject()) provides_ = *prov;

                    auto pins = mj->get("pins");
                    if (pins && pins->isObject()) pins_ = *pins;
                }
            }
        }

        auto verObj = json::Value::object();
        verObj.set("kind", json::Value(isChip ? "chip" : "driver"));
        verObj.set("chip", json::Value(chip));
        verObj.set("developer", json::Value(cfg->developer));
        verObj.set("url", json::Value("{baseurl}/" + cfg->developer + "/" +
                                      pkgBase + ".tar.gz"));
        verObj.set("sig_url", json::Value("{baseurl}/" + cfg->developer + "/" +
                                          pkgBase + ".tar.gz.sig"));
        verObj.set("sha256", json::Value(sha));
        verObj.set("size", json::Value(static_cast<int64_t>(size)));
        verObj.set("license", json::Value(license));
        verObj.set("description", description_);
        verObj.set("config", config_);
        verObj.set("provides", provides_);
        verObj.set("requires", requires_);
        if (isChip) {
            verObj.set("pins", pins_);
        }

        versions->set(version, verObj);
        pkgInfo.set("versions", *versions);
        packages->set(pkgKey, pkgInfo);
        index.set("packages", *packages);

        auto writeSt = json::writeFile(index, indexPath);
        if (!writeSt.ok()) {
            err("写 index.json 失败: " + writeSt.error().message());
            return 1;
        }
        ok("已更新 index.json");
    }

    // 14. 签名 index.json
    info("签名 index.json...");
    fs::path indexSigPath = repoRoot / "index.json.sig";
    if (alg == "ed25519" || alg == "ed448") {
        st = hpm::util::runProcess("openssl",
            {"pkeyutl", "-sign", "-inkey", keyPath_,
             "-rawin", "-in", indexPath.string(), "-out", indexSigPath.string()},
            false);
    } else {
        st = hpm::util::runProcess("openssl",
            {"dgst", "-sha256", "-sign", keyPath_,
             "-out", indexSigPath.string(), indexPath.string()},
            false);
    }
    if (!st.ok()) {
        err("签名 index.json 失败: " + st.error().message());
        return 1;
    }
    ok("已签名 " + indexSigPath.string());

    // 15. git add / commit
    info("提交...");
    st = runGit(repoRoot,
        {"add",
         (fs::path(cfg->developer) / tarName).string(),
         (fs::path(cfg->developer) / sigName).string(),
         "index.json", "index.json.sig"},
        false);
    if (!st.ok()) {
        err("git add 失败: " + st.error().message());
        return 1;
    }

    st = runGit(repoRoot, {"commit", "-m", "Update " + pkgKey + "@" + version}, false);
    if (!st.ok()) {
        info("没有变更需要提交，跳过");
    } else {
        ok("已提交 " + pkgKey + "@" + version);
    }

    // 16. push
    auto originUrl = runGit(repoRoot, {"remote", "get-url", "origin"}, false);
    bool hasRemote = originUrl.ok();

    if (hasRemote) {
        info("推送...");
        st = runGit(repoRoot, {"push", "origin", "main"}, false);
        if (!st.ok()) {
            err("git push 失败: " + st.error().message());
            info("请手动运行: git -C publish push origin main");
            return 1;
        }
        ok("已推送 " + pkgKey + "@" + version);

        // 17. 同步到开发者中心
        info("同步到开发者中心...");
        {
            std::string requiresJsonStr = "{}";
            {
                fs::path mp = fs::exists(manifestPath) ? manifestPath : chipPath;
                if (fs::exists(mp)) {
                    auto mj = json::parseFile(mp);
                    if (mj && mj->has("requires")) {
                        auto reqs = mj->get("requires");
                        if (reqs) requiresJsonStr = json::dump(*reqs, -1);
                    }
                }
            }

            auto payload = json::Value::object();
            payload.set("name", json::Value(pkgKey));
            payload.set("kind", json::Value(isChip ? "chip" : "driver"));
            payload.set("requiresJson", json::Value(requiresJsonStr));
            payload.set("version", json::Value(version));
            payload.set("url", json::Value("{baseurl}/" + cfg->developer + "/" + tarName));
            payload.set("sigUrl", json::Value("{baseurl}/" + cfg->developer + "/" + sigName));
            payload.set("sha256", json::Value(sha));
            payload.set("size", json::Value(static_cast<int64_t>(size)));
            payload.set("license", json::Value(license));

            auto body = json::Value::object();
            body.set("accessToken", json::Value(cfg->accessToken));
            body.set("action", json::Value("update_package"));
            body.set("payload", payload);

            auto resp = hpm::cli::httpPostJson(
                HPM_DEV_CENTER_API,
                json::dump(body, -1),
                "Authorization: Bearer " + std::string(HPM_DEV_CENTER_APP_KEY));

            if (resp.status == 200) {
                auto rj = json::parse(resp.body);
                if (rj && rj->getBool("success", false)) {
                    ok("已同步到开发者中心");
                } else {
                    err("同步返回失败: " + resp.body);
                    info("产物已在 git 仓库，可稍后重试");
                }
            } else if (resp.status == 401) {
                err("accessToken 或 AppKey 失效，请重新运行 hpm-dev login");
            } else if (resp.status == 403) {
                err("用户已取消授权或账户被冻结");
            } else {
                err("同步失败: HTTP " + std::to_string(resp.status) + " " + resp.body);
            }
        }
    } else {
        info("未设置远程仓库，跳过推送");
        info("设置远程后可运行: git -C publish push origin main");
        info("账户索引未同步，推送后请重新运行 publish");
    }

    return 0;
}

} // namespace hpm::cli