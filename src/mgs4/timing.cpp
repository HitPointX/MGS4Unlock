#include "mgs4/timing.h"

#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/module.h"

// MGS4 drives most of its gameplay from a real frame delta, which is why raising
// the framerate leaves movement, animation and audio behaving correctly. Its
// cutscene playback ("polygon demos") is the exception: that system advances by
// a whole 60 Hz tick per call, so at 120 fps it runs at double speed and then
// stalls periodically as it resynchronises against the audio.
//
// The engine already tracks how many 60 Hz ticks a frame covered, in a small
// frame-timing struct. At 120 fps that counter is non-zero on roughly every
// other frame and zero in between. Gating the cutscene update on it lets the
// cutscene advance at its native 60 Hz while everything else keeps rendering at
// full rate. We do not cap the framerate.
//
// Two independent derivations land in the same struct, which is what gives us
// confidence it is the right one:
//
//     rva 0x23cf7b08   demo time-delta base   (lea r15 in the cutscene update)
//     rva 0x23cf7b10   60 Hz tick delta       (base + 0x8)
//     rva 0x23cf7b20   frame delta seconds    (movss in the camera update)
//     rva 0x23cf7b2c   300 Hz tick delta      (frame delta + 0xC)

namespace
{
    // Cloth. Snake's coat and kilt are integrated with a fixed step of roughly
    // 1/60 per call, so above 60 fps the simulation advances faster than
    // wall-clock time and the sway visibly speeds up.
    //
    // Gating the solver to the engine's native tick fixes the rate, but skipping
    // the call outright would leave the renderer drawing a stale transform on
    // the frames in between, which reads as judder. The engine separates the two:
    // a publish entry point pushes the current transform without integrating, so
    // on skipped frames we publish and only the integration is throttled.
    constexpr const char* kClothProducerUpdateSignature =
        "48 89 5C 24 18 55 57 41 56 48 81 EC 10 01 00 00 48 8B B9 70 03 00 00 "
        "45 33 F6 41 8B E8 48 8B D9";

    constexpr const char* kClothTransformPublishSignature =
        "48 8B 41 08 4C 8B D1 83 38 00 C7 40 04 00 00 00 00 74 ?? 49 8B 42 08 "
        "4C 63 40 04 44 3B 00";

    using ClothTransformPublishFn = void(__fastcall*)(uint8_t*);
    ClothTransformPublishFn g_clothTransformPublish = nullptr;
    SafetyHookInline g_clothProducerUpdate{};

    std::atomic<uint64_t> g_clothCalls{0};
    std::atomic<uint64_t> g_clothSkipped{0};

    // Derived from this build with tools/mksig.py, which wildcards the operands
    // that move on a relink. Verified to match exactly once across .text.
    constexpr const char* kPolygonDemoUpdateSignature =
        "48 89 5C 24 20 56 57 41 54 41 56 41 57 48 83 EC 40 48 8B 1D ?? ?? ?? ?? "
        "4C 8D B1 90 00 00 00 33 F6 4C 8D 3D ?? ?? ?? ?? 48 8B F9";

    // Inside that function: lea r15, [rip + disp32] loads the frame-timing
    // struct. The instruction is 7 bytes with the displacement at +3.
    constexpr ptrdiff_t kTimeDeltaLeaOffset = 0x21;
    constexpr ptrdiff_t kTimeDeltaLeaLength = 7;
    constexpr ptrdiff_t kTimeDeltaDispOffset = 3;
    constexpr std::array<uint8_t, 3> kLeaR15RipOpcode = {0x4C, 0x8D, 0x3D};

    constexpr ptrdiff_t kTickDelta60Offset = 8;

    const int32_t* g_frameTickDelta60 = nullptr;
    SafetyHookInline g_polygonDemoUpdate{};

    std::atomic<uint64_t> g_demoCalls{0};
    std::atomic<uint64_t> g_demoSkipped{0};

    // True on frames where the engine's 60 Hz clock actually advanced. Anything
    // gated on this runs at its native rate no matter how fast we render.
    bool IsNativeTick()
    {
        return g_frameTickDelta60 == nullptr || *g_frameTickDelta60 != 0;
    }

    void __fastcall ClothProducerUpdateHook(uint8_t* producer, float updateArgument,
                                            int32_t updateType)
    {
        g_clothCalls.fetch_add(1, std::memory_order_relaxed);

        if (IsNativeTick())
        {
            g_clothProducerUpdate.unsafe_call(producer, updateArgument, updateType);
            return;
        }

        g_clothSkipped.fetch_add(1, std::memory_order_relaxed);
        if (producer && g_clothTransformPublish)
            g_clothTransformPublish(producer);
    }

