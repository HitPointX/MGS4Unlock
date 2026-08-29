#pragma once

namespace mgs4
{
    // Corrects character movement and animation speed above 60 fps.
    //
    // Character control advances in whole ticks of a 300 Hz counter. 300 divides
    // evenly by 60, but not by 120 or 240, so the fractional part of each frame
    // is discarded and animation runs slow. The error is small at 120 and
    // obvious at 240. Carrying the remainder between frames makes the long-run
    // average match elapsed time.
    //
    // Takes the frame timing struct the cutscene fix already resolves.
    bool InstallCharacterTiming(const void* frameTimingStruct);

    void LogCharacterCounters(double intervalSeconds);
} // namespace mgs4
