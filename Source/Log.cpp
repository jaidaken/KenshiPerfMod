#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fstream>
#include <cstdarg>
#include <cstdio>

static std::ofstream s_logFile;
static LARGE_INTEGER s_startTime;
static LARGE_INTEGER s_freq;

static double GetElapsedSeconds()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_startTime.QuadPart) / (double)s_freq.QuadPart;
}

static void WriteEntry(const char* level, const std::string& msg)
{
    if (!s_logFile.is_open())
        return;

    char timeBuf[32];
    double elapsed = GetElapsedSeconds();
    int minutes = (int)(elapsed / 60.0);
    double seconds = elapsed - (minutes * 60.0);
    sprintf(timeBuf, "%02d:%06.3f", minutes, seconds);

    s_logFile << "[" << timeBuf << "] " << level << " " << msg << "\n";
    s_logFile.flush();
}

void PerfLog::Init()
{
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_startTime);

    // Find our DLL's directory
    HMODULE hModule = NULL;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&PerfLog::Init,
        &hModule);

    std::string logPath = "KenshiPerfMod_log.txt";
    if (hModule)
    {
        char path[MAX_PATH];
        GetModuleFileNameA(hModule, path, MAX_PATH);
        std::string fullPath(path);
        size_t lastSlash = fullPath.find_last_of("\\/");
        if (lastSlash != std::string::npos)
            logPath = fullPath.substr(0, lastSlash + 1) + "KenshiPerfMod_log.txt";
    }

    s_logFile.open(logPath, std::ios::trunc);
    if (s_logFile.is_open())
    {
        WriteEntry("INFO", "=== KenshiPerfMod Log Started ===");
        InfoF("Log file: %s", logPath.c_str());
    }
}

void PerfLog::Shutdown()
{
    if (s_logFile.is_open())
    {
        WriteEntry("INFO", "=== KenshiPerfMod Log Ended ===");
        s_logFile.close();
    }
}

void PerfLog::Info(const std::string& msg)  { WriteEntry("INFO ", msg); }
void PerfLog::Error(const std::string& msg) { WriteEntry("ERROR", msg); }
void PerfLog::Debug(const std::string& msg) { WriteEntry("DEBUG", msg); }

void PerfLog::InfoF(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Info(std::string(buf));
}

void PerfLog::ErrorF(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Error(std::string(buf));
}

void PerfLog::DebugF(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Debug(std::string(buf));
}
