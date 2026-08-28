#include "mgs4/picker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"

// The in-game framerate picker is driven by a -1-terminated int32 array in
// .data holding the selectable rates. On build [Code]a84606af it reads
// {30, 40, 60, -1} and sits at RVA 0x1b08de8.
//
// Two things make this a one-word patch rather than a menu reverse-engineering
// job. First, .data is not covered by the Steam DRM stub (only .text is
// encrypted), so the table is readable and writable without waiting for the
// unpack. Second, the binary contains no "30"/"40"/"60" label strings at all,
// which means the menu formats its labels from these very integers -- so
// changing a value changes what the player sees.

namespace
{
    // The framerate cvar-apply function clamps any parsed value into
    // {30, 40, 60}: below 30 snaps to 30, above 60 snaps to 60, and values in
    // between land on 30 or 40 depending on how close they are. It runs
    // whenever a config value is (re)applied, which is why a value seeded once
    // into the cached target does not stay put: this clamp eventually runs
    // again and puts 60 straight back.
    //
    // The signature covers the function's real prologue and is verified unique
    // across .text. The mid-hook lands a fixed offset further in, at the first
    // instruction of the clamp itself, which is validated against its exact
    // bytes before the hook is installed.
    constexpr const char* kClampFunctionSignature =
        "48 89 5C 24 18 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00 48";
    constexpr ptrdiff_t kClampOffset = 0xB4;                          // cmp ecx,0x3c
    constexpr ptrdiff_t kClampStoreOffset = 0x1A;                     // mov [rip+d],ecx
    constexpr std::array<uint8_t, 3> kClampFirstInstruction = {0x83, 0xF9, 0x3C};

    SafetyHookMid g_clampBypass{};
    int g_clampAllowedValue = 0;

    void ClampBypassHook(SafetyHookContext& ctx)
    {
        // ecx holds the parsed value at this point. Anything other than our one
        // allowed value falls through unchanged, so the stock 30/40/60 clamping
        // still applies to everything else.
        if (static_cast<int32_t>(ctx.rcx & 0xFFFFFFFF) == g_clampAllowedValue)
            ctx.rip += kClampStoreOffset;
    }

    constexpr uintptr_t kKnownTableRva = 0x1b08de8;

    // entries[3] is the engine's memoized target framerate, -1 until resolved.
    constexpr size_t kCachedTargetIndex = 3;
    constexpr int32_t kUnresolvedTarget = -1;
    constexpr size_t kEntryCount = 3;
    constexpr int32_t kTerminator = -1;

    // {30, 40, 60, -1} plus the terminator, as a byte signature.
    constexpr const char* kTableSignature =
        "1E 00 00 00 28 00 00 00 3C 00 00 00 FF FF FF FF";

    struct Table
    {
        int32_t* entries = nullptr;
        const char* how = "";
    };

    bool ShapeLooksRight(const int32_t* entries)
    {
        if (!entries)
            return false;

        // Rates must be plausible and strictly ascending, and the array must be
        // terminated where we expect. This is what stops us writing into
        // whatever happens to live at the old RVA after a game update.
        for (size_t i = 0; i < kEntryCount; ++i)
        {
            if (entries[i] < 10 || entries[i] > 1000)
                return false;
            if (i > 0 && entries[i] <= entries[i - 1])
                return false;
        }

        return entries[kEntryCount] == kTerminator;
    }

    // The option array is the head of a small framerate-control struct. The
    // engine's target-fps accessor reads entries[3] and compares it against a
    // -1 sentinel to decide whether the rate has been resolved yet, then bounds
    // the computed rate against entries[6], which holds 128.
    //
    // Dumping the whole struct on every run is what tells us, from the log
    // alone, whether the game re-populates the options from config after we
    // write, what it settles on as the cached target, and whether the cap needs
    // raising before 240 is reachable.
    void LogSurroundingStruct(const int32_t* entries)
    {
        logging::Info("picker: control struct = "
                      "{{options {}, {}, {} | cachedTarget {} | flag {} | pad {} | maxCap {}}}",
                      entries[0], entries[1], entries[2], entries[3], entries[4], entries[5],
                      entries[6]);

        if (entries[6] > 0 && entries[6] < 1000)
            logging::Info("picker: the engine's max-fps cap reads {}", entries[6]);
    }

    int32_t* g_table = nullptr;

    Table Locate()
    {
        // Search only the initialized part of .data. Scanning the full mapped
        // range costs about 120 ms here, which is long enough for the game to
        // read its saved settings and validate them against the stock option
        // list before our rewrite lands.
        const module_info::Section data = module_info::Data().initialized();
        if (!data.valid())
        {
            logging::Error("picker: the .data section was not found");
            return {};
        }

        const size_t matches = memory::CountMatches(data, kTableSignature);
        if (matches == 1)
        {
            auto* found = reinterpret_cast<int32_t*>(memory::Scan(data, kTableSignature));
            return {found, "signature"};
        }

        if (matches > 1)
        {
            logging::Warn("picker: the option-table signature matched {} times, so it is ambiguous",
                      matches);
        }
        else
        {
            logging::Warn("picker: the option-table signature did not match");
        }

        // Fall back to the address recorded for the pinned build. The shape
        // check below still has to pass before we write anything.
        auto* known = reinterpret_cast<int32_t*>(module_info::FromRva(kKnownTableRva));
        return {known, "known RVA"};
    }
} // namespace

