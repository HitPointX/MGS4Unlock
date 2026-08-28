#pragma once

#include <array>
#include <filesystem>

namespace config
{
    struct Settings
    {
        // The complete set of rates the in-game picker should offer, replacing
        // the stock {30, 40, 60}. The menu formats its labels from these
        // integers, so whatever is written here is what the player sees. Kept
        // sorted so the menu reads in ascending order.
        std::array<int, 3> pickerValues{30, 60, 120};

        // Master switch for the picker patch.
        bool patchPicker = true;

        // Gates cutscene playback to the engine's native 60 Hz tick. Without
        // this, cutscenes play at double speed above 60 fps.
        bool gateCutscenes = true;

        // Writes the decrypted sections to disk once the DRM stub has run, for
        // offline analysis. Off by default: it costs ~30 MB per launch.
        bool dumpSections = false;

        // How long to wait for the Steam DRM stub to decrypt .text.
        unsigned unpackTimeoutMs = 30000;
    };

    // Reads the ini next to our DLL, writing a commented default file if none
    // exists. Missing or malformed keys keep their defaults.
    void Load(const std::filesystem::path& file);

    const Settings& Get();
} // namespace config
