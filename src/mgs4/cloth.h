#pragma once

namespace mgs4
{
    // Installs the coordinated cloth timing fix.
    //
    // Cloth cannot be corrected by gating the solver alone. The engine times all
    // simulation work through one shared function, so gating cloth while leaving
    // that function untouched leaves cloth and the rest of the pipeline running
    // on different clocks, which shows on screen as two overlapping layers of
    // the same garment. See cloth.cpp for the full reasoning.
    //
    // Takes the frame timing globals the cutscene fix already resolves, rather
    // than resolving them again.
    bool InstallClothTiming(const void* frameTimingStruct, const void* frameTickDelta60);

    void LogClothCounters();
} // namespace mgs4
