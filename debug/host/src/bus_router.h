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

// debug/host/src/bus_router.h
#pragma once

#include <hpm/debug/plugin.h>

#include <mcus/event_stream.h>
#include <mcus/types.h>

#include <atomic>
#include <functional>
#include <mutex>

namespace hpm::debug {

// 在 mcus 的 BusEvent/USB 事件和设备插件之间转发。
// 所有 ToHost 事件广播给插件；插件通过 send()/sendSpiResponse() 回到 in 环。
class BusRouter {
public:
    using Dispatch = std::function<void(const HpmDebugEvent&)>;

    BusRouter(mcus::EventStream* events, Dispatch dispatch);

    void route(const mcus::BusEvent& ev);
    void routeUsb(const mcus::UsbEvent& ev);

    int send(uint32_t bus_id, uint32_t xfer_id, uint8_t kind,
             uint8_t direction, const uint8_t* data, size_t len,
             uint8_t flags);
    int sendSpiResponse(uint32_t xfer_id, const uint8_t* data, size_t len,
                        uint8_t flags);
    int sendUsb(uint32_t xfer_id, uint8_t ep, uint8_t token,
                uint8_t handshake, const uint8_t* data, size_t len);

private:
    int pushIn(const mcus::DebugEvent& ev);

    mcus::EventStream* events_;
    Dispatch dispatch_;
    std::mutex pushMutex_;
    std::atomic<uint32_t> spiXferId_{0};
    std::atomic<bool> spiResponded_{false};
};

} // namespace hpm::debug
