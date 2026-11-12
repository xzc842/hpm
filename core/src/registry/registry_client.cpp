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

// core/src/registry/registry_client.cpp
#include <hpm/registry/registry_client.h>

#include <hpm/json/json_io.h>
#include <hpm/registry/official_cert.h>
#include <hpm/registry/signature.h>
#include <hpm/util/process.h>

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <cctype>
#include <iostream>
#include <sstream>
#include "hpm/config.h"

namespace hpm::registry {
namespace fs = std::filesystem;
namespace {

std::string urlDir(const std::string& url) {
    auto slash = url.find_last_of('/');
    return slash == std::string::npos ? std::string() : url.substr(0, slash);
}


size_t writeToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

size_t writeToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::ofstream*>(userdata);
    f->write(static_cast<char*>(ptr), static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

struct ProgressCtx {
    DownloadProgress progress;
};

int progressCallback(void* clientp,
                     curl_off_t dltotal,
                     curl_off_t dlnow,
                     curl_off_t /*ultotal*/,
                     curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<ProgressCtx*>(clientp);
    if (!ctx || !ctx->progress) return 0;
    if (dltotal > 0) {
        ctx->progress(static_cast<uint64_t>(dlnow),
                      static_cast<uint64_t>(dltotal));
    } else {
        ctx->progress(static_cast<uint64_t>(dlnow), 0);
    }
    return 0;
}

} // namespace

// ─────────────────────────────────────────────
// 配置
// ─────────────────────────────────────────────

RegistryConfig RegistryConfig::load() {
    RegistryConfig cfg;

    cfg.registries.push_back(HPM_DEFAULT_REGISTRY_URL);

    if (const char* env = std::getenv(HPM_ENV_REGISTRY); env && *env) {
        cfg.registries.clear();
        std::string s(env);
        std::istringstream iss(s);
        std::string token;
        while (std::getline(iss, token, ',')) {
            if (!token.empty()) cfg.registries.push_back(token);
        }
    }

    if (const char* env = std::getenv(HPM_ENV_HOME); env && *env) {
        cfg.cacheDir = std::filesystem::path(env);
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        cfg.cacheDir = std::filesystem::path(home) / ".hpm";
    } else {
        cfg.cacheDir = std::filesystem::current_path() / ".hpm";
    }

    auto configFile = cfg.cacheDir / "config.json";
    if (std::filesystem::exists(configFile)) {
        auto j = json::parseFile(configFile);
        if (j) {
            auto regs = j->get("registries");
            if (regs && regs->isArray()) {
                cfg.registries.clear();
                auto n = regs->size();
                if (n) {
                    for (size_t i = 0; i < *n; ++i) {
                        auto item = regs->at(i);
                        if (item) {
                            auto s = item->asString();
                            if (s) cfg.registries.push_back(*s);
                        }
                    }
                }
            }
            cfg.offline = j->getBool("offline", false);
        }
    }

    if (const char* env = std::getenv(HPM_ENV_SKIP_SIGNATURE); env && *env) {
        cfg.skipSignature = (std::string(env) == "1");
    }

    return cfg;
}

// ─────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────

RegistryClient::RegistryClient(const RegistryConfig& config)
    : config_(config) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

RegistryClient::~RegistryClient() {
    curl_global_cleanup();
}

std::filesystem::path RegistryClient::packagesDir() const {
    return config_.cacheDir / "packages";
}

std::filesystem::path RegistryClient::tarballsDir() const {
    return config_.cacheDir / "cache" / "tarballs";
}

// ─────────────────────────────────────────────
// 名称拆分
// ─────────────────────────────────────────────

void RegistryClient::splitName(const std::string& fullName,
                                std::string& domain,
                                std::string& name) {
    auto slash = fullName.find('/');
    if (slash == std::string::npos) {
        domain.clear();
        name = fullName;
    } else {
        domain = fullName.substr(0, slash);
        name = fullName.substr(slash + 1);
    }
}

// ─────────────────────────────────────────────
// HTTP
// ─────────────────────────────────────────────

Result<std::string> RegistryClient::httpGet(const std::string& url,
                                              DownloadProgress progress) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return Error(ErrorCode::NetworkError, "curl_easy_init 失败");
    }

    bool isFile = (url.rfind("file://", 0) == 0);

