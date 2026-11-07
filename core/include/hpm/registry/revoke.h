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

// core/include/hpm/registry/revoke.h
#pragma once

#include <hpm/registry/certificate.h>
#include <hpm/result.h>

#include <string>
#include <vector>

namespace hpm::registry {

struct RevokedCert {
    std::string serial;
    std::string kind;
    std::string name;
    std::string san;
    std::string subject;
    std::string certSha256;
    std::string revokedAt;
    std::string reason;
};

struct RevocationList {
    int schema = 1;
    std::string updatedAt;
    std::string issuerSan;
    std::string issuerFingerprint;
    std::vector<RevokedCert> entries;

    bool contains(const CertificatePtr& cert) const;
    bool fresh(int days = 7) const;
};

Result<RevocationList> parseRevocation(const std::string& jsonText);
std::string normalizeFingerprint(const std::string& value);
std::string normalizeSerial(const std::string& value);

} // namespace hpm::registry
