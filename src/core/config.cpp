#include "core/config.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

#include "core/log.h"

namespace
{
    config::Settings g_settings;

    constexpr const char* kDefaultFile = R"(; MGS4Unlock configuration.

[Settings]
; The rates the in-game picker should offer, replacing the stock 30, 40, 60.
; The menu formats its labels from these numbers, so this is literally what you
; will see. Three or more are supported (up to 8); they are sorted for you.
;
; Note that 240 relies on the engine's 300 Hz character tick, which allows 1.25
; ticks per frame. Above 300 fps that drops below one tick per frame and the
; timing model breaks down, so do not go higher.
PickerValues = 30, 60, 120, 240

; Framerate handed to the engine at startup. 0 follows the choice you made in
; the in-game menu. Set a number here to force it regardless of the menu.
TargetFramerate = 0

; Set to false to leave the picker untouched.
PatchPicker = true

; Gates cutscene playback to the engine's native 60 Hz tick. Without this,
; cutscenes run at double speed above 60 fps.
GateCutscenes = true

; Gates the cloth solver to the engine's native 60 Hz tick. Without this,
; Snake's coat and kilt sway at double speed above 60 fps.
;
; Off by default: gating currently causes a doubled, semi-transparent cloth.
; Cause not yet understood. Set to true only when debugging that.
GateCloth = false

; Observation-only hooks on the remaining cloth solvers. Reports which garment
; instances each one handles, to identify which solver drives what.
ClothDiagnostics = true

; Writes the decrypted .text/.rdata/.data to dump/ after the DRM stub runs.
; Only needed when developing new signatures; costs ~30 MB per launch.
DumpSections = false

; Milliseconds to wait for the Steam DRM stub to decrypt .text before giving up.
UnpackTimeoutMs = 30000
)";

    std::string_view Trim(std::string_view text)
    {
        const auto notSpace = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
        const auto begin = std::find_if(text.begin(), text.end(), notSpace);
        const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
        return begin < end ? std::string_view(&*begin, static_cast<size_t>(end - begin))
                           : std::string_view{};
    }

    bool ParseInt(std::string_view text, int& out)
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }

    // Parses up to N comma-separated integers, reporting how many were read.
    // Any malformed entry rejects the whole list, so a typo leaves the defaults
    // in place rather than half-applying.
    template <size_t N>
    bool ParseIntList(std::string_view text, std::array<int, N>& out, size_t& outCount)
    {
        size_t count = 0;
        size_t start = 0;

        while (start <= text.size())
        {
            const size_t comma = text.find(',', start);
            const size_t end = comma == std::string_view::npos ? text.size() : comma;

            if (count >= N)
                return false;
            if (!ParseInt(Trim(text.substr(start, end - start)), out[count]))
                return false;
            ++count;

            if (comma == std::string_view::npos)
                break;
            start = comma + 1;
        }

        outCount = count;
        return count > 0;
    }

    bool ParseBool(std::string_view text, bool& out)
    {
        std::string lowered(text);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowered == "true" || lowered == "1" || lowered == "yes")
        {
            out = true;
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "no")
        {
            out = false;
            return true;
        }
        return false;
    }
} // namespace

void config::Load(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
    {
        std::ofstream out(file);
        if (out)
        {
            out << kDefaultFile;
            logging::Info("config: no ini found, wrote defaults to {}", narrow(file.wstring()));
        }
        else
        {
            logging::Warn("config: could not read or create {}, using defaults",
                      narrow(file.wstring()));
        }
        return;
    }

    std::string line;
    while (std::getline(in, line))
    {
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' ||
            trimmed.front() == '[')
            continue;

        const size_t equals = trimmed.find('=');
        if (equals == std::string_view::npos)
            continue;

        const std::string_view key = Trim(trimmed.substr(0, equals));
        const std::string_view value = Trim(trimmed.substr(equals + 1));

        if (key == "PickerValues")
        {
            std::array<int, kMaxPickerValues> parsed{};
            size_t count = 0;
            if (ParseIntList(value, parsed, count) && count >= 1)
            {
                // Sorted so the menu always reads in ascending order regardless
                // of how they were written in the ini.
                std::sort(parsed.begin(), parsed.begin() + count);
                g_settings.pickerValues = parsed;
                g_settings.pickerValueCount = count;
            }
            else
            {
                logging::Warn("config: PickerValues needs 1 to {} numbers: '{}'", kMaxPickerValues,
                              value);
            }
        }
        else if (key == "TargetFramerate")
        {
            if (!ParseInt(value, g_settings.targetFramerate))
                logging::Warn("config: TargetFramerate is not a number: '{}'", value);
        }
        else if (key == "PatchPicker")
        {
            if (!ParseBool(value, g_settings.patchPicker))
                logging::Warn("config: PatchPicker is not a boolean: '{}'", value);
        }
        else if (key == "GateCutscenes")
        {
            if (!ParseBool(value, g_settings.gateCutscenes))
                logging::Warn("config: GateCutscenes is not a boolean: '{}'", value);
        }
        else if (key == "GateCloth")
        {
            if (!ParseBool(value, g_settings.gateCloth))
                logging::Warn("config: GateCloth is not a boolean: '{}'", value);
        }
        else if (key == "ClothDiagnostics")
        {
            if (!ParseBool(value, g_settings.clothDiagnostics))
                logging::Warn("config: ClothDiagnostics is not a boolean: '{}'", value);
        }
        else if (key == "DumpSections")
        {
            if (!ParseBool(value, g_settings.dumpSections))
                logging::Warn("config: DumpSections is not a boolean: '{}'", value);
        }
        else if (key == "UnpackTimeoutMs")
        {
            int parsed = 0;
            if (ParseInt(value, parsed) && parsed >= 0)
                g_settings.unpackTimeoutMs = static_cast<unsigned>(parsed);
            else
                logging::Warn("config: UnpackTimeoutMs is not a positive number: '{}'", value);
        }
    }

    logging::Info("config: loaded {}", narrow(file.wstring()));
}

const config::Settings& config::Get()
{
    return g_settings;
}
