#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace module_info
{
    struct Section
    {
        uint8_t* begin = nullptr;
        size_t size = 0;

        [[nodiscard]] bool valid() const { return begin != nullptr && size != 0; }
        [[nodiscard]] uint8_t* end() const { return begin + size; }
        [[nodiscard]] bool contains(const void* p) const
        {
            const auto* b = static_cast<const uint8_t*>(p);
            return b >= begin && b < end();
        }
    };

    // Resolves the host executable's image base and section table. Must be
    // called before anything else here.
    bool Initialize();

    uintptr_t Base();

    // Section lookup by name, e.g. ".text", ".rdata", ".data".
    Section Find(std::string_view name);

    const Section& Text();
    const Section& RData();
    const Section& Data();

    // Turns a preferred-image-base RVA (as read off the on-disk file, whose
    // preferred base is 0x140000000) into a live pointer, accounting for ASLR.
    uint8_t* FromRva(uintptr_t rva);

    // mgs4.exe ships wrapped in a Steam DRM stub: .text on disk is ciphertext
    // and is only decrypted in memory some time after the process starts.
    // Blocks until .text looks like real x86-64 code, or until the timeout
    // elapses. Returns false on timeout.
    //
    // Detection is by content rather than by a fixed sleep: compiled MSVC code
    // is densely padded with int3 (0xCC) between functions, and encrypted bytes
    // are not, so the density of 4-byte 0xCC runs separates the two states by
    // orders of magnitude.
    bool WaitForUnpack(unsigned timeoutMs);

    // Fraction of the .text section made up of 4-byte 0xCC runs. Exposed for
    // logging so we can see the transition in the field.
    double PaddingDensity();
} // namespace module_info
