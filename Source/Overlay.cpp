#include "Overlay.h"
#include "Log.h"
#include "Settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <core/Functions.h>

#include <mygui/MyGUI.h>

#include <cstdio>

static MyGUI::TextBox* s_overlayText = NULL;
static bool s_visible = false;
static bool s_initialized = false;
static bool s_createFailed = false;

// Rolling average for smoother display
static const int AVG_FRAMES = 60;
static float s_totalHistory[AVG_FRAMES] = {0};
static float s_charsHistory[AVG_FRAMES] = {0};
static int s_historyIdx = 0;

static void CreateOverlayWidget()
{
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui)
    {
        PerfLog::Error("Overlay: MyGUI::Gui instance is null");
        s_createFailed = true;
        return;
    }

    // Use Kenshi's own skin for text and the "Info" layer which renders on top of everything
    s_overlayText = gui->createWidget<MyGUI::TextBox>(
        "Kenshi_TextboxStandardText",
        MyGUI::IntCoord(10, 50, 420, 200),
        MyGUI::Align::Default,
        "Info",
        "KenshiPerfMod_Overlay");

    if (s_overlayText)
    {
        s_overlayText->setTextColour(MyGUI::Colour(0.2f, 1.0f, 0.4f));
        s_overlayText->setCaption("KenshiPerfMod - F12 to toggle");
        s_overlayText->setVisible(true);
        s_visible = true;
        s_initialized = true;
        PerfLog::Info("Overlay widget created");
    }
    else
    {
        PerfLog::Error("Overlay: createWidget returned null");
        s_createFailed = true;
    }
}

void PerfOverlay::Init()
{
    if (!PerfSettings::GetEnableProfiling())
        return;

    PerfLog::Info("Overlay will be created on first update");
}

void PerfOverlay::Update(float totalMs, float gameLogicMs, float spatialMs, float spatialCount, float killListMs, float unused, int charCount)
{
    // Lazy init
    if (!s_initialized && !s_createFailed)
        CreateOverlayWidget();

    if (!s_overlayText || !s_visible)
        return;

    // Update rolling average
    s_totalHistory[s_historyIdx] = totalMs;
    s_charsHistory[s_historyIdx] = spatialMs;
    s_historyIdx = (s_historyIdx + 1) % AVG_FRAMES;

    float avgTotal = 0, avgSpatial = 0;
    for (int i = 0; i < AVG_FRAMES; ++i)
    {
        avgTotal += s_totalHistory[i];
        avgSpatial += s_charsHistory[i];
    }
    avgTotal /= AVG_FRAMES;
    avgSpatial /= AVG_FRAMES;

    float fps = (avgTotal > 0.001f) ? (1000.0f / avgTotal) : 0.0f;
    float spatialPct = (avgTotal > 0.001f) ? (avgSpatial / avgTotal * 100.0f) : 0.0f;

    char buf[512];
    sprintf(buf,
        "KenshiPerfMod  [F12 hide]\n"
        "FPS: %.0f (%.1f ms)\n"
        "Characters: %d\n"
        "Spatial queries: %.0f/frame %.2f ms (%.0f%%)\n"
        "killList: %.3f ms",
        fps, avgTotal,
        charCount,
        spatialCount, spatialMs, spatialPct,
        killListMs);

    s_overlayText->setCaption(buf);
}

void PerfOverlay::Toggle()
{
    if (!s_initialized || !s_overlayText)
        return;

    s_visible = !s_visible;
    s_overlayText->setVisible(s_visible);
    PerfLog::InfoF("Overlay %s", s_visible ? "shown" : "hidden");
}

bool PerfOverlay::IsVisible()
{
    return s_visible;
}

void PerfOverlay::CheckToggleKey()
{
    static bool keyWasDown = false;
    bool keyIsDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;

    if (keyIsDown && !keyWasDown)
        Toggle();

    keyWasDown = keyIsDown;
}
