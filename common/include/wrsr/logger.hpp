#pragma once
// ============================================================================
// logger.hpp — Thread-safe file logger placed beside SOVIET64.exe
// ============================================================================

namespace wrsr::log {

void init(const wchar_t* filename);
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

} // namespace wrsr::log
