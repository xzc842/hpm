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

// core/src/registry/official_cert.cpp
#include <hpm/registry/official_cert.h>

#include <curl/curl.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "hpm/config.h"

namespace hpm::registry {

namespace {

size_t writeToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::ofstream*>(userdata);
    f->write(static_cast<char*>(ptr),
             static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

struct CertProgressCtx {
    TransferObserver observer;
    std::string label;
    uint64_t total = 0;
};

int certProgress(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                 curl_off_t, curl_off_t) {
    auto* ctx = static_cast<CertProgressCtx*>(clientp);
    if (!ctx || !ctx->observer) return 0;
    const uint64_t total = dltotal > 0 ? static_cast<uint64_t>(dltotal) : 0;
    if (total > 0) ctx->total = total;
    TransferEvent ev;
    ev.label = ctx->label;
    ev.current = static_cast<uint64_t>(dlnow);
    ev.total = total ? total : ctx->total;
    ev.phase = TransferPhase::Progress;
    ctx->observer(ev);
    return 0;
}

} // namespace

const char* OfficialCertManager::officialUrl() {
    if (const char* env = std::getenv(HPM_ENV_OFFICIAL_CERT); env && *env) {
        return env;
    }
    return HPM_OFFICIAL_CERT_URL;
}

OfficialCertManager::OfficialCertManager(std::filesystem::path cacheDir,
                                         TransferObserver observer)
    : cacheDir_(std::move(cacheDir)), observer_(std::move(observer)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

std::filesystem::path OfficialCertManager::localPath() const {
    return cacheDir_ / HPM_FILE_OFFICIAL_CERT;
}

std::filesystem::path OfficialCertManager::fingerprintPath() const {
    return cacheDir_ / HPM_FILE_OFFICIAL_CERT_FP;
}

Result<CertificatePtr> OfficialCertManager::loadRoot() const {
    Result<CertificatePtr> root = Error(ErrorCode::Internal, "unset");
    if (const char* env = std::getenv(HPM_ENV_ROOT_CA); env && *env) {
        root = loadCertificateFile(env);
    } else {
        root = loadCertificatePem(HPM_ROOT_CA_PEM, "HPM_ROOT_CA_PEM");
    }
    if (!root) {
        return Error(root.error().code(),
                     "根证书加载失败（请替换 config.h 中的 HPM_ROOT_CA_PEM）: " +
                         root.error().message());
    }
    auto verified = verifyRootCertificate(*root);
    if (!verified) return verified.error();
    return root;
}

Result<std::filesystem::path> OfficialCertManager::ensure() {
    auto root = loadRoot();
    if (!root) return root.error();

    auto local = localPath();
    auto tmp = local;
    tmp += ".tmp";

    auto dl = download(tmp);
    if (!dl) return dl.error();

    auto cert = loadCertificateFile(tmp);
    if (!cert) {
        std::filesystem::remove(tmp);
        return cert.error();
    }

    auto verified = verifyCertificate(*cert, *root, "hpm://official", true);
    if (!verified) {
        std::filesystem::remove(tmp);
        return verified.error();
    }

    std::error_code ec;
    std::filesystem::rename(tmp, local, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return Error(ErrorCode::IoError,
                     "无法保存官方证书: " + ec.message());
    }

    std::ofstream fp(fingerprintPath());
    fp << certificateFingerprint(*cert) << "\n";

    return local;
}

Result<void> OfficialCertManager::download(
    const std::filesystem::path& dest) {
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

    auto url = officialUrl();
    const bool isFile = (std::string(url).rfind("file://", 0) == 0);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HPM_HTTP_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HPM_USER_AGENT);

    CertProgressCtx progress{observer_, "official.crt", 0};
    if (observer_) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, certProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        TransferEvent begin;
        begin.label = "official.crt";
        begin.phase = TransferPhase::Begin;
        observer_(begin);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    f.close();

    if (res != CURLE_OK) {
        std::filesystem::remove(dest);
        return Error(ErrorCode::NetworkError,
                     "官方证书下载失败: " +
                         std::string(curl_easy_strerror(res)));
    }
    if (!isFile && httpCode != 200) {
        std::filesystem::remove(dest);
        return Error(ErrorCode::NetworkError,
                     "官方证书下载失败: HTTP " +
                         std::to_string(httpCode));
    }
    return {};
}

} // namespace hpm::registry
