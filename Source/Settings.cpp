#include "Settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/error/en.h>

#include <Debug.h>

static rapidjson::Document settingsDOM;
static bool settingsLoaded = false;

static bool GetBool(const char* key, bool defaultVal)
{
    if (!settingsLoaded || !settingsDOM.HasMember("KenshiPerfMod"))
        return defaultVal;
    const rapidjson::Value& cfg = settingsDOM["KenshiPerfMod"];
    if (cfg.HasMember(key) && cfg[key].IsBool())
        return cfg[key].GetBool();
    return defaultVal;
}

static int GetInt(const char* key, int defaultVal)
{
    if (!settingsLoaded || !settingsDOM.HasMember("KenshiPerfMod"))
        return defaultVal;
    const rapidjson::Value& cfg = settingsDOM["KenshiPerfMod"];
    if (cfg.HasMember(key) && cfg[key].IsInt())
        return cfg[key].GetInt();
    return defaultVal;
}

static float GetFloat(const char* key, float defaultVal)
{
    if (!settingsLoaded || !settingsDOM.HasMember("KenshiPerfMod"))
        return defaultVal;
    const rapidjson::Value& cfg = settingsDOM["KenshiPerfMod"];
    if (cfg.HasMember(key) && (cfg[key].IsFloat() || cfg[key].IsDouble()))
        return (float)cfg[key].GetDouble();
    return defaultVal;
}

static std::string GetString(const char* key, const std::string& defaultVal)
{
    if (!settingsLoaded || !settingsDOM.HasMember("KenshiPerfMod"))
        return defaultVal;
    const rapidjson::Value& cfg = settingsDOM["KenshiPerfMod"];
    if (cfg.HasMember(key) && cfg[key].IsString())
        return cfg[key].GetString();
    return defaultVal;
}

// Get the directory where our DLL lives (the mod folder)
static std::string GetModDirectory()
{
    HMODULE hModule = NULL;
    // Get handle to our own DLL
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetModDirectory,
        &hModule);
    if (!hModule)
        return "";

    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string fullPath(path);
    size_t lastSlash = fullPath.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        return fullPath.substr(0, lastSlash + 1);
    return "";
}

void PerfSettings::Init()
{
    // Look for config next to the DLL first, then in game root
    std::string modDir = GetModDirectory();
    std::string configPath = modDir + "KenshiPerfMod.json";
    std::ifstream file(configPath);
    if (!file.is_open())
    {
        // Fallback: game root directory
        file.open("KenshiPerfMod.json");
    }
    if (!file.is_open())
    {
        DebugLog("[KenshiPerfMod] No KenshiPerfMod.json found, using defaults");
        return;
    }

    rapidjson::IStreamWrapper isw(file);
    if (settingsDOM.ParseStream(isw).HasParseError())
    {
        ErrorLog("[KenshiPerfMod] Error parsing KenshiPerfMod.json: "
            + std::string(rapidjson::GetParseError_En(settingsDOM.GetParseError())));
        return;
    }

    settingsLoaded = true;
    DebugLog("[KenshiPerfMod] Settings loaded");
}

// Phase 0
bool PerfSettings::GetEnableProfiling()          { return GetBool("EnableProfiling", false); }
void PerfSettings::SetEnableProfiling(bool v)     { /* runtime toggle - TODO */ }
std::string PerfSettings::GetProfileOutputPath()  { return GetString("ProfileOutputPath", "KenshiPerfMod_profile.csv"); }
int PerfSettings::GetWorkerThreadCount()          { return GetInt("WorkerThreadCount", 0); }

// Phase 1
bool PerfSettings::GetEnableSpatialGrid()         { return GetBool("EnableSpatialGrid", true); }
float PerfSettings::GetGridCellSize()             { return GetFloat("GridCellSize", 200.0f); }

// Phase 2
bool PerfSettings::GetEnableSimulationLOD()       { return GetBool("EnableSimulationLOD", true); }
float PerfSettings::GetNearDistance()              { return GetFloat("NearDistance", 500.0f); }
float PerfSettings::GetMediumDistance()            { return GetFloat("MediumDistance", 2000.0f); }
int PerfSettings::GetFarUpdateInterval()          { return GetInt("FarUpdateInterval", 4); }
int PerfSettings::GetPathRequestsPerFrame()       { return GetInt("PathRequestsPerFrame", 10); }

// Phase 3
bool PerfSettings::GetEnableParallelThreadedUpdates() { return GetBool("EnableParallelThreadedUpdates", true); }

// Phase 4
bool PerfSettings::GetEnableParallelCharUpdate()  { return GetBool("EnableParallelCharUpdate", false); }
bool PerfSettings::GetEnablePrefetch()            { return GetBool("EnablePrefetch", true); }

// Phase 5
bool PerfSettings::GetEnableDailyUpdateSpreading(){ return GetBool("EnableDailyUpdateSpreading", true); }
int PerfSettings::GetDailyUpdateBatchSize()       { return GetInt("DailyUpdateBatchSize", 20); }

// Phase 6
bool PerfSettings::GetEnableParallelBuildings()   { return GetBool("EnableParallelBuildings", false); }

// Phase 7
bool PerfSettings::GetEnableModLoadOptimizer()    { return GetBool("EnableModLoadOptimizer", true); }
bool PerfSettings::GetEnablePathInterning()       { return GetBool("EnablePathInterning", true); }

// Phase 8
bool PerfSettings::GetEnableOgreAnimThreading()   { return GetBool("EnableOgreAnimThreading", false); }