    std::string out;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "hpm/0.1.0");

    ProgressCtx ctx{progress};
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Error(ErrorCode::NetworkError,
                     "HTTP 失败: " + std::string(curl_easy_strerror(res)));
    }
    if (!isFile && httpCode != 200) {
        return Error(ErrorCode::NetworkError,
                     "HTTP " + std::to_string(httpCode) + ": " + url);
    }
    return out;
}

Result<void> RegistryClient::httpDownload(
    const std::string& url,
    const std::filesystem::path& dest,
    DownloadProgress progress) {

    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);

    std::ofstream f(dest, std::ios::binary);
    if (!f) {
        return Error(ErrorCode::IoError, "无法写入: " + dest.string());
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return Error(ErrorCode::NetworkError, "curl_easy_init 失败");
    }

    bool isFile = (url.rfind("file://", 0) == 0);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "hpm/0.1.0");

    ProgressCtx ctx{progress};
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    f.close();

    if (res != CURLE_OK) {
        std::filesystem::remove(dest);
        return Error(ErrorCode::NetworkError,
                     "下载失败: " + std::string(curl_easy_strerror(res)) +
                     " URL: " + url);
    }
    if (!isFile && httpCode != 200) {
        std::filesystem::remove(dest);
        return Error(ErrorCode::NetworkError,
                     "HTTP " + std::to_string(httpCode) + ": " + url);
    }
    return {};
}

// ─────────────────────────────────────────────
// 吊销列表
// ─────────────────────────────────────────────

Result<RevocationList> RegistryClient::loadRevocation(
    const std::string& baseUrl,
    const std::filesystem::path& certPath) {

    if (config_.offline) {
        std::cerr << "warning: 您处于离线模式，无法获取安全更新\n";
        return RevocationList{};
    }

    if (baseUrl.empty()) {
        return Error(ErrorCode::InvalidField, "吊销列表 baseUrl 为空");
    }

    const std::string url = baseUrl + "/revoke.json";
    auto text = httpGet(url);
    if (!text) {
        return Error(ErrorCode::NetworkError,
                     "吊销列表拉取失败: " + text.error().message());
    }

    auto sigText = httpGet(url + ".sig");
    if (!sigText) {
        return Error(ErrorCode::NetworkError,
                     "吊销列表签名拉取失败: " +
                         sigText.error().message());
    }

    const fs::path tmpDir = config_.cacheDir / "cache" / "tmp";
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    const fs::path tmpJson = tmpDir / "revoke.json.tmp";
    const fs::path tmpSig = tmpDir / "revoke.json.sig.tmp";
    {
        std::ofstream f(tmpJson);
        if (!f) return Error(ErrorCode::IoError, "无法写 revoke.json 临时文件");
        f << *text;
    }
    {
        std::ofstream f(tmpSig);
        if (!f) return Error(ErrorCode::IoError, "无法写 revoke 签名临时文件");
        f << *sigText;
    }

    auto ok = verifySignatureWithCert(certPath, tmpSig, tmpJson);
    if (!ok) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return ok.error();
    }
    if (!*ok) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return Error(ErrorCode::ChecksumMismatch,
                     "吊销列表签名无效: " + url);
    }

    auto parsed = parseRevocation(*text);
    if (!parsed) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return parsed.error();
    }
    if (!parsed->fresh(HPM_REVOKE_MAX_AGE_DAYS)) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return Error(ErrorCode::ChecksumMismatch,
                     "吊销列表已超过 7 天有效期: " + url);
    }

    fs::remove(tmpJson);
    fs::remove(tmpSig);
    return *parsed;
}

