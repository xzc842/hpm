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

// core/include/hpm/registry/official_cert.h
#pragma once

#include <hpm/registry/certificate.h>
#include <hpm/result.h>

#include <filesystem>

namespace hpm::registry {

// 官方仓库证书管理：
// - 根证书固定在 config.h / 环境变量中；
// - 官方证书从固定地址下载；
// - 只有通过根证书验证的官方证书才会被使用。
class OfficialCertManager {
public:
    explicit OfficialCertManager(std::filesystem::path cacheDir);

    Result<std::filesystem::path> ensure();

    static const char* officialUrl();

    std::filesystem::path localPath() const;
    std::filesystem::path fingerprintPath() const;

private:
    Result<void> download(const std::filesystem::path& dest);
    Result<CertificatePtr> loadRoot() const;

    std::filesystem::path cacheDir_;
};

} // namespace hpm::registry
