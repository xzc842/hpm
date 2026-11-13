// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hpm::registry {

enum class TransferPhase { Begin, Progress, Done, Fail };
struct TransferEvent {
    std::string label;
    uint64_t current = 0;
    uint64_t total = 0;
    TransferPhase phase = TransferPhase::Progress;
};
using TransferObserver = std::function<void(const TransferEvent&)>;

} // namespace hpm::registry
