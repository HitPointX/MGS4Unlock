#include "mgs4/timing.h"

#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <string>
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
    // The producer update is not just a solver call. Its first ~0xC4 bytes drain
    // two internal command queues and unconditionally flip a bit at producer+0x14
    // that looks like a double-buffer selector -- it runs on every call the
    // manager makes, which is every frame, regardless of whether anything
    // actually needs to simulate. Skipping the whole function on non-native
    // frames (as an early attempt here did) skips that bookkeeping too, and the
    // renderer ends up reading mismatched buffer state, which showed as a
    // doubled, ghosted image rather than the judder that gating usually causes.
    //
    // The function's own internal logic already separates the two concerns: past
    // the bookkeeping, it runs a couple of hash-gated checks and, when they say
    // there is nothing to simulate, jumps straight to a call into the transform
    // publish function and then a shared epilogue. That is the exact behaviour
    // we want on a non-native frame, and it is a path the stock code already
    // takes under some conditions, not a control-flow shape invented here.
    //
    // So rather than hook the function's entry, a mid-hook sits at the start of
    // the simulate-or-not decision (right before the first hash-gate call) and,
    // on a non-native frame, redirects straight to that existing publish-then-
    // return path. The bookkeeping before this point always runs; only the
    // simulation body in between is skipped. The block between the two
    // addresses was checked with a full disassembly (not a raw byte scan) and
    // contains no push/pop/stack-pointer adjustment, so the redirect lands with
    // the same stack frame and the same live registers (producer pointer in
    // rbx, a zero constant in r14) that the target expects.
    constexpr const char* kClothProducerUpdateSignature =
        "48 89 5C 24 18 55 57 41 56 48 81 EC 10 01 00 00 48 8B B9 70 03 00 00 "
        "45 33 F6 41 8B E8 48 8B D9";
    constexpr ptrdiff_t kClothSimulateGateOffset = 0xC4;    // call <hash gate>
    constexpr ptrdiff_t kClothPublishAndReturnOffset = 0x3C2; // mov rcx,rbx / call publish

    // The epilogue restores rsi from this stack slot, but the instruction that
    // *saves* rsi into it sits after our hook point, so on the redirected path
    // the slot is never written. Restoring from it would hand the caller a
    // garbage callee-saved register. Writing the live rsi there ourselves makes
    // the restore a no-op instead. rsp is unchanged between the hook site and
    // the epilogue (verified: no push/pop/rsp adjustment in that range), so the
    // slot address computed here is the one the epilogue will read.
    constexpr ptrdiff_t kClothSavedRsiSlot = 0x138;

    // Early in the function, before the simulate gate, a bit is toggled at
    // producer+0x14. That field is not a plain frame counter: it is read as a
    // 16-bit value, passed into the solver, and written back afterwards, so it
    // is simulation state that only a real simulation pass consumes and
    // advances. Letting the toggle stand on a frame we skip advances it with
    // nothing to consume it, which desynchronises the state the renderer draws
    // from and shows up as a doubled, semi-transparent overlay. Toggling it
    // back leaves the field exactly as the last simulated frame left it.
    //
    // rbx holds the producer pointer here and is never reassigned between the
    // toggle and the hook site (verified by disassembling that range).
    constexpr ptrdiff_t kClothSimStateOffset = 0x14;
    constexpr uint32_t kClothSimStateToggleBit = 1;
    constexpr uint8_t kClothSimulateGateOpcode = 0xE8; // call rel32

    SafetyHookMid g_clothSimulateGate{};
    uintptr_t g_clothPublishAndReturnTarget = 0;

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

    void ClothSimulateGateHook(SafetyHookContext& ctx)
    {
        g_clothCalls.fetch_add(1, std::memory_order_relaxed);

        if (IsNativeTick())
            return;

        g_clothSkipped.fetch_add(1, std::memory_order_relaxed);

        // See kClothSimStateOffset: undo the toggle that ran before this point,
        // since the simulation pass that would have consumed it is being skipped.
        if (ctx.rbx != 0)
            *reinterpret_cast<uint32_t*>(ctx.rbx + kClothSimStateOffset) ^= kClothSimStateToggleBit;

        // See kClothSavedRsiSlot: the epilogue we are jumping into restores rsi
        // from a slot that only the skipped code path writes.
        *reinterpret_cast<uint64_t*>(ctx.rsp + kClothSavedRsiSlot) = ctx.rsi;

        ctx.rip = g_clothPublishAndReturnTarget;
    }

    void PolygonDemoUpdateHook(uint8_t* demo)
    {
        g_demoCalls.fetch_add(1, std::memory_order_relaxed);

        if (IsNativeTick())
            g_polygonDemoUpdate.unsafe_call(demo);
        else
            g_demoSkipped.fetch_add(1, std::memory_order_relaxed);
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
    constexpr ptrdiff_t kHairChainCountOffset = 0x260;

    SafetyHookInline g_directJacketUpdate{};
    SafetyHookInline g_hairSimulationUpdate{};

    std::atomic<uint64_t> g_jacketCalls{0};
    std::atomic<uint64_t> g_hairCalls{0};
    std::atomic<uint32_t> g_seenJacketPoints[8]{};
    std::atomic<uint32_t> g_seenHairChains[8]{};

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

    void __fastcall DirectJacketUpdateHook(uint8_t* jacket, uint8_t* context)
    {
        g_jacketCalls.fetch_add(1, std::memory_order_relaxed);
        if (jacket)
        {
            RecordDistinct(g_seenJacketPoints, std::size(g_seenJacketPoints),
                           *reinterpret_cast<uint16_t*>(jacket + kJacketPointCountOffset));
        }
        g_directJacketUpdate.unsafe_call(jacket, context);
    }

    void __fastcall HairSimulationUpdateHook(uint8_t* hair)
    {
        g_hairCalls.fetch_add(1, std::memory_order_relaxed);
        if (hair)
        {
            RecordDistinct(g_seenHairChains, std::size(g_seenHairChains),
                           *reinterpret_cast<uint32_t*>(hair + kHairChainCountOffset));
        }
        g_hairSimulationUpdate.unsafe_call(hair);
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

    const size_t updateMatches = memory::CountMatches(text, kClothProducerUpdateSignature);
    if (updateMatches != 1)
    {
        logging::Error("timing: cloth producer signature matched {} times, expected 1",
                       updateMatches);
        return false;
    }

    uint8_t* update = memory::Scan(text, kClothProducerUpdateSignature);
    logging::Address("clothProducerUpdate", reinterpret_cast<uintptr_t>(update));

    uint8_t* gate = update + kClothSimulateGateOffset;
    if (*gate != kClothSimulateGateOpcode)
    {
        logging::Error("timing: expected a call at the cloth simulate-gate site, found {:02X}",
                       gate[0]);
        return false;
    }

    g_clothPublishAndReturnTarget =
        reinterpret_cast<uintptr_t>(update) + kClothPublishAndReturnOffset;
    logging::Address("clothSimulateGate", reinterpret_cast<uintptr_t>(gate));
    logging::Address("clothPublishAndReturn", g_clothPublishAndReturnTarget);

    g_clothSimulateGate = safetyhook::create_mid(gate, &ClothSimulateGateHook);
    if (!g_clothSimulateGate)
    {
        logging::Error("timing: failed to hook the cloth simulate gate");
        g_clothPublishAndReturnTarget = 0;
        return false;
    }

    logging::Info("timing: cloth simulation gated to its native 60 Hz tick");
    return true;
}

bool mgs4::InstallClothDiagnostics()
{
    const module_info::Section& text = module_info::Text();

    constexpr const char* kDirectJacketSignature =
        "48 8B C4 55 53 48 8D A8 78 FE FF FF 48 81 EC 78 02 00 00 4C 8B 89 58 04 00 00 "
        "48 8B D9";
    constexpr const char* kHairSimulationSignature =
        "40 53 48 81 EC D0 00 00 00 48 8B D9 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 81 "
        "48 02 00 00 48 85 C0";

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

    if (memory::CountMatches(text, kHairSimulationSignature) == 1)
    {
        uint8_t* hair = memory::Scan(text, kHairSimulationSignature);
        logging::Address("hairSimulationUpdate", reinterpret_cast<uintptr_t>(hair));
        g_hairSimulationUpdate =
            safetyhook::create_inline(hair, reinterpret_cast<void*>(&HairSimulationUpdateHook));
        if (!g_hairSimulationUpdate)
        {
            logging::Error("timing: failed to hook the hair simulation update");
            ok = false;
        }
    }
    else
    {
        logging::Error("timing: the hair-simulation signature was not unique");
        ok = false;
    }

    if (ok)
        logging::Info("timing: cloth diagnostics installed (observation only)");
    return ok;
}

void mgs4::LogTimingCounters()
{
    // Each counter reports independently. An earlier version returned early when
    // the cutscene count was zero, which silently suppressed every other line
    // during normal gameplay.
    const uint64_t calls = g_demoCalls.load(std::memory_order_relaxed);
    const uint64_t skipped = g_demoSkipped.load(std::memory_order_relaxed);
    if (calls != 0)
    {
        logging::Info("timing: cutscene update called {} times, {} gated out ({:.1f}%)", calls,
                      skipped,
                      100.0 * static_cast<double>(skipped) / static_cast<double>(calls));
    }

    const uint64_t clothCalls = g_clothCalls.load(std::memory_order_relaxed);
    const uint64_t clothSkipped = g_clothSkipped.load(std::memory_order_relaxed);
    if (clothCalls != 0)
    {
        logging::Info("timing: cloth update called {} times, {} gated out ({:.1f}%)", clothCalls,
                      clothSkipped,
                      100.0 * static_cast<double>(clothSkipped) / static_cast<double>(clothCalls));
    }

    const auto formatSeen = [](std::atomic<uint32_t>* table, size_t count) {
        std::string out;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t value = table[i].load(std::memory_order_relaxed);
            if (value == 0)
                break;
            if (!out.empty())
                out += ", ";
            out += std::to_string(value);
        }
        return out.empty() ? std::string("none") : out;
    };

    const uint64_t jacketCalls = g_jacketCalls.load(std::memory_order_relaxed);
    if (jacketCalls != 0)
    {
        logging::Info("timing: direct-jacket called {} times, instance sizes seen: {}", jacketCalls,
                      formatSeen(g_seenJacketPoints, std::size(g_seenJacketPoints)));
    }

    const uint64_t hairCalls = g_hairCalls.load(std::memory_order_relaxed);
    if (hairCalls != 0)
    {
        logging::Info("timing: hair simulation called {} times, chain counts seen: {}", hairCalls,
                      formatSeen(g_seenHairChains, std::size(g_seenHairChains)));
    }
}
