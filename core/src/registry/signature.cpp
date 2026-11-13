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

// core/src/registry/signature.cpp
#include <hpm/registry/signature.h>

#include <hpm/registry/certificate.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace hpm::registry {

namespace {

std::vector<unsigned char> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>());
}

std::string toHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

Result<bool> verifyWithKey(EVP_PKEY* pkey,
                           const std::filesystem::path& sigFile,
                           const std::filesystem::path& dataFile) {
    if (!pkey) {
        return Error(ErrorCode::InvalidField, "证书没有公钥");
    }

    auto sig = readFile(sigFile);
    if (sig.empty()) {
        return Error(ErrorCode::IoError,
                     "无法读签名: " + sigFile.string());
    }

    auto data = readFile(dataFile);
    if (data.empty()) {
        return Error(ErrorCode::IoError,
                     "无法读数据: " + dataFile.string());
    }

    const int keyType = EVP_PKEY_base_id(pkey);
    const EVP_MD* md = nullptr;
    if (keyType == EVP_PKEY_RSA || keyType == EVP_PKEY_EC) {
        md = EVP_sha256();
    } else if (keyType == EVP_PKEY_ED25519 ||
               keyType == EVP_PKEY_ED448) {
        md = nullptr;
    } else {
        return Error(ErrorCode::InvalidField,
                     "不支持的证书公钥类型: " + std::to_string(keyType));
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx) {
        if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, pkey) == 1) {
            if (md) {
                if (EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1) {
                    ok = EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1;
                }
            } else {
                ok = EVP_DigestVerify(ctx, sig.data(), sig.size(),
                                      data.data(), data.size()) == 1;
            }
        }
        EVP_MD_CTX_free(ctx);
    }
    return ok;
}

} // namespace

Result<bool> verifySignatureWithCert(const std::filesystem::path& certPem,
                                     const std::filesystem::path& sigFile,
                                     const std::filesystem::path& dataFile) {
    auto cert = loadCertificateFile(certPem);
    if (!cert) return cert.error();

    auto key = certificatePublicKey(*cert);
    if (!key) {
        return Error(ErrorCode::InvalidField,
                     "无法读取证书公钥: " + certPem.string());
    }
    return verifyWithKey(key.get(), sigFile, dataFile);
}

Result<bool> verifySignatureWithPem(const std::string& certPem,
                                    const std::filesystem::path& sigFile,
                                    const std::filesystem::path& dataFile) {
    auto cert = loadCertificatePem(certPem, "signature cert");
    if (!cert) return cert.error();
    auto key = certificatePublicKey(*cert);
    if (!key) {
        return Error(ErrorCode::InvalidField,
                     "无法读取 PEM 证书公钥");
    }
    return verifyWithKey(key.get(), sigFile, dataFile);
}

std::string sha256File(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(f.gcount()));
    }

    EVP_DigestFinal_ex(ctx, hash, &hashLen);
    EVP_MD_CTX_free(ctx);

    return toHex(hash, hashLen);
}

} // namespace hpm::registry
