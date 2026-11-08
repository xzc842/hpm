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

// core/src/registry/certificate.cpp
#include <hpm/registry/certificate.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace hpm::registry {

namespace {

Result<CertificatePtr> fromX509(X509* cert, const std::string& label) {
    if (!cert) {
        return Error(ErrorCode::InvalidField,
                     "证书格式错误: " + label);
    }
    return CertificatePtr(cert, X509Deleter{});
}

bool hasSanUri(X509* cert, const std::string& expected) {
    int critical = 0;
    auto* names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, &critical, nullptr));
    if (!names) return false;

    bool found = false;
    const int count = sk_GENERAL_NAME_num(names);
    for (int i = 0; i < count && !found; ++i) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
        if (name->type != GEN_URI) continue;

        const ASN1_STRING* uri = name->d.uniformResourceIdentifier;
        if (!uri) continue;
        const unsigned char* data = ASN1_STRING_get0_data(uri);
        const int len = ASN1_STRING_length(uri);
        if (data && expected.size() == static_cast<size_t>(len) &&
            std::memcmp(data, expected.data(), expected.size()) == 0) {
            found = true;
        }
    }
    GENERAL_NAMES_free(names);
    return found;
}

bool hasCodeSigningEku(X509* cert) {
    int critical = 0;
    auto* eku = static_cast<EXTENDED_KEY_USAGE*>(
        X509_get_ext_d2i(cert, NID_ext_key_usage, &critical, nullptr));
    if (!eku) return false;

    bool found = false;
    const int count = sk_ASN1_OBJECT_num(eku);
    for (int i = 0; i < count; ++i) {
        const ASN1_OBJECT* obj = sk_ASN1_OBJECT_value(eku, i);
        if (OBJ_obj2nid(obj) == NID_code_sign) {
            found = true;
            break;
        }
    }
    EXTENDED_KEY_USAGE_free(eku);
    return found;
}

} // namespace

Result<CertificatePtr> loadCertificateFile(const std::filesystem::path& path) {
    FILE* f = fopen(path.string().c_str(), "r");
    if (!f) {
        return Error(ErrorCode::IoError,
                     "无法打开证书: " + path.string());
    }
    X509* cert = PEM_read_X509(f, nullptr, nullptr, nullptr);
    fclose(f);
    return fromX509(cert, path.string());
}

Result<CertificatePtr> loadCertificatePem(const std::string& pem,
                                          const std::string& label) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return Error(ErrorCode::Internal, "BIO_new_mem_buf 失败");
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return fromX509(cert, label);
}

Result<void> verifyCertificate(const CertificatePtr& leaf,
                               const CertificatePtr& issuer,
                               const std::string& expectedSanUri,
                               bool requireCa) {
    if (!leaf || !issuer) {
        return Error(ErrorCode::InvalidField, "证书为空");
    }

    if (X509_check_issued(issuer.get(), leaf.get()) != X509_V_OK) {
        return Error(ErrorCode::ChecksumMismatch,
                     "证书 issuer 不匹配: " +
                         certificateSubject(leaf));
    }

    if ((X509_get_extension_flags(issuer.get()) & EXFLAG_CA) == 0) {
        return Error(ErrorCode::ChecksumMismatch,
                     "签发者证书不是 CA: " +
                         certificateSubject(issuer));
    }
    if ((X509_get_key_usage(issuer.get()) & KU_KEY_CERT_SIGN) == 0) {
        return Error(ErrorCode::ChecksumMismatch,
                     "签发者证书缺少 keyCertSign: " +
                         certificateSubject(issuer));
    }
    if (requireCa) {
        const long raw_pathlen = X509_get_pathlen(issuer.get());
        const int pathlen = static_cast<int>(raw_pathlen);
        if (pathlen >= 0 && pathlen < 1) {
            return Error(ErrorCode::ChecksumMismatch,
                         "签发者证书 pathlen 不允许再签发 CA");
        }
    }

    X509_STORE* store = X509_STORE_new();
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    if (!store || !ctx) {
        if (store) X509_STORE_free(store);
        if (ctx) X509_STORE_CTX_free(ctx);
        return Error(ErrorCode::Internal, "创建证书链上下文失败");
    }

    X509_STORE_add_cert(store, issuer.get());
    X509_STORE_CTX_init(ctx, store, leaf.get(), nullptr);

    X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(ctx);
    X509_VERIFY_PARAM_set_flags(param,
        X509_V_FLAG_X509_STRICT | X509_V_FLAG_PARTIAL_CHAIN);

    const int rc = X509_verify_cert(ctx);
    if (rc != 1) {
        const int err = X509_STORE_CTX_get_error(ctx);
        const std::string message = X509_verify_cert_error_string(err);
        X509_STORE_CTX_free(ctx);
        X509_STORE_free(store);
        return Error(ErrorCode::ChecksumMismatch,
                     "证书链验证失败: " + message);
    }
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);

    const uint32_t flags = X509_get_extension_flags(leaf.get());
    const bool isCa = (flags & EXFLAG_CA) != 0;
    if (isCa != requireCa) {
        return Error(ErrorCode::ChecksumMismatch,
                     requireCa ? "证书不是 CA" : "证书不应是 CA");
    }

    const uint32_t usage = X509_get_key_usage(leaf.get());
    if (requireCa) {
        if ((usage & KU_KEY_CERT_SIGN) == 0) {
            return Error(ErrorCode::ChecksumMismatch,
                         "CA 证书缺少 keyCertSign");
        }
    } else if ((usage & KU_DIGITAL_SIGNATURE) == 0) {
        return Error(ErrorCode::ChecksumMismatch,
                     "证书缺少 digitalSignature");
    }

    if (!hasCodeSigningEku(leaf.get())) {
        return Error(ErrorCode::ChecksumMismatch,
                     "证书缺少 codeSigning EKU");
    }

    if (!expectedSanUri.empty() && !hasSanUri(leaf.get(), expectedSanUri)) {
        return Error(ErrorCode::ChecksumMismatch,
                     "证书 SAN 不匹配，期望: " + expectedSanUri);
    }

    return {};
}

