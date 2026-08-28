#pragma once

namespace mgs4
{
    // Rewrites one entry of the in-game framerate picker's option table so the
    // menu offers a higher rate. Safe to call before the DRM stub has finished
    // decrypting .text: the table lives in .data, which is never encrypted.
    bool PatchFrameratePicker();

    // Seeds the engine's cached target framerate so it does not fall back to 60.
    //
    // The engine resolves its target once, on first use, from a config variable
    // lookup that fails here, and memoizes the result. It returns the memoized
    // value untouched whenever it is not the -1 sentinel, so writing the wanted
    // rate before the game first asks makes that lookup moot. `framerate` is
    // normally the player's own saved choice.
    bool SeedTargetFramerate(int framerate);

    // Re-reads the option table, reports what the engine resolved as its target
    // framerate, and rewrites the options if something put the stock list back.
    void ReassertFrameratePicker();
} // namespace mgs4