Result<RevocationList> RegistryClient::loadRootRevocation() {
    if (config_.offline) {
        std::cerr << "warning: 您处于离线模式，无法获取根吊销列表\n";
        return RevocationList{};
    }

    const char* env = std::getenv(HPM_ENV_ROOT_REVOKE);
    const std::string url =
        (env && *env) ? std::string(env) : std::string(HPM_ROOT_REVOKE_URL);
    auto text = httpGet(url);
    if (!text) {
        return Error(ErrorCode::NetworkError,
                     "根吊销列表拉取失败: " + text.error().message());
    }
    auto sigText = httpGet(url + ".sig");
    if (!sigText) {
        return Error(ErrorCode::NetworkError,
                     "根吊销列表签名拉取失败: " +
                         sigText.error().message());
    }

    const fs::path tmpDir = config_.cacheDir / "cache" / "tmp";
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    const fs::path tmpJson = tmpDir / "ROOT_revoke.json.tmp";
    const fs::path tmpSig = tmpDir / "ROOT_revoke.json.sig.tmp";
    {
        std::ofstream f(tmpJson);
        if (!f) return Error(ErrorCode::IoError, "无法写根吊销列表临时文件");
        f << *text;
    }
    {
        std::ofstream f(tmpSig);
        if (!f) return Error(ErrorCode::IoError, "无法写根吊销签名临时文件");
        f << *sigText;
    }

    auto ok = verifySignatureWithPem(HPM_ROOT_CA_PEM, tmpSig, tmpJson);
    if (!ok || !*ok) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        if (!ok) return ok.error();
        return Error(ErrorCode::ChecksumMismatch,
                     "根吊销列表签名无效: " + url);
    }

    auto parsed = parseRevocation(*text);
    if (!parsed) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return parsed.error();
    }
    if (!parsed->freshSince(HPM_ROOT_REVOKE_MAX_AGE_DAYS,
                            HPM_ROOT_REVOKE_MIN_TIMESTAMP)) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return Error(ErrorCode::ChecksumMismatch,
                     "根吊销列表已过期或早于限制时间: " + url);
    }

    auto root = loadCertificatePem(HPM_ROOT_CA_PEM, "HPM_ROOT_CA_PEM");
    if (!root) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return root.error();
    }
    if (!parsed->issuerFingerprint.empty() &&
        normalizeFingerprint(parsed->issuerFingerprint) !=
            normalizeFingerprint(certificateFingerprint(*root))) {
        fs::remove(tmpJson);
        fs::remove(tmpSig);
        return Error(ErrorCode::ChecksumMismatch,
                     "根吊销列表 issuer 不是内置根证书: " + url);
    }

    fs::remove(tmpJson);
    fs::remove(tmpSig);
    return *parsed;
}

