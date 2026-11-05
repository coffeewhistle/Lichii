// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

#include "common/common_types.h"

namespace Service {

// Counts a command answered with a placeholder success because nothing implements it.
void NoteStubbedCommand(std::string_view service_name, u32 command_id);

// Logs the counted commands, most called first, and clears the counts.
void LogStubbedCommandSummary();

} // namespace Service
