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

// cli/src/output.cpp
#include "output.h"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define ISATTY isatty(fileno(stdout))
#endif

namespace hpm::cli {

namespace {

bool g_color = true;
bool g_json_mode = false;

std::ostream& out() { return g_json_mode ? std::cerr : std::cout; }

const char* c_reset  = "\033[0m";
const char* c_red    = "\033[31m";
const char* c_green  = "\033[32m";
const char* c_yellow = "\033[33m";
const char* c_blue   = "\033[34m";
const char* c_gray   = "\033[90m";

const char* col(const char* code) {
    return g_color ? code : "";
}

} // namespace

void Output::setColor(bool on) {
    g_color = on && ISATTY;
}

void Output::setJsonMode(bool on) {
    g_json_mode = on;
}

void Output::info(const std::string& msg) {
    out() << col(c_blue) << "info" << col(c_reset) << " " << msg << "\n";
}

void Output::success(const std::string& msg) {
    out() << col(c_green) << "ok" << col(c_reset) << " " << msg << "\n";
}

void Output::warn(const std::string& msg) {
    out() << col(c_yellow) << "warn" << col(c_reset) << " " << msg << "\n";
}

void Output::error(const std::string& msg) {
    std::cerr << col(c_red) << "error" << col(c_reset) << " " << msg << "\n";
}

void Output::step(const std::string& msg) {
    out() << col(c_blue) << "==>" << col(c_reset) << " " << msg << "\n";
}

void Output::detail(const std::string& msg) {
    out() << "    " << col(c_gray) << msg << col(c_reset) << "\n";
}

} // namespace hpm::cli