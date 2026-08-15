#pragma once

// Minimal tracing for the TwinScreen UMDF driver. Logs to the debugger
// (DbgView / WinDbg / Visual Studio output) so no file I/O is required.

#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace TwinScreen
{
    inline void TwinLog(const char* level, const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsprintf_s(buf, sizeof(buf), fmt, args);
        va_end(args);

        char line[1152];
        sprintf_s(line, sizeof(line), "[TwinScreen][%s] %s\n", level, buf);
        OutputDebugStringA(line);
    }
}

#define TS_TRACE(...) TwinScreen::TwinLog("I", __VA_ARGS__)
#define TS_WARN(...)  TwinScreen::TwinLog("W", __VA_ARGS__)
#define TS_ERROR(...) TwinScreen::TwinLog("E", __VA_ARGS__)
