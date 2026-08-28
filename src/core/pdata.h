#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// mgs4.exe ships with its exception directory intact and in the clear: .pdata
// holds ~90k RUNTIME_FUNCTION records giving the exact start of every function
// in .text. Only .text and the Steam stub's .bind are encrypted.
//
// That gives us two things the reference implementation had no access to:
//
//   1. A trustworthy unpack detector. We know precisely where functions begin,
//      so we can ask whether those addresses currently hold plausible x86-64
//      prologues. Ciphertext scores near chance; real code scores near 100%.
//
//   2. Anchored signature scanning. A function-prologue signature only ever
//      needs testing at real function starts -- roughly 90k positions instead
//      of 23 million -- which is both far faster and structurally unable to
//      match inside another function's body.

namespace pdata
{
    struct RuntimeFunction
    {
        uint32_t beginRva;
        uint32_t endRva;
        uint32_t unwindRva;
    };

    // Locates the exception directory. Safe to call before .text is decrypted.
    bool Initialize();

    std::span<const RuntimeFunction> Functions();

    // Fraction of sampled function starts holding a byte that can legitimately
    // begin an MSVC-generated x86-64 function. Uniformly random bytes score
    // around 0.08; decrypted code scores above 0.95.
    double PrologueAgreement(size_t sampleCount = 512);
} // namespace pdata