bool RegistryClient::isRevoked(const CertificatePtr& cert,
                               const std::string& domain) const {
    if (!cert) return false;
    if (mainRevokeLoaded_ && mainRevoke_.contains(cert)) return true;
    if (!domain.empty()) {
        auto it = domainRevokes_.find(domain);
        if (it != domainRevokes_.end() && it->second.contains(cert)) {
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────
// 主索引
// ─────────────────────────────────────────────

Result<Index> RegistryClient::fetchIndex(DownloadProgress progress) {
    if (indexLoaded_) return cachedIndex_;

    std::filesystem::path officialCertPath;
    if (!config_.skipSignature) {
        OfficialCertManager certMgr(config_.cacheDir);
        auto certPath = certMgr.ensure();
        if (!certPath) {
            return Error(ErrorCode::PermissionDenied,
                         "官方证书获取失败: " + certPath.error().message());
        }
        auto cert = loadCertificateFile(*certPath);
        if (!cert) return cert.error();
        officialCert_ = *cert;
        officialCertPath = *certPath;
        officialCertPath_ = *certPath;
    }

    if (!config_.skipSignature) {
        auto rootRev = loadRootRevocation();
        if (!rootRev) {
            return Error(ErrorCode::PermissionDenied,
                         "根吊销列表加载失败: " +
                             rootRev.error().message());
        }
        if (rootRev->contains(officialCert_)) {
            return Error(ErrorCode::PermissionDenied,
                         "官方证书已被根吊销列表吊销");
        }
        rootRevoke_ = std::move(*rootRev);
        rootRevokeLoaded_ = true;
    }

    std::string lastError;

    for (const auto& url : config_.registries) {
        auto text = httpGet(url, progress);
        if (!text) {
            lastError = text.error().message();
            continue;
        }

        std::string baseUrl = url;
        auto slash = baseUrl.find_last_of('/');
        if (slash != std::string::npos) {
            baseUrl = baseUrl.substr(0, slash);
        }

        if (!config_.skipSignature) {
            auto sigUrl = url + ".sig";
            auto sigText = httpGet(sigUrl);
            if (!sigText) {
                lastError = "索引签名下载失败: " + sigText.error().message();
                continue;
            }

            auto tmpDir = config_.cacheDir / "cache" / "tmp";
            std::filesystem::create_directories(tmpDir);
            auto tmpIndex = tmpDir / "index.json";
            auto tmpSig = tmpDir / "index.json.sig";

            {
                std::ofstream f(tmpIndex);
                f << *text;
            }
            {
                std::ofstream f(tmpSig);
                f << *sigText;
            }

            auto ok = verifySignatureWithCert(officialCertPath, tmpSig, tmpIndex);
            if (!ok) {
                lastError = "索引签名验证失败: " + ok.error().message();
                continue;
            }
            if (!*ok) {
                lastError = "索引签名无效（可能被篡改）";
                continue;
            }
        }

        if (!config_.skipSignature) {
            auto rev = loadRevocation(baseUrl, officialCertPath_);
            if (!rev) {
                lastError = "吊销列表加载失败: " + rev.error().message();
                continue;
            }
            if (rev->contains(officialCert_)) {
                lastError = "官方证书已被吊销";
                continue;
            }
            mainRevoke_ = std::move(*rev);
            mainRevokeLoaded_ = true;
        }

        auto idx = parseIndex(*text, baseUrl);
        if (!idx) {
            lastError = idx.error().message();
            continue;
        }

        cachedIndex_ = *idx;
        indexLoaded_ = true;
        return cachedIndex_;
    }

    return Error(ErrorCode::NetworkError,
                 "所有镜像失败。最后错误: " + lastError);
}

// ─────────────────────────────────────────────
// 域索引
// ─────────────────────────────────────────────

Result<const Index*> RegistryClient::fetchDomainIndex(
    const std::string& domain,
    DownloadProgress progress) {

    auto it = domainIndexes_.find(domain);
    if (it != domainIndexes_.end()) {
        return &it->second;
    }

    auto mainIdx = fetchIndex(progress);
    if (!mainIdx) return mainIdx.error();

    auto dit = mainIdx->domains.find(domain);
    if (dit == mainIdx->domains.end()) {
        return Error(ErrorCode::NotFound,
                     "主索引里找不到域: " + domain);
    }

    const auto& info = dit->second;

    if (config_.skipSignature) {
        auto text = httpGet(info.index, progress);
        if (!text) return text.error();

        std::string baseUrl = info.index;
        auto slash = baseUrl.find_last_of('/');
        if (slash != std::string::npos) {
            baseUrl = baseUrl.substr(0, slash);
        }

        auto idx = parseIndex(*text, baseUrl);
        if (!idx) return idx.error();

        domainIndexes_[domain] = std::move(*idx);
        return &domainIndexes_[domain];
    }

    // 1. 下载域证书
    auto keyDir = config_.cacheDir / "cache" / "domains" / domain;
    std::filesystem::create_directories(keyDir);

    auto certPath = keyDir / (domain + ".crt");
    if (info.cert.empty()) {
        return Error(ErrorCode::InvalidField,
                     "域索引缺少 cert: " + domain);
    }
    if (!std::filesystem::exists(certPath)) {
        auto dl = httpDownload(info.cert, certPath, nullptr);
        if (!dl) return dl.error();
    }

    auto domainCert = loadCertificateFile(certPath);
    if (!domainCert) return domainCert.error();

    // 2. 用官方证书验证域证书
    auto verified = verifyCertificate(*domainCert, officialCert_,
                                      "hpm://domain/" + domain, true);
    if (!verified) return verified.error();

    if (isRevoked(*domainCert, "")) {
        return Error(ErrorCode::PermissionDenied,
                     "域证书已被官方吊销: " + domain);
    }

    domainCertPaths_[domain] = certPath;

    // 2.5 下载并验证域吊销列表
    auto domainBase = urlDir(info.index);
    auto domainRev = loadRevocation(domainBase, certPath);
    if (!domainRev) {
        return Error(ErrorCode::ChecksumMismatch,
                     "域吊销列表加载失败: " +
                         domainRev.error().message());
    }
    if (domainRev->contains(*domainCert)) {
        return Error(ErrorCode::PermissionDenied,
                     "域证书已被域吊销列表吊销: " + domain);
    }
    domainRevokes_[domain] = std::move(*domainRev);

    // 3. 下载域索引 + 签名
    auto text = httpGet(info.index, progress);
    if (!text) return text.error();

    auto sigUrl = info.index + ".sig";
    auto sigText = httpGet(sigUrl);
    if (!sigText) {
        return Error(ErrorCode::NetworkError,
                     "域索引签名下载失败: " + sigText.error().message());
    }

    auto tmpDir = config_.cacheDir / "cache" / "tmp";
    std::filesystem::create_directories(tmpDir);
    auto tmpIndex = tmpDir / ("domain-" + domain + "-index.json");
    auto tmpSig = tmpDir / ("domain-" + domain + "-index.json.sig");

    {
        std::ofstream f(tmpIndex);
        f << *text;
    }
    {
        std::ofstream f(tmpSig);
        f << *sigText;
    }

    // 4. 用域证书验证域索引
    auto ok = verifySignatureWithCert(certPath, tmpSig, tmpIndex);
    if (!ok) return ok.error();
    if (!*ok) {
        return Error(ErrorCode::ChecksumMismatch,
                     "域索引签名无效: " + domain);
    }

    // 5. 解析域索引
    std::string baseUrl = info.index;
    auto slash = baseUrl.find_last_of('/');
    if (slash != std::string::npos) {
        baseUrl = baseUrl.substr(0, slash);
    }

    auto idx = parseIndex(*text, baseUrl);
    if (!idx) return idx.error();

    domainIndexes_[domain] = std::move(*idx);
    return &domainIndexes_[domain];
}

// ─────────────────────────────────────────────
// 包
// ─────────────────────────────────────────────

Result<std::filesystem::path> RegistryClient::ensurePackage(
    const std::string& fullName,
    const std::string& version,
    DownloadProgress progress) {

    std::string domain;
    std::string name;
    splitName(fullName, domain, name);

    // 目录：<packages>/<domain>/<name>-<version>/ 或 <packages>/<name>-<version>/
    fs::path baseDir = packagesDir();
    if (!domain.empty()) {
        baseDir = baseDir / domain;
    }
    auto pkgDir = baseDir / (name + "-" + version);

    if (std::filesystem::exists(pkgDir)) {
        // 在线时即使包已缓存，也要重新检查开发者证书是否被吊销。
        if (!config_.offline && !config_.skipSignature) {
            Index mainIdxStorage;
            const Index* idx = nullptr;
            if (domain.empty()) {
                auto mainIdx = fetchIndex(progress);
                if (mainIdx) {
                    mainIdxStorage = std::move(*mainIdx);
                    idx = &mainIdxStorage;
                }
            } else {
                auto domIdx = fetchDomainIndex(domain, progress);
                if (domIdx) idx = *domIdx;
            }
            if (idx) {
                auto pit = idx->packages.find(name);
                if (pit != idx->packages.end()) {
                    auto vit = pit->second.versions.find(version);
                    if (vit != pit->second.versions.end() &&
                        !vit->second.developer.empty()) {
                        auto cert = fetchAndVerifyDeveloperCert(
                            domain, vit->second.developer);
                        if (!cert) return cert.error();
                    }
                }
            }
        }
        return pkgDir;
    }

    if (config_.offline) {
        return Error(ErrorCode::NetworkError,
                     "离线模式，包未缓存: " + fullName + "@" + version);
    }

    // 选索引。mainIdx 必须活到 ensurePackage 结束，否则 info 会悬空。
    Index mainIdxStorage;
    const Index* idx = nullptr;
    if (domain.empty()) {
        auto mainIdx = fetchIndex(progress);
        if (!mainIdx) return mainIdx.error();
        mainIdxStorage = std::move(*mainIdx);
        idx = &mainIdxStorage;
    } else {
        auto domIdx = fetchDomainIndex(domain, progress);
        if (!domIdx) return domIdx.error();
        idx = *domIdx;
    }

    auto* info = idx->find(name, version);
    if (!info) {
        return Error(ErrorCode::NotFound,
                     "索引里找不到包: " + fullName + "@" + version);
    }

    // tarball 缓存目录也用域
    auto tarDir = tarballsDir();
    if (!domain.empty()) {
        tarDir = tarDir / domain;
    }
    auto tarball = tarDir / (name + "-" + version + ".tar.gz");

    if (!std::filesystem::exists(tarball)) {
        auto dl = httpDownload(info->url, tarball, progress);
        if (!dl) return dl.error();
    }

    // 签名验证
    if (!config_.skipSignature) {
        auto sigSt = verifyPackageSignature(tarball, info->sigUrl,
                                             domain, info->developer);
        if (!sigSt.ok()) {
            std::filesystem::remove(tarball);
            return sigSt.error();
        }
    }

    // SHA256
    if (!info->sha256.empty()) {
        auto actual = sha256File(tarball);
        if (actual != info->sha256) {
            std::filesystem::remove(tarball);
            return Error(ErrorCode::ChecksumMismatch,
                         "SHA256 不匹配: " + fullName + "@" + version);
        }
    }

    // 解压到 baseDir（<packages>/ 或 <packages>/<domain>/）
    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);
    auto ex = extractTarball(tarball, baseDir);
    if (!ex.ok()) return ex.error();

    return pkgDir;
}

// ─────────────────────────────────────────────
// 开发者公钥
// ─────────────────────────────────────────────

Result<std::filesystem::path> RegistryClient::fetchAndVerifyDeveloperCert(
    const std::string& domain,
    const std::string& devName) {

    std::string key = domain + "/" + devName;
    auto it = developerCertPaths_.find(key);
    if (it != developerCertPaths_.end()) return it->second;

    Index mainIdxStorage;
    const Index* idx = nullptr;
    CertificatePtr issuer;

    if (domain.empty()) {
        auto mainIdx = fetchIndex();
        if (!mainIdx) return mainIdx.error();
        mainIdxStorage = std::move(*mainIdx);
        idx = &mainIdxStorage;
        issuer = officialCert_;
    } else {
        auto domIdx = fetchDomainIndex(domain);
        if (!domIdx) return domIdx.error();
        idx = *domIdx;
        auto issuerPath = domainCertPaths_.find(domain);
        if (issuerPath == domainCertPaths_.end()) {
            return Error(ErrorCode::Internal, "域证书缓存缺失: " + domain);
        }
        auto cert = loadCertificateFile(issuerPath->second);
        if (!cert) return cert.error();
        issuer = *cert;
    }

    auto dit = idx->developers.find(devName);
    if (dit == idx->developers.end()) {
        return Error(ErrorCode::NotFound,
                     "索引里找不到开发者: " + devName +
                     (domain.empty() ? "" : "（域 " + domain + "）"));
    }

    const auto& info = dit->second;
    if (info.cert.empty()) {
        return Error(ErrorCode::InvalidField,
                     "开发者缺少 cert: " + devName);
    }

    auto keyDir = config_.cacheDir / "cache" / "keys" /
                  (domain.empty() ? "main" : domain) / devName;
    std::filesystem::create_directories(keyDir);

    auto certPath = keyDir / (devName + ".crt");
    if (!std::filesystem::exists(certPath)) {
        auto dl = httpDownload(info.cert, certPath, nullptr);
        if (!dl) return dl.error();
    }

    auto cert = loadCertificateFile(certPath);
    if (!cert) return cert.error();

    auto verified = verifyCertificate(*cert, issuer,
                                      "hpm://developer/" + devName, false);
    if (!verified) return verified.error();

    if (isRevoked(*cert, domain)) {
        return Error(ErrorCode::PermissionDenied,
                     "开发者证书已被吊销: " +
                         (domain.empty() ? devName : domain + "/" + devName));
    }

    developerCertPaths_[key] = certPath;
    return certPath;
}

Status RegistryClient::verifyPackageSignature(
    const std::filesystem::path& tarball,
    const std::string& sigUrl,
    const std::string& domain,
    const std::string& devName) {

    if (sigUrl.empty()) {
        return Error(ErrorCode::InvalidField,
                     "包没有签名 URL: " + tarball.string());
    }

    auto sigPath = tarball;
    sigPath += ".sig";
    if (!std::filesystem::exists(sigPath)) {
        auto dl = httpDownload(sigUrl, sigPath, nullptr);
        if (!dl) return dl.error();
    }

    auto cert = fetchAndVerifyDeveloperCert(domain, devName);
    if (!cert) return cert.error();

    auto ok = verifySignatureWithCert(*cert, sigPath, tarball);
    if (!ok) return ok.error();
    if (!*ok) {
        return Error(ErrorCode::ChecksumMismatch,
                     "包签名无效: " + tarball.string());
    }
    return {};
}

// ─────────────────────────────────────────────
// 解压
// ─────────────────────────────────────────────

Status RegistryClient::extractTarball(
    const std::filesystem::path& tarball,
    const std::filesystem::path& destDir) {

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    auto st = util::runProcess("tar",
        {"-xzf", tarball.string(), "-C", destDir.string()}, false);
    if (!st.ok()) {
        return Error(ErrorCode::IoError,
                     "解压失败: " + tarball.string() + " - " + st.error().message());
    }
    return {};
}

} // namespace hpm::registry