bool mgs4::PatchFrameratePicker()
{
    const config::Settings& settings = config::Get();

    const Table table = Locate();
    if (!table.entries)
    {
        logging::Error("picker: could not locate the framerate option table");
        return false;
    }

    g_table = table.entries;
    logging::Address("framerateOptionTable", reinterpret_cast<uintptr_t>(table.entries));
    logging::Info("picker: located the option table by {}", table.how);
    LogSurroundingStruct(table.entries);

    if (!ShapeLooksRight(table.entries))
    {
        logging::Error("picker: the table failed validation, refusing to write. Found {{{}, {}, {}, {}}}",
                   table.entries[0], table.entries[1], table.entries[2], table.entries[3]);
        return false;
    }

    logging::Info("picker: current options are {{{}, {}, {}}}", table.entries[0], table.entries[1],
              table.entries[2]);

    // Write the whole array rather than swapping a single entry: the menu
    // renders the options in array order, so replacing just the middle slot
    // leaves the list reading 30 / 120 / 60.
    std::array<int32_t, kEntryCount> wanted{};
    for (size_t i = 0; i < kEntryCount; ++i)
    {
        const int value = settings.pickerValues[i];
        if (value < 10 || value > 1000)
        {
            logging::Error("picker: PickerValues entry {} ({}) is out of range", i, value);
            return false;
        }
        wanted[i] = static_cast<int32_t>(value);
    }

    if (std::equal(wanted.begin(), wanted.end(), table.entries))
    {
        logging::Info("picker: the options already match the configuration, nothing to do");
        return true;
    }

    if (!memory::Write(table.entries, wanted))
    {
        logging::Error("picker: the write to the option table failed");
        return false;
    }

    std::array<int32_t, kEntryCount> readback{};
    if (!memory::Read(table.entries, readback) || readback != wanted)
    {
        logging::Error("picker: readback mismatch, wanted {{{}, {}, {}}} but found {{{}, {}, {}}}",
                       wanted[0], wanted[1], wanted[2], readback[0], readback[1], readback[2]);
        return false;
    }

    logging::Info("picker: options are now {{{}, {}, {}}}", table.entries[0], table.entries[1],
                  table.entries[2]);
    return true;
}

void mgs4::ReassertFrameratePicker()
{
    if (!g_table)
        return;

    LogSurroundingStruct(g_table);

    const config::Settings& settings = config::Get();
    std::array<int32_t, kEntryCount> wanted{};
    for (size_t i = 0; i < kEntryCount; ++i)
        wanted[i] = static_cast<int32_t>(settings.pickerValues[i]);

    if (std::equal(wanted.begin(), wanted.end(), g_table))
        return;

    logging::Warn("picker: the option table was changed underneath us, rewriting");
    if (memory::Write(g_table, wanted))
        logging::Info("picker: options restored to {{{}, {}, {}}}", g_table[0], g_table[1],
                      g_table[2]);
}

bool mgs4::SeedTargetFramerate(int framerate)
{
    if (!g_table)
    {
        logging::Error("picker: cannot seed the target framerate, the table was not located");
        return false;
    }

    if (framerate < 10 || framerate > 1000)
    {
        logging::Error("picker: refusing to seed an out-of-range target framerate ({})", framerate);
        return false;
    }

    int32_t* cached = &g_table[kCachedTargetIndex];
    const int32_t before = *cached;

    // Only seed while the engine has not resolved a target of its own. If it
    // already has one, overwriting would fight whatever set it.
    if (before != kUnresolvedTarget)
    {
        logging::Info("picker: the engine already resolved a target of {}, leaving it alone",
                      before);
        return false;
    }

    if (!memory::Write(cached, static_cast<int32_t>(framerate)))
    {
        logging::Error("picker: failed to seed the target framerate");
        return false;
    }

    logging::Info("picker: seeded the engine's target framerate to {} (was unresolved)", framerate);
    return true;
}

bool mgs4::InstallFramerateClampBypass(int allowed)
{
    const module_info::Section& text = module_info::Text();

    const size_t matches = memory::CountMatches(text, kClampFunctionSignature);
    if (matches != 1)
    {
        logging::Error("picker: the clamp function signature matched {} times, expected 1",
                       matches);
        return false;
    }

    uint8_t* function = memory::Scan(text, kClampFunctionSignature);
    uint8_t* clamp = function + kClampOffset;

    if (std::memcmp(clamp, kClampFirstInstruction.data(), kClampFirstInstruction.size()) != 0)
    {
        logging::Error("picker: expected cmp ecx,0x3c at the clamp site, found {:02X} {:02X} {:02X}",
                       clamp[0], clamp[1], clamp[2]);
        return false;
    }

    logging::Address("framerateClampFunction", reinterpret_cast<uintptr_t>(function));
    logging::Address("framerateClampSite", reinterpret_cast<uintptr_t>(clamp));

    g_clampAllowedValue = allowed;
    g_clampBypass = safetyhook::create_mid(clamp, &ClampBypassHook);
    if (!g_clampBypass)
    {
        logging::Error("picker: failed to hook the framerate clamp");
        return false;
    }

    logging::Info("picker: the framerate clamp will now let {} fps through", allowed);
    return true;
}
