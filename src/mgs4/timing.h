#pragma once

namespace mgs4
{
    // Gates cutscene ("polygon demo") playback to the engine's native 60 Hz
    // tick, so it plays at the right speed while the game keeps rendering at
    // the selected framerate. Requires .text to be decrypted first.
    bool InstallCutsceneTimingFix();

    // Installs observation-only hooks on the other two cloth solvers (the
    // direct-jacket and hair paths). These do not change behaviour: they call
    // the original and report which instances each solver is handling, so a
    // given garment can be matched to the solver that actually drives it
    // instead of being guessed at.
    bool InstallClothDiagnostics();

    // Reports how often the cutscene update ran versus was gated out. At 120 fps
    // roughly half of the calls should be gated; at 60 fps, none.
    void LogTimingCounters(double intervalSeconds);

    // The frame timing globals resolved by the cutscene fix, so other fixes can
    // reuse them instead of resolving the same struct again. Null until
    // InstallCutsceneTimingFix has succeeded.
    const void* FrameTimingStruct();
    const void* FrameTickDelta60();
} // namespace mgs4
