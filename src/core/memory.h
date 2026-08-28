#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/module.h"

namespace memory
{
    // Restores the original page protection on scope exit. The reference
    // implementation calls VirtualProtect ad hoc and never restores, which
    // leaves .rdata constant pools permanently writable.
    class ProtectGuard
    {
    public:
        ProtectGuard(void* address, size_t size, DWORD protection = PAGE_EXECUTE_READWRITE)
            : address_(address), size_(size)
        {
            ok_ = ::VirtualProtect(address, size, protection, &old_) != FALSE;
        }

        ~ProtectGuard()
        {
            if (ok_)
            {
                DWORD ignored = 0;
                ::VirtualProtect(address_, size_, old_, &ignored);
            }
        }

        ProtectGuard(const ProtectGuard&) = delete;
        ProtectGuard& operator=(const ProtectGuard&) = delete;

        [[nodiscard]] bool ok() const { return ok_; }

    private:
        void* address_;
        size_t size_;
        DWORD old_ = 0;
        bool ok_ = false;
    };

    // A parsed signature. Wildcard bytes are held as an empty optional so that
    // no real byte value can collide with the wildcard marker.
    using Pattern = std::vector<std::optional<uint8_t>>;

    // Accepts IDA-style signatures: space-separated hex bytes with "?" or "??"
    // for wildcards, e.g. "48 89 5C 24 ?? 57 48 83 EC".
    Pattern ParsePattern(std::string_view signature);

    // Scans a byte range for the Nth (skip-th, 0-based) match. Returns nullptr
    // if there is no such match.
    uint8_t* Scan(std::span<uint8_t> haystack, const Pattern& pattern, size_t skip = 0);
    uint8_t* Scan(const module_info::Section& section, std::string_view signature, size_t skip = 0);

    // Counts matches. Useful to assert a signature is unique before trusting it
    // -- disambiguating by match ordinal (as the reference does) is fragile,
    // because any recompile that reorders functions silently changes meaning.
    size_t CountMatches(const module_info::Section& section, std::string_view signature);

    // Resolves a RIP-relative displacement. `displacement` points at the 4-byte
    // signed disp32 field, and `instructionEnd` is the address of the next
    // instruction (i.e. past any trailing immediate).
    uint8_t* ResolveRipRelative(const uint8_t* displacement, const uint8_t* instructionEnd);

    // Convenience for the common case where disp32 is the final field.
    uint8_t* ResolveRipRelative(const uint8_t* displacement);

    template <typename T>
    bool Read(const void* address, T& out)
    {
        if (!address)
            return false;
        std::memcpy(&out, address, sizeof(T));
        return true;
    }

    // Writes a value, temporarily lifting page protection. Returns false if the
    // protection change failed rather than faulting.
    template <typename T>
    bool Write(void* address, const T& value)
    {
        if (!address)
            return false;

        ProtectGuard guard(address, sizeof(T), PAGE_READWRITE);
        if (!guard.ok())
            return false;

        std::memcpy(address, &value, sizeof(T));
        return true;
    }
} // namespace memory
