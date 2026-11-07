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

// core/include/hpm/registry/certificate.h
#pragma once

#include <hpm/result.h>

#include <openssl/x509.h>

#include <filesystem>
#include <memory>
#include <string>

namespace hpm::registry {

struct X509Deleter {
    void operator()(X509* cert) const { X509_free(cert); }
};
using CertificatePtr = std::shared_ptr<X509>;

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
using PkeyPtr = std::shared_ptr<EVP_PKEY>;

// 从文件/PEM 字符串读取证书
Result<CertificatePtr> loadCertificateFile(const std::filesystem::path& path);
Result<CertificatePtr> loadCertificatePem(const std::string& pem,
                                          const std::string& label);

// 验证 leaf 是否由 issuer 签发，并检查 CA/KeyUsage/EKU/SAN
Result<void> verifyCertificate(const CertificatePtr& leaf,
                               const CertificatePtr& issuer,
                               const std::string& expectedSanUri,
                               bool requireCa);

// 验证自签名根证书
Result<void> verifyRootCertificate(const CertificatePtr& root);

// 证书公钥
PkeyPtr certificatePublicKey(const CertificatePtr& cert);

std::string certificateSerial(const CertificatePtr& cert);
std::string certificateFingerprint(const CertificatePtr& cert);
std::string certificateSubject(const CertificatePtr& cert);
std::string certificateIssuer(const CertificatePtr& cert);

} // namespace hpm::registry