    void PolygonDemoUpdateHook(uint8_t* demo)
    {
        g_demoCalls.fetch_add(1, std::memory_order_relaxed);

        if (IsNativeTick())
            g_polygonDemoUpdate.unsafe_call(demo);
        else
            g_demoSkipped.fetch_add(1, std::memory_order_relaxed);
    }
} // namespace

bool mgs4::InstallCutsceneTimingFix()
{
    const module_info::Section& text = module_info::Text();

    const size_t matches = memory::CountMatches(text, kPolygonDemoUpdateSignature);
    if (matches != 1)
    {
        logging::Error("timing: the cutscene-update signature matched {} times, expected 1",
                       matches);
        return false;
    }

    uint8_t* update = memory::Scan(text, kPolygonDemoUpdateSignature);
    logging::Address("polygonDemoUpdate", reinterpret_cast<uintptr_t>(update));

    // Validate the instruction we are about to decode rather than trusting the
    // offset. A signature can match while the body has shifted underneath it.
    uint8_t* lea = update + kTimeDeltaLeaOffset;
    if (std::memcmp(lea, kLeaR15RipOpcode.data(), kLeaR15RipOpcode.size()) != 0)
    {
        logging::Error("timing: expected lea r15,[rip+d] at +{:#x}, found {:02X} {:02X} {:02X}",
                       kTimeDeltaLeaOffset, lea[0], lea[1], lea[2]);
        return false;
    }

    uint8_t* timing = memory::ResolveRipRelative(lea + kTimeDeltaDispOffset,
                                                 lea + kTimeDeltaLeaLength);
    g_frameTickDelta60 = reinterpret_cast<const int32_t*>(timing + kTickDelta60Offset);

    logging::Address("frameTimingStruct", reinterpret_cast<uintptr_t>(timing));
    logging::Address("frameTickDelta60", reinterpret_cast<uintptr_t>(g_frameTickDelta60));

    g_polygonDemoUpdate = safetyhook::create_inline(update,
                                                    reinterpret_cast<void*>(&PolygonDemoUpdateHook));
    if (!g_polygonDemoUpdate)
    {
        logging::Error("timing: failed to hook the cutscene update");
        g_frameTickDelta60 = nullptr;
        return false;
    }

    logging::Info("timing: cutscene playback gated to its native 60 Hz tick");
    return true;
}

bool mgs4::InstallClothTimingFix()
{
    const module_info::Section& text = module_info::Text();

    const size_t publishMatches = memory::CountMatches(text, kClothTransformPublishSignature);
    const size_t updateMatches = memory::CountMatches(text, kClothProducerUpdateSignature);
    if (publishMatches != 1 || updateMatches != 1)
    {
        logging::Error("timing: cloth signatures matched {} and {} times, expected 1 each",
                       updateMatches, publishMatches);
        return false;
    }

    uint8_t* publish = memory::Scan(text, kClothTransformPublishSignature);
    uint8_t* update = memory::Scan(text, kClothProducerUpdateSignature);
    logging::Address("clothTransformPublish", reinterpret_cast<uintptr_t>(publish));
    logging::Address("clothProducerUpdate", reinterpret_cast<uintptr_t>(update));

    // Publishing on skipped frames is what keeps the cloth from juddering, so
    // there is no point gating without it.
    g_clothTransformPublish = reinterpret_cast<ClothTransformPublishFn>(publish);

    g_clothProducerUpdate =
        safetyhook::create_inline(update, reinterpret_cast<void*>(&ClothProducerUpdateHook));
    if (!g_clothProducerUpdate)
    {
        logging::Error("timing: failed to hook the cloth producer update");
        g_clothTransformPublish = nullptr;
        return false;
    }

    logging::Info("timing: cloth simulation gated to its native 60 Hz tick");
    return true;
}

void mgs4::LogTimingCounters()
{
    const uint64_t calls = g_demoCalls.load(std::memory_order_relaxed);
    const uint64_t skipped = g_demoSkipped.load(std::memory_order_relaxed);
    if (calls == 0)
        return;

    logging::Info("timing: cutscene update called {} times, {} gated out ({:.1f}%)", calls, skipped,
                  100.0 * static_cast<double>(skipped) / static_cast<double>(calls));

    const uint64_t clothCalls = g_clothCalls.load(std::memory_order_relaxed);
    const uint64_t clothSkipped = g_clothSkipped.load(std::memory_order_relaxed);
    if (clothCalls != 0)
    {
        logging::Info("timing: cloth update called {} times, {} gated out ({:.1f}%)", clothCalls,
                      clothSkipped,
                      100.0 * static_cast<double>(clothSkipped) / static_cast<double>(clothCalls));
    }
}