Result<void> verifyRootCertificate(const CertificatePtr& root) {
    if (!root) {
        return Error(ErrorCode::InvalidField, "根证书为空");
    }

    const uint32_t flags = X509_get_extension_flags(root.get());
    if ((flags & EXFLAG_CA) == 0) {
        return Error(ErrorCode::ChecksumMismatch, "根证书不是 CA");
    }
    if (X509_NAME_cmp(X509_get_subject_name(root.get()),
                      X509_get_issuer_name(root.get())) != 0) {
        return Error(ErrorCode::ChecksumMismatch, "根证书不是自签名");
    }
    if ((X509_get_key_usage(root.get()) & KU_KEY_CERT_SIGN) == 0) {
        return Error(ErrorCode::ChecksumMismatch,
                     "根证书缺少 keyCertSign");
    }

    auto key = certificatePublicKey(root);
    if (!key) {
        return Error(ErrorCode::InvalidField, "无法读取根证书公钥");
    }
    if (X509_verify(root.get(), key.get()) != 1) {
        return Error(ErrorCode::ChecksumMismatch, "根证书自签名无效");
    }
    return {};
}

PkeyPtr certificatePublicKey(const CertificatePtr& cert) {
    if (!cert) return {};
    return PkeyPtr(X509_get_pubkey(cert.get()), PkeyDeleter{});
}

std::string certificateSerial(const CertificatePtr& cert) {
    if (!cert) return "";
    const ASN1_INTEGER* serial = X509_get_serialNumber(cert.get());
    if (!serial) return "";
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
    if (!bn) return "";
    char* hex = BN_bn2hex(bn);
    BN_free(bn);
    if (!hex) return "";
    std::string out(hex);
    OPENSSL_free(hex);
    return out;
}

std::string certificateFingerprint(const CertificatePtr& cert) {
    if (!cert) return "";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (X509_digest(cert.get(), EVP_sha256(), digest, &len) != 1) {
        return "";
    }
    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
        if (i > 0) oss << ":";
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(digest[i]);
    }
    return oss.str();
}

std::string certificateSubject(const CertificatePtr& cert) {
    if (!cert) return "";
    char buf[512];
    X509_NAME_oneline(X509_get_subject_name(cert.get()), buf, sizeof(buf));
    return buf;
}

std::string certificateIssuer(const CertificatePtr& cert) {
    if (!cert) return "";
    char buf[512];
    X509_NAME_oneline(X509_get_issuer_name(cert.get()), buf, sizeof(buf));
    return buf;
}

} // namespace hpm::registry
