#include "mgs4/timing.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <string>
#include <cstdint>

#include <windows.h>

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
    const uint8_t* g_frameTimingStruct = nullptr;
    SafetyHookInline g_polygonDemoUpdate{};

    std::atomic<uint64_t> g_demoCalls{0};
    std::atomic<uint64_t> g_demoSkipped{0};

    // Set whenever the cutscene system actually advances, cleared by the frame
    // update. Cloth uses this to decide what rate to run at: during a cutscene
    // the animation that moves a garment's anchor points advances at 60 Hz, so
    // cloth simulated at the full frame rate takes several steps against
    // anchors that have not moved and then lurches when they do.
    std::atomic<bool> g_cutsceneAdvancing{false};
    std::atomic<uint64_t> g_lastDemoRunTick{0};

    // True on frames where the engine's 60 Hz clock actually advanced. Anything
    // gated on this runs at its native rate no matter how fast we render.
    bool IsNativeTick()
    {
        return g_frameTickDelta60 == nullptr || *g_frameTickDelta60 != 0;
    }

    // Records a value if it has not been seen before. Small fixed table: we only
    // care about the handful of distinct instance sizes in play.
    void RecordDistinct(std::atomic<uint32_t>* table, size_t count, uint32_t value)
    {
        if (value == 0)
            return;

        for (size_t i = 0; i < count; ++i)
        {
            uint32_t existing = table[i].load(std::memory_order_relaxed);
            if (existing == value)
                return;
            if (existing == 0 &&
                table[i].compare_exchange_strong(existing, value, std::memory_order_relaxed))
                return;
        }
    }

    void PolygonDemoUpdateHook(uint8_t* demo)
    {
        g_demoCalls.fetch_add(1, std::memory_order_relaxed);

        if (IsNativeTick())
        {
            g_lastDemoRunTick.store(::GetTickCount64(), std::memory_order_relaxed);
            g_cutsceneAdvancing.store(true, std::memory_order_relaxed);
            g_polygonDemoUpdate.unsafe_call(demo);
        }
        else
        {
            g_demoSkipped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Observation only. MGS4 runs several independent cloth solvers and which
    // one drives a given garment is not obvious from looking at it, so rather
    // than guess, these hooks call the original unconditionally and just report
    // what they are handling. Each solver identifies its instances by a size
    // field, so logging the distinct values seen tells us how many instances are
    // live and lets a specific garment be matched to a specific solver.
    //
    // Nothing here changes behaviour. Once we know which solver owns the robe
    // and which owns the headdress, the actual fix can target the right one.
    constexpr ptrdiff_t kJacketPointCountOffset = 0x00;

    SafetyHookInline g_directJacketUpdate{};

    std::atomic<uint64_t> g_jacketCalls{0};
    std::atomic<uint32_t> g_seenJacketPoints[8]{};

    std::atomic<uint64_t> g_jacketGated{0};

    // Offsets taken from the solver itself, which reads a pointer at +0x98 and
    // copies four sixteen-byte rows from it into +0x4b0 onwards. Replicated
    // here so a gated frame still publishes a current transform.
    constexpr ptrdiff_t kJacketTransformSourcePointer = 0x98;
    constexpr ptrdiff_t kJacketTransformDestination = 0x4B0;
    constexpr size_t kJacketTransformBytes = 0x40;

    void PublishJacketTransform(uint8_t* jacket)
    {
        if (!jacket)
            return;

        auto* source = *reinterpret_cast<uint8_t**>(jacket + kJacketTransformSourcePointer);
        if (!source)
            return;

        std::memcpy(jacket + kJacketTransformDestination, source, kJacketTransformBytes);
    }

    void __fastcall DirectJacketUpdateHook(uint8_t* jacket, uint8_t* context)
    {
        g_jacketCalls.fetch_add(1, std::memory_order_relaxed);
        if (jacket)
        {
            RecordDistinct(g_seenJacketPoints, std::size(g_seenJacketPoints),
                           *reinterpret_cast<uint16_t*>(jacket + kJacketPointCountOffset));
        }

        // Snake's jacket runs on its own solver, separate from both the cloth
        // producer and the hair chains. Like them it hangs off animated bones,
        // so during a cutscene it has to advance at the rate the animation moves
        // those bones rather than at the frame rate. Left alone it takes four
        // steps against a motionless body at 240 fps and then lurches.
        //
        // Returning early is not enough on its own. Near the end of the solve
        // the function copies a transform out of the object at +0x98 into a
        // published slot at +0x4b0, four sixteen-byte rows of a matrix, and the
        // renderer reads that slot. Skipping the whole call skips the copy too,
        // so the renderer keeps drawing an older transform and the jacket
        // appears doubled. Publishing it ourselves on a gated frame keeps what
        // is drawn current while the solve itself is skipped.
        if (config::Get().clothFollowsCutscene && mgs4::IsCutsceneAdvancing() && !IsNativeTick())
        {
            g_jacketGated.fetch_add(1, std::memory_order_relaxed);
            PublishJacketTransform(jacket);
            return;
        }

        g_directJacketUpdate.unsafe_call(jacket, context);
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
    g_frameTimingStruct = timing;
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

bool mgs4::InstallClothDiagnostics()
{
    const module_info::Section& text = module_info::Text();

    constexpr const char* kDirectJacketSignature =
        "48 8B C4 55 53 48 8D A8 78 FE FF FF 48 81 EC 78 02 00 00 4C 8B 89 58 04 00 00 "
        "48 8B D9";

    bool ok = true;

    if (memory::CountMatches(text, kDirectJacketSignature) == 1)
    {
        uint8_t* jacket = memory::Scan(text, kDirectJacketSignature);
        logging::Address("directJacketUpdate", reinterpret_cast<uintptr_t>(jacket));
        g_directJacketUpdate =
            safetyhook::create_inline(jacket, reinterpret_cast<void*>(&DirectJacketUpdateHook));
        if (!g_directJacketUpdate)
        {
            logging::Error("timing: failed to hook the direct-jacket update");
            ok = false;
        }
    }
    else
    {
        logging::Error("timing: the direct-jacket signature was not unique");
        ok = false;
    }

    // The hair solver is hooked by the cloth module, which needs it functionally.

    if (ok)
        logging::Info("timing: cloth diagnostics installed (observation only)");
    return ok;
}

void mgs4::LogTimingCounters(double intervalSeconds)
{
    static uint64_t lastDemo = 0, lastDemoGated = 0, lastJacket = 0;

    const uint64_t demo = g_demoCalls.load(std::memory_order_relaxed);
    const uint64_t demoGated = g_demoSkipped.load(std::memory_order_relaxed);
    const uint64_t jacket = g_jacketCalls.load(std::memory_order_relaxed);

    const auto rate = [intervalSeconds](uint64_t now, uint64_t& last) {
        const double per = intervalSeconds > 0.0
                               ? static_cast<double>(now - last) / intervalSeconds
                               : 0.0;
        last = now;
        return per;
    };

    const double demoRate = rate(demo, lastDemo);
    const double demoGatedRate = rate(demoGated, lastDemoGated);
    const double jacketRate = rate(jacket, lastJacket);

    if (demo != 0)
    {
        // "ran" is the rate that actually advances the cutscene. Anything driven
        // by cutscene animation should be moving at this rate, not the frame rate.
        logging::Info("survey: cutscene ran {:.0f}/s (gated {:.0f}/s)", demoRate - demoGatedRate,
                      demoGatedRate);
    }

    if (jacket != 0)
    {
        std::string sizes;
        for (const auto& slot : g_seenJacketPoints)
        {
            const uint32_t value = slot.load(std::memory_order_relaxed);
            if (value == 0)
                break;
            if (!sizes.empty())
                sizes += ", ";
            sizes += std::to_string(value);
        }
        static uint64_t lastJacketGated = 0;
        const double jacketGatedRate = rate(g_jacketGated.load(std::memory_order_relaxed),
                                            lastJacketGated);
        logging::Info("survey: direct jacket {:.0f}/s (gated {:.0f}/s), instance sizes: {}",
                      jacketRate - jacketGatedRate, jacketGatedRate,
                      sizes.empty() ? "none" : sizes);
    }
}

bool mgs4::IsCutsceneAdvancing()
{
    if (!g_cutsceneAdvancing.load(std::memory_order_relaxed))
        return false;

    // The flag is sticky, so it needs a timeout or it would stay set for the
    // rest of the session once a cutscene had played. A cutscene that is
    // running updates far more often than this.
    constexpr uint64_t kStaleAfterMs = 250;
    const uint64_t last = g_lastDemoRunTick.load(std::memory_order_relaxed);
    if (::GetTickCount64() - last > kStaleAfterMs)
    {
        g_cutsceneAdvancing.store(false, std::memory_order_relaxed);
        return false;
    }

    return true;
}

const void* mgs4::FrameTimingStruct()
{
    return g_frameTimingStruct;
}

const void* mgs4::FrameTickDelta60()
{
    return g_frameTickDelta60;
}
