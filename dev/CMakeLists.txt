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

find_package(libzip REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(hpm-dev
    src/main.cpp
    src/dev_context.cpp
    src/dev_config.cpp
    src/commands/init_cmd.cpp
    src/commands/validate_cmd.cpp
    src/commands/publish_cmd.cpp
    src/commands/login_cmd.cpp
    src/commands/creat_cmd.cpp
    src/http.cpp

)

target_link_libraries(hpm-dev
    PRIVATE
        hpm_core
        CLI11::CLI11
        OpenSSL::SSL
        OpenSSL::Crypto
        libzip::zip
        Threads::Threads
)

target_include_directories(hpm-dev
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(hpm-dev SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/httplib)
install(TARGETS hpm-dev RUNTIME DESTINATION bin)
set_target_properties(hpm-dev PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
