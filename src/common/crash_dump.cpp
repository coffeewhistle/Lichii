// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/crash_dump.h"

#ifdef _WIN32
#include <atomic>
#include <string>
#include <system_error>

#include <windows.h>

#include <dbghelp.h>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#endif

namespace Common {

#ifdef _WIN32
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

constexpr size_t MaxDirectoryLength = 900;

MiniDumpWriteDumpFn write_dump = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
std::wstring dump_directory_wide;
wchar_t pid_suffix[32]{};
std::atomic_flag dump_started;

// Writes `value` as exactly `width` zero padded decimal digits and advances `out`.
void AppendDigits(wchar_t*& out, unsigned value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    }
    out += width;
}

void AppendString(wchar_t*& out, const wchar_t* text) {
    while (*text != L'\0') {
        *out++ = *text++;
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception) {
    if (dump_started.test_and_set()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    SYSTEMTIME time;
    GetLocalTime(&time);

    wchar_t path[MaxDirectoryLength + 64];
    wchar_t* out = path;
    AppendString(out, dump_directory_wide.c_str());
    AppendString(out, L"\lichii-");
    AppendDigits(out, time.wYear, 4);
    AppendDigits(out, time.wMonth, 2);
    AppendDigits(out, time.wDay, 2);
    *out++ = L'-';
    AppendDigits(out, time.wHour, 2);
    AppendDigits(out, time.wMinute, 2);
    AppendDigits(out, time.wSecond, 2);
    AppendString(out, pid_suffix);
    *out = L'\0';

    char path_utf8[4 * (MaxDirectoryLength + 64)];
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8, static_cast<int>(sizeof(path_utf8)),
                            nullptr, nullptr) == 0) {
        path_utf8[0] = '\0';
    }
    LOG_CRITICAL(Common, "Unhandled exception {:#x}, writing minidump to {}",
                 static_cast<unsigned long>(exception->ExceptionRecord->ExceptionCode), path_utf8);

    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exception;
        info.ClientPointers = FALSE;

        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info,
                             nullptr, nullptr);
        if (!ok) {
            // The large address space can make the richer dump fail; retry with thread info only.
            SetFilePointer(file, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(file);
            ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo,
                            &info, nullptr, nullptr);
        }
        CloseHandle(file);
        if (!ok) {
            LOG_CRITICAL(Common, "Writing the minidump failed (error {})",
                         static_cast<unsigned long>(GetLastError()));
        }
    }

    if (previous_filter != nullptr) {
        previous_filter(exception);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // Anonymous namespace
#endif

void InstallCrashHandler(const std::filesystem::path& dump_directory) {
#ifdef _WIN32
    dump_directory_wide = dump_directory.wstring();
    if (dump_directory_wide.empty() || dump_directory_wide.size() > MaxDirectoryLength) {
        LOG_WARNING(Common, "Crash dumps disabled: unusable dump directory");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dump_directory, ec);

    // Resolved at runtime so a missing dbghelp.dll never breaks startup.
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr) {
        LOG_WARNING(Common, "Crash dumps disabled: dbghelp.dll is unavailable");
        return;
    }
    write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
        reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (write_dump == nullptr) {
        LOG_WARNING(Common, "Crash dumps disabled: MiniDumpWriteDump is unavailable");
        return;
    }

    wchar_t* suffix = pid_suffix;
    *suffix++ = L'-';
    wchar_t digits[16];
    int count = 0;
    for (unsigned pid = GetCurrentProcessId(); count == 0 || pid != 0; pid /= 10) {
        digits[count++] = static_cast<wchar_t>(L'0' + pid % 10);
    }
    while (count > 0) {
        *suffix++ = digits[--count];
    }
    AppendString(suffix, L".dmp");
    *suffix = L'\0';

    previous_filter = SetUnhandledExceptionFilter(CrashFilter);
    LOG_INFO(Common, "Minidumps will be written to {}", FS::PathToUTF8String(dump_directory));
#else
    (void)dump_directory;
#endif
}

} // namespace Common
