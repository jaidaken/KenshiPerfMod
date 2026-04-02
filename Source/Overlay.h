#pragma once

namespace PerfOverlay
{
    // Initialize overlay. Must be called after MyGUI is ready (in-game, not at startup).
    // Hook is installed at startup, overlay created when game reaches main menu.
    void Init();

    // Update overlay text with latest profiling data.
    // Called from the main loop hook each frame (main thread only).
    void Update(float totalMs, float gameLogicMs, float spatialMs, float spatialCount,
        float killListMs, float charsSpawned, int charCount, float gameSpeed,
        int platoonsActivated, int terrainLoads);

    // Toggle overlay visibility. Can be bound to a hotkey.
    void Toggle();

    // Returns true if overlay is currently visible.
    bool IsVisible();

    // Check for toggle key press. Call from main loop each frame.
    void CheckToggleKey();
}
