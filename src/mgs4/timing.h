#pragma once

namespace mgs4
{
    // Gates cutscene ("polygon demo") playback to the engine's native 60 Hz
    // tick, so it plays at the right speed while the game keeps rendering at
    // the selected framerate. Requires .text to be decrypted first.
    bool InstallCutsceneTimingFix();

    // Reports how often the cutscene update ran versus was gated out. At 120 fps
    // roughly half of the calls should be gated; at 60 fps, none.
    void LogTimingCounters();
} // namespace mgs4
