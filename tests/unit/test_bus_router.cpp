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

#include <catch2/catch_test_macros.hpp>

#include "bus_router.h"

#include <mcus/event_stream.h>

#include <sys/mman.h>
#include <unistd.h>

#include <string>

namespace {

std::string uniqueName() {
    return "/hpm_bus_test_" + std::to_string(::getpid());
}

mcus::DebugEvent makeSpiRequest(uint32_t xferId) {
    mcus::DebugEvent ev{};
    ev.type = mcus::DebugEventType::Bus;
    ev.u.bus.bus_id = 0;
    ev.u.bus.xfer_id = xferId;
    ev.u.bus.kind = static_cast<uint8_t>(mcus::BusEventKind::SpiXfer);
    ev.u.bus.direction =
        static_cast<uint8_t>(mcus::BusDirection::ToHost);
    ev.u.bus.len = 1;
    ev.u.bus.data[0] = 0xA5;
    return ev;
}

} // namespace

TEST_CASE("BusRouter responds to SPI request", "[bus]") {
    const std::string name = uniqueName();
    mcus::EventStream stream(name, 8, true);

    hpm::debug::BusRouter* router = nullptr;
    hpm::debug::BusRouter r(
        &stream,
        [&](const HpmDebugEvent& ev) {
            REQUIRE(ev.type == HPM_DEBUG_EVENT_BUS);
            REQUIRE(ev.u.bus.kind ==
                    static_cast<uint8_t>(mcus::BusEventKind::SpiXfer));
            const uint8_t miso = 0x3C;
            router->sendSpiResponse(ev.u.bus.xfer_id, &miso, 1, 0);
        });
    router = &r;

    auto req = makeSpiRequest(7);
    REQUIRE(stream.push(req));

    mcus::DebugEvent out;
    REQUIRE(stream.pop(out));
    r.route(out.u.bus);

    mcus::DebugEvent resp;
    REQUIRE(stream.pop_in(resp));
    REQUIRE(resp.type == mcus::DebugEventType::Bus);
    REQUIRE(resp.u.bus.xfer_id == 7);
    REQUIRE(resp.u.bus.direction ==
            static_cast<uint8_t>(mcus::BusDirection::ToMcu));
    REQUIRE(resp.u.bus.data[0] == 0x3C);

    ::shm_unlink(name.c_str());
}

TEST_CASE("BusRouter defaults SPI MISO when no plugin responds", "[bus]") {
    const std::string name = uniqueName();
    mcus::EventStream stream(name, 8, true);
    hpm::debug::BusRouter r(&stream, nullptr);

    auto req = makeSpiRequest(9);
    REQUIRE(stream.push(req));

    mcus::DebugEvent out;
    REQUIRE(stream.pop(out));
    r.route(out.u.bus);

    mcus::DebugEvent resp;
    REQUIRE(stream.pop_in(resp));
    REQUIRE(resp.u.bus.xfer_id == 9);
    REQUIRE(resp.u.bus.data[0] == 0xFF);

    ::shm_unlink(name.c_str());
}

TEST_CASE("BusRouter sends UART RX to mcus", "[bus]") {
    const std::string name = uniqueName();
    mcus::EventStream stream(name, 8, true);
    hpm::debug::BusRouter r(&stream, nullptr);

    const uint8_t data[] = {'h', 'i'};
    REQUIRE(r.send(1, 0, static_cast<uint8_t>(mcus::BusEventKind::UartRx),
                   static_cast<uint8_t>(mcus::BusDirection::ToMcu),
                   data, sizeof(data), 0) == 0);

    mcus::DebugEvent resp;
    REQUIRE(stream.pop_in(resp));
    REQUIRE(resp.u.bus.kind ==
            static_cast<uint8_t>(mcus::BusEventKind::UartRx));
    REQUIRE(resp.u.bus.bus_id == 1);
    REQUIRE(resp.u.bus.len == 2);
    REQUIRE(resp.u.bus.data[1] == 'i');

    ::shm_unlink(name.c_str());
}
