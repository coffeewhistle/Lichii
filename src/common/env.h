// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <cstdlib>
#include <string_view>
#include <type_traits>

#include "common/logging.h"

namespace Common::Env {

/// True if the variable exists, even when its value is empty.
inline bool IsSet(const char* name) {
    return std::getenv(name) != nullptr;
}

/// Unset gives the default, a value starting with '0' gives false, anything else gives true.
inline bool Flag(const char* name, bool default_value = false) {
    const char* value = std::getenv(name);
    return value == nullptr ? default_value : value[0] != '0';
}

/// Raw value, or nullptr if the variable is unset.
inline const char* String(const char* name) {
    return std::getenv(name);
}

/// Parses the whole value as an integer. Base 16 accepts a 0x prefix, base 0 picks 16 for 0x and
/// 10 otherwise. Malformed input logs a warning and returns the default.
template <typename T>
T Number(const char* name, T default_value, int base = 10) {
    static_assert(std::is_integral_v<T>);
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return default_value;
    }
    const std::string_view text{value};
    const bool has_prefix = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    int parse_base = base;
    std::string_view digits = text;
    if (has_prefix && (base == 16 || base == 0)) {
        parse_base = 16;
        digits.remove_prefix(2);
    } else if (base == 0) {
        parse_base = 10;
    }
    T parsed{};
    const char* const last = digits.data() + digits.size();
    const auto [end, ec] = std::from_chars(digits.data(), last, parsed, parse_base);
    if (ec != std::errc{} || end != last) {
        LOG_WARNING(Common, "{}={}: not a valid number, using {}", name, value, default_value);
        return default_value;
    }
    return parsed;
}

} // namespace Common::Env
