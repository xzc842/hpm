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
# mcus 源码通过 git submodule debug/mcus 引入，作为 hpm 的一部分编译：
#   build/bin/mcus
#   build/lib/libmcus_core.so
#   build/lib/libmcus_isa_8051.so

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mcus/src/main.cpp)
    message(FATAL_ERROR
        "debug/mcus 不存在。请先初始化子模块：\n"
        "  git submodule update --init --recursive")
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

# ISA SDK：模型解析、日志、InstructionSet 默认钩子。
# 芯片包里的 debug/isa_*.cpp 在运行时链接这个静态库。
add_library(mcus_isa_sdk STATIC
    ${MCUS_SOURCE_DIR}/src/model.cpp
    ${MCUS_SOURCE_DIR}/src/log.cpp
    ${MCUS_SOURCE_DIR}/src/isa_base.cpp
)

target_include_directories(mcus_isa_sdk
    PUBLIC
        ${MCUS_SOURCE_DIR}/include
)

target_link_libraries(mcus_isa_sdk
    PUBLIC
        nlohmann_json::nlohmann_json
)

target_compile_options(mcus_isa_sdk PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(mcus_isa_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 模拟器核心：不含具体 ISA，ISA 由 .so 动态加载
add_library(mcus_core SHARED
    ${MCUS_SOURCE_DIR}/src/shared_memory.cpp
    ${MCUS_SOURCE_DIR}/src/event_stream.cpp
    ${MCUS_SOURCE_DIR}/src/cpu.cpp
    ${MCUS_SOURCE_DIR}/src/simulator.cpp
    ${MCUS_SOURCE_DIR}/src/config.cpp
    ${MCUS_SOURCE_DIR}/src/isa_registry.cpp
    ${MCUS_SOURCE_DIR}/src/firmware.cpp
)

target_include_directories(mcus_core
    PUBLIC
        ${MCUS_SOURCE_DIR}/include
)

target_link_libraries(mcus_core
    PUBLIC
        mcus_isa_sdk
        Threads::Threads
        nlohmann_json::nlohmann_json
        toml11::toml11
)

target_compile_options(mcus_core PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)

# 内置 ISA 动态库（hpm debug 默认仍可用；芯片包可覆盖）
add_library(mcus_isa_8051 SHARED
    ${MCUS_SOURCE_DIR}/src/isa_8051.cpp
)
target_link_libraries(mcus_isa_8051 PRIVATE mcus_isa_sdk)
target_compile_options(mcus_isa_8051 PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(mcus_isa_8051 PROPERTIES INSTALL_RPATH "$ORIGIN")

add_library(mcus_isa_arm SHARED
    ${MCUS_SOURCE_DIR}/src/isa_arm.cpp
)
target_link_libraries(mcus_isa_arm PRIVATE mcus_isa_sdk)
target_compile_options(mcus_isa_arm PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(mcus_isa_arm PROPERTIES INSTALL_RPATH "$ORIGIN")

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

# 芯片 ISA 由芯片包 debug/isa_*.cpp 现场编译，不安装内置 ISA。
install(TARGETS mcus mcus_core mcus_isa_sdk dump_shm
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# 插件开发头文件：拷贝到 build/include，供驱动 debug 插件编译时 -I
file(COPY ${CMAKE_SOURCE_DIR}/core/include/hpm/debug/plugin.h
     DESTINATION ${CMAKE_BINARY_DIR}/include/hpm/debug)

# mcus SDK 头文件：芯片包 debug/isa_*.cpp 需要 -I build/include
file(COPY ${MCUS_SOURCE_DIR}/include/mcus
     DESTINATION ${CMAKE_BINARY_DIR}/include)

add_subdirectory(host)

install(FILES ${CMAKE_SOURCE_DIR}/core/include/hpm/debug/plugin.h
        DESTINATION include/hpm/debug)

# 芯片包 debug/isa_*.cpp 现场编译所需的 SDK 头文件和源文件
install(DIRECTORY ${MCUS_SOURCE_DIR}/include/mcus
        DESTINATION include)
install(FILES
        ${MCUS_SOURCE_DIR}/src/model.cpp
        ${MCUS_SOURCE_DIR}/src/log.cpp
        ${MCUS_SOURCE_DIR}/src/isa_base.cpp
        DESTINATION src/mcus)
