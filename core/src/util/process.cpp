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

// core/src/util/process.cpp
#include <hpm/util/process.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#include <direct.h>
#include <process.h>
#else
#define POPEN popen
#define PCLOSE pclose
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hpm::util {

namespace {

// 简单的 shell 转义
std::string shellEscape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

Status runProcess(const std::string& program,
                  const std::vector<std::string>& args,
                  bool verbose) {
    std::ostringstream cmd;
    cmd << shellEscape(program);
    for (const auto& a : args) {
        cmd << " " << shellEscape(a);
    }
    if (verbose) {
        cmd << " 2>&1";     // 把 stderr 合并到 stdout
    }

    FILE* pipe = POPEN(cmd.str().c_str(), "r");
    if (!pipe) {
        return Error(ErrorCode::IoError, "无法启动进程: " + program);
    }

    std::string output;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
        if (verbose) {
            std::fputs(buf.data(), stdout);
        }
    }

    int rc = PCLOSE(pipe);
    if (rc != 0) {
        if (!verbose) {
            return Error(ErrorCode::BuildFailed,
                         program + " 退出码 " + std::to_string(rc) +
                             "\n输出:\n" + output);
        }
        return Error(ErrorCode::BuildFailed,
                     program + " 退出码 " + std::to_string(rc));
    }
    return {};
}

#ifndef _WIN32
// 父进程收到 Ctrl+C / timeout 时转发给子进程，等子进程退出后自己再返回
volatile sig_atomic_t g_forward_pid = 0;

void forwardSignal(int sig) {
    pid_t pid = static_cast<pid_t>(g_forward_pid);
    if (pid > 0) ::kill(pid, sig);
}
#endif

Result<ProcessExit> runProcessInherit(
    const std::string& program,
    const std::vector<std::string>& args,
    const std::filesystem::path& workDir) {
#ifndef _WIN32
    struct sigaction act {};
    struct sigaction dfl {};
    struct sigaction oldInt {};
    struct sigaction oldQuit {};
    struct sigaction oldTerm {};

    act.sa_handler = forwardSignal;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;

    g_forward_pid = 0;
    ::sigaction(SIGINT, &act, &oldInt);
    ::sigaction(SIGQUIT, &act, &oldQuit);
    ::sigaction(SIGTERM, &act, &oldTerm);

    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(program);
    for (const auto& a : args) storage.push_back(a);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::sigaction(SIGINT, &oldInt, nullptr);
        ::sigaction(SIGQUIT, &oldQuit, nullptr);
        ::sigaction(SIGTERM, &oldTerm, nullptr);
        return Error(ErrorCode::IoError,
                     "fork 失败: " + std::string(std::strerror(errno)));
    }

    if (pid == 0) {
        ::sigaction(SIGINT, &dfl, nullptr);
        ::sigaction(SIGQUIT, &dfl, nullptr);
        ::sigaction(SIGTERM, &dfl, nullptr);

        if (!workDir.empty() && ::chdir(workDir.c_str()) != 0) {
            std::fprintf(stderr, "hpm: 无法进入目录 %s: %s\n",
                         workDir.c_str(), std::strerror(errno));
            ::_exit(127);
        }

        ::execvp(program.c_str(), argv.data());
        std::fprintf(stderr, "hpm: 无法启动 %s: %s\n",
                     program.c_str(), std::strerror(errno));
        ::_exit(127);
    }

    g_forward_pid = pid;

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    ::sigaction(SIGINT, &oldInt, nullptr);
    ::sigaction(SIGQUIT, &oldQuit, nullptr);
    ::sigaction(SIGTERM, &oldTerm, nullptr);
    g_forward_pid = 0;

    if (waited < 0) {
        return Error(ErrorCode::IoError,
                     "waitpid 失败: " + std::string(std::strerror(errno)));
    }

    ProcessExit result;
    if (WIFEXITED(status)) {
        result.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal = WTERMSIG(status);
    }
    return result;
#else
    (void)program;
    (void)args;
    (void)workDir;
    return Error(ErrorCode::InvalidArgument,
                 "hpm debug 目前仅支持 Linux");
#endif
}

} // namespace hpm::util