// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace Common {

/// Installs a handler that writes a minidump to `dump_directory` when the process dies from an
/// unhandled exception. Only does something on Windows; call it once, early in main.
void InstallCrashHandler(const std::filesystem::path& dump_directory);

} // namespace Common
