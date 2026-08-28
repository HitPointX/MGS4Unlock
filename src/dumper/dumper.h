#pragma once

#include <filesystem>

namespace dumper
{
    // Writes the decrypted .text, .rdata and .data contents to `directory`.
    // Must be called only after module_info::WaitForUnpack has succeeded.
    bool DumpSections(const std::filesystem::path& directory);
} // namespace dumper
