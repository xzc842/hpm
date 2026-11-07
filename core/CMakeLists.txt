# Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
add_library(hpm_core STATIC
    src/version.cpp
    src/model/version.cpp
    src/model/version_range.cpp
    src/json/json_value.cpp
    src/json/json_io.cpp
    src/manifest/field_reader.cpp
    src/manifest/project_loader.cpp
    src/manifest/driver_loader.cpp
    src/manifest/state_loader.cpp
    src/manifest/chip_loader.cpp
    src/capability/pin_allocator.cpp
    src/capability/pin_matcher.cpp
    src/capability/method_resolver.cpp
    src/capability/capability_checker.cpp
    src/resolver/id_generator.cpp
    src/resolver/resolver.cpp
    src/generator/template_engine.cpp
    src/generator/generator.cpp
    src/util/fs_utils.cpp
    src/util/process.cpp
    src/generator/hpm_h_generator.cpp
    src/registry/index.cpp
    src/registry/registry_client.cpp
    src/registry/certificate.cpp
    src/registry/revoke.cpp
    src/registry/signature.cpp
    src/registry/official_cert.cpp
)

target_include_directories(hpm_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hpm_core
    PUBLIC
        nlohmann_json::nlohmann_json
        fmt::fmt
        tomlplusplus::tomlplusplus
    PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
)