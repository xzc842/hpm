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

// debug/host/src/bus_router.cpp
#include "bus_router.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace hpm::debug {

BusRouter::BusRouter(mcus::EventStream* events, Dispatch dispatch)
    : events_(events), dispatch_(std::move(dispatch)) {}

void BusRouter::route(const mcus::BusEvent& ev) {
    if (ev.direction != static_cast<uint8_t>(mcus::BusDirection::ToHost)) {
        return;
    }

    HpmDebugBusEvent be{};
    be.time = 0;
    be.bus_id = ev.bus_id;
    be.xfer_id = ev.xfer_id;
    be.kind = ev.kind;
    be.direction = ev.direction;
    be.flags = ev.flags;
    be.len = ev.len;
    be.data = ev.data;

    HpmDebugEvent out{};
    out.type = HPM_DEBUG_EVENT_BUS;
    out.u.bus = be;

    const bool isSpi =
        ev.kind == static_cast<uint8_t>(mcus::BusEventKind::SpiXfer);
    if (isSpi) {
        spiXferId_.store(ev.xfer_id, std::memory_order_release);
        spiResponded_.store(false, std::memory_order_release);
    }

    if (dispatch_) dispatch_(out);

    // SPI 主机在等 MISO；如果插件没有同步回复，给默认 0xFF，避免冻结到超时。
    if (isSpi && !spiResponded_.load(std::memory_order_acquire)) {
        const uint8_t miso = 0xFF;
        sendSpiResponse(ev.xfer_id, &miso, 1, 0);
    }
}

void BusRouter::routeUsb(const mcus::UsbEvent& ev) {
    HpmDebugUsbEvent ue{};
    ue.time = 0;
    ue.xfer_id = ev.xfer_id;
    ue.len = ev.len;
    ue.ep = ev.ep;
    ue.token = ev.token;
    ue.handshake = ev.handshake;
    ue.data = ev.data;

    HpmDebugEvent out{};
    out.type = HPM_DEBUG_EVENT_USB;
    out.u.usb = ue;

    if (dispatch_) dispatch_(out);
}

int BusRouter::send(uint32_t bus_id, uint32_t xfer_id, uint8_t kind,
                    uint8_t direction, const uint8_t* data, size_t len,
                    uint8_t flags) {
    mcus::DebugEvent ev{};
    ev.type = mcus::DebugEventType::Bus;
    auto& b = ev.u.bus;
    b.bus_id = bus_id;
    b.xfer_id = xfer_id;
    b.kind = kind;
    b.direction = direction;
    b.flags = flags;
    b.len = static_cast<uint16_t>(std::min<size_t>(len, sizeof(b.data)));
    if (data && b.len) std::memcpy(b.data, data, b.len);

    if (kind == static_cast<uint8_t>(mcus::BusEventKind::SpiXfer) &&
        direction == static_cast<uint8_t>(mcus::BusDirection::ToMcu) &&
        xfer_id == spiXferId_.load(std::memory_order_acquire)) {
        spiResponded_.store(true, std::memory_order_release);
    }
    return pushIn(ev);
}

int BusRouter::sendSpiResponse(uint32_t xfer_id, const uint8_t* data,
                               size_t len, uint8_t flags) {
    return send(0, xfer_id,
                static_cast<uint8_t>(mcus::BusEventKind::SpiXfer),
                static_cast<uint8_t>(mcus::BusDirection::ToMcu),
                data, len, flags);
}

int BusRouter::sendUsb(uint32_t xfer_id, uint8_t ep, uint8_t token,
                       uint8_t handshake, const uint8_t* data, size_t len) {
    mcus::DebugEvent ev{};
    ev.type = mcus::DebugEventType::Usb;
    auto& u = ev.u.usb;
    u.xfer_id = xfer_id;
    u.ep = ep;
    u.token = token;
    u.handshake = handshake;
    u.len = static_cast<uint16_t>(std::min<size_t>(len, sizeof(u.data)));
    if (data && u.len) std::memcpy(u.data, data, u.len);
    return pushIn(ev);
}

int BusRouter::pushIn(const mcus::DebugEvent& ev) {
    if (!events_) return -1;

    std::lock_guard<std::mutex> lock(pushMutex_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        if (events_->push_in(ev)) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return -1;
}

} // namespace hpm::debug
