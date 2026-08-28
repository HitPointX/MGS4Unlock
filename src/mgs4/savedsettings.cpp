#include "mgs4/savedsettings.h"

#include <charconv>
#include <fstream>
#include <string>
#include <system_error>

#include "core/log.h"

// The game persists the player's "Max Frame Rate" choice as a plain integer in
//
//     <game>/mgs4_savedata_win/<steamid>/mgs4/mgs4.savedsettings
//
// which is a small CRLF ini:
//
//     [SavedSettings]
//     displayIndex=0
//     api=dx11
//     vsync=false
//     fpsLimiter=120
//     ...
//
// The file is written correctly on quit, but nothing reads it back into the
// engine's target-framerate path, which is why a chosen rate above the stock
// list does not survive a relaunch. Reading it ourselves lets us restore the
// player's actual choice.

namespace
{
    constexpr const char* kKey = "fpsLimiter=";

    std::filesystem::path FindSettingsFile(const std::filesystem::path& gameDir)
    {
        // Layout is <game>/MGS4/<our dll> and <game>/mgs4_savedata_win/<id>/mgs4/.
        // The account id is not known to us, so scan for it.
        //
        // gameDir arrives with a trailing separator, which makes its last
        // component empty, so parent_path() would only strip the separator
        // rather than moving up a level. Normalise before walking up.
        std::filesystem::path base = gameDir;
        if (base.filename().empty())
            base = base.parent_path();

        const std::filesystem::path saves = base.parent_path() / "mgs4_savedata_win";

        std::error_code ec;
        if (!std::filesystem::is_directory(saves, ec))
        {
            logging::Warn("savedsettings: no save directory at {}", narrow(saves.wstring()));
            return {};
        }

        for (const auto& account : std::filesystem::directory_iterator(saves, ec))
        {
            if (!account.is_directory())
                continue;

            std::filesystem::path candidate = account.path() / "mgs4" / "mgs4.savedsettings";
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        }

        return {};
    }
} // namespace

std::optional<int> mgs4::ReadSavedFramerate(const std::filesystem::path& gameDir)
{
    const std::filesystem::path file = FindSettingsFile(gameDir);
    if (file.empty())
    {
        logging::Warn("savedsettings: could not find mgs4.savedsettings");
        return std::nullopt;
    }

    std::ifstream in(file);
    if (!in)
    {
        logging::Warn("savedsettings: could not read {}", narrow(file.wstring()));
        return std::nullopt;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.starts_with(kKey))
            continue;

        std::string_view value(line);
        value.remove_prefix(std::char_traits<char>::length(kKey));
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.remove_suffix(1);

        int parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{})
        {
            logging::Warn("savedsettings: fpsLimiter is not a number: '{}'", value);
            return std::nullopt;
        }

        logging::Info("savedsettings: {} has fpsLimiter={}", narrow(file.filename().wstring()),
                      parsed);
        return parsed;
    }

    logging::Warn("savedsettings: no fpsLimiter entry in {}", narrow(file.wstring()));
    return std::nullopt;
}
