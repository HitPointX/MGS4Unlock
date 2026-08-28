#pragma once

namespace mgs4
{
    // Gates cutscene ("polygon demo") playback to the engine's native 60 Hz
    // tick, so it plays at the right speed while the game keeps rendering at
    // the selected framerate. Requires .text to be decrypted first.
    bool InstallCutsceneTimingFix();

    // Gates cloth simulation to the engine's native 60 Hz tick, without
    // disturbing the per-frame bookkeeping (a double-buffer flip among it) that
    // the same function performs on every call regardless of simulation rate.
    // Without this, Snake's coat and kilt sway at double speed above 60 fps.
    bool InstallClothTimingFix();

    // Installs observation-only hooks on the other two cloth solvers (the
    // direct-jacket and hair paths). These do not change behaviour: they call
    // the original and report which instances each solver is handling, so a
    // given garment can be matched to the solver that actually drives it
    // instead of being guessed at.
    bool InstallClothDiagnostics();

    // Reports how often the cutscene update ran versus was gated out. At 120 fps
    // roughly half of the calls should be gated; at 60 fps, none.
    void LogTimingCounters();
} // namespace mgs4
