#include "mgs4/character.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"

// Character control timing.
//
// Character movement and animation are not driven by the frame clock directly.
// They advance in whole ticks of a 300 Hz counter, and each frame the engine
// converts the elapsed frame time into a number of those ticks. 300 divides
// evenly by 60, so at the framerate the game shipped with a frame is worth
// exactly five ticks and nothing is lost.
//
// Above 60 the division stops being exact:
//
//     60 fps   -> 5.00 ticks per frame
//     120 fps  -> 2.50
//     240 fps  -> 1.25
//
// The engine wants a whole number, so the fractional part is dropped. At 120 the
// leftover half tick is large enough that the error is not obvious, but at 240
// each frame discards a quarter tick, and character animation ends up advancing
// at roughly four fifths of real time. That is the "everything feels slower at
// 240" symptom, and it gets worse the further the framerate is from a divisor
// of 300.
//
// Carrying the remainder from one frame to the next fixes it: the tick counts
// still come out whole, but their long-run average matches elapsed time exactly.
// At 240 that produces a repeating 1,1,1,2 pattern averaging 1.25.
//
// Note this also sets the practical ceiling for the whole approach. Above 300
// fps a frame is worth less than a single tick, so the counter would have to be
// zero on some frames, and the character update is not built to be called with
// nothing to advance. 300 fps is the hard limit regardless of anything else.

namespace
{
    constexpr const char* kFrameTimeUpdateSignature =
        "40 53 48 83 EC 20 83 3D ?? ?? ?? ?? 00 BB";

    // Both live in the frame timing struct the cutscene fix already resolves.
    constexpr ptrdiff_t kFrameDeltaOffset = 0x18;
    constexpr ptrdiff_t kTickDelta300Offset = 0x24;

    constexpr double kCharacterTickRate = 300.0;

    SafetyHookInline g_frameTimeUpdate{};

    const float* g_frameDeltaSeconds = nullptr;
    int32_t* g_frameTickDelta300 = nullptr;

    // Only ever touched from the thread that runs the frame update.
    double g_tickRemainder = 0.0;

    std::atomic<uint64_t> g_framesAdjusted{0};

    void __fastcall FrameTimeUpdateHook()
    {
        g_frameTimeUpdate.unsafe_call();

        if (!g_frameDeltaSeconds || !g_frameTickDelta300)
            return;

        const double delta = static_cast<double>(*g_frameDeltaSeconds);
        const double exactTicks = delta * kCharacterTickRate;

        // Below one tick per frame the engine would need zero-tick frames, which
        // its character update does not expect. Leave its own value alone and
        // drop the remainder rather than feed it something it cannot handle.
        if (!(exactTicks >= 1.0) || !std::isfinite(exactTicks))
        {
            g_tickRemainder = 0.0;
            return;
        }

        const double accumulated = g_tickRemainder + exactTicks;
        const auto wholeTicks = static_cast<int32_t>(accumulated);
        g_tickRemainder = accumulated - wholeTicks;

        *g_frameTickDelta300 = wholeTicks;
        g_framesAdjusted.fetch_add(1, std::memory_order_relaxed);
    }
} // namespace

bool mgs4::InstallCharacterTiming(const void* frameTimingStruct)
{
    if (!frameTimingStruct)
    {
        logging::Error("character: the frame timing struct is unavailable");
        return false;
    }

    const auto* base = static_cast<const uint8_t*>(frameTimingStruct);
    g_frameDeltaSeconds = reinterpret_cast<const float*>(base + kFrameDeltaOffset);
    g_frameTickDelta300 =
        reinterpret_cast<int32_t*>(const_cast<uint8_t*>(base) + kTickDelta300Offset);

    logging::Address("frameTickDelta300", reinterpret_cast<uintptr_t>(g_frameTickDelta300));

    const module_info::Section& text = module_info::Text();
    const size_t matches = memory::CountMatches(text, kFrameTimeUpdateSignature);
    if (matches != 1)
    {
        logging::Error("character: the frame-time update signature matched {} times, expected 1",
                       matches);
        return false;
    }

    uint8_t* update = memory::Scan(text, kFrameTimeUpdateSignature);
    logging::Address("frameTimeUpdate", reinterpret_cast<uintptr_t>(update));

    g_frameTimeUpdate =
        safetyhook::create_inline(update, reinterpret_cast<void*>(&FrameTimeUpdateHook));
    if (!g_frameTimeUpdate)
    {
        logging::Error("character: failed to hook the frame-time update");
        g_frameDeltaSeconds = nullptr;
        g_frameTickDelta300 = nullptr;
        return false;
    }

    logging::Info("character: 300 Hz tick remainder is now carried between frames");
    return true;
}

void mgs4::LogCharacterCounters()
{
    const uint64_t adjusted = g_framesAdjusted.load(std::memory_order_relaxed);
    if (adjusted == 0)
        return;

    const int32_t ticks = g_frameTickDelta300 ? *g_frameTickDelta300 : 0;
    logging::Info("character: {} frames adjusted, last tick delta {}, remainder {:.3f}", adjusted,
                  ticks, g_tickRemainder);
}
