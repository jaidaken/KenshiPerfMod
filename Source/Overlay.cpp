#include "Overlay.h"
#include "Log.h"
#include "Settings.h"

#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/gui/MainBarGUI.h>
#include <core/Functions.h>

#include <mygui/MyGUI.h>

#include <cstdio>

static MyGUI::TextBox* s_overlayText = NULL;
static bool s_visible = false;
static bool s_initialized = false;

// Rolling average for smoother display
static const int AVG_FRAMES = 60;
static float s_totalHistory[AVG_FRAMES] = {0};
static float s_charsHistory[AVG_FRAMES] = {0};
static int s_historyIdx = 0;

static void CreateOverlayWidget()
{
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui)
        return;

    s_overlayText = gui->createWidget<MyGUI::TextBox>(
        "TextBox",
        MyGUI::IntCoord(10, 10, 400, 160),
        MyGUI::Align::Default,
        "Overlapped",
        "KenshiPerfMod_Overlay");

    if (s_overlayText)
    {
        s_overlayText->setTextColour(MyGUI::Colour(0.0f, 1.0f, 0.3f));
        s_overlayText->setFontName("KenshiFont16");
        s_overlayText->setTextShadow(true);
        s_overlayText->setCaption("KenshiPerfMod");
        s_overlayText->setVisible(false);
        s_visible = false;
        s_initialized = true;
        PerfLog::Info("Overlay widget created");
    }
    else
    {
        PerfLog::Error("Failed to create overlay widget");
    }
}

// Hook MainBarGUI constructor to know when the in-game UI is ready
static MainBarGUI* (*MainBarGUI_ctor_orig)(MainBarGUI*) = NULL;
static MainBarGUI* MainBarGUI_ctor_hook(MainBarGUI* thisptr)
{
    MainBarGUI* result = MainBarGUI_ctor_orig(thisptr);

    if (!s_initialized)
    {
        CreateOverlayWidget();
    }

    return result;
}

void PerfOverlay::Init()
{
    if (!PerfSettings::GetEnableProfiling())
        return;

    KenshiLib::AddHook(
        (void*)KenshiLib::GetRealAddress(&MainBarGUI::_CONSTRUCTOR),
        (void*)MainBarGUI_ctor_hook,
        (void**)&MainBarGUI_ctor_orig);

    PerfLog::Info("Overlay hook installed (waiting for in-game UI)");
}

void PerfOverlay::Update(float totalMs, float charsMs, float charsUTMs, float sysMsgMs, float killListMs, float dailyMs, int charCount)
{
    if (!s_initialized || !s_overlayText || !s_visible)
        return;

    // Update rolling average
    s_totalHistory[s_historyIdx] = totalMs;
    s_charsHistory[s_historyIdx] = charsMs;
    s_historyIdx = (s_historyIdx + 1) % AVG_FRAMES;

    float avgTotal = 0, avgChars = 0;
    for (int i = 0; i < AVG_FRAMES; ++i)
    {
        avgTotal += s_totalHistory[i];
        avgChars += s_charsHistory[i];
    }
    avgTotal /= AVG_FRAMES;
    avgChars /= AVG_FRAMES;

    float fps = (avgTotal > 0.001f) ? (1000.0f / avgTotal) : 0.0f;
    float charsPct = (avgTotal > 0.001f) ? (avgChars / avgTotal * 100.0f) : 0.0f;

    char buf[512];
    sprintf(buf,
        "KenshiPerfMod\n"
        "FPS: %.0f (%.1f ms)\n"
        "Characters: %d\n"
        "charsUpdate: %.1f ms (%.0f%%)\n"
        "charsUpdateUT: %.1f ms\n"
        "sysMessages: %.1f ms\n"
        "killList: %.1f ms\n"
        "%s",
        fps, avgTotal,
        charCount,
        charsMs, charsPct,
        charsUTMs,
        sysMsgMs,
        killListMs,
        (dailyMs > 0.1f) ? "dailyUpdate: ACTIVE" : "");

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

// Called each frame from the profiler main loop hook to check toggle key
void PerfOverlay::CheckToggleKey()
{
    // F12 to toggle overlay
    static bool keyWasDown = false;
    bool keyIsDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;

    if (keyIsDown && !keyWasDown)
        Toggle();

    keyWasDown = keyIsDown;
}
