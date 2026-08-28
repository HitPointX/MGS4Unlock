#pragma once

namespace mgs4
{
    // Rewrites one entry of the in-game framerate picker's option table so the
    // menu offers a higher rate. Safe to call before the DRM stub has finished
    // decrypting .text: the table lives in .data, which is never encrypted.
    bool PatchFrameratePicker();

    // Installs a mid-hook on the config-value clamp that snaps any requested
    // framerate above 60 back down to 60. Without this, seeding the cached
    // target only holds until the clamp runs again, which happens periodically.
    // `allowed` is the one value the clamp should be allowed to let through
    // unclamped; every other input keeps the stock 30/40/60 clamping behaviour.
    bool InstallFramerateClampBypass(int allowed);

    // Changes which value the installed clamp bypass lets through, without
    // reinstalling the hook. Needed because the player's live menu selection
    // can change after startup, and the clamp must let the newly selected rate
    // through too.
    void SetFramerateClampAllowedValue(int allowed);

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

    // Re-reads the cached target framerate and forces it back to `framerate` if
    // it no longer matches. Unlike SeedTargetFramerate, this overwrites a value
    // the engine has already resolved.
    //
    // At least one code path sets the target directly through a small setter
    // function that bypasses the clamp bypass hook entirely, so seeding once and
    // clamp-bypassing the config-apply path are not sufficient on their own.
    // Polling for drift catches it regardless of which path caused it.
    void ReassertTargetFramerate(int framerate);
} // namespace mgs4
