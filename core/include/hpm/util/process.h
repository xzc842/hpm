/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// core/include/hpm/util/process.h
#pragma once

#include <hpm/result.h>

#include <filesystem>
#include <string>
#include <vector>

namespace hpm::util {

// 运行外部命令，捕获输出
// - verbose 为 true 时把 stdout/stderr 直接透传到终端
// - 返回非零退出码 → Error
Status runProcess(const std::string& program,
                  const std::vector<std::string>& args,
                  bool verbose = false);

// 子进程退出信息
struct ProcessExit {
    int  code = 0;        // WEXITSTATUS
    bool signaled = false; // 是否被信号终止
    int  signal = 0;      // 终止信号
};

// 在当前终端启动子进程（stdio 继承），等待其退出。
//
// Linux 下 fork/exec：父进程忽略 SIGINT/SIGQUIT，Ctrl+C 只终止子进程，
// 父进程在 waitpid 返回后恢复信号处理。workDir 非空时子进程切换工作目录。
Result<ProcessExit> runProcessInherit(
    const std::string& program,
    const std::vector<std::string>& args,
    const std::filesystem::path& workDir = {});

} // namespace hpm::util