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

        // Framerate to hand the engine at startup. 0 means follow whatever the
        // player last chose in the in-game menu, which the game saves but never
        // reads back into its target-framerate path.
        int targetFramerate = 0;

        // Master switch for the picker patch.
        bool patchPicker = true;

        // Gates cutscene playback to the engine's native 60 Hz tick. Without
        // this, cutscenes play at double speed above 60 fps.
        bool gateCutscenes = true;

        // Gates the cloth solver to the engine's native 60 Hz tick.
        bool gateCloth = true;

        // On frames where the cloth solver is skipped, re-publish the existing
        // transform instead of leaving it untouched. Intended to avoid judder,
        // but if the publish path advances a double buffer it causes ghosting
        // instead, so it defaults off.
        bool clothPublishOnSkip = false;

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
