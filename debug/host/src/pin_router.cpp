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

// debug/host/src/pin_router.cpp
#include "pin_router.h"

#include <algorithm>
#include <vector>

namespace hpm::debug {

PinRouter::PinRouter(InfoFn info, EmitFn emit, TimeFn time)
    : info_(std::move(info)), emit_(std::move(emit)), time_(std::move(time)) {}

void PinRouter::subscribe(const std::set<uint32_t>& pins, bool all,
                          const std::string& mode, uint32_t intervalMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
    all_ = all;
    pins_ = pins;
    mode_ = mode;
    intervalMs_ = intervalMs;
    pending_.clear();
    lastFlush_ = std::chrono::steady_clock::now();
}

void PinRouter::unsubscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    pending_.clear();
    pins_.clear();
}

void PinRouter::snapshot(const std::set<uint32_t>& pins) {
    if (!emit_) return;
    auto arr = json::Value::array();
    for (uint32_t id : pins) {
        arr.push(info_(id, 7));
    }
    auto out = json::Value::object();
    out.set("device", "mcu");
    out.set("type", "event");
    out.set("event", "pin.snapshot");
    out.set("time", time_ ? time_() : 0);
    out.set("pins", arr);
    emit_(out);
}

void PinRouter::queue(const mcus::PinEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    if (!all_ && !pins_.count(ev.pin_id)) return;

    auto it = pending_.find(ev.pin_id);
    if (it == pending_.end()) {
        pending_.emplace(ev.pin_id, ev);
    } else {
        it->second.value = ev.value;
        it->second.function = ev.function;
        it->second.direction = ev.direction;
        it->second.changed |= ev.changed;
    }
}

void PinRouter::flush() {
    if (!emit_) return;

    std::vector<uint32_t> ids;
    std::map<uint32_t, uint32_t> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastFlush_).count();
        if (elapsed < static_cast<long long>(intervalMs_)) return;

        if (mode_ == "all") {
            ids.assign(pins_.begin(), pins_.end());
        } else {
            for (const auto& [id, ev] : pending_) {
                ids.push_back(id);
                changed[id] = ev.changed;
            }
        }
        pending_.clear();
        lastFlush_ = now;
    }

    if (ids.empty()) return;

    auto arr = json::Value::array();
    for (uint32_t id : ids) {
        arr.push(info_(id, changed.count(id) ? changed[id] : 0));
    }

    auto out = json::Value::object();
    out.set("device", "mcu");
    out.set("type", "pin");
    out.set("time", time_ ? time_() : 0);
    out.set("pins", arr);
    emit_(out);
}

} // namespace hpm::debug
