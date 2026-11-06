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

# tests/CMakeLists.txt

add_executable(hpm_tests
    unit/test_version.cpp
    unit/test_json.cpp
    unit/test_model.cpp
    unit/test_manifest.cpp
    unit/test_capability.cpp
    unit/test_resolver.cpp
    unit/test_generator.cpp
    unit/test_toolchain.cpp
    unit/test_bus_router.cpp
    ../debug/host/src/bus_router.cpp
)

target_link_libraries(hpm_tests
    PRIVATE
        hpm_core
        mcus_core
        Catch2::Catch2WithMain
)

target_include_directories(hpm_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/debug/host/src
)

include(Catch)
catch_discover_tests(hpm_tests)