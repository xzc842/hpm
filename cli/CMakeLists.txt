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

# cli/CMakeLists.txt

add_executable(hpm
    src/main.cpp
    src/cmd_context.cpp
    src/command_registry.cpp
    src/output.cpp
    src/commands/init_cmd.cpp
    src/commands/sync_cmd.cpp
    src/commands/build_cmd.cpp
    src/commands/flash_cmd.cpp
    src/commands/clean_cmd.cpp
    src/commands/debug_cmd.cpp
    src/commands/dev_export_cmd.cpp
)

target_link_libraries(hpm
    PRIVATE
        hpm_core
        hpm_debug_host
        CLI11::CLI11
)