// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "core/hle/service/nx2_stub_stats.h"

namespace Service {

namespace {

std::mutex g_mutex;
std::map<std::pair<std::string, u32>, u64> g_counts;

} // Anonymous namespace

void NoteStubbedCommand(std::string_view service_name, u32 command_id) {
    std::scoped_lock lk{g_mutex};
    ++g_counts[{std::string{service_name}, command_id}];
}

void LogStubbedCommandSummary() {
    std::vector<std::pair<std::pair<std::string, u32>, u64>> entries;
    {
        std::scoped_lock lk{g_mutex};
        entries.assign(g_counts.begin(), g_counts.end());
        g_counts.clear();
    }
    if (entries.empty()) {
        return;
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr size_t MaxLines = 100;
    LOG_INFO(Service, "Auto-stubbed IPC commands this session ({} kinds):", entries.size());
    for (size_t i = 0; i < (std::min)(entries.size(), MaxLines); ++i) {
        const auto& [key, count] = entries[i];
        LOG_INFO(Service, "  {}::{}: {} calls", key.first, key.second, count);
    }
    if (entries.size() > MaxLines) {
        LOG_INFO(Service, "  ... and {} more", entries.size() - MaxLines);
    }
}

} // namespace Service
