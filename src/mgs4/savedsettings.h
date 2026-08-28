#pragma once

#include <filesystem>
#include <optional>

namespace mgs4
{
    // Reads the player's chosen "Max Frame Rate" out of the game's own settings
    // file. `gameDir` is the directory holding mgs4.exe.
    std::optional<int> ReadSavedFramerate(const std::filesystem::path& gameDir);
} // namespace mgs4
