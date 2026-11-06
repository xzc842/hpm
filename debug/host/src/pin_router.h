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

// debug/host/src/pin_router.h
#pragma once

#include <hpm/json/json_io.h>

#include <mcus/types.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace hpm::debug {

// 引脚订阅、合并和批量推送。
class PinRouter {
public:
    using InfoFn = std::function<json::Value(uint32_t id, uint32_t changed)>;
    using EmitFn = std::function<void(const json::Value&)>;
    using TimeFn = std::function<uint64_t()>;

    PinRouter(InfoFn info, EmitFn emit, TimeFn time);

    void subscribe(const std::set<uint32_t>& pins, bool all,
                   const std::string& mode, uint32_t intervalMs);
    void unsubscribe();
    void snapshot(const std::set<uint32_t>& pins);

    void queue(const mcus::PinEvent& ev);
    void flush();

private:
    InfoFn info_;
    EmitFn emit_;
    TimeFn time_;

    std::mutex mutex_;
    bool active_ = false;
    bool all_ = false;
    std::set<uint32_t> pins_;
    std::string mode_ = "change";
    uint32_t intervalMs_ = 50;
    std::chrono::steady_clock::time_point lastFlush_;
    std::map<uint32_t, mcus::PinEvent> pending_;
};

} // namespace hpm::debug
