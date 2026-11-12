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

// core/include/hpm/registry/signature.h
#pragma once

#include <hpm/result.h>

#include <filesystem>
#include <string>

namespace hpm::registry {

// 用 X.509 证书中的公钥验证 detached signature
Result<bool> verifySignatureWithCert(const std::filesystem::path& certPem,
                                     const std::filesystem::path& sigFile,
                                     const std::filesystem::path& dataFile);

// 直接用 PEM 字符串中的证书公钥验证 detached signature。
Result<bool> verifySignatureWithPem(const std::string& certPem,
                                    const std::filesystem::path& sigFile,
                                    const std::filesystem::path& dataFile);

// 计算文件 SHA256（十六进制字符串）
std::string sha256File(const std::filesystem::path& file);

} // namespace hpm::registry
