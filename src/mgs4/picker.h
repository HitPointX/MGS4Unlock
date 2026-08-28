#pragma once

namespace mgs4
{
    // Rewrites one entry of the in-game framerate picker's option table so the
    // menu offers a higher rate. Safe to call before the DRM stub has finished
    // decrypting .text: the table lives in .data, which is never encrypted.
    bool PatchFrameratePicker();
} // namespace mgs4
