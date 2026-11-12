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

// core/src/registry/revoke.cpp
#include <hpm/registry/revoke.h>

#include <hpm/json/json_io.h>

#include <cctype>
#include <ctime>
#include <cstdio>

namespace hpm::registry {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::time_t parseUtc(const std::string& value) {
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &m, &d,
                    &hh, &mm, &ss) != 6) {
        return static_cast<std::time_t>(-1);
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    return timegm(&tm);
}

} // namespace

std::string normalizeFingerprint(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(
                static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::string normalizeSerial(const std::string& value) {
    std::string out = upper(value);
    size_t pos = 0;
    while (pos + 1 < out.size() && out[pos] == '0') ++pos;
    return out.substr(pos);
}

bool RevocationList::contains(const CertificatePtr& cert) const {
    if (!cert) return false;
    const std::string serial = normalizeSerial(certificateSerial(cert));
    const std::string fp = normalizeFingerprint(certificateFingerprint(cert));

    for (const auto& entry : entries) {
        if (!entry.serial.empty() &&
            normalizeSerial(entry.serial) == serial) {
            return true;
        }
        if (!entry.certSha256.empty() &&
            normalizeFingerprint(entry.certSha256) == fp) {
            return true;
        }
    }
    return false;
}

bool RevocationList::fresh(int days) const {
    return freshSince(days, "");
}

bool RevocationList::freshSince(int days,
                                const std::string& minUpdatedAt) const {
    const std::time_t updated = parseUtc(updatedAt);
    if (updated < 0) return false;
    if (!minUpdatedAt.empty()) {
        const std::time_t minUpdated = parseUtc(minUpdatedAt);
        if (minUpdated < 0 || updated < minUpdated) return false;
    }
    const std::time_t now = std::time(nullptr);
    if (now + 300 < updated) return false; // 未来时间，拒绝
    return (now - updated) <= static_cast<std::time_t>(days) * 24 * 60 * 60;
}

Result<RevocationList> parseRevocation(const std::string& jsonText) {
    auto j = json::parse(jsonText);
    if (!j || !j->isObject()) {
        return Error(ErrorCode::InvalidField, "revoke.json 顶层必须是对象");
    }

    RevocationList list;
    list.schema = static_cast<int>(j->getInt("schema", 1));
    list.updatedAt = j->getString("updatedAt");

    auto issuer = j->get("issuer");
    if (issuer && issuer->isObject()) {
        list.issuerSan = issuer->getString("san");
        list.issuerFingerprint = issuer->getString("certSha256");
    }

    auto entries = j->get("entries");
    if (entries && entries->isArray()) {
        auto n = entries->size();
        for (size_t i = 0; i < (n ? *n : 0); ++i) {
            auto e = entries->at(i);
            if (!e || !e->isObject()) continue;
            RevokedCert r;
            r.serial = e->getString("serial");
            r.kind = e->getString("kind");
            r.name = e->getString("name");
            r.san = e->getString("san");
            r.subject = e->getString("subject");
            r.certSha256 = e->getString("certSha256");
            r.revokedAt = e->getString("revokedAt");
            r.reason = e->getString("reason");
            if (!r.serial.empty() || !r.certSha256.empty()) {
                list.entries.push_back(std::move(r));
            }
        }
    }

    return list;
}

} // namespace hpm::registry
