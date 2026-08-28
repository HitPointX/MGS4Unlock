#include "core/memory.h"

#include <cctype>

namespace
{
    int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
} // namespace

memory::Pattern memory::ParsePattern(std::string_view signature)
{
    Pattern pattern;

    for (size_t i = 0; i < signature.size();)
    {
        const char c = signature[i];

        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }

        if (c == '?')
        {
            // Accept both "?" and "??" as a single wildcard byte.
            ++i;
            if (i < signature.size() && signature[i] == '?')
                ++i;
            pattern.emplace_back(std::nullopt);
            continue;
        }

        const int hi = HexValue(c);
        if (hi < 0 || i + 1 >= signature.size())
            return {};

        const int lo = HexValue(signature[i + 1]);
        if (lo < 0)
            return {};

        pattern.emplace_back(static_cast<uint8_t>(hi * 16 + lo));
        i += 2;
    }

    return pattern;
}

uint8_t* memory::Scan(std::span<uint8_t> haystack, const Pattern& pattern, size_t skip)
{
    if (pattern.empty() || haystack.size() < pattern.size())
        return nullptr;

    // Anchor on the first non-wildcard byte so the common case rejects quickly
    // without walking the whole pattern.
    size_t anchor = 0;
    while (anchor < pattern.size() && !pattern[anchor].has_value())
        ++anchor;
    if (anchor == pattern.size())
        return nullptr;

    const uint8_t anchorByte = *pattern[anchor];
    const size_t last = haystack.size() - pattern.size();
    size_t found = 0;

    for (size_t i = 0; i <= last; ++i)
    {
        if (haystack[i + anchor] != anchorByte)
            continue;

        bool match = true;
        for (size_t j = 0; j < pattern.size(); ++j)
        {
            if (pattern[j].has_value() && haystack[i + j] != *pattern[j])
            {
                match = false;
                break;
            }
        }

        if (match && found++ == skip)
            return haystack.data() + i;
    }

    return nullptr;
}

uint8_t* memory::Scan(const module_info::Section& section, std::string_view signature, size_t skip)
{
    if (!section.valid())
        return nullptr;

    return Scan(std::span<uint8_t>(section.begin, section.size), ParsePattern(signature), skip);
}

size_t memory::CountMatches(const module_info::Section& section, std::string_view signature)
{
    if (!section.valid())
        return 0;

    const Pattern pattern = ParsePattern(signature);
    const std::span<uint8_t> haystack(section.begin, section.size);

    size_t count = 0;
    while (Scan(haystack, pattern, count) != nullptr)
        ++count;

    return count;
}

uint8_t* memory::ResolveRipRelative(const uint8_t* displacement, const uint8_t* instructionEnd)
{
    if (!displacement || !instructionEnd)
        return nullptr;

    int32_t offset = 0;
    std::memcpy(&offset, displacement, sizeof(offset));
    return const_cast<uint8_t*>(instructionEnd) + offset;
}

uint8_t* memory::ResolveRipRelative(const uint8_t* displacement)
{
    return ResolveRipRelative(displacement, displacement + 4);
}
