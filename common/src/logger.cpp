// ============================================================================
// logger.cpp — Thread-safe file logger implementation
// ============================================================================
#include "wrsr/logger.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace wrsr::log {

namespace {
HANDLE     g_logFile = INVALID_HANDLE_VALUE;
std::mutex g_logMutex;

void writeLine(const char* level, const char* fmt, va_list args)
{
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char header[64];
    std::snprintf(header, sizeof(header), "[%02d:%02d:%02d.%03d][%s] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);

    char body[1024];
    std::vsnprintf(body, sizeof(body), fmt, args);

    std::string line = std::string(header) + body + "\n";

    std::lock_guard lock(g_logMutex);
    DWORD written = 0;
    WriteFile(g_logFile, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}
} // namespace

void init(const wchar_t* filename)
{
    std::lock_guard lock(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    for (DWORD i = n; i > 0; --i) {
        if (path[i] == L'\\') {
            path[i + 1] = L'\0';
            break;
        }
    }

    std::wstring logPath = std::wstring(path) + filename;
    g_logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void info(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    writeLine("INFO", fmt, a);
    va_end(a);
}

void warn(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    writeLine("WARN", fmt, a);
    va_end(a);
}

void error(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    writeLine("ERR ", fmt, a);
    va_end(a);
}

} // namespace wrsr::log
