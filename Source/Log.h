#pragma once

#include <string>

namespace PerfLog
{
    // Initialize the log file. Call once at startup.
    void Init();

    // Close the log file.
    void Shutdown();

    // Log a message with timestamp.
    void Info(const std::string& msg);
    void Error(const std::string& msg);
    void Debug(const std::string& msg);

    // Log with printf-style formatting.
    void InfoF(const char* fmt, ...);
    void ErrorF(const char* fmt, ...);
    void DebugF(const char* fmt, ...);
}
