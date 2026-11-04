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

# debug/CMakeLists.txt
#
# mcus 源码通过 debug/mcus 软链接引入，作为 hpm 的一部分编译：
#   build/bin/mcus
#   build/lib/libmcus_core.so
#   build/lib/libmcus_isa_8051.so

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mcus/src/main.cpp)
    message(FATAL_ERROR
        "debug/mcus 不存在。请先创建软链接，例如：\n"
        "  ln -s /path/to/mcus ${CMAKE_CURRENT_SOURCE_DIR}/mcus")
endif()

include(FetchContent)

FetchContent_Declare(
    toml11
    GIT_REPOSITORY https://github.com/ToruNiina/toml11.git
    GIT_TAG        v4.0.2
)
FetchContent_MakeAvailable(toml11)

find_package(Threads REQUIRED)

set(MCUS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mcus)

# 模拟器核心：不含 8051 ISA
add_library(mcus_core SHARED
    ${MCUS_SOURCE_DIR}/src/model.cpp
    ${MCUS_SOURCE_DIR}/src/shared_memory.cpp
    ${MCUS_SOURCE_DIR}/src/event_stream.cpp
    ${MCUS_SOURCE_DIR}/src/cpu.cpp
    ${MCUS_SOURCE_DIR}/src/simulator.cpp
    ${MCUS_SOURCE_DIR}/src/config.cpp
    ${MCUS_SOURCE_DIR}/src/isa_registry.cpp
    ${MCUS_SOURCE_DIR}/src/firmware.cpp
    ${MCUS_SOURCE_DIR}/src/log.cpp
)

target_include_directories(mcus_core
    PUBLIC
        ${MCUS_SOURCE_DIR}/include
)

target_link_libraries(mcus_core
    PUBLIC
        Threads::Threads
        nlohmann_json::nlohmann_json
        toml11::toml11
)

target_compile_options(mcus_core PRIVATE -Wall -Wextra -Wpedantic)

# 8051 指令集：外部 ISA 动态库
add_library(mcus_isa_8051 SHARED
    ${MCUS_SOURCE_DIR}/src/isa_8051.cpp
)

target_link_libraries(mcus_isa_8051 PRIVATE mcus_core)

target_compile_options(mcus_isa_8051 PRIVATE -Wall -Wextra -Wpedantic)

set_target_properties(mcus_isa_8051 PROPERTIES
    INSTALL_RPATH "$ORIGIN"
)

# 模拟器可执行文件
add_executable(mcus ${MCUS_SOURCE_DIR}/src/main.cpp)

target_link_libraries(mcus
    PRIVATE
        mcus_core
        ${CMAKE_DL_LIBS}
)

set_target_properties(mcus PROPERTIES
    BUILD_RPATH "$ORIGIN/../lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# 调试工具：打印共享内存
add_executable(dump_shm ${MCUS_SOURCE_DIR}/tools/dump_shm.cpp)
target_link_libraries(dump_shm PRIVATE mcus_core)

install(TARGETS mcus mcus_core mcus_isa_8051 dump_shm
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
