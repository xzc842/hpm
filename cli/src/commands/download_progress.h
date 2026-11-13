// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <hpm/registry/registry_client.h>
#include <hpm/util/progress_bar.h>

#include <map>
#include <memory>
#include <string>

namespace hpm::cli {

// 为每个下载文件维护独立进度条；索引、签名、吊销列表、包都走同一事件。
class DownloadProgressDisplay {
public:
    void update(const registry::TransferEvent& ev) {
        auto it = bars_.find(ev.label);
        if (ev.phase == registry::TransferPhase::Begin) {
            bars_[ev.label] =
                std::make_unique<util::ProgressBar>(ev.label, 0);
            bars_[ev.label]->update(0, 0);
            return;
        }
        if (it == bars_.end()) {
            bars_[ev.label] =
                std::make_unique<util::ProgressBar>(ev.label, 0);
            it = bars_.find(ev.label);
        }
        if (ev.phase == registry::TransferPhase::Progress) {
            it->second->update(ev.current, ev.total);
        } else if (ev.phase == registry::TransferPhase::Done) {
            it->second->update(ev.current, ev.total);
            it->second->done();
        } else if (ev.phase == registry::TransferPhase::Fail) {
            it->second->fail();
        }
    }

private:
    std::map<std::string, std::unique_ptr<util::ProgressBar>> bars_;
};

} // namespace hpm::cli